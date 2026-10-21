# Plano de implementação — Checkpoint 4 (Aluno 2)

Prazo: **21/10/2026**. Papel: Aluno 2.

Este documento descreve **como** o Aluno 2 implementa o operation log e a replicação por SMR. Complementa o plano do Aluno 1 em [`aluno-1.md`](aluno-1.md) (PR #46): lá estão `ELECTION`, `OK` e `COORDINATOR`. A fonte de verdade é a seção *STATE MACHINE REPLICATION* e o item 9 (*CHECKPOINT 4*) de [`docs/specification/trabalho_2026_SD.pdf`](../../specification/trabalho_2026_SD.pdf).

A referência do teste de integração é `tests/c4/run.sh`, com o roster `tests/config/c4.conf`. Os harnesses do C1, do C2 e do C3 permanecem como estão.

Não substitui o relatório final do checkpoint (`deliverable/`).

## O que o checkpoint pede do Aluno 2

Da especificação, no item 9:

- operation log
- log index
- transaction ID
- version
- checksum

O exemplo do enunciado é:

```text
LOG #101
PUT fileA

LOG #102
PUT fileB

LOG #103
DELETE fileA
```

A avaliação sobe SP1–SP4, mata o coordenador, elege outro e retoma as operações. O passo seguinte é um `upload`. O registro desse upload fica logicamente igual em SP1, SP2 e SP3: mesmo nome, mesmo ObjectID, mesma `version` e os mesmos hashes. A unidade replicada é esse registro. Cada Super Peer continua com a própria hash table. As tabelas convergem porque cada entrada do log é aplicada nos sobreviventes, na mesma ordem. O envio da tabela inteira é o checkpoint e o IST, que ficam no checkpoint 5.

2PC (`PREPARE`, `COMMIT`, `ABORT`) e IST (`SNAPSHOT`, `STATE_TRANSFER`) são o checkpoint 5. No CP4 o coordenador replica a entrada do log em broadcast. O formato do registro permanece quando o 2PC entrar no lugar desse broadcast.

## O que o script faz

`tests/c4/run.sh` carrega `tests/common.sh`, que carrega `tests/env.sh`. Esses dois já estão em `dev`. Esta entrega acrescenta o script e o roster.

A sequência é fixa:

1. Sobe quatro processos, todos com o mesmo arquivo e com `--port` diferente:

   ```bash
   bin/node --config tests/config/c4.conf --port 5101
   bin/node --config tests/config/c4.conf --port 5102
   bin/node --config tests/config/c4.conf --port 5103
   bin/node --config tests/config/c4.conf --port 5104
   ```

   A stdout e a stderr dos quatro vão para o mesmo log.

2. Espera 8 segundos.

3. Manda `kill -9` no quarto processo. A ordem do laço é 5101, 5102, 5103, 5104, então o morto é a porta 5104, a linha `SP4` do roster, `prioridade` 4. O comentário do script diz que esse nó é o coordenador esperado.

4. Espera `FAILURE_TIMEOUT_SEC + 4` segundos. O default em `tests/env.sh` é 15, então a espera é de 19 segundos.

5. Se `bin/client` existir, chama:

   ```bash
   bin/client --cmd put --name c4-file --value value1 --host 127.0.0.1 --port 5101
   bin/client --cmd state --host 127.0.0.1 --port 5101
   ```

6. Exige no log, com `grep -E`:

   - `ELECTION` ou `Election`
   - `COORDINATOR` ou `Coordinator`
   - `SMR`, `log`, `LogIndex` ou `checkpoint`

7. Exige a ausência de segmentation fault, core dump, double free, heap-buffer-overflow e deadlock.

O roster, `tests/config/c4.conf`, é uma linha CSV por Super Peer:

```text
nome,tipo,ip,porta,id,prioridade
SP1,superpeer,127.0.0.1,5101,00000001,1
...
SP4,superpeer,127.0.0.1,5104,00000004,4
```

`node_roster_load` em `include/common/config.h` confere `id` e `prioridade` e não os usa no NodeID. O NodeID continua `SHA-256(IP || porta)`.

### `put` e `state` contam como `&&`

O arquivo do harness escreve `|| true` depois de cada comando do cliente. O critério deste plano lê essa sequência como `&&`: `put` e `state` saem com status 0, nessa ordem. Um `summary` verde com os dois falhando não fecha o checkpoint.

- `put` sela a entrada `c4-file` e o cliente só termina quando o `ACK` volta.
- `state`, na mesma porta 5101, imprime esse nome e o `LogIndex` em que ela ficou.

`ELECTION` e `COORDINATOR` são do Aluno 1. O Aluno 2 imprime a linha do log. Os dois lados precisam estar no mesmo binário para o script fechar.

## Estado atual

Em `dev`, depois do checkpoint 3 e do plano do Aluno 1:

- `ELECTION`, `OK` e `COORDINATOR` já estão no enum de `include/common/protocol.h`. Em `src/common/protocol.c` os três ainda têm `payload_sizes` `-1` e caem no `default`. A PR #46 descreve ligá-los como payload de tamanho 0. Este plano não mexe nesses três `case`.
- `STORE` grava na hash table em `src/superpeer/superpeer.c` via `metadata_table_put`. Não há log, coordenador nem réplica entre Super Peers.
- `metadata_table_put` em `include/superpeer/metadata.h` incrementa `version` quando o ObjectID já existe. Reaplicar a mesma entrada por esse caminho somaria a versão de novo.
- O C1 e o C2 sobem um único Super Peer em `127.0.0.1:55101` e fazem `STORE` antes de qualquer eleição. Esse caminho continua aplicando localmente e respondendo `ACK`.
- `recv_message` devolve `GOSSIP`, `SNAPSHOT` e `STATE_TRANSFER` sem ler o corpo. A mensagem nova do log fica fora dessa lista: o corpo é lido e o CRC é conferido antes do handler.

## Design

O cliente manda `STORE` ou `PUT` a qualquer Super Peer. Quem não é o coordenador encaminha. O coordenador sela `LogIndex`, `version` e checksum, manda `LOG_APPEND` aos outros Super Peers vivos e aplica a mesma entrada localmente. Cada um instala o registro com a `version` que veio na entrada.

Módulo novo `include/superpeer/smr.h` e `src/superpeer/smr.c`. `superpeer_t` ganha o log e um `smr_lock`, separado de `members_lock` e `metadata_lock`. O lock não é segurado durante `connect` ou `send`.

Cada entrada, alinhada à tabela *Log Distribuído* da spec:

- `log_index` — `uint64`, começa em 1, só cresce no coordenador.
- `transaction_id` — 16 bytes, o mesmo tamanho de `trsc_id`.
- `operation` — `STORE`, `DELETE`, `UPDATE` e `PUT`. `PUT` é o exemplo `LOG #n` / `PUT fileA` e o `--cmd put` do script.
- `object_id` — 32 bytes. No `PUT` do harness, `SHA-256(nome || valor)`. No `STORE` do upload, o ObjectID que já veio no registro.
- `timestamp` — tempo lógico igual ao `log_index`. O relógio de parede fica fora dos bytes do checksum, para as réplicas concordarem.
- `version` — a versão que a hash table deve ficar depois do apply. Objeto novo: a versão do `STORE`, ou 1 no `PUT`. Objeto que já existe: a versão armazenada mais um, decidida uma vez, no coordenador, antes de selar a entrada.
- `checksum` — `SHA-256` do comando canônico, 32 bytes, com o wrapper `sha256()` de `src/common/node.c`. O campo do checksum fica zerado nessa conta.

`metadata_table_install`, ao lado de `metadata_table_put` em `src/superpeer/metadata.c`, copia o registro com a `version` da entrada. Apply com `log_index` já aplicado é no-op. Apply com buraco (índice maior que `last_applied + 1`) espera.

Quem não é o coordenador não muta a tabela. Encaminha o `STORE` ou o `PUT` ao `coordinator_id` que o plano do Aluno 1 gravar. Sem coordenador conhecido — o C1, o C2, e a janela antes do primeiro `COORDINATOR` — aplica localmente, como hoje, e responde `ACK`. Sucessor local não abre socket para si.

O tipo novo entra no fim do enum, imediatamente antes de `MSG_TYPE_MAX`, para os números de `ACK`, `ERROR` e das mensagens do Chord permanecerem. Nome: `LOG_APPEND`. O payload leva a entrada, o nome (até 256 bytes) e, no `STORE`, o `file_metadata` empacotado por `metadata_pack`. Se o metadata não couber num tamanho fixo, o corpo é contado e `deserialize_message` lê esses bytes, no mesmo caminho do `STORE`. `pack` e `unpack` ficam em `src/common/protocol.c`. O handler em `handle_connection` confere o checksum, instala e responde `ACK`.

Gancho para o plano do Aluno 1, uma função só: `smr_on_coordinator(sp, coordinator_id, i_am_leader)`. O ramo de `COORDINATOR` e a vitória da eleição chamam isso. O novo líder imprime a retomada e, com log não vazio, a marca de checkpoint no último índice aplicado. Não pede IST e não abre 2PC. O líder antigo que volta como `FOLLOWER` não reaplica o próprio log por cima do que o novo líder já replicou.

### Linhas que o script lê

Na subida, uma linha na stdout do Super Peer:

```text
SMR log ready LogIndex 0
```

A cada entrada selada:

```text
LOG #<index>
PUT <nome>
LogIndex <index>
```

Quando o novo coordenador assume:

```text
SMR checkpoint LogIndex <ultimo>
```

`SMR`, `log`, `LogIndex` e `checkpoint` batem com o terceiro `grep`. `ELECTION` e `COORDINATOR` ficam com o Aluno 1. Se o Aluno 2 os imprimir, o script passa com a eleição ainda ausente.

`resume operations` sai nesta retomada do log. O plano do Aluno 1 também cita essa frase; aqui só o gancho do SMR a imprime, para ela significar que o log voltou a aceitar operação.

### Cliente, pedido ao Aluno 1

O cliente é `src/peer/peer.c`. Dois comandos, no mesmo estilo do `lookup` do C3. Os dois saem 0 quando o Super Peer confirma. O harness os chama nessa ordem, e o pronto do C4 exige os dois.

- `--cmd put --name <nome> --value <valor> --host --port` — manda a operação `PUT` ao Super Peer da porta. O valor não é arquivo. O ObjectID é `SHA-256(nome || valor)`. No harness, nome `c4-file` e valor `value1`, na porta 5101. Essa porta pode não ser o coordenador: o seguidor encaminha, o líder sela, a 5101 aplica a mesma entrada e só então o cliente recebe o `ACK`.
- `--cmd state --host --port` — imprime o `LogIndex` corrente e os nomes presentes na hash table. Depois do `put` do harness, a saída da porta 5101 contém `c4-file` e o índice dessa entrada.

Falta `--value` no `getopt_long`. Sem essa flag o processo cai no `default` e sai com erro antes de falar com o Super Peer.

O `upload` da avaliação oral continua sendo o `STORE` que já existe. O coordenador sela a entrada e os sobreviventes instalam o mesmo registro.

## Build e testes

### `tests/superpeer/test_smr.c`

Sem socket, no molde de `test_chord.c`. Dois logs no mesmo processo, entradas entregues por chamada direta.

- O índice começa em 1 e cresce de um em um.
- O checksum do mesmo comando é estável. Um byte alterado no nome muda o checksum e o apply recusa.
- Aplicar duas vezes o mesmo índice deixa a `version` quieta.
- Um índice com buraco não avança `last_applied`.
- Dois logs que recebem as mesmas entradas terminam com o mesmo nome, o mesmo ObjectID e a mesma `version`.
- Sem coordenador, um `STORE` local ainda entra na tabela (o caso do C2).

### `Makefile`

`obj/superpeer/smr.o` entra no link de quem já liga `superpeer.o`. Alvo `bin/test_smr` e uma linha no alvo `test`.

### Os scripts, sem edição

Nenhum harness é alterado: `scripts/demo/script_testes_cp1.sh`, `tests/c2/run.sh`, `tests/c3/run.sh`, `tests/c4/run.sh`, `tests/common.sh`, `tests/env.sh`. O `|| true` do C4 permanece no arquivo. A verificação do pronto trata `put` e `state` como `&&`, à parte do script.

Critério de pronto:

1. `make test` verde, com `test_smr`.
2. C1, C2 e C3 no mesmo veredito de hoje. O `STORE` do C2 continua `ACK` e o arquivo baixado continua igual ao enviado.
3. Com a eleição da PR #46 no mesmo binário: `bash tests/c4/run.sh` imprime `FAIL: 0`. O log tem `ELECTION`, `COORDINATOR` e `LogIndex`. `put` e `state` saem 0. O `state` na porta 5101 mostra `c4-file`. Duas execuções seguidas do C4 dão o mesmo veredito.

## Ordem de trabalho

1. Log em memória, `metadata_table_install` e `test_smr`. Não depende da eleição.
2. `LOG_APPEND` no protocolo, num ponto do `switch` que não é o de `ELECTION`, `OK` e `COORDINATOR`.
3. `STORE` passa por `smr_propose`. Sem coordenador, apply local. Com coordenador remoto, encaminha e só então responde `ACK`.
4. `smr_on_coordinator` e as linhas `SMR log`, `LOG #`, `LogIndex`, `SMR checkpoint`.
5. Pedir ao Aluno 1 a chamada do gancho no anúncio de `COORDINATOR`, o flag `--value` e os comandos `put` e `state`. Cada um sai 0 quando o Super Peer confirma.
6. Rodar, nesta ordem, `make test`, o script do C1, `tests/c2/run.sh`, `tests/c3/run.sh` e `tests/c4/run.sh`. No C4, conferir o status do `put` e do `state` como se o script usasse `&&`.

Os passos 1 e 2 não esperam a eleição. O passo 6 fecha com os tokens `ELECTION` e `COORDINATOR` vindos do Aluno 1 e com `put` e `state` saindo 0.

## Riscos

- **SP4 e o maior NodeID.** O script mata a porta 5104 porque o comentário assume a maior `prioridade` do roster. O plano do Aluno 1 elege por `NodeID`, e o NodeID é hash do endpoint, então o coordenador inicial pode ser outra porta. O `grep` de `ELECTION` e `COORDINATOR` ainda fecha com a eleição de boot, nos 8 s anteriores ao `kill`. A frase da spec "SP4 é coordenador" fecha se a comparação passar a usar a coluna `prioridade`, hoje descartada por `node_roster_load`. Isso é decisão do Aluno 1. O log não depende de qual porta venceu: o `put` na 5101 encaminha para quem for o líder.
- **Os 8 s e os 19 s.** A eleição depois do `kill` precisa caber nos 19 s: 15 s de silêncio do heartbeat mais a eleição que o plano do Aluno 1 limita a menos de 5 s. A linha `SMR log ready` sai na subida. O `put` só recebe `ACK` depois que já existe coordenador.
- **`put` e `state` são parte do pronto.** O harness escreve `|| true`, então um cliente que só imprime o uso ainda deixa o `summary` verde. A verificação trata os dois como `&&`: status 0, `ACK` do `put` e `c4-file` no `state` da porta 5101.
- **Conflito em `protocol.c`, `superpeer.h` e `superpeer_run`.** O plano do Aluno 1 edita os mesmos três pontos que o heartbeat e o Chord. `LOG_APPEND` fica fora dos `case` de `ELECTION`, `OK` e `COORDINATOR`. A branch do código nasce de `dev` depois que a eleição entrar, ou rebaseia em cima dela.
- **`STORE` do C2.** Encaminhar para um coordenador que ainda não existe, ou abrir socket para si, segura o `upload` dos 2 s do C2. Sem `coordinator_id`, o apply é local e o `ACK` sai como hoje.
- **`metadata_table_put` no replay.** Usar o put atual no seguidor soma `version` de novo e a comparação entre SP1, SP2 e SP3 falha com o mesmo ObjectID. O apply usa `metadata_table_install`.
- **Checksum com relógio de parede.** `time(NULL)` dentro dos bytes hasheados faz duas réplicas discordarem do mesmo comando. O timestamp da entrada é o `log_index`.
- **Corpo deixado no socket.** Colocar `LOG_APPEND` no mesmo atalho de `GOSSIP`, `SNAPSHOT` e `STATE_TRANSFER` dessincroniza a conexão seguinte. O tipo novo é lido por inteiro antes do handler.
