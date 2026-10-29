# Entrega — Checkpoint 4 (Aluno 1): Eleição Bully

**Projeto:** BitTorrent P2P Híbrido  
**Disciplina:** Programação Distribuída — UEMS  
**Checkpoint:** 04 — Election & SMR  
**Data de entrega:** 21/10/2026  
**Autor:** Guilherme Zanan Piveta (Aluno 1)

---

## 1. Introdução

Este documento descreve a parte do Aluno 1 no quarto checkpoint: a eleição de
coordenador entre os Super Peers pelo algoritmo Bully. Pela divisão da
especificação, o Aluno 1 implementa as mensagens `ELECTION`, `OK` e
`COORDINATOR` e o fluxo:

```text
coordenador morreu → timeout → ELECTION → nós de maior prioridade → OK
→ novo coordenador → COORDINATOR
```

O operation log e a replicação por SMR são a parte do Aluno 2 e não são
descritos aqui. A seção 6 lista apenas os pontos em que as duas partes se
encontram.

O cenário de avaliação da especificação sobe SP1 a SP4, com o SP4 como
coordenador, executa `kill -9` no SP4 e espera que o sistema detecte a falha,
inicie uma eleição, eleja o nó de maior prioridade e anuncie o novo
`COORDINATOR`.

## 2. Escopo

- mensagens `ELECTION`, `OK` e `COORDINATOR`, em `src/common/protocol.c` e
  `include/common/protocol.h`;
- estado de coordenação, rodada de eleição, disparo e recepção, em
  `src/superpeer/superpeer.c` e `include/superpeer/superpeer.h`;
- prioridade da eleição pela coluna `prioridade` do roster, em
  `src/common/config.c` e `include/common/config.h`;
- comandos `put` e `state` do cliente, pedidos pelo Aluno 2 para o harness,
  em `src/peer/peer.c`.

Trabalho entregue nos PRs #47 (eleição), #49 (prioridade pelo roster) e #50
(`put` e `state`).

## 3. Mensagens no Fio

`ELECTION`, `OK` e `COORDINATOR` já existiam no enum de tipos desde o
checkpoint 1, mas não eram serializadas. As três passaram a ser mensagens
**sem payload**, como `PING`, `PONG` e `HEARTBEAT`: a identidade de quem envia
vai no `src_node` do header. As funções `send_election`, `send_ok` e
`send_coordinator` montam um header de origem, com um `TransactionID` novo, no
mesmo molde de `send_heartbeat`.

As mensagens não têm o mesmo padrão de transporte:

- **`ELECTION` → `OK` é uma chamada síncrona no mesmo socket.** Quem inicia a
  eleição conecta no nó superior, envia `ELECTION` e espera o `OK` na mesma
  conexão, como já fazem as chamadas do Chord (`GET_PREDECESSOR`,
  `GET_SUCCESSORS`). Assim, a pergunta "algum nó superior respondeu?" sai
  direto do retorno da chamada, sem precisar de uma fila de respostas.
- **`COORDINATOR` é unidirecional.** O vencedor abre uma conexão para cada
  outro Super Peer vivo, envia o anúncio e fecha, como no `HEARTBEAT`.

A espera pelo `OK` tem um timeout de recepção (`SO_RCVTIMEO`) de
`ELECTION_TIMEOUT_SEC` (1 s). Um nó superior que caiu recusa a conexão ou
esgota esse tempo, e a eleição segue sem ele. Com isso, uma rodada fica abaixo
do limite de 5 s do RNF-010.

## 4. Eleição

### 4.1 Estado de coordenação

O `superpeer_t` ganhou os campos da eleição, todos protegidos por `coord_lock`:

| Campo | Significado |
| --- | --- |
| `coordinator_id` | NodeID de quem este nó considera coordenador |
| `has_coordinator` | 1 se `coordinator_id` é válido |
| `coord_role` | papel deste nó: `COORD_FOLLOWER`, `COORD_CANDIDATE` ou `COORD_LEADER` |
| `election_needed` | pedido pendente para a thread de eleição |

Os três papéis correspondem aos estados `FOLLOWER → CANDIDATE → COORDINATOR` da
especificação. Todo nó sobe sem coordenador conhecido e como `FOLLOWER`.

### 4.2 Prioridade

A especificação define `Priority = NodeID` e diz que o maior NodeID vence. No
projeto, porém, o NodeID do Super Peer é `SHA-256(IP || porta)`, um valor sem
relação com a ordem SP1…SP4. Comparando só o NodeID, quem vencia no roster da
avaliação era o SP2, e o `kill -9` do SP4 não derrubava o coordenador.

O roster, que todos os Super Peers leem, tem uma coluna `prioridade`
(`nome,tipo,ip,porta,id,prioridade`) que antes era validada e descartada. A
função `node_roster_priorities` passou a ler `(ip, porta, prioridade)` de cada
linha `superpeer`, e o Super Peer guarda essa tabela na inicialização. A
comparação da eleição (`outranks_self`) usa primeiro a prioridade e só recorre
ao NodeID para desempatar.

Sem roster, como no C1 e no C2, que usam um `.conf` de `chave=valor`, todos os
nós ficam com prioridade 0 e a comparação cai no NodeID, como manda a
especificação. O NodeID e o anel Chord não mudaram; só a ordem da eleição.

### 4.3 Uma rodada de eleição

A função `start_election` executa uma rodada:

1. O nó passa a `COORD_CANDIDATE` e imprime `ELECTION started by <NodeID>`.
2. Copia da tabela de membros, sob `members_lock`, os Super Peers que o superam
   em prioridade, deixando de fora os `FAILED` e `REMOVED`. Os `SUSPECT` ficam
   na lista, porque a própria chamada é o teste de vida.
3. Solta o lock e envia `ELECTION` a cada um desses nós.
4. Se **algum** responde `OK`, existe um nó superior vivo. O nó volta a
   `COORD_FOLLOWER` e espera o anúncio.
5. Se **nenhum** responde, o nó vence: grava a si mesmo como coordenador, passa
   a `COORD_LEADER`, imprime `highest NodeID wins` e `COORDINATOR <NodeID>` e
   envia `COORDINATOR` aos demais.

Todos convergem para o mesmo líder porque um nó só desiste quando um nó
superior responde, e quem responde também entra na disputa. A disputa sobe até
o maior nó vivo, que é o único que não recebe nenhum `OK`.

### 4.4 Disparo

A thread `election_worker`, criada em `superpeer_run` ao lado das threads de
heartbeat e de manutenção do Chord, acorda a cada 500 ms e inicia uma rodada em
dois casos:

- **boot sem coordenador:** passados `ELECTION_BOOT_GRACE_SEC` (5 s) desde o
  início, se o nó ainda não conhece nenhum coordenador. A espera dá tempo da
  membership se povoar;
- **pedido pendente (`election_needed`):** posto pela thread de heartbeat
  quando o coordenador aparece como `FAILED` na tabela de membros, ou pelo
  recebimento de um `ELECTION` de um nó inferior.

A versão entregue no PR #47 disparava a eleição quando o coordenador deixava
de estar `ALIVE`, porque o Gossip ainda não estava em `dev`. Quando o Gossip
entrou, o gatilho passou a esperar o `FAILED` confirmado por ele (PR #53, do
Aluno 2), como pede a seção de integração com o Gossip: a confirmação evita
eleições provocadas por um atraso momentâneo do coordenador.

Quando cede a um nó superior, a thread espera `COORDINATOR_WAIT_SEC` (15 s)
pelo anúncio antes de tentar de novo, para não reiniciar a eleição a cada
ciclo.

### 4.5 Recepção

Em `handle_connection`:

- **`ELECTION`** vem sempre de um nó inferior. O receptor responde `OK` no
  mesmo socket e pede a própria eleição, porque é superior a quem perguntou e
  pode vencer;
- **`COORDINATOR`** grava o remetente em `coordinator_id`, ajusta `coord_role`
  e imprime `COORDINATOR <NodeID>`.

### 4.6 Concorrência

Duas regras mantêm a eleição livre de deadlock e sem travar as outras threads:

- `coord_lock` e `members_lock` nunca são tomados juntos. A thread de heartbeat
  copia o coordenador sob `coord_lock`, solta esse lock, varre a membership sob
  `members_lock` e só depois volta ao `coord_lock` para pedir a eleição;
- nenhum lock fica preso durante `connect`, `send` ou `recv`. Os alvos da
  eleição e do anúncio são copiados sob lock e contatados depois.

## 5. Cliente: `put` e `state`

O harness do C4 chama dois comandos do cliente, que o plano do Aluno 2 pediu
ao Aluno 1. Os dois estão em `src/peer/peer.c` e saem com 0 só quando o Super
Peer confirma:

- **`--cmd put --name <nome> --value <valor>`** calcula
  `ObjectID = SHA-256(nome ‖ valor)` e envia o nome e o valor ao Super Peer
  indicado, saindo com 0 só quando recebe `ACK`. Na versão do PR #50, o
  pedido seguia como um `STORE` sem chunks. O PR #52 do Aluno 2 trocou o envio
  por uma mensagem `PUT` própria, para o log registrar a operação como `PUT`.
- **`--cmd state --host <ip> --port <porta>`** envia uma mensagem `GET_STATE`
  vazia e imprime o texto que o Super Peer devolve: um registro da hash table
  por linha (`nome vX`), mais a linha `LogIndex <n>` que o Aluno 2 acrescentou.

O texto do `state` é montado no Super Peer, e não no cliente. A saída do
cliente vai para o mesmo log que o harness inspeciona, e com isso o `LogIndex`
que aparece ali é sempre o do Super Peer, nunca um valor inventado pelo
cliente.

## 6. Pontos de Contato com o Aluno 2

- **`coordinator_id`**: o log do Aluno 2 lê esse campo para saber para onde
  encaminhar um `STORE` ou um `PUT`.
- **`smr_on_coordinator`**: função do Aluno 2, chamada pelo código da eleição
  ao vencer uma rodada e ao receber um `COORDINATOR`. As chamadas ficam fora do
  `coord_lock`, para que o lock do log nunca seja tomado com ele preso.
- **Gatilho em `FAILED`**: a eleição depende da confirmação de falha feita
  pelo Gossip.

## 7. Decisões Técnicas

1. **`ELECTION`/`OK` síncronos, `COORDINATOR` unidirecional.** A resposta `OK`
   chega na mesma conexão do `ELECTION`, o que dispensa uma fila de respostas;
   o anúncio não precisa de resposta.

2. **Prioridade pela coluna do roster.** O NodeID por hash não reproduz a ordem
   SP1…SP4 que a avaliação usa. A coluna `prioridade` decide, e o NodeID
   desempata e continua sendo a regra quando não há roster.

3. **Mesmo mecanismo no boot e na recuperação.** O coordenador inicial é
   escolhido por uma eleição comum depois de 5 s de espera, sem caso especial.

4. **Timeout curto na espera do `OK`.** Um nó superior morto não prende a
   rodada por mais de `ELECTION_TIMEOUT_SEC`.

5. **Locks nunca aninhados e nenhuma rede sob lock.** `coord_lock` e
   `members_lock` são independentes, e toda conexão acontece fora deles.

## 8. Verificação

| Item da avaliação | Evidência |
| --- | --- |
| SP4 é o coordenador no início | No boot, os quatro Super Peers recebem `COORDINATOR` do NodeID do SP4 (prioridade 4). |
| `kill -9` no SP4 → detectar falha | Os sobreviventes imprimem `SUSPECT` e, com a confirmação do Gossip, `FAILED` para a porta 5104. |
| Iniciar eleição | `ELECTION started by <NodeID>` nos sobreviventes. |
| Vence o de maior prioridade | O SP3 (prioridade 3) imprime `highest NodeID wins` e `COORDINATOR <NodeID do SP3>`. |
| Anúncio do novo coordenador | SP1 e SP2 imprimem `COORDINATOR <NodeID do SP3>`. |
| Eleição em menos de 5 s (RNF-010) | Cada espera por `OK` é limitada a 1 s; a eleição fica pronta logo após o `FAILED`. |
| `put` e `state` saem 0 | O `put` recebe `RX ACK` e o `state` imprime o registro `c4-file v1`. |

### 8.1 Evidência de Execução

Execução realizada em 28/10/2026, sobre a `dev` no commit `0289e21`:

```text
$ bash tests/c4/run.sh
==== C4 — Bully + SMR ====
[PASS] Eleição foi iniciada
[PASS] Novo coordenador foi anunciado
[PASS] Há evidência de SMR/log
[PASS] Sem crash/deadlock reportado em c4.log
PASS: 4
FAIL: 0
```

Trecho do log do C4 com as linhas da eleição e do cliente. Os NodeIDs estão
abreviados (SP4 = `2eb7805dfa1b…`, SP3 = `1321761cf82b…`) e as linhas
repetidas pelos outros Super Peers foram omitidas:

```text
ELECTION started by 2eb7805dfa1b…
highest NodeID wins
COORDINATOR 2eb7805dfa1b…
...                                  (kill -9 no SP4)
SUSPECT 2eb7805dfa1b… 127.0.0.1:5104
FAILED 2eb7805dfa1b… 127.0.0.1:5104
ELECTION started by 1321761cf82b…
highest NodeID wins
COORDINATOR 1321761cf82b…
...
PUT c4-file
TX PUT
RX ACK
State 127.0.0.1:5101
LogIndex 1
Objects: 1
c4-file v1
```

Os harnesses dos checkpoints anteriores continuam passando com o mesmo binário:

```text
$ bash scripts/demo/script_testes_cp1.sh
PASS: 10
FAIL: 0
CHECKPOINT C1: APROVADO

$ bash tests/c2/run.sh
PASS: 8
FAIL: 0

$ bash tests/c3/run.sh
PASS: 5
FAIL: 0
```

`tests/common/test_config.c` cobre a leitura das prioridades do roster
(valores, linha `peer` ignorada, capacidade insuficiente e prioridade não
numérica). A eleição em si não tem teste unitário; ela é exercitada de ponta a
ponta pelo `tests/c4/run.sh`.

## 9. Limitações Conhecidas

1. **O coordenador antigo retoma a liderança quando volta.** A especificação
   pede que ele entre como `FOLLOWER` e só dispute uma nova eleição se houver
   outra falha. No código, um Super Peer que reinicia não sabe quem é o
   coordenador, porque o `COORDINATOR` só é enviado durante eleições. Depois
   dos 5 s de espera ele inicia uma eleição e, se tiver a maior prioridade,
   vence. Isso foi observado: depois de derrubar o SP4 e subi-lo de novo, ele
   voltou a ser anunciado como `COORDINATOR`. A correção prevista é o nó que
   volta perguntar o coordenador atual antes da eleição de boot.

2. **A saída de um coordenador por `LEAVE` não dispara eleição.** O `LEAVE`
   marca o membro como `REMOVED`, e o gatilho da eleição só reage a `FAILED`.

3. **Rodadas repetidas no boot.** Cada `ELECTION` recebido faz o nó superior
   iniciar a própria rodada, por isso o boot mostra várias rodadas e o mesmo
   `COORDINATOR` repetido até estabilizar. O resultado converge, mas o log
   fica mais longo.

4. **A linha `highest NodeID wins` diz NodeID.** A frase é a da
   especificação, mas o vencedor é o de maior prioridade no roster; o NodeID só
   decide no desempate ou quando não há roster.

5. **A eleição não tem teste unitário.** Ela é verificada de ponta a ponta pelo
   harness.

## 10. Conclusão

A parte do Aluno 1 no Checkpoint 4 deu aos Super Peers um coordenador escolhido
pelo algoritmo Bully. No cenário da avaliação, o SP4 começa como coordenador,
e, quando cai, os sobreviventes detectam a falha, elegem o SP3 e o anunciam a
todos. A eleição se liga ao restante do sistema por dois pontos: o
`coordinator_id`, que o log do Aluno 2 usa para encaminhar operações, e o
`smr_on_coordinator`, chamado a cada troca de líder.
