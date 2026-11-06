# Plano de implementação — Checkpoint 5 (Aluno 1)

Papel: Aluno 1.

Este documento descreve **como** o Aluno 1 implementa o cache LFU e a
transferência de arquivo. Os pontos em que isso encosta no 2PC e no IST do
Aluno 2 estão em [`contrato-aluno-1-aluno-2.md`](contrato-aluno-1-aluno-2.md),
e o plano do Aluno 2 está em [`aluno-2.md`](aluno-2.md). A fonte de verdade é o
item 10 (*CHECKPOINT 5*) e as seções *CACHE DISTRIBUÍDO LFU* e de
transferência de arquivos de
[`docs/specification/trabalho_2026_SD.pdf`](../../specification/trabalho_2026_SD.pdf).

Não substitui o relatório final do checkpoint (`deliverable/`).

## O que o checkpoint pede do Aluno 1

Do item 10: "Aluno 1: implementa LFU + Transferência", com `cache hit`,
`cache miss`, `frequency` e `eviction`. O contrato (item C1) fixa que
"transferência", na linha do Aluno 1, é a de arquivo; a transferência de
estado é o IST do Aluno 2.

Da seção do cache LFU:

```text
Lookup → LFU Cache → Hit?
Sim → Retorna
Não → Hash Table → Atualiza Cache
```

- cada acesso incrementa `Frequency++`;
- ao atingir o limite: menor frequência, depois menor último acesso, e a
  entrada é removida;
- o cache é dividido em *Metadata Cache*, *Lookup Cache*, *Finger Cache* e
  *Routing Cache*, cada região com capacidade independente;
- métricas: *Hit Rate* e *Miss Rate*.

Da seção de transferência de arquivos:

- download: `Lookup → Lista de Peers → Transferência Paralela → Descompressão →
  Validação → Reconstrução`;
- recuperação: se um peer falha, `Timeout → Selecionar Réplica → Retomar
  Download`;
- SHA-256 por fragmento e do documento inteiro;
- estados: `CREATED → QUEUED → STARTED → TRANSFERRING → VERIFYING → FINISHED →
  REPLICATED`.

A seção de replicação define `ReplicationFactor = 3`: o sistema tenta manter
três réplicas em Super Peers distintos.

## O que o script faz

`tests/c5/run.sh` sobe três Super Peers com `tests/config/c5.conf` (portas
5101, 5102 e 5103), espera 8 s e chama o cliente:

```bash
bin/client --cmd upload --file tests/data/documento_a.pdf
bin/client --cmd lookup --name documento_a.pdf --host 127.0.0.1 --port 5101
bin/client --cmd lookup --name documento_a.pdf --host 127.0.0.1 --port 5101
bin/client --cmd join --host 127.0.0.1 --port 5101
```

Os comandos do cliente terminam com `|| true`. Os asserts, com `grep -E` sobre
o log combinado:

| Assert | Dono |
| --- | --- |
| `PREPARE\|Prepare` | Aluno 2 |
| `COMMIT\|ABORT` | Aluno 2 |
| `LFU\|frequency\|cache\|CACHE` | **Aluno 1** |
| `IST\|STATE_TRANSFER\|checkpoint\|version` | Aluno 2 |
| sem crash | os dois |

O assert do Aluno 1 já passa com o primeiro `lookup` (`cache miss`) e o segundo
(`cache hit`). O script não exercita eviction, invalidação nem download.

### O harness de fluxo

`tests/flow/run.sh` (PR #61, do Aluno 2) segue o roteiro do item 10 com um log
por processo e confere linhas **exatas**, não um regex sobre o log combinado.
Em `dev` ele fecha com 13 PASS e 0 FAIL. As fases que tocam o Aluno 1:

| Fase | O que confere |
| --- | --- |
| 2 e 4 | `upload` de A–C com os três Super Peers vivos e de D–E com o SP2 morto, na porta 5101. O cliente tem de sair com 0. |
| todas | `--cmd state` em cada porta: as linhas exatas `LogIndex N` e `Objects: N` e uma linha começando por `nome v` para cada registro. |
| 6 | Dois `lookup` de `documento_a.pdf` na porta 5101. O log do SP1 tem, em ordem, as linhas exatas `cache miss`, `LFU frequency 1`, `cache hit` e `LFU frequency 2`. Nenhum outro Super Peer tem a linha `cache hit`. |
| 7 | `download` de A na porta 5101. O cliente sai com 0, o log dele tem as linhas exatas `Download completed` e `SHA-256 verified`, e o arquivo baixado é igual ao enviado. |

A fase 2 também confere o 2PC no log do coordenador: entre `PREPARE` e
`COMMIT`, a linha `STORE nome` tem de vir logo depois de `LOG #n`. Nada que o
Aluno 1 imprima no Super Peer pode cair entre essas linhas.

## Estado atual

Em `dev`:

- **LFU mínimo.** O PR #59 do Aluno 2 colocou `lfu_observe` em
  `src/superpeer/superpeer.c`, chamado por `superpeer_handle_lookup`. É um
  contador de nomes com 8 posições (`LFU_CAP`) em variáveis `static`, que
  imprime `cache hit`, `cache miss`, `LFU frequency N` e `eviction`, e despeja
  pela menor frequência com desempate pelo acesso mais antigo. O cache **não
  guarda o registro**: o lookup lê a hash table em todo acesso, inclusive no
  "hit".
- **Gancho de invalidação.** O ponteiro `lfu_invalidate` do contrato C6 ainda
  é um `TODO` em `include/superpeer/superpeer.h`, e a chamada é outro `TODO` em
  `src/superpeer/smr.c`.
- **Transferência.** `src/peer/upload.c` e `src/peer/download.c` usam 4
  threads (`TRANSFER_WORKERS`) e conferem o SHA-256 de cada chunk e do arquivo
  inteiro, desde o checkpoint 2. O download conversa com um único Super Peer,
  sem timeout. O PR #59 fez o cliente tentar o segundo endereço do
  `bootstrap` de `config/peer1.conf` apenas quando o primeiro recusa a
  conexão.
- **Chunks não são replicados.** Só o metadado passa pelo log. O chunk fica em
  `data/storage` de quem recebeu o `DOWNLOAD_REP` (`superpeer_handle_chunk_put`).
  Como esse caminho é relativo e os Super Peers dos harnesses rodam no mesmo
  diretório, todos compartilham a mesma pasta, e um download de qualquer porta
  funciona sem que haja réplica.
- **Estados da transferência.** Não existem.
- **Estado do Super Peer.** `GET_STATE` devolve `LogIndex N`, `Objects: N` e uma
  linha `nome vX` por registro.

## Escopo

- módulo LFU com capacidade, frequência, último acesso, eviction, métricas e
  invalidação, testável sem socket;
- região *Metadata Cache* servindo o `LOOKUP` por nome de verdade: um hit
  devolve o registro guardado, sem ler a hash table;
- o ponteiro `lfu_invalidate` do contrato C6;
- *Hit Rate* e *Miss Rate* no texto do `state`;
- storage de chunks separado por Super Peer;
- réplica dos chunks no dono do ObjectID e nos sucessores dele;
- download com timeout que troca de réplica e retoma só os chunks que faltam;
- estados da transferência no cliente.

Ficam fora:

- 2PC e IST, que são do Aluno 2;
- as regiões *Finger Cache* e *Routing Cache*. A finger table e a lista de
  sucessores do Chord já são a informação de roteamento guardada em memória;
  uma segunda cópia em cache não economiza RPC. A região *Lookup Cache* entra
  só no fim, como passo opcional;
- cache negativo (guardar "não encontrado").

## Design

### Módulo LFU, em `include/superpeer/lfu.h` e `src/superpeer/lfu.c`

Uma região do cache é um `lfu_cache_t` independente:

```c
typedef struct {
  char key[METADATA_FILENAME_MAX];        /* nome do registro */
  uint8_t object_id[METADATA_OBJECT_ID_SIZE];
  uint8_t *value;                         /* bytes guardados; o cache é dono */
  size_t value_len;
  uint64_t frequency;
  uint64_t last_access;                   /* relógio lógico do cache */
  int used;
} lfu_entry_t;

typedef struct {
  lfu_entry_t *entries;
  size_t capacity;
  uint64_t clock;
  uint64_t hits, misses, evictions;
  pthread_mutex_t lock;
} lfu_cache_t;
```

Operações:

- `lfu_get`: procura pela chave. No hit, incrementa `frequency`, atualiza
  `last_access` e copia `value` para o chamador. Conta `hits` ou `misses`.
- `lfu_put`: insere com `frequency = 1`. Cheio, despeja a entrada de menor
  `frequency` e, no empate, a de menor `last_access`, contando `evictions`.
- `lfu_invalidate_object`: remove as entradas com aquele ObjectID.
- `lfu_stats`: devolve os três contadores.

`last_access` é um contador do cache, e não `time()`, para o desempate não
depender do relógio. O `lock` é próprio de cada região e é uma folha: nenhum
outro lock é tomado com ele preso, e nenhum lock de fora precisa estar preso
para chamar o módulo.

### Metadata Cache no `LOOKUP`

O `superpeer_t` ganha a região `metadata_cache`, criada em `superpeer_init` com
capacidade `LFU_METADATA_CAP`. `superpeer_handle_lookup` passa a seguir a
especificação:

```text
LOOKUP nome:
    lfu_get(metadata_cache, nome)
    hit  → responde com os bytes guardados        → "cache hit", "LFU frequency N"
    miss → hash table (metadata_lock) → metadata_pack
         → lfu_put(nome, ObjectID, bytes)         → "cache miss", "LFU frequency 1"
           (se despejou alguém)                   → "eviction"
    não está na tabela → ERROR, nada entra no cache
```

O valor guardado é a saída de `metadata_pack`, a mesma que o `LOOKUP` já
responde, então o hit não muda o formato da resposta. `lfu_observe`, o
`lfu_slots` e o `LFU_CAP` do PR #59 saem. As linhas de log permanecem iguais,
uma por linha e sem texto a mais, porque a fase 6 do harness de fluxo as
confere como linhas exatas.

### Invalidação (contrato C6)

O `superpeer_t` ganha o campo do contrato:

```c
void (*lfu_invalidate)(struct superpeer *sp, const uint8_t object_id[32]);
```

`superpeer_init` aponta o campo para uma função que chama
`lfu_invalidate_object` na `metadata_cache`. A invalidação não imprime nada:
ela roda logo depois do apply, e uma linha a mais entre `LOG #n` e o `COMMIT`
quebraria a conferência do 2PC no harness de fluxo. O Aluno 2 chama o ponteiro depois
de cada apply de `COMMIT` e do IST, com o `smr_lock` solto, como diz o contrato.
O `TODO` atual dele fica dentro de `smr_log_apply`, que não recebe o
`superpeer_t`; a chamada vai para quem chama o apply. Sem essa chamada, um hit
devolve o registro anterior ao `COMMIT`.

### Métricas

O texto do `GET_STATE` ganha uma linha com as métricas da `metadata_cache`:

```text
Cache hits 1 misses 1 evictions 0 hit rate 50%
```

A taxa de miss é o complemento. Nenhuma palavra dessa linha bate com os
`grep` dos outros harnesses. O harness de fluxo procura `LogIndex N`,
`Objects: N` e `nome v` no `state`, então uma linha a mais não interfere.

### Storage por Super Peer

`SUPERPEER_STORAGE_ROOT` deixa de ser um caminho fixo. Cada Super Peer grava em
`data/storage/<porta>`. Sem isso, réplica e falha não são observáveis numa
máquina só, porque todos leem a mesma pasta. O C2 continua igual: um único Super
Peer recebe o upload e serve o download.

### Réplica dos chunks

Os chunks vão para os mesmos Super Peers que o Chord usaria para a chave: o
dono do ObjectID e os sucessores dele, até `ReplicationFactor = 3` nós
distintos. Quem faz a réplica é o cliente, com RPCs que já existem, num módulo
novo do cliente, `src/peer/replica.c` e `include/peer/replica.h`:

```text
upload:
    STORE do metadado → ACK                       (como hoje; 2PC do Aluno 2)
    FIND_SUCCESSOR(ObjectID) no Super Peer de entrada → dono
    GET_SUCCESSORS no dono → [dono, s1, s2], sem repetição
    envia cada chunk (DOWNLOAD_REP) a cada um da lista
```

Com isso, qualquer Super Peer acha os chunks sem saber quem recebeu o upload. O
Super Peer não ganha lógica nova de encaminhamento, nem tipo novo no enum. Num
Super Peer sozinho (C1, C2), a lista é ele mesmo.

O `STORE` vem antes dos chunks porque o Super Peer recusa um chunk cujo
registro ainda não está na hash table dele: ele confere o índice e o tamanho do
chunk contra o metadado.

O `ACK` do `STORE` garante o `COMMIT` no Super Peer de entrada, mas o `COMMIT`
pode chegar às outras réplicas um pouco depois, e até lá elas recusam o chunk.
Por isso o cliente testa a conexão com cada réplica antes de enviar. Conexão
recusada quer dizer que o nó está fora, e ele é pulado na hora. Um nó vivo que
recusa os chunks recebe novas tentativas, até 10 vezes com 200 ms de intervalo.

O upload termina se o `STORE` teve `ACK` e ao menos uma réplica confirmou todos
os chunks. O cliente imprime quantas
réplicas cada arquivo ficou tendo. Na fase 4 do harness de fluxo o SP2 está
morto e ainda pode aparecer na lista de sucessores; a conexão é recusada, o nó
é pulado, e o upload sai com 0.

### Download com troca de réplica

```text
download nome:
    LOOKUP nome → metadado                        (pode vir do cache)
    FIND_SUCCESSOR(ObjectID) + GET_SUCCESSORS → [dono, s1, s2, entrada]
    busca os chunks no primeiro da lista, em paralelo
    um DOWNLOAD_REQ sem resposta em REPLICA_TIMEOUT_SEC
        → próximo da lista; refaz só os chunks que faltam
    SHA-256 de cada chunk e do arquivo, como hoje
```

O Super Peer de entrada vai no fim da lista quando ainda não está nela. Ele é
a última tentativa, para os chunks gravados antes da réplica existir, quando
ficavam só em quem recebia o upload.

O `LOOKUP` continua indo ao Super Peer da `--port`. Mandá-lo ao dono faria
outro Super Peer registrar `cache hit`, o que a fase 6 do harness de fluxo
proíbe. As linhas `Download completed` e `SHA-256 verified` continuam saindo
exatamente assim, porque a fase 7 as confere.

O timeout, `REPLICA_TIMEOUT_SEC` (3 s), é `SO_RCVTIMEO` e `SO_SNDTIMEO` no
socket do `DOWNLOAD_REQ` e nas RPCs `FIND_SUCCESSOR` e `GET_SUCCESSORS`. O contrato C8 pede que
um timeout do cliente fique acima da pausa `BT_PAUSE_AFTER_PREPARE`. Essa pausa
fica no `STORE` do upload, e o timeout novo fica só no `DOWNLOAD_REQ`, então os
dois não se cruzam. Ainda assim, o valor é combinado com o Aluno 2.

### Estados da transferência

O cliente imprime uma linha por transição:

```text
Transfer documento_a.pdf CREATED
Transfer documento_a.pdf QUEUED
Transfer documento_a.pdf STARTED
Transfer documento_a.pdf TRANSFERRING
Transfer documento_a.pdf VERIFYING
Transfer documento_a.pdf FINISHED
Transfer documento_a.pdf REPLICATED 3
```

| Estado | Upload | Download |
| --- | --- | --- |
| `CREATED` | metadado e chunks gerados localmente | metadado obtido no `LOOKUP` |
| `QUEUED` | `STORE` confirmado com `ACK`; chunks na fila | réplicas descobertas |
| `STARTED` | réplicas descobertas | início da busca dos chunks |
| `TRANSFERRING` | chunks indo para as réplicas | chunks chegando, com SHA-256 de cada um |
| `VERIFYING` | conferência dos chunks em cada réplica | SHA-256 do arquivo inteiro |
| `FINISHED` | ao menos uma réplica com todos os chunks | arquivo gravado |
| `REPLICATED` | quantas réplicas ficaram com todos os chunks | — |

No upload, a ordem dos estados segue a ordem `STORE` → chunks explicada em
*Réplica dos chunks*.

Nenhuma linha do cliente usa `FAILED`, `IST`, `PREPARE`, `COMMIT` ou
`checkpoint`, que são palavras procuradas pelos `grep` do C3, do C4 e do C5. A
troca de nó sai como `Transfer <nome> replica <ip:porta> sem resposta; pulando`
no upload e `…; seguindo para <ip:porta>` no download. Os erros que encerram o
comando vão para a saída de erro, com o prefixo `upload:` ou `download:`.

### Lookup Cache (opcional)

Se sobrar tempo: uma segunda região, `lookup_cache`, guarda o resultado de
`FIND_SUCCESSOR` por ObjectID no Super Peer. Ela é esvaziada inteira quando o
sucessor ou o predecessor muda, ou quando `chord_drop_node` tira um nó. Um
caminho antigo levaria o cliente a um dono que já saiu do anel.

## Build e testes

### `tests/superpeer/test_lfu.c`

Sem socket:

- miss e depois hit da mesma chave, com `frequency` 2;
- cheio: sai a de menor frequência;
- empate de frequência: sai a de acesso mais antigo;
- `lfu_invalidate_object` remove a chave e o `get` seguinte é miss;
- contadores de hit, miss e eviction batem;
- o valor devolvido no hit é igual ao inserido.

### Transferência

- Em `tests/peer/test_download.c`, a montagem da lista de réplicas
  (`replica_merge`): o dono primeiro, os sucessores sem repetição, o teto de
  3 nós e o Super Peer de entrada no fim.
- Ensaio manual com os três Super Peers do `tests/config/c5.conf`: upload,
  `kill -9` no dono do ObjectID e download completo a partir de outra réplica,
  com o SHA-256 conferido.

### `Makefile`

`obj/superpeer/lfu.o` entra no link de quem já liga `superpeer.o`. Alvo novo
`bin/test_lfu` e uma linha no alvo `test`.

### Critério de pronto

1. `make test` verde, com `test_lfu`.
2. C1, C2, C3 e C4 com o mesmo veredito de hoje.
3. `tests/c5/run.sh` com `FAIL: 0`. O segundo `lookup` é um hit de verdade: a
   hash table não é lida.
4. `tests/flow/run.sh` com 13 PASS e 0 FAIL, como hoje.
5. O `state` mostra a linha de métricas do cache.
6. Com o dono do ObjectID morto, o download termina por outra réplica e o
   arquivo é igual ao enviado.

## Ordem de trabalho

1. Módulo LFU e `test_lfu`. Não depende de nada.
2. `Makefile`.
3. `metadata_cache` no `LOOKUP`, saída do `lfu_observe`. Rodar o C5 e o
   harness de fluxo.
4. Campo `lfu_invalidate` e avisar o Aluno 2 para fazer a chamada no apply.
5. Métricas no `state`.
6. Storage por porta. Rodar o C2.
7. Réplica dos chunks no upload.
8. Timeout e troca de réplica no download, com o ensaio do `kill -9`.
9. Estados da transferência. Rodar todos os harnesses, inclusive o de fluxo.
10. Opcional: *Lookup Cache*.

Os passos 1 a 5 fecham o LFU; os passos 6 a 9 fecham a transferência.

## Riscos

- **Hit com registro velho.** Sem a chamada de `lfu_invalidate` no apply, o
  cache devolve o registro anterior ao `COMMIT`. O passo 4 só fecha quando a
  chamada existir do lado do Aluno 2.
- **Palavras dos `grep`.** Uma linha do cliente com `FAILED` passaria o assert
  de falha do C3; com `IST` ou `checkpoint`, o do IST do C5. Os textos novos
  evitam essas palavras.
- **Linhas exatas do harness de fluxo.** `cache miss`, `cache hit`,
  `LFU frequency N`, `Download completed` e `SHA-256 verified` são conferidas
  como linhas inteiras. Mudar o texto, ou juntar outra informação na mesma
  linha, reprova as fases 6 e 7 mesmo com o comportamento certo.
- **Storage compartilhado.** Enquanto todos os Super Peers gravarem na mesma
  pasta, a réplica parece funcionar sem funcionar. O passo 6 vem antes da
  réplica.
- **Upload mais lento.** Três cópias de cada chunk triplicam o envio. O C2 tem
  um único Super Peer e não muda.
- **Dono que mudou.** Entre o upload e o download o anel pode mudar e o dono
  novo não ter os chunks. A lista de réplicas cobre a queda de um nó; uma
  reorganização maior do anel fica sem cobertura neste checkpoint.
- **Conflito com o Aluno 2** em `superpeer_t` e em `superpeer.c`. O
  `lfu_invalidate` é o único campo que o contrato prevê do lado do Aluno 1.

## Decisões tomadas

1. **Regiões *Finger* e *Routing* fora.** A finger table e a lista de
   sucessores do Chord já são a informação de roteamento em memória; uma cópia
   em cache não economiza RPC e teria de ser mantida em sincronia. A *Lookup
   Cache* fica como passo opcional.
2. **Réplica pelo cliente, no dono e nos sucessores.** O cliente envia cada
   chunk aos nós que o Chord aponta para o ObjectID, com RPCs que já existem.
   A alternativa, o Super Peer encaminhar os chunks, mudaria
   `src/superpeer/superpeer.c` e exigiria evitar o reencaminhamento de uma
   cópia. Vale avisar o Aluno 2, porque a escolha evita mexer no código dele.
3. **Storage por porta.** Cada Super Peer grava em `data/storage/<porta>`. Sem
   isso, réplica e troca de réplica não são observáveis numa máquina só. Entra
   junto com a réplica (passos 6 e 7), nunca sozinha: com a pasta separada e
   sem réplica, o download só acharia os chunks no Super Peer que recebeu o
   upload.
