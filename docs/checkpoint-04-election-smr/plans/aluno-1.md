# Plano de implementação — Checkpoint 4 (Aluno 1)

Prazo: **21/10/2026**. Papel: Aluno 1.

Este documento descreve **como** o Aluno 1 implementa a eleição de coordenador pelo algoritmo Bully. A referência do algoritmo é o Bully clássico: prioridade por `NodeID`, maior `NodeID` vence, mensagens `ELECTION`, `OK` e `COORDINATOR`, e os estados `FOLLOWER → CANDIDATE → COORDINATOR`. A fonte de verdade é a seção *BULLY ALGORITHM E RECUPERAÇÃO DO COORDENADOR* da especificação (`docs/specification/trabalho_2026_SD.pdf`), somada à divisão de trabalho do checkpoint 4, onde o Aluno 1 é responsável por `ELECTION`, `OK` e `COORDINATOR`, e o Aluno 2 pelo operation log / SMR.

Este plano parte de `dev` depois que o anel Chord do checkpoint 3 (PR #44) e o heartbeat (PR #40) já estão integrados. Ele reaproveita, sem reescrever, o detector de falhas que já existe: a thread de heartbeat em `src/superpeer/superpeer.c` já varre a membership a cada 1 segundo e rebaixa para `MEMBER_SUSPECT` quem fica em silêncio por `HEARTBEAT_TIMEOUT_SEC` (15 s). É esse rebaixamento que dispara a eleição.

Não substitui o relatório final do checkpoint (`deliverable/`).

## O que o checkpoint pede do Aluno 1

Da especificação (seção do Bully e item 9 da metodologia, *CHECKPOINT 4*):

- **Três mensagens:** `ELECTION` (inicia a eleição), `OK` (existe nó superior vivo) e `COORDINATOR` (anúncio do novo líder).
- **Prioridade = `NodeID`. O maior `NodeID` vence.** Estados do nó: `FOLLOWER → CANDIDATE → COORDINATOR`.
- **Fluxo exigido:** o coordenador morre → timeout → `ELECTION` para os nós de maior prioridade → `OK` de quem é superior → o maior vira novo coordenador → `COORDINATOR` em broadcast → o sistema retoma as operações.
- **RNF-010:** a eleição deve durar menos de 5 segundos.
- **Integração com o Gossip:** o Gossip confirma a suspeita de falha (`SUSPECT → FAILED`) antes da eleição, para reduzir eleições provocadas por atraso. O Gossip é do Aluno 2; este plano deixa a costura pronta.
- **Recuperação do coordenador antigo:** quando um ex-coordenador volta, ele entra como `FOLLOWER`, sincroniza via IST e **não** força nova eleição; só volta a disputar se ocorrer outra falha.

Avaliação descrita na especificação para o C4: sobem SP1–SP4, SP4 é o coordenador (maior prioridade), executa-se `kill -9` no coordenador, e o sistema deve `detect failure → start election → highest NodeID wins → COORDINATOR → resume operations`; em seguida um `upload` e a comparação dos metadados entre os sobreviventes, que devem ficar logicamente consistentes. A parte de consistência de metadados/logs é do Aluno 2.

## Estado atual

Em `dev`, depois do checkpoint 3:

- `include/common/protocol.h` já declara os tipos `ELECTION`, `OK` e `COORDINATOR` no enum `message_type`, reservados para o CP4. Eles ainda **não estão ligados**: em `src/common/protocol.c` a tabela `payload_sizes` marca os três como `-1` (tamanho variável) e eles caem no `default` de `serialize_message` e `deserialize_message`, que devolve erro. É a mesma situação em que o `HEARTBEAT` estava antes do checkpoint 3.
- `node_id_cmp` em `src/common/node.c` dá ordem total entre identificadores. "Maior `NodeID` vence" é `node_id_cmp(a, b) > 0`.
- A thread de heartbeat em `src/superpeer/superpeer.c` já varre a membership a cada 1 s e marca `MEMBER_SUSPECT` quem passou de `HEARTBEAT_TIMEOUT_SEC` sem batimento. O retorno a `MEMBER_ALIVE` acontece no RX de `HEARTBEAT`. É o gancho pronto para o disparo da eleição.
- Não há, em lugar nenhum, estado de coordenador nem de líder. É o que este plano acrescenta.
- `send_heartbeat` em `src/common/protocol.c` é o molde: mensagem de origem (`fill_origin_header`, `TransactionID` novo), sem payload, `src_node` carrega a identidade de quem emite.

## Escopo

- Ligar `ELECTION`, `OK` e `COORDINATOR` no protocolo (serialização, desserialização e helpers de envio).
- Estado de coordenador no Super Peer: quem é o líder atual e o papel deste nó (`FOLLOWER` / `CANDIDATE` / `COORDINATOR`).
- A rotina de eleição Bully: enviar `ELECTION` aos nós de maior `NodeID`, decidir pela presença ou ausência de `OK`, e anunciar `COORDINATOR` quando vencer.
- O lado de recepção em `handle_connection`: responder `OK` a um `ELECTION` de nó inferior e registrar o novo líder ao receber `COORDINATOR`.
- O disparo: eleição no boot (quando ainda não há coordenador) e eleição quando o coordenador atual cai.

Ficam fora, por serem do Aluno 2: a confirmação `FAILED` via Gossip (aqui o disparo usa `SUSPECT` do coordenador como gatilho, com a costura pronta para o Gossip), o operation log, o SMR, o 2PC e o IST. Também fica fora deste plano a semeadura da membership a partir do roster — ver *Riscos*.

## Design

### Mensagens no fio, em `src/common/protocol.c` e `include/common/protocol.h`

Os três tipos já existem no enum; falta ligá-los, no mesmo molde do `HEARTBEAT`. Todas são mensagens de origem, sem payload — a identidade de quem emite vai no `src_node` do header:

- `payload_sizes[ELECTION]`, `payload_sizes[OK]` e `payload_sizes[COORDINATOR]` passam de `-1` para `0` (tamanho fixo de zero byte, como `PING`, `PONG` e `HEARTBEAT`).
- Os três entram nos `switch` de `serialize_message` e `deserialize_message`, agrupados com `PING` / `PONG` / `HEARTBEAT` (caminho sem corpo), para não caírem mais no `default`.
- Três helpers novos, ao lado de `send_heartbeat`:

  ```c
  int send_election(int fd, const node_id_t *self);
  int send_ok(int fd, const node_id_t *self);
  int send_coordinator(int fd, const node_id_t *self);
  ```

  Cada um monta o header com `fill_origin_header(&hdr, self, <TIPO>, 0)` e chama `send_message` sem payload, idêntico a `send_heartbeat`.

### Transporte: `ELECTION` → `OK` é síncrono; `COORDINATOR` é broadcast

O `HEARTBEAT` é fire-and-forget: conecta, envia, fecha. A eleição **não** pode ser: quem inicia precisa saber se algum nó superior respondeu `OK`. A escolha:

- `ELECTION` → `OK` é uma RPC síncrona no mesmo `fd`, como as RPCs de Chord (`GET_PREDECESSOR`, `GET_SUCCESSORS`) já fazem: o iniciante conecta no nó superior, envia `ELECTION` e aguarda o `OK` no mesmo socket, com timeout curto de recepção. `OK` recebido → existe superior vivo. Conexão recusada ou timeout → aquele superior está fora.
- `COORDINATOR` é unidirecional, em broadcast, recebido em `handle_connection` como o `HEARTBEAT`. O vencedor abre uma conexão para cada outro Super Peer vivo, envia e fecha.

Essa divisão evita construir uma caixa de entrada assíncrona para coletar `OK`s: a decisão "alguém superior respondeu?" sai direto do retorno das RPCs.

### Estado de coordenador, em `include/superpeer/superpeer.h`

Campos novos em `superpeer_t`:

```c
typedef enum {
  COORD_FOLLOWER,   /* segue um coordenador conhecido (ou nenhum ainda) */
  COORD_CANDIDATE,  /* eleição em andamento iniciada por este nó */
  COORD_LEADER      /* este nó é o coordenador */
} coord_role_t;

node_id_t coordinator_id;     /* quem este nó considera coordenador */
int       has_coordinator;    /* coordinator_id é válido */
coord_role_t coord_role;      /* papel deste nó na eleição */
pthread_mutex_t coord_lock;   /* serializa os campos acima */
```

E dois `#define` novos, ao lado de `HEARTBEAT_SEC`:

- `ELECTION_TIMEOUT_SEC` — teto da RPC `ELECTION`/`OK` por nó superior (curto, ~1 s), para caber no RNF-010.
- `COORDINATOR_WAIT_SEC` — tempo que um `CANDIDATE` que recebeu `OK` espera por um anúncio `COORDINATOR` antes de reiniciar a eleição (defensivo contra o superior que respondeu `OK` e morreu em seguida).

`coord_lock` é um lock pequeno, independente de `members_lock`. Ele nunca é segurado durante I/O de rede, mesma disciplina do heartbeat: os alvos da eleição são copiados sob `members_lock`, o lock é solto, e só então as RPCs correm. `members_lock` e `coord_lock` nunca são tomados juntos.

### A rotina de eleição

Roda numa thread própria (`election_worker`), criada em `superpeer_run` junto das demais, destacada, para que uma eleição de alguns segundos não atrase a cadência de 1 s do heartbeat. A thread espera por um pedido de eleição (um flag `election_needed`, verificado a cada ~500 ms) e executa:

```text
start_election():
    coord_role = CANDIDATE
    higher = snapshot dos membros SUPERPEER vivos com NodeID > self   (sob members_lock, depois solta)
    got_ok = 0
    para cada h em higher:
        fd = net_connect(h)
        se fd >= 0:
            se send_election(fd, self) == NET_OK e recebe OK dentro de ELECTION_TIMEOUT_SEC:
                got_ok = 1
            net_close(fd)
    se got_ok:
        coord_role = FOLLOWER          # algum superior vai anunciar COORDINATOR
        arma espera de COORDINATOR_WAIT_SEC; se nada chegar, dispara nova eleição
    senão:
        # este nó venceu
        coord_role = COORDINATOR
        coordinator_id = self; has_coordinator = 1
        imprime a eleição vencida e o novo coordenador
        broadcast de COORDINATOR para os demais Super Peers vivos (unidirecional)
```

`higher` só inclui membros `SUPERPEER` que não estejam `MEMBER_FAILED` nem `MEMBER_REMOVED`, como o heartbeat já filtra para o envio. Num anel de poucos Super Peers, a varredura sequencial com timeout curto fica bem dentro dos 5 s do RNF-010.

### Recepção, em `handle_connection` (`src/superpeer/superpeer.c`)

Dois ramos novos, no mesmo lugar do ramo de `HEARTBEAT`:

- **RX `ELECTION`** (de um nó inferior, por definição, já que ninguém manda `ELECTION` para baixo): responde `OK` no mesmo `fd` e marca `election_needed`. Sendo superior a quem iniciou, este nó também disputa — é o comportamento clássico do Bully: ao receber `ELECTION` de um inferior, responde `OK` e começa a própria eleição.
- **RX `COORDINATOR`**: grava `coordinator_id = src_node`, `has_coordinator = 1`, `coord_role = FOLLOWER` e imprime o novo líder. Se este nó se achava coordenador e o anúncio vem de um `NodeID` maior, ele cede.

### Disparo e bootstrap

Um único mecanismo cobre o boot e a recuperação:

- **Boot:** todo nó sobe com `has_coordinator = 0`. Passado um curto período de graça (para a membership se povoar e para um `COORDINATOR` de uma eleição já em curso chegar), se ainda não há coordenador conhecido, dispara-se uma eleição. O maior `NodeID` vence e anuncia `COORDINATOR`; todos registram. Assim o coordenador inicial — o SP4 do exemplo da especificação — surge sem caso especial.
- **Falha:** na varredura, se o membro cujo `NodeID` é o `coordinator_id` passa a `SUSPECT` (ou `FAILED`/`REMOVED`), limpa-se `has_coordinator` e marca-se `election_needed`. O detector de falhas do checkpoint 3 é quem provê esse sinal.
- **Debounce:** uma eleição por vez. Enquanto `coord_role == CANDIDATE`, um novo pedido não reinicia a varredura. Depois de perder ou vencer, só se dispara de novo quando o coordenador muda ou é perdido outra vez. Isso evita tempestade de eleições.

**Decisão sobre o gatilho — `SUSPECT`, não `FAILED`.** A especificação manda o Gossip confirmar `FAILED` antes da eleição. O Gossip é do Aluno 2. Para que a eleição seja testável de forma isolada já neste checkpoint, o gatilho usa o coordenador em `SUSPECT`. A costura fica pronta: quando o Gossip do Aluno 2 entrar, basta trocar a condição de disparo de `MEMBER_SUSPECT` para `MEMBER_FAILED` confirmado.

**Decisão sobre o coordenador que volta.** O Bully de livro faz o nó de maior `NodeID` que retorna "bullyar" e reassumir via nova eleição. A especificação **sobrepõe** isso: o ex-coordenador que volta entra como `FOLLOWER` e não força eleição. Por isso a eleição de boot é suprimida quando já se conhece um `COORDINATOR`: só dispara se `has_coordinator` continuar `0` depois do período de graça. É um desvio deliberado do Bully clássico, ditado pela especificação.

### Logs que a avaliação lê

A especificação descreve a saída esperada do C4 como `detect failure → start election → highest NodeID wins → COORDINATOR → resume operations`, mas, diferente do checkpoint 3, **ainda não há um `tests/c4/run.sh` do professor** fixando os tokens exatos do `grep`. Até esse script chegar, o Super Peer imprime tokens reconhecíveis e alinhados com o texto da especificação:

```text
ELECTION started by <hex>
OK -> <hex>
COORDINATOR <hex>
```

Mais linhas humanas de apoio (`detect failure`, `resume operations`) junto das transições. Os tokens `ELECTION`, `OK` e `COORDINATOR` em maiúsculas batem com os nomes das mensagens da especificação e com o conjunto que o harness do C3 já aceitava (`SUSPECT|FAILED|REMOVED|Election|ELECTION`). Quando o `run.sh` do C4 for fornecido, este plano e os `printf` se ajustam aos tokens exatos.

## Build e testes

### `tests/superpeer/test_election.c` (novo)

Sem socket, no molde de `test_chord.c`: a decisão da eleição é lógica pura e pode ser exercitada com um `superpeer_t` em memória e uma membership montada à mão.

- Dado um snapshot de membership, a função que escolhe os alvos devolve só os `SUPERPEER` vivos com `NodeID > self`.
- Nó com o maior `NodeID` do conjunto não encontra superior e vence (vira `COORDINATOR`).
- Nó não-máximo encontra superior, cede, fica `FOLLOWER`.
- Receber `COORDINATOR` de um `NodeID` maior grava o líder e rebaixa a `FOLLOWER`.
- O coordenador atual em `SUSPECT` marca `election_needed`; em `ALIVE` não marca.

### `tests/common/test_protocol.c`

Casos novos para `ELECTION`, `OK` e `COORDINATOR`: `serialize` seguido de `deserialize` preserva tipo e `src_node`, e o tamanho de payload é zero. Mesmos vetores fixos dos tipos sem corpo que já existem.

### `Makefile`

`bin/test_election` como alvo novo, ligando `superpeer.o` (e `chord.o`, que já entra no link de `superpeer.o`), e uma linha a mais no alvo `test`.

### Scripts, sem edição

Nenhum harness existente é alterado. `make test`, `scripts/demo/script_testes_cp1.sh`, `tests/c2/run.sh` e `tests/c3/run.sh` continuam com o mesmo veredito. O C3 segue imprimindo `SUSPECT`; a eleição é aditiva.

Critério de pronto:

1. `make test` verde, incluindo `test_election` e os casos novos de protocolo.
2. Os harnesses do C1, C2 e C3 seguem aprovando, sem regressão.
3. Com a membership povoada (ver *Riscos*): subindo quatro ou cinco Super Peers, o log mostra um `COORDINATOR` eleito no boot; ao matar o coordenador, o log mostra, dentro da janela do detector mais a eleição, um novo `COORDINATOR` com `NodeID` maior entre os sobreviventes, e nenhum crash.

## Ordem de trabalho

1. Ligar `ELECTION`, `OK` e `COORDINATOR` no `protocol.c`/`protocol.h` (três edições + três helpers) e cobrir com `test_protocol.c`.
2. Estado de coordenador em `superpeer.h` (`coord_role_t`, campos, `coord_lock`, os dois `#define`) e inicialização em `superpeer_init`.
3. Lógica pura da escolha de alvos e da decisão da eleição, com `test_election.c` e a rede falsa.
4. `Makefile`: `bin/test_election` e a linha do `test`.
5. A thread `election_worker` e seu spawn em `superpeer_run`, com o flag `election_needed`.
6. Os ramos de `ELECTION` e `COORDINATOR` em `handle_connection`.
7. O disparo: boot (sem coordenador → eleição após graça) e falha (coordenador em `SUSPECT` → eleição), com debounce.
8. Logs `ELECTION` / `OK` / `COORDINATOR`. Subir quatro Super Peers, matar o coordenador e ver um novo ser eleito e anunciado.

Os passos 1 a 4 não dependem de nada do Aluno 2. O passo 8 só fecha de ponta a ponta com a membership povoada.

## Riscos

- **Semeadura da membership (mesmo bloqueio do C3).** O Bully só alcança alguém se cada Super Peer tiver os outros na própria membership. Hoje os nós não se mandam `JOIN` a partir do roster — essa semeadura é do Aluno 2 (roster em `src/common/config.c` e inserção na membership via `join`/`stabilize`). O Aluno 1 constrói e testa o mecanismo da eleição de forma isolada (`test_election.c`); o teste de ponta a ponta depende de a semeadura do Aluno 2 estar em `dev`.
- **RNF-010, os 5 segundos.** Detecção aos 15 s de silêncio mais a eleição. A eleição em si tem de ser curta: timeout de RPC por nó superior em torno de 1 s e poucos Super Peers. O que pode estourar o orçamento é esperar `OK` de um superior morto; o timeout curto de recepção é o que segura isso.
- **Gatilho em `SUSPECT` sem Gossip.** Sem a confirmação `FAILED` do Gossip, um atraso temporário do coordenador pode disparar uma eleição desnecessária. É o custo de testar isolado; a troca para `FAILED` confirmado é uma linha quando o Gossip do Aluno 2 entrar.
- **Coordenador que volta.** Se a supressão da eleição de boot falhar, o ex-coordenador reassume e interrompe o sistema sem necessidade, contrariando a especificação. O período de graça e a checagem de `has_coordinator` antes de disparar no boot são o que evita isso.
- **Eleições concorrentes.** Dois nós detectam a falha e iniciam eleições ao mesmo tempo. O Bully converge: o inferior recebe `OK` do superior e cede; o superior segue. O debounce e a ordem total de `node_id_cmp` garantem um único vencedor, o de maior `NodeID`.
- **`kill -9` no meio de um `send`.** `src/superpeer/main.c` já ignora `SIGPIPE`. A conexão com o coordenador morto falha no `net_connect`/`recv`, e a eleição trata como "superior fora". O assert de ausência de crash lê o log combinado dos processos.
- **Conflito de merge** em `src/common/protocol.c` (tabelas e `switch`), `include/superpeer/superpeer.h` e `superpeer_run`, os mesmos pontos que o Chord e o heartbeat tocam. A branch nasce de `dev` depois dos merges do C3.
- **Tokens do harness do C4.** Enquanto o `tests/c4/run.sh` do professor não é fornecido, os `printf` seguem o texto da especificação (`ELECTION`, `OK`, `COORDINATOR`, `highest NodeID wins`). Quando o script chegar, os tokens exatos são conferidos e ajustados.
