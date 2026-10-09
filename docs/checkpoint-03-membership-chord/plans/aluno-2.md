# Plano de implementação — Checkpoint 3 (Aluno 2)

Prazo: **09/10/2026**. Papel: Aluno 2.

Este documento descreve **como** o Aluno 2 implementa o anel Chord. A referência do algoritmo é o Chord: sucessor, predecessor, finger table, `join`, `stabilize`, `notify`, `fix_fingers` e `check_predecessor`. A referência do teste de integração é o `tests/c3/run.sh` da [PR #40](https://github.com/Erwin5642/bittorrent-project/pull/40), junto com `tests/config/c3.conf`.

A PR #40 também traz o heartbeat do Aluno 1. Este plano parte de `dev` depois que essa PR for corrigida e mergeada: as duas mexem em `src/common/protocol.c`, `include/superpeer/superpeer.h` e em `superpeer_run`.

Os outros harnesses continuam reproduzíveis. Rodar duas vezes o mesmo script, no mesmo binário, produz o mesmo NodeID, o mesmo anel e o mesmo resultado. Isso vale para `scripts/demo/script_testes_cp1.sh`, para `tests/c2/run.sh` e para `make test`, além do C3. Nenhum desses arquivos é editado.

Não substitui o relatório final do checkpoint (`deliverable/`).

## O que o script faz

`tests/c3/run.sh` carrega `tests/common.sh`, que por sua vez carrega `tests/env.sh`. Esses dois já estão em `dev`. A PR acrescenta o script e o roster.

A sequência é fixa:

1. Sobe cinco processos, todos com o mesmo arquivo e com `--port` diferente:

   ```bash
   bin/node --config tests/config/c3.conf --port 5101
   bin/node --config tests/config/c3.conf --port 5102
   bin/node --config tests/config/c3.conf --port 5103
   bin/node --config tests/config/c3.conf --port 5104
   bin/node --config tests/config/c3.conf --port 5105
   ```

   A stdout e a stderr dos cinco vão para o mesmo log. O script não passa `--name` nem `--node-id`. O comentário no script diz que o NodeID, quando o flag não vem, é derivado da config.

2. Espera 8 segundos.

3. Se `bin/client` existir, chama, engolindo o código de saída:

   ```bash
   bin/client --cmd topology --host 127.0.0.1 --port 5101
   bin/client --cmd lookup --object-id <64 hex> --host 127.0.0.1 --port 5101
   ```

   O object-id é `ab` repetido 32 vezes (`printf 'ab%.0s' {1..32}`): 32 bytes, hex válido.

4. Exige no log, com `grep -E`:

   - `successor` ou `Successor`
   - `finger` ou `Finger`
   - `GOSSIP`, `Gossip`, `heartbeat` ou `HEARTBEAT`

5. Manda `kill -9` no terceiro processo. Como a ordem do laço é 5101, 5102, 5103, 5104, 5105, o morto é a porta 5103, a linha `SP3` do roster.

6. Espera `FAILURE_TIMEOUT_SEC + 3` segundos. O default em `tests/env.sh` é 15, então a espera é de 18 segundos.

7. Exige no log `SUSPECT`, `FAILED`, `REMOVED`, `Election` ou `ELECTION`, e a ausência de segmentation fault, core dump, double free, heap-buffer-overflow e deadlock.

O roster, `tests/config/c3.conf`, é uma linha CSV por Super Peer:

```text
nome,tipo,ip,porta,id,prioridade
SP1,superpeer,127.0.0.1,5101,00000001,1
...
SP5,superpeer,127.0.0.1,5105,00000005,5
```

O que o script **não** confere: o caminho da busca, o dono da chave, o anel fechado, a finger table correta, nem se o `lookup` e o `topology` tiveram sucesso. Os dois comandos do cliente rodam com `|| true`.

## O que os outros scripts fixam

Os três harnesses sobem o mesmo binário. O C1 e o C2 usam `tests/config/c1.conf` (`chave=valor`, `127.0.0.1`, porta `55101`, `bootstrap` vazio). O C3 usa o roster. A identidade do Super Peer tem de ser a mesma função nos dois formatos, senão o anel do C3 é estável e o do C1 muda a cada execução.

`scripts/demo/script_testes_cp1.sh`:

- Sobe `bin/node --config tests/config/c1.conf --port 55101 --name C1SP1`.
- Exige as linhas `Node C1SP1 started` e `NodeID:` no log. O hex em si não é conferido. Com NodeID derivado do endpoint, duas execuções imprimem o mesmo hex.
- Em até 3 s o `ping` na porta 55101 recebe `RX PONG`. O `accept` não pode esperar o `join`.
- `JOIN` responde `RX ACK`, `LEAVE` responde `RX ACK`, vinte `ping` concorrentes respondem, o log do servidor tem pelo menos 21 linhas `RX PING`, e versão de protocolo inválida fecha o socket sem resposta.
- O cliente desse script é um peer (`config/peer1.conf`, `type=peer`). O `JOIN` dele entra na membership e não entra no anel.

`tests/c2/run.sh`:

- Sobe `bin/node --config tests/config/c1.conf` sem `--port`. A porta é a do arquivo, 55101, a mesma do C1. O NodeID coincide com o do C1.
- Espera 2 s e roda `upload` e `download` de `small.txt`, `exact_4MiB.bin`, `over_4MiB.bin` e `random_10MiB.bin`. O cliente usa `config/peer1.conf`, cujo bootstrap é `127.0.0.1:55101`.
- O log precisa continuar contendo `SHA-256` ou `ObjectID`, `LZ4` ou `compress`, `chunk` ou `Chunk`. Essas linhas saem de `upload_print_report` e do download. O Chord não as reescreve.
- O arquivo baixado é comparado byte a byte com o enviado. `tests/generate_fixtures.sh` gera os quatro arquivos sem aleatoriedade (`/dev/zero` e uma fórmula fixa). O `LOOKUP` de 256 bytes, o `STORE` e o `DOWNLOAD_*` permanecem o caminho do checkpoint 2.
- O nó está sozinho (`bootstrap` vazio). Sucessor é ele mesmo, e a manutenção não abre socket para si.

`make test` e `tests/c1/test_protocol.c` (chamado pelo script do C1) usam vetores fixos. `node_id_generate` em `tests/common/test_node.c` continua recebendo um UUID e produzindo o digest de sempre. A função nova do endpoint não substitui essa.

## Quem produz cada linha do log

| Assert do script | Quem escreve | O que precisa ser verdade |
|---|---|---|
| `successor` / `Successor` | o próprio Super Peer, durante os 8 s | `join` e `stabilize` já rodaram e logaram o sucessor |
| `finger` / `Finger` | o próprio Super Peer, durante os 8 s | `fix_fingers` já logou ao menos uma entrada |
| `heartbeat` / `HEARTBEAT` | a thread de heartbeat da PR #40 | os Super Peers já estão na membership uns dos outros, senão ninguém recebe batimento |
| `SUSPECT` / `FAILED` / `REMOVED` | a thread de heartbeat da PR #40, ao rebaixar quem ficou em silêncio | a porta 5103 está na membership dos sobreviventes antes do `kill`, com `last_heartbeat` sendo renovado enquanto ela responde |

O Chord repara o anel quando a 5103 deixa de responder. O script não lê essa reparação. Ele lê a palavra `SUSPECT`, que sai do heartbeat. Gossip e eleição não entram neste plano: o assert aceita `HEARTBEAT` e `SUSPECT`, que a PR #40 já imprime.

Os logs de sucessor e finger saem do processo Super Peer. Depender do `bin/client` deixaria o assert na mão de um comando que o script está preparado para ignorar.

## Objetivo

Cinco Super Peers formam um anel sozinhos a partir do roster, no prazo de 8 segundos do script. Cada chave fica no primeiro nó no sentido horário a partir do identificador dela. Quando a porta 5103 morre, o predecessor dela passa a apontar para o próximo sucessor vivo, e `tests/c3/run.sh` termina com zero falhas.

## Escopo

- NodeID estável do Super Peer
- leitura do roster
- sucessor, predecessor e lista de `r` sucessores
- finger table de 256 entradas
- `join`, `stabilize`, `notify`, `fix_fingers`, `check_predecessor`
- `find_successor` iterativo
- transferência das chaves do intervalo `(predecessor, n]`
- réplica de cada chave nos `r` sucessores

A transferência e a réplica vêm depois do script verde. O `tests/c3/run.sh` não as exercita. A suíte interna exercita.

Ficam fora: Gossip, eleição, e qualquer campo do roster que o anel não usa. As colunas `id` (`00000001` …) e `prioridade` são lidas para validar a linha e não entram no NodeID nem na ordem do anel.

## Estado atual

Em `dev`:

- `src/common/node.c` gera `NodeID = SHA256(IP || Porta || UUID)` e compara identificadores com `node_id_cmp`. O UUID muda a cada execução.
- `src/common/config.c` só aceita `chave=valor`. O CSV do roster faz `node_config_load` falhar, e o `bin/node --config tests/config/c3.conf` não sobe.
- `src/superpeer/superpeer.c` tem membership, metadata e uma thread por conexão. O processo escuta. Não envia `JOIN` para ninguém. Cada Super Peer só tem a si mesmo na tabela.
- `src/peer/peer.c` aceita `--cmd`, `--host` e `--port`. Não tem `--cmd topology`, `--cmd lookup` nem `--object-id`. Um flag desconhecido cai no `default` do `getopt_long` e o processo sai com erro. O script engole esse erro.
- `PING` / `PONG` já existem e servem ao `check_predecessor`.

A PR #40 acrescenta, quando mergeada, a thread de heartbeat, `send_heartbeat`, `last_heartbeat` como `time_t`, `tests/c3/run.sh` e `tests/config/c3.conf`.

## Design

### Roster, em `src/common/config.c`

Nova função ao lado de `node_config_load`:

```c
int node_roster_load(const char *path, uint16_t self_port,
                     node_config_t *out, char *name, size_t name_cap);
```

- Lê `nome,tipo,ip,porta,id,prioridade`. Linha vazia e `#` são ignoradas. Coluna a menos rejeita o arquivo.
- A linha cuja porta é `self_port` vira `ip`, `port`, `type` e `name`.
- `bootstrap` recebe a primeira linha `superpeer` de porta diferente. No roster do script, SP2…SP5 entram por `127.0.0.1:5101`. SP1 fica sem bootstrap e cria o anel.
- `superpeer_init` tenta `node_config_load` primeiro. `tests/config/c1.conf` continua carregando por aí, com o mesmo `ip`, `port` e `bootstrap` vazio de hoje. Só um arquivo sem `=` cai em `node_roster_load`. `--name` na CLI continua vencendo o nome do roster. O C3 não passa `--name`. O C1 passa `--name C1SP1`, e é esse nome que aparece em `Node C1SP1 started`.

### NodeID

O Super Peer usa um identificador estável: `SHA256` dos 4 bytes do IPv4 em network byte order seguidos da porta em 2 bytes big-endian. A porta é a da CLI quando `--port` vem (C1 passa 55101, C3 passa 5101–5105) e a do arquivo quando não vem (C2). Função nova `node_id_from_endpoint` em `src/common/node.c`. `node_id_generate` permanece, com UUID, para o peer e para os vetores de `tests/common/test_node.c`.

`superpeer_init` grava esse valor em `self_id` e continua imprimindo `Node <nome> started` e `NodeID: <hex>`. É o mesmo identificador do Chord, do `JOIN` e do heartbeat. C1 e C2, os dois em `127.0.0.1:55101`, imprimem o mesmo hex. C3 imprime um hex por porta, igual em toda execução. O peer segue efêmero: o C1 não confere o NodeID de quem faz `JOIN`.

### Módulo, em `include/superpeer/chord.h` e `src/superpeer/chord.c`

```c
#define CHORD_M 256
#define CHORD_R 3

typedef struct {
  node_id_t id;
  uint32_t  ipv4;   /* network byte order */
  uint16_t  port;   /* host byte order */
  int       valid;
} chord_node_t;

typedef struct {
  chord_node_t self;
  chord_node_t predecessor;
  chord_node_t successors[CHORD_R];  /* [0] é o sucessor imediato */
  chord_node_t fingers[CHORD_M];
  pthread_mutex_t lock;
} chord_t;
```

`CHORD_R = 3` cobre a queda de um nó num anel de cinco: o predecessor da porta 5103 tem para quem apontar sem esperar redescobrir o anel inteiro.

No nó sozinho, o sucessor é ele mesmo e o predecessor está vazio. A entrada `i` da finger table, `i` de 0 a 255, guarda `successor(n + 2^i)`. A entrada 0 é o sucessor imediato.

O lock protege a memória. O destino de uma chamada é copiado, o lock é solto, e o socket abre fora dele. `chord.lock` e `members_lock` nunca são tomados juntos.

A lógica pura, testável sem socket:

- `chord_id_add_pow2`
- `chord_in_open` para `(a, b)` e `chord_in_half_open` para `(a, b]`, com a volta pelo zero e com `a == b` significando o anel inteiro
- `chord_closest_preceding`
- `chord_apply_notify`
- `chord_drop_node`, que tira o nó do predecessor, puxa a lista de sucessores e limpa os fingers que apontavam para ele

### Manutenção

Uma thread criada em `superpeer_run`, antes do `accept`, roda a cada 1 segundo, nesta ordem:

```text
stabilize():
    x = predecessor do successor
    se x está em (n, successor)
        successor = x
    copia a lista de sucessores devolvida por ele, deslocada
    notify(successor)

fix_fingers():
    i anda de um em um e volta a 0 depois de 255
    se o início do finger ainda cai em (n, finger[i-1]]
        copia o anterior, sem rede
    senão
        finger[i] = find_successor(n + 2^i)

check_predecessor():
    se o predecessor não responde a PING
        predecessor = vazio
```

Um finger atrasado alonga a busca. Um sucessor errado parte o anel, por isso `stabilize` vem primeiro.

O script mata o processo aos 8 segundos e dá mais 18 para alguém notar. Com o período de 1 segundo, a troca de sucessor acontece na rodada seguinte à queda, bem dentro dessa janela. A palavra `SUSPECT` continua saindo do heartbeat, 15 segundos depois do último batimento da porta 5103.

### Entrada

O C3 não manda `JOIN`. Cada processo entra sozinho. O `accept` começa na hora, na mesma ordem de hoje: `superpeer_init` faz `net_listen`, `superpeer_run` entra no laço. A entrada no anel corre na thread de manutenção. O C1 precisa de `PONG` em até 3 s e o C2 fala com o servidor depois de 2 s. Uma espera de até 10 s antes do `accept` reprova os dois.

```text
sem bootstrap: cria o anel (sucessor = ele mesmo) e não abre socket
com bootstrap: predecessor = vazio
               successor   = find_successor(self) perguntado ao bootstrap
```

É o caso do C1 e do C2: `bootstrap` vazio, um nó só, sucessor igual a si mesmo. `stabilize`, `fix_fingers` e `check_predecessor` não conectam em si. O laço de `PING` do C1 e o `upload` do C2 ficam com o `accept` livre.

Com bootstrap, a tentativa se repete a cada 500 ms por até 10 s, em paralelo ao `accept`. No C3 os cinco processos sobem juntos e o da porta 5101 pode ainda não estar em `listen`.

`JOIN` com `node_type = PEER`, que é o que o cliente do C1 envia, só atualiza a membership. Não vira sucessor, predecessor nem finger. `LEAVE` desse peer também fica na membership.

O `JOIN` de membership segue no mesmo passo, para os dois lados:

- quem entra envia `JOIN` com `node_type = SUPERPEER` ao bootstrap e, no `ACK`, insere o bootstrap na própria membership;
- quem recebe o `JOIN` já insere quem entrou, como hoje.

`stabilize` e `fix_fingers` inserem na membership cada Super Peer que o anel passa a conhecer. Inserção nova, com `last_heartbeat = time(NULL)` e estado `ALIVE`. Entrada que já existe não é reescrita: um `FAILED` marcado pelo heartbeat permanece `FAILED`.

É essa inserção que faz o assert de `HEARTBEAT` aparecer nos primeiros 8 segundos e o assert de `SUSPECT` aparecer depois do `kill` da porta 5103. Sem ela, a thread de heartbeat só olha para a própria linha da tabela.

### Busca

`find_successor` é iterativo. O Super Peer que recebeu o pedido pergunta, a cada salto, qual é o próximo passo, e abre ele mesmo a conexão seguinte.

```text
find_successor(id):
    repete
        pergunta ao nó corrente o próximo passo para id
        se id está em (corrente, successor]
            esse successor é o dono
        senão
            o nó corrente passa a ser o nó devolvido
    até o dono, no máximo 256 saltos
```

O próximo passo é o finger de maior índice dentro de `(n, id)`. Sem nenhum, a busca segue pelo sucessor. Salto que não responde é descartado e o finger anterior entra no lugar. O dono da chave `abab…ab` que o script pede é simplesmente `successor` desse identificador.

### Mensagens

Quatro tipos novos, acrescentados no fim do enum, depois de `ERROR` e antes de `MSG_TYPE_MAX`, para não renumerar os tipos que já existem. O registro de nó no fio tem 38 bytes: `node_id` (32), `ipv4` (4), `port` (2), big-endian.

| Operação | Pedido | Resposta |
|---|---|---|
| `CLOSEST_PRECEDING` | identificador, 32 bytes | um nó, 38 bytes. Quando o identificador cai em `(n, successor]`, o nó devolvido é o sucessor e um byte de status diz que a busca acabou |
| `GET_PREDECESSOR` | vazio | um nó, ou status vazio |
| `GET_SUCCESSORS` | vazio | `count` seguido de até `CHORD_R` nós |
| `NOTIFY` | um nó, 38 bytes | `ACK` |

`JOIN`, `LEAVE`, `PING` e `PONG` permanecem como estão. A consulta de metadata por nome permanece o `LOOKUP` de 256 bytes: o download do checkpoint 2 usa esse formato em `src/peer/download.c`.

Os `pack` / `unpack` ficam em `src/common/protocol.c` e `include/common/protocol.h`, no mesmo molde de `metadata_pack`. `payload_sizes` ganha os quatro tipos. `CLOSEST_PRECEDING`, `GET_PREDECESSOR`, `GET_SUCCESSORS` e `NOTIFY` entram na lista de payload cru de `deserialize_message` só se o tamanho não for fixo; os tamanhos fixos seguem pelo `pack` / `unpack` normal.

`handle_connection` em `src/superpeer/superpeer.c` ganha um ramo para cada tipo.

### Logs que o script lê

Uma linha só quando o valor muda, saindo na stdout do Super Peer:

```text
Successor -> 127.0.0.1:5103
Finger[0] -> 127.0.0.1:5103
```

`Successor` e `Finger` batem com os dois primeiros asserts. A porta identifica o nó no log do `kill`. O heartbeat da PR #40 escreve `TX HEARTBEAT` e `SUSPECT` por conta própria.

### Chaves

Cada nó é dono de `(predecessor, self]`. `put` grava no `successor` da chave. `get` é um `find_successor` e uma leitura local.

Quando o predecessor deixa de ser vazio, o nó pede ao sucessor as chaves desse intervalo. O sucessor as remove ao confirmar. Repetir a operação depois de uma resposta perdida devolve vazio.

Na saída voluntária o nó envia as próprias chaves ao sucessor antes de fechar. A queda da porta 5103 não avisa: as réplicas cobrem.

Cada chave fica copiada nos `CHORD_R` sucessores. Quando a lista muda, o sucessor novo recebe o que falta e quem saiu da janela descarta o que não é mais seu. Uma leitura no meio da transferência encontra a chave na réplica.

O script não passa por aqui. A ordem de trabalho deixa este passo por último.

### Cliente

O script chama dois comandos que `src/peer/peer.c` ainda rejeita. O cliente é do Aluno 1. O pedido:

- `--object-id` no `getopt_long`, com 64 caracteres hexadecimais.
- `--cmd topology`: conecta em `--host`:`--port`, manda `GET_PREDECESSOR` e `GET_SUCCESSORS` e imprime o que voltar.
- `--cmd lookup --object-id <hex>`: pede ao Super Peer de entrada o `find_successor` desse identificador e imprime o dono (`ip` e `porta`).

Como o script usa `|| true`, o `tests/c3/run.sh` fica verde antes desses comandos existirem, desde que o Super Peer já tenha logado `Successor`, `Finger` e o heartbeat tenha logado `HEARTBEAT` e `SUSPECT`. Os comandos entram para o script deixar de produzir erro de uso.

## Build e testes

### `Makefile`

`obj/superpeer/chord.o` entra no link de `bin/superpeer` e em todo alvo que já liga `superpeer.o` (`test_superpeer`, `test_chunk_service`). Alvo novo `bin/test_chord`, e uma linha a mais no alvo `test`.

### `tests/superpeer/test_chord.c`

Sem socket. Vários `chord_t` no mesmo processo, mensagens entregues por chamada direta, ordem decidida pelo teste.

- `chord_id_add_pow2`: `i = 0`, `i = 255`, carry entre bytes, volta pelo zero.
- Intervalos: trecho normal, trecho que cruza o zero, extremos, `a == b`.
- Depois de `stabilize` o bastante, percorrer os sucessores visita cada nó uma vez e, em cada um, o predecessor do sucessor é ele mesmo.
- Para chaves sorteadas, `find_successor` coincide com o menor identificador maior ou igual à chave.
- Um nó entra num anel já estável e as chaves de `(predecessor, n]` passam para ele.
- O sucessor imediato para de responder. O anterior assume o próximo da lista e a chave do nó morto ainda é lida na réplica.

`tests/common/test_config.c` ganha os casos de `node_roster_load`: linha escolhida pela porta, bootstrap na primeira linha `superpeer`, porta 5101 sem bootstrap, porta ausente, linha curta.

### Os scripts, sem edição

Nenhum harness é alterado: `scripts/demo/script_testes_cp1.sh`, `tests/c2/run.sh`, `tests/c3/run.sh`, `tests/common.sh`, `tests/env.sh`, `tests/generate_fixtures.sh`. Duas execuções seguidas de cada um dão o mesmo veredito e, no C1 e no C3, o mesmo `NodeID:` para a mesma porta.

Critério de pronto:

1. `make test` verde, com os vetores de `node_id_generate` iguais aos de hoje.
2. `bash scripts/demo/script_testes_cp1.sh` imprime `CHECKPOINT C1: APROVADO`. O `NodeID:` do log é o mesmo nas duas execuções.
3. `bash tests/c2/run.sh` imprime `FAIL: 0`, e cada arquivo baixado é igual ao enviado.

O C3, na mesma ordem:

1. os cinco processos sobem com `tests/config/c3.conf`;
2. em até 8 s o log tem `Successor` e `Finger`;
3. em até 8 s o log tem `HEARTBEAT`;
4. depois do `kill -9` da porta 5103 e dos 18 s seguintes, o log tem `SUSPECT`;
5. o log não tem crash;
6. `summary` imprime `FAIL: 0`.

## Ordem de trabalho

1. Combinar com o Aluno 1 a inserção na membership (só insere, não ressuscita `FAILED`) e os dois comandos do cliente.
2. Roster e `node_id_from_endpoint`, com teste.
3. Lógica pura do anel e `test_chord` com a rede falsa.
4. `Makefile`.
5. Esperar o merge da PR #40 e abrir a branch a partir de `dev`.
6. Os quatro tipos no fio.
7. `join`, `stabilize`, `notify`, logs `Successor` e `Finger`. Subir as portas 5101 e 5102 e ver o círculo fechar.
8. `fix_fingers` e `find_successor`.
9. Lista de sucessores, `check_predecessor` e `chord_drop_node` quando a RPC falha. Rodar, nesta ordem, `make test`, `scripts/demo/script_testes_cp1.sh`, `tests/c2/run.sh` e `tests/c3/run.sh`. Repetir o C1 e o C3 e conferir que o `NodeID:` de cada porta não mudou.
10. Transferência de chaves e réplica. A suíte falsa cobre. Os quatro comandos do passo 9 permanecem verdes.

Os passos 2 a 4 não dependem da PR #40. O passo 9 é o primeiro em que o script é critério.

## Riscos

- **Os 8 segundos.** Os cinco processos sobem juntos. Se o `listen` da porta 5101 atrasar, o `join` dos outros falha na primeira tentativa. A repetição a cada 500 ms cabe na janela. Se o anel não logar `Successor` a tempo, o primeiro assert falha e o sintoma está no log.
- **`HEARTBEAT` e `SUSPECT` são da PR #40.** Com a membership vazia, o Chord pode estar certo e o script falha nesses dois asserts. A inserção na membership é parte do `join`, não um acessório.
- **Janela do `SUSPECT`.** O heartbeat rebaixa aos 15 s de silêncio e o script espera 18 s depois do `kill`. O último batimento da porta 5103 precisa ter acontecido perto da morte. Com envio a cada 5 s, o `SUSPECT` cai por volta dos 15–20 s de silêncio, dentro dos 18 s medidos a partir do `kill`.
- **`--object-id` derruba o cliente hoje.** `getopt_long` manda o processo para o `default` e ele sai com erro. O script esconde isso com `|| true`. Enquanto o Aluno 1 não acrescentar o flag, o log do script contém a mensagem de uso e os asserts seguem valendo pelo que o Super Peer imprimiu.
- **Conflito de merge com a PR #40** nas tabelas de `protocol.c` e no `superpeer_run`. A branch nasce depois do merge.
- **`kill -9` no meio de um `send`.** `src/superpeer/main.c` já ignora `SIGPIPE`. A conexão com a porta 5103 falha e `chord_drop_node` trata o erro. O assert de crash lê o log combinado dos cinco, inclusive o que a 5103 escreveu antes de morrer.
- **C1 e C2 no mesmo endereço.** Os dois usam `127.0.0.1:55101`. O NodeID é o hash desse endpoint, com ou sem `--port 55101`. Tratar o `--port` como outra identidade faz o log do C1 divergir do processo que o C2 sobe com o mesmo arquivo.
- **`accept` atrasado.** Colocar o retry do `join` antes do laço de `accept` estoura os 3 s do C1 e os 2 s do C2. A entrada no anel fica na thread de manutenção.
- **RPC para si mesmo.** No C1 e no C2 o sucessor é o próprio processo. Conectar em si disputa o `accept` com o `ping` e com o `upload`. Sucessor local não gera socket.
- **`JOIN` de peer no anel.** O C1 manda um `JOIN` de `type=peer`. Colocar esse cliente no anel muda o sucessor do único Super Peer e a execução seguinte, com outro NodeID de peer, monta outro anel. Peer fica só na membership.
- **`LOOKUP` de 256 bytes.** Mudar o tamanho ou o handler que o `download` usa faz o C2 baixar outro conteúdo, e `assert_file_equal` falha mesmo com o anel certo.
