# Plano de implementação — Checkpoint 5 (Aluno 2)

Prazo: **30/10/2026**. Papel: Aluno 2.

Este documento descreve **como** o Aluno 2 implementa o Two-Phase Commit e o Incremental State Transfer. Os pontos em que isso encosta no Aluno 1 estão em [`contrato-aluno-1-aluno-2.md`](contrato-aluno-1-aluno-2.md). A fonte de verdade é o item 10 (*CHECKPOINT 5*) e as seções *TWO-PHASE COMMIT (2PC)* e *INCREMENTAL STATE TRANSFER (IST)* de [`docs/specification/trabalho_2026_SD.pdf`](../../specification/trabalho_2026_SD.pdf).

O checkpoint 4 replicava cada entrada do operation log com um broadcast de `LOG_APPEND`. Aqui esse broadcast sai. A entrada só entra na hash table depois do `COMMIT`. O formato do registro (`smr_entry_t`, `log_append_pack`) permanece: `PREPARE` e `STATE_TRANSFER` carregam os mesmos bytes.

Não há `tests/c5/run.sh`. O roteiro de avaliação é o do item 10 da spec. Os harnesses do C1, do C2, do C3 e do C4 permanecem como estão.

Não substitui o relatório final do checkpoint (`deliverable/`).

## O que o checkpoint pede do Aluno 2

Da especificação, no item 10:

```text
PREPARE
  ↓
YES / NO
  ↓
COMMIT / ABORT
```

E o IST:

```text
Novo nó: version 1032
Coordenador: 1050
Transferir: 1033 … 1050 em vez de toda a base.
```

O fluxo da seção de 2PC é `STORE` → coordenador → `PREPARE` → participantes → YES / NO → `COMMIT` ou `ABORT` → atualização da máquina de estados. A seção de IST pede o último checkpoint e, em seguida, só os logs posteriores. A seção de recuperação diz: se o coordenador cai durante o `PREPARE`, há timeout, eleição, o novo coordenador consulta os logs e toma a decisão final.

O item 10 ainda descreve o roteiro. Sobem SP1, SP2 e SP3. Há `UPLOAD A`, `UPLOAD B` e `UPLOAD C`. SP2 cai. Enquanto SP2 está fora, há `UPLOAD D` e `UPLOAD E`. SP2 reinicia, faz `JOIN`, pede IST, recebe os logs que faltam e fica `READY`. À parte, uma falha forçada na fase `PREPARE` não pode deixar os participantes inconsistentes.

LFU (`cache hit`, `cache miss`, `frequency`, `eviction`) e a transferência de arquivo ficam com o Aluno 1. O Aluno 2 não implementa cache.

## Estado atual

Em `dev`, depois do checkpoint 4:

- `PREPARE`, `COMMIT`, `ABORT`, `SNAPSHOT` e `STATE_TRANSFER` já estão no enum de `include/common/protocol.h`. Em `src/common/protocol.c` os cinco têm `payload_sizes` `-1`. `SNAPSHOT` e `STATE_TRANSFER` saem de `recv_message` sem o corpo ser lido. `PREPARE`, `COMMIT` e `ABORT` caem no `default` de `serialize_message` e `deserialize_message`.
- `payload_is_data` cobre `STORE`, `LOOKUP`, `DOWNLOAD_REQ`, `DOWNLOAD_REP` e `LOG_APPEND`. Um `PREPARE` com o metadata do `STORE` passa de `MAX_CONTROL_PAYLOAD_SZ` e seria recusado se não entrar nessa lista.
- `seal_locked` em `src/superpeer/smr.c` grava a entrada na hash table e só depois `broadcast_append` manda o `LOG_APPEND`. Um destino que não conecta é ignorado. O `ACK` lido no broadcast não desfaz o apply.
- Quem não é o coordenador encaminha a proposta como `LOG_APPEND` com `log_index` 0. O coordenador sela e devolve a entrada. Esse caminho fica.
- `smr_on_coordinator` em `include/superpeer/smr.h`, quando este nó vira líder, copia `last_applied` para `checkpoint_index` e imprime `SMR checkpoint` e `resume operations`. Seguidores retornam sem pedir estado. Não há IST.
- O log é memória do processo, teto `SMR_LOG_MAX` (256) em `include/superpeer/smr.h`. Reiniciar o processo zera o log.
- `superpeer_handle_store` em `src/superpeer/superpeer.c` responde `ACK` quando `smr_propose_store` retorna 1, e `ERROR` com `"replication failed"` quando retorna 0. O `upload` do cliente já usa esse `STORE`.
- Cada conexão aceita roda em thread própria (`connection_worker`). Um `PREPARE` de volta, enquanto o encaminhador espera a resposta do coordenador, tem thread para ser atendido.
- Não existe módulo de cache. O ponteiro de invalidação do contrato C6 nasce `NULL`.

O C1 e o C2 sobem um único Super Peer. Sem outros participantes, o `STORE` continua terminando em `ACK`.

## Design

### 2PC

O cliente manda `STORE` ou `PUT` a qualquer Super Peer, como no checkpoint 4. Quem não é o coordenador encaminha com `LOG_APPEND` de índice 0. O coordenador sela `log_index`, `transaction_id`, `version` e checksum, guarda a entrada num slot `PREPARED` e **não** chama `smr_log_apply`. Em seguida abre o 2PC.

```text
Cliente → STORE
Coordenador sela a entrada no slot PREPARED
PREPARE(entrada) → cada participante vivo
ACK = YES, ERROR = NO
todos YES → COMMIT → apply → ACK ao cliente
algum NO, ou falha de conexão → ABORT → ERROR ao cliente
```

Um slot por processo. `two_pc_lock` no `superpeer_t` cobre a rodada no coordenador: outro `STORE` espera. O lock não é segurado durante `connect` ou `send`, e não é tomado junto com `smr_lock`. `smr_lock` só protege o slot e o log, em trechos curtos.

Participantes: membros `SUPERPEER` que não estão `SUSPECT`, `FAILED` nem `REMOVED`, e que não são o próprio coordenador. O filtro é o de `broadcast_append`, com `SUSPECT` a mais. Quem ficou de fora alcança o estado pelo IST.

O voto é YES só quando as três condições fecham: o checksum confere, `log_index` é `last_applied + 1`, e não há outro `PREPARED`. Qualquer outra situação responde NO. No fio, YES é `ACK` e NO é `ERROR`, no mesmo socket do `PREPARE`. O stdout imprime a palavra `YES` ou `NO`. Não há tipo novo no enum.

`COMMIT` e `ABORT` levam `transaction_id` (16 bytes) e `log_index` (`uint64`, big-endian), 24 bytes. `COMMIT` aplica com `smr_log_apply`. Aplicar de novo o mesmo índice continua no-op e não soma `version`. `ABORT` descarta o slot e não escreve na hash table.

Sem participante remoto — o C1, o C2, e a janela com um Super Peer só — o coordenador comita local e `smr_propose_store` retorna 1. O `ACK` do `STORE` sai como hoje.

A resposta do `LOG_APPEND` de índice 0 continua levando a entrada selada. Se o `COMMIT` já aplicou essa entrada no participante que encaminhou, o `smr_log_apply` da resposta é no-op.

`PREPARE` e cada `STATE_TRANSFER` usam os bytes de `log_append_pack` (`LOG_APPEND_PREFIX` mais o metadata, quando a operação é `STORE` ou `UPDATE`). Os helpers de pack e unpack desses dois tipos ficam ao lado de `log_append_pack` em `src/common/protocol.c`.

### IST

O pedinte informa o próprio `last_applied`. O remetente devolve o checkpoint e as entradas estritamente posteriores a esse índice. Não há segunda cópia da tabela e não há compactação: o corpo transferido é o trecho do log que o pedinte não tem. Com o pedinte em 1032 e o coordenador em 1050, saem as entradas 1033 a 1050. Com o pedinte em 0, sai o log inteiro que o remetente ainda tem em memória.

RPC no mesmo socket:

```text
SNAPSHOT pedido:    kind = 1, last_applied
SNAPSHOT resposta:  kind = 2, checkpoint_index, last_applied do remetente, missing_count
STATE_TRANSFER × missing_count, em ordem de log_index
```

`kind`, os inteiros e a ordem são big-endian, como o resto do framing. `missing_count = 0` encerra a RPC: o pedinte já está em dia. Cada `STATE_TRANSFER` é uma `smr_entry_t`. O pedinte aplica em ordem. Buraco não avança `last_applied`, a mesma regra de `smr_log_apply`.

O pedido sai de `smr_on_coordinator` quando este nó vira seguidor. Isso cobre o SP2 que reinicia e o ex-coordenador que volta como `FOLLOWER`. Ao virar líder, a ordem é outra:

1. Pergunta o `last_applied` de cada Super Peer vivo, com o mesmo `SNAPSHOT`.
2. Se alguém está à frente, puxa o trecho que falta.
3. Resolve o slot `PREPARED` em aberto, com a regra da seção seguinte.
4. Marca `checkpoint_index` com `last_applied`, como o checkpoint 4 já faz.
5. Imprime `resume operations`.

`checkpoint_index` continua sendo o marco. O snapshot não é um dump da hash table.

### Coordenador cai no PREPARE

O log é memória. O processo morto leva o slot embora. A decisão usa só o que os sobreviventes ainda têm:

- Algum sobrevivente tem `COMMIT` daquele `transaction_id`: o novo coordenador completa o `COMMIT` em quem ainda não aplicou.
- Ninguém tem `COMMIT`: `ABORT`. A hash table de quem votou YES permanece no último `COMMIT`. O upload interrompido não aparece na tabela.

A demonstração usa `BT_PAUSE_AFTER_PREPARE`, em segundos, lida pelo coordenador entre o último YES e o envio do `COMMIT`. Sem a variável, a pausa é zero. O cliente não lê essa variável.

### Linhas que a avaliação lê

No `COMMIT` que instala um nome:

```text
UPLOAD <nome>
```

Na rodada de 2PC:

```text
PREPARE
YES
NO
COMMIT
ABORT
```

`YES` e `NO` são o voto impresso. No fio continuam `ACK` e `ERROR`.

No IST do nó que voltou:

```text
IST
versions missing
receive logs
READY
```

`SMR checkpoint` e `resume operations` continuam saindo quando o líder termina a recuperação. `ELECTION` e `COORDINATOR` continuam com o Aluno 1.

### Gancho do LFU

Depois de cada apply de `COMMIT` e de cada entrada aplicada pelo IST, com `smr_lock` já solto, o Super Peer chama o ponteiro `lfu_invalidate` de `superpeer_t` se ele não for nulo. O Aluno 1 preenche o ponteiro quando o cache existir. `NULL` é no-op. O Gossip não carrega metadata; a invalidação não espera `GOSSIP`.

## Build e testes

### `tests/superpeer/test_2pc.c`

Sem socket, no molde de `tests/superpeer/test_smr.c`. A decisão e o apply são chamada direta.

- Todos os votos YES: a entrada entra na tabela uma vez, com a `version` selada.
- Um voto NO: a tabela não ganha o registro e o slot `PREPARED` fica vazio.
- Aplicar duas vezes o mesmo `log_index` deixa a `version` quieta.
- Coordenador ausente no `PREPARE`, e nenhum sobrevivente com `COMMIT` daquele `transaction_id`: a decisão é `ABORT`.
- Um sobrevivente com `COMMIT`: a decisão completa o `COMMIT` em quem ainda não aplicou.

### `tests/superpeer/test_ist.c`

Dois logs no mesmo processo. O delta é calculado por chamada direta, sem socket.

- Pedinte com `last_applied` 3 e remetente com 5 recebe só as entradas 4 e 5.
- Pedinte com `last_applied` 0 recebe o log inteiro do remetente.
- `missing_count` 0 quando os dois já estão no mesmo índice.
- Um índice com buraco não avança `last_applied`.

### `Makefile`

`bin/test_2pc` e `bin/test_ist`, no mesmo estilo de `bin/test_smr`, e uma linha de cada no alvo `test`.

### Os scripts, sem edição

Nenhum harness é alterado: `scripts/demo/script_testes_cp1.sh`, `tests/c2/run.sh`, `tests/c3/run.sh`, `tests/c4/run.sh`, `tests/common.sh`, `tests/env.sh`.

Critério de pronto:

1. `make test` verde, com `test_2pc` e `test_ist`.
2. C1, C2 e C3 no mesmo veredito de hoje. O `STORE` do C2 continua `ACK` e o arquivo baixado continua igual ao enviado.
3. `bash tests/c4/run.sh` imprime `FAIL: 0`. `put` e `state` saem 0. O `ACK` do `put` só sai depois do `COMMIT`, e o `state` na porta 5101 mostra `c4-file`.
4. No roteiro do item 10, com três Super Peers: A, B e C presentes nos três; D e E replicados enquanto SP2 está fora; depois do reinício, SP2 imprime `IST`, `versions missing`, `receive logs` e `READY`, e fica com A–E. Com `BT_PAUSE_AFTER_PREPARE` e o coordenador morto nessa janela, o upload interrompido não aparece na tabela dos sobreviventes.

## Ordem de trabalho

1. Slot `PREPARED`, decisão `COMMIT` / `ABORT` e `test_2pc`. Não depende de socket nem do Aluno 1.
2. Delta do IST e `test_ist`, também sem socket.
3. Layouts em `src/common/protocol.c`: `PREPARE` e `STATE_TRANSFER` no caminho de `LOG_APPEND`, `COMMIT` e `ABORT` com 24 bytes, `SNAPSHOT` com o pedido e a resposta. Sai o retorno de `recv_message` que devolve `SNAPSHOT` e `STATE_TRANSFER` sem ler o corpo. `PREPARE` e `STATE_TRANSFER` entram em `payload_is_data`.
4. `smr_propose_store` e `smr_propose_put` trocam `broadcast_append` pela rodada de 2PC. `seal_locked` deixa de aplicar antes do voto.
5. IST em `smr_on_coordinator`: seguidor pede, líder puxa quem está à frente e resolve o `PREPARED`.
6. Linhas `UPLOAD`, `PREPARE`, `YES`, `NO`, `COMMIT`, `ABORT`, `IST`, `versions missing`, `receive logs`, `READY`. Pausa `BT_PAUSE_AFTER_PREPARE`.
7. Chamada de `lfu_invalidate` depois do apply, se o ponteiro não for nulo.
8. Rodar, nesta ordem, `make test`, o script do C1, `tests/c2/run.sh`, `tests/c3/run.sh` e `tests/c4/run.sh`.

Os passos 1 e 2 não esperam o Aluno 1. O passo 3 é o apoio em `protocol.c` do contrato C3. O passo 8 fecha com o `ACK` do `STORE` ainda saindo de `superpeer_handle_store`.

## Riscos

- **Apply antes do voto.** Deixar `smr_log_apply` dentro de `seal_locked` grava o upload nos participantes que receberam `PREPARE` mesmo quando a rodada aborta. O apply fica no `COMMIT`.
- **`STORE` do C2.** Esperar voto de um participante que não existe, ou abrir socket para si, segura o upload dos 2 s do C2. Sem participante remoto, o commit é local.
- **`put` do C4.** O harness só vê o arquivo se o `ACK` esperar o `COMMIT`. Responder `ACK` no `PREPARE` faz o `state` da porta 5101 correr na frente da tabela.
- **Corpo deixado no socket.** Manter `SNAPSHOT` ou `STATE_TRANSFER` no atalho que não lê o payload dessincroniza a mensagem seguinte. Os cinco tipos do checkpoint são lidos por inteiro antes do handler.
- **Teto de payload.** `PREPARE` com metadata de `STORE` estoura `MAX_CONTROL_PAYLOAD_SZ` se ficar fora de `payload_is_data`. O mesmo vale para `STATE_TRANSFER`.
- **Lock na rede.** Segurar `smr_lock` ou `two_pc_lock` durante `connect` ou `send` trava o `PREPARE` que volta para o encaminhador. A thread da conexão atende o voto; os locks saem antes do I/O.
- **Log do coordenador morto.** Não há disco. A decisão lê os sobreviventes. Sem nenhum `COMMIT` registrado, a rodada aborta, e os que votaram YES ficam na versão anterior.
- **`SUSPECT` de fora da rodada.** Um Super Peer suspeito não vota. Se ele estava vivo, o IST posterior entrega o trecho que faltou. Incluí-lo no 2PC faz a rodada esperar o timeout de conexão.
- **Teto de 256 entradas.** IST não reconstrói o que nenhum sobrevivente ainda tem em memória. O roteiro A–E cabe. Um log que passou de `SMR_LOG_MAX` recusa entrada nova, como o checkpoint 4 já recusa.
- **Pausa e o cliente.** `BT_PAUSE_AFTER_PREPARE` só existe no coordenador. Um timeout novo no `upload` do Aluno 1, menor que essa pausa, falha a demonstração. O contrato C8 trava os dois lados antes de esse timeout aparecer.
- **Conflito em `protocol.c` e `superpeer_t`.** O LFU do Aluno 1 pode editar os mesmos arquivos. Os tipos novos não entram no enum. O ponteiro `lfu_invalidate` é o único campo que o Aluno 1 acrescenta para este plano.
- **Cache sem o gancho.** Apply sem chamar `lfu_invalidate` deixa o lookup do Aluno 1 devolver o registro anterior ao `COMMIT`. A chamada fica no plano mesmo enquanto o ponteiro é `NULL`.
