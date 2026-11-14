# Relatório Técnico — Checkpoint 6: Integração e teste final

**Projeto:** BitTorrent P2P Híbrido  
**Disciplina:** Programação Distribuída — UEMS  
**Checkpoint:** 06 — Integração e teste final  
**Data:** 14/11/2026  
**Autores:** Guilherme Zanan Piveta (Aluno 1) e João Vitor Antunes da Silva (Aluno 2)  
**Código:** branch `dev`, commit `f5382b5`

---

## 1. Introdução

O item 10 (*CHECKPOINT 6*) da especificação pede que o Aluno 1 demonstre
`upload`, `download`, `chunks`, `compression`, `cache` e `replica`, e que o
Aluno 2 demonstre `Chord`, `Gossip`, `Bully`, `SMR`, `2PC` e `IST`. Os dois
podem demonstrar juntos integração, debugging, documentação e testes.

Diferente dos checkpoints 2 a 5, este quase não mexeu nos protocolos nem nos
algoritmos. O código dos Super Peers ficou como estava no checkpoint 5. A
mudança de código está no cliente (`bin/client`), que ganhou um modo sessão e
uma linha de progresso no download. O resto da entrega é infraestrutura para
**operar e demonstrar** o sistema:

1. Um **harness distribuído** (`tests/cluster/`) que sobe Super Peers e clientes
   em várias máquinas por ssh, injeta falhas, inspeciona o estado interno dos
   nós e roda o roteiro final.
2. Um **app de cliente** (`app/`) que roda um Super Peer e um cliente por
   máquina, descobre outros Super Peers por um **servidor de rendezvous**
   (`rendezvous/`) e liga as máquinas por **hole punching TCP**, sem alterar o
   código dos Super Peers.

Até o checkpoint 5 todo teste rodava numa só máquina (`tests/flow/`, `tests/c1`
a `tests/c5`) e cada cliente executava um comando e morria. O plano do harness
está em `docs/checkpoint-06-integration/plans/harness-distribuido.md`; o guia de
uso, em `tests/cluster/README.md`.

## 2. Escopo implementado

**Cliente (`bin/client`)**

- Modo sessão: `--interactive` (comandos na stdin, um por linha) e `--batch
  ARQUIVO` (`-` lê da stdin; `--keep-going` não para no primeiro erro). O modo
  `--cmd` continua igual. Implementado em `src/peer/peer.c`; a quebra de linha
  em tokens (aspas duplas agrupam um argumento) está em `src/peer/session.c` e
  `include/peer/session.h`.
- A sessão tem um NodeID só e escreve os marcadores `[ready]`, `[done N rc=R]` e
  `[bye]`, que o harness usa para saber quando um comando terminou.
- O download emite `Transfer <nome> PROGRESS <chunks feitos> <total>` a cada 1%
  dos chunks (`src/peer/download.c`, `src/peer/peer.c`), e a falha do upload
  mostra o `errno`.

**Harness distribuído (`tests/cluster/`)**

- `cluster.sh`: CLI única. `cluster.conf` descreve as máquinas; cada `host` gera
  por padrão 3 Super Peers e 30 clientes, e o roster e o `.conf` de cada cliente
  são gerados a partir dele.
- `lib/`: `transport.sh` (ssh com `ControlMaster`, ou execução local),
  `remote.sh` (ciclo de vida do processo na máquina remota), `node.sh` e
  `client.sh` (Super Peers e clientes persistentes), `steps.sh` e `asserts.sh`
  (passos e conferências dos cenários) e `inspect.sh` (inspeção).
- `scenarios/`: `smoke.sh`, `final.sh` e `carga.sh`.
- `watch.sh` (painéis em tmux) e `tui/` (interface Textual com visão de cliente
  e de gerenciamento do `cluster.conf`).

**App e rendezvous**

- `rendezvous/rendezvous.c`: servidor de rendezvous em C, texto sobre TCP (uma
  conexão, um comando). `REGISTER`, `UNREGISTER`, `LIST`, `PING`, `PUNCH`,
  `WHOAMI` e `PROBE`. Scripts de apoio: `rdv.sh` (cliente em bash),
  `nat-probe.py` (testa se duas máquinas atrás de NAT conseguem hole punching),
  `deploy.sh` e `aws-template.yaml` (instância EC2 com IP elástico).
- `app/app.py`, `app/node.py`, `app/tunnel.py`: app em Textual, gerência dos
  processos e túnel. Detalhes na seção 5.

**Testes**

- Novo: `tests/peer/test_session.c` (22 checagens do `session_split`).
- Os harnesses dos checkpoints anteriores não foram alterados e continuam
  valendo (seção 6).

## 3. Harness distribuído

### 3.1 Topologia e ciclo de vida

```text
   máquina do operador (tests/cluster/)                    máquinas remotas
  ┌──────────────────────────────────────┐   ssh      ┌──────────────────────────┐
  │ cluster.conf ─► cluster.sh           │ ─────────► │ run/<nome>/  (cwd do nó) │
  │                  ├ deploy / up / down│            │   proc.log               │
  │                  ├ kill / start      │            │   pid                    │
  │                  ├ client do / batch │            │   in.fifo  (só cliente)  │
  │ logs/<nome>.log ◄┤ espelho + carimbo │ ◄───────── │ bin/superpeer|bin/client │
  │ scenarios/*.sh ──┘ asserts           │ tail -F    └──────────────────────────┘
  └──────────────────────────────────────┘
```

- **Um ponto de controle.** Toda ação passa por `cluster.sh`; cenários, `watch.sh`
  e a TUI só chamam esse comando.
- **Estado remoto mínimo.** Cada máquina guarda o código, `run/` e os processos.
  Cada processo roda no próprio `run/<nome>/`, porque o storage de chunks é
  relativo ao diretório de trabalho.
- **Sobrevive à sessão ssh.** `remote.sh` lança com `setsid nohup`, grava o
  `pid` e permite `stop` (SIGTERM, depois SIGKILL) e `kill9`. Este último é o
  `kill -9 SPx` da especificação.
- **Clientes persistentes.** Cada cliente é um `bin/client --interactive` com
  stdin ligada a uma FIFO (`in.fifo`). `client do` envia a linha e espera o
  `[done N rc=R]`.
- **Escala.** Basta acrescentar linhas `host`. O limite é o do parser de roster
  (`node_roster_load`): 64 Super Peers, cerca de 21 máquinas com 3 cada. As
  portas dos Super Peers são únicas no cluster inteiro, e a coluna `ip` do
  roster é o IP que as outras máquinas alcançam. O `check` valida ambos.
- **Senhas.** `copy-keys` copia a chave com `ssh-copy-id` e `sshpass`, perguntando
  a senha ou lendo `$SSHPASS`. A senha nunca vai para arquivo; depois disso tudo
  usa `BatchMode`.

### 3.2 Logs e relógio

A especificação assume relógios locais independentes. Por isso um espelho por
máquina (`ssh tail -F`, com lock) grava cada linha em
`tests/cluster/logs/<nome>.<vida>.log` com o carimbo **da máquina do operador**,
no momento em que a linha chega. As medidas de tempo (eleição, detecção, IST)
usam esse relógio único. Quando um processo é reiniciado, a nova vida vira outro
arquivo e `<nome>.log` passa a apontar para ela. O `rexec` descarta tudo o que o
ssh remoto imprime antes do comando (`.bashrc`, `motd`), que corrompia caminhos
e `sha256` capturados.

### 3.3 Inspeção dos Super Peers

Os comandos abaixo não mexem no código dos nós: leem os logs espelhados e, em
`state` e `lookup`, consultam o Super Peer ao vivo (`GET_STATE` e `LOOKUP`).

| Comando | O que mostra |
| --- | --- |
| `overview` | Uma linha por Super Peer: vivo ou não, NodeID, sucessor, coordenador, `LogIndex`, membros ALIVE/SUSPECT/FAILED |
| `members` | Matriz observador x membro (Gossip e detecção) |
| `chord [--fingers]` | Anel por NodeID, sucessor reportado contra o esperado (`ok` ou `DIVERGE`) e finger table |
| `coordinator` | Coordenador que cada nó reconhece e quantas eleições viu (Bully) |
| `state [--files]` | `LogIndex`, objetos e métricas do LFU, com aviso de divergência (SMR, IST, cache) |
| `metadata`, `lookup` | Metadados registrados e caminho Chord até o dono do objeto |
| `events <SP> [cat]` | Últimos eventos por categoria: `membership`, `election`, `chord`, `2pc`, `ist`, `lfu`, `log`, `metadata` |

Há um comando para cada item da lista do Aluno 2, o que facilita mostrar cada
um deles ao vivo na demonstração.

### 3.4 Cenários

| Cenário | O que faz |
| --- | --- |
| `smoke` | Sobe tudo, um upload, um download em outra máquina, confere o `LogIndex` |
| `final` | Roteiro do CP6, uma fase por item das listas dos dois alunos |
| `carga` | Todos os clientes ao mesmo tempo, cada um envia um arquivo e baixa o do seguinte (`WAVE=<n>` limita quantos agem por vez) |

Ao terminar, `run.sh` grava `tests/cluster/out/relatorio.md` (fases, métricas de
tempo e caminho dos logs) e derruba o cluster (`CLUSTER_KEEP=1` mantém).

## 4. Roteiro final e evidência

O cenário `final` tem uma fase por item da lista do item 10. O relatório gerado
na execução de 13/11/2026 (5 Super Peers em 5 hosts do `cluster.conf`, 15
clientes) foi:

| Fase | Item da especificação | Resultado |
| --- | --- | --- |
| Boot de 5 Super Peers em 5 máquinas | Bully, Chord | PASS (coordenador SP5) |
| 15 clientes persistentes abertos | integração | PASS |
| Uploads de A, B e C por C1 | SMR, 2PC, `upload` | PASS (mesmo log e estado nos 5 nós) |
| `lookup` pelo ObjectID de A | Chord | PASS (dono SP3) |
| `kill -9` de SP2 | Gossip, detecção | PASS |
| Uploads de D e E com um participante fora | 2PC | PASS |
| SP2 volta e recebe A até E | IST | PASS |
| Duas consultas de A em SP1 | cache LFU | PASS (miss e frequência 1, depois hit e frequência 2) |
| Download de A por C1 e por C4 (outra máquina) | `download` | PASS (`sha256` conferido) |
| Arquivo de 9 MiB por C4 | chunks, compressão | PASS (3 chunks de 4 MiB, `sha256` idêntico) |
| Derruba SP3 (dono de A) e baixa A de novo | réplica | PASS (A baixado por C1 sem SP3) |

| Métrica | Medido | Alvo |
| --- | --- | --- |
| Eleição no boot | 4,791 s | < 5 s (RNF-010) |
| Detecção (`kill` até `SUSPECT` no coordenador) | 14,209 s | ~15 s (heartbeat 5 s, timeout 15 s) |
| IST (do `IST` ao `STORE` do último log) | 0,330 s | < 2 s (RNF-008) |

A eleição ficou abaixo do alvo nesta execução, mas a margem é pequena: em
ensaios anteriores com 3 Super Peers em duas máquinas ela levou de 5,0 a 5,5 s.
A medida vai do primeiro log do primeiro nó até a vitória do coordenador, o que
inclui a subida dos processos.

Outros ensaios, todos sem falha: `final` com 3 Super Peers numa máquina e com 6
em duas, e `carga` com 60 clientes persistentes (30 por máquina) e 6 Super
Peers, em ondas de 10 e com todos de uma vez, terminando com o mesmo `LogIndex
60` nos 6 Super Peers.

### 4.1 Mapa dos itens do Aluno 1

| Item | Onde está e como foi demonstrado |
| --- | --- |
| `upload` | `src/peer/upload.c`; fases dos uploads A–E |
| `download` | `src/peer/download.c`; `sha256` conferido em duas máquinas |
| `chunks` | `src/peer/file_pipeline.c`, `src/peer/storage.c`; arquivo de 9 MiB em 3 chunks |
| `compression` | `src/common/compression.c`; mesma fase dos chunks |
| `cache` | `src/superpeer/lfu.c`; fase do LFU, `state` mostra as métricas |
| `replica` | `src/peer/replica.c`; fase que derruba o dono de A |

### 4.2 Mapa dos itens do Aluno 2

| Item | Onde está e como foi demonstrado |
| --- | --- |
| Chord | `src/superpeer/chord.c`; `chord`, `lookup` |
| Gossip | `src/superpeer/superpeer.c`; `members`, fase do `kill -9` |
| Bully | `src/superpeer/superpeer.c`; `coordinator`, fase do boot |
| SMR | `src/superpeer/smr.c`; `state` igual nos 5 nós |
| 2PC | `src/superpeer/smr.c`; `events <SP> 2pc` |
| IST | `src/superpeer/smr.c`; `events <SP> ist` no SP2 reiniciado |

## 5. App com rendezvous e hole punching

O app (`app/run.sh`) é a aplicação de usuário: roda **um Super Peer e um cliente
nesta máquina**, em uma interface Textual com as abas Cliente, Super Peer e
Configuração.

Ao abrir:

1. Lê `app/data/config.json` (rendezvous, portas, IP anunciado).
2. Registra a porta do Super Peer no rendezvous com `rendezvous/rdv.sh`. O
   servidor guarda o endereço público e devolve até 3 outros Super Peers.
3. Sobe o túnel (`app/tunnel.py`) e espera abrir a conexão com um deles, por até
   45 s. Sem nenhum túnel, nada é iniciado, para não criar uma rede separada.
4. Grava `sp.conf` com o `ip=` virtual e, se houver, o `bootstrap=`.
5. Sobe o Super Peer e o cliente (`bin/client --interactive`).
6. Renova o registro a cada 20 s. Ao sair, faz `unregister` e encerra os processos.

**Túnel sem alterar o Super Peer.** O Super Peer só fala com endereços de
loopback. Cada nó recebe um endereço virtual `127.x.y.z`, derivado do id dele no
rendezvous e igual em todas as máquinas. Para cada outro Super Peer o app
escuta em `127.x.y.z:<porta dele>` e leva as conexões por **uma** conexão TCP
direta, aberta por hole punching: as duas pontas conectam uma na outra ao mesmo
tempo, sempre a partir da mesma porta local, que também fala com o rendezvous
(comando `PUNCH`). Do outro lado, o app entrega em `127.0.0.1:<porta do próprio
Super Peer>` e recusa qualquer outra porta.

**Recursos do app:** upload de arquivo ou de **pasta** (a pasta vira um `.zip`
temporário e é enviada como arquivo comum), download com barra de progresso
alimentada pelas linhas `PROGRESS`, busca, guardar valor, estado do nó e log do
Super Peer com filtro.

**Rendezvous na AWS.** `rendezvous/deploy.sh up` publica o servidor numa EC2
(`t3.micro`) com IP elástico, imagem no ECR e logs no CloudWatch, com `--network
host`, porque o servidor precisa do IP real de cada Super Peer.

**Limitações conhecidas** (também em `app/README.md` e `rendezvous/README.md`):

- O túnel não cifra nem autentica; NAT simétrico impede o hole punching.
- Só Linux, por usar vários endereços de loopback.
- A porta do Super Peer precisa ser diferente em cada máquina.
- O app precisa estar aberto nas duas pontas.
- O rendezvous guarda estado só em memória, atende uma conexão por vez, guarda
  no máximo 256 registros e não tem autenticação (`ALLOWED_CIDR` restringe o acesso).

## 6. Testes

`make test` termina com código 0, no commit `f5382b5`:

| Binário | Resultado |
| --- | --- |
| `bin/test_node` | 52 testes ok |
| `bin/test_config` | 86 testes ok |
| `bin/test_superpeer` | 109 testes ok |
| `bin/test_metadata` | 57 testes ok |
| `bin/test_compression` | 16 testes ok |
| `bin/test_file_pipeline` | 37 testes ok |
| `bin/test_storage` | 24 testes ok |
| `bin/test_session` | 22 testes ok (novo) |
| `bin/test_upload` | 28 testes ok |
| `bin/test_download` | 27 testes ok |
| `bin/test_chunk_wire` | 31 testes ok |
| `bin/test_chunk_service` | 21 testes ok |
| `bin/test_chord` | 131 testes ok |
| `bin/test_chord_ring` | 15 testes ok |
| `bin/test_smr` | 143 testes ok |
| `bin/test_2pc` | 58 testes ok |
| `bin/test_ist` | 45 testes ok |
| `bin/test_lfu` | 33 testes ok |
| `bin/test_network` | 19 testes ok |

Os harnesses `tests/c1` a `tests/c5` e `tests/flow/run.sh` não foram alterados
neste checkpoint e não foram reexecutados para este relatório; os resultados
registrados são os do relatório do checkpoint 5. O ensaio de integração deste
checkpoint é o cenário `final` do harness distribuído (seção 4).

## 7. Pendências e riscos

- **Alcance dos ensaios.** O cluster foi executado em máquinas reais de três
  sub-redes de laboratório diferentes, e o app foi testado com máquinas em
  países diferentes, atrás de NAT. Em todos os ensaios deste checkpoint cada
  `host` do `cluster.conf` é uma máquina real; as máquinas simuladas ficaram nos
  checkpoints anteriores.
- **Hole punching.** O túnel depende do tipo de NAT de cada rede, e NAT simétrico
  o impede (seção 5). Em redes novas, convém rodar o `nat-probe.py` antes e ter
  VPN ou rede local como plano B.
- **Limites herdados do CP5.** O operation log em memória, com teto de 256
  entradas, limita o que o IST reconstrói, e o envio de chunks no upload ainda
  não tem prazo.
- **Sem autenticação.** O rendezvous e o túnel aceitam qualquer par que os
  alcance; adequado para a demonstração, não para uso real.

## 8. Como reproduzir

```bash
cp tests/cluster/cluster.conf.example tests/cluster/cluster.conf   # ajuste hosts e usuários
tests/cluster/cluster.sh copy-keys      # chave ssh
tests/cluster/cluster.sh check          # ssh, compilador, bibliotecas, relógio, portas
tests/cluster/cluster.sh deploy         # código, compilação e arquivos de teste
tests/cluster/watch.sh final            # cenário final com painéis em tmux
tests/cluster/run.sh final              # o mesmo, sem tmux; gera out/relatorio.md
tests/cluster/cluster.sh overview       # inspeção durante a demonstração
```

Para o app: `make && app/run.sh` (a aba Configuração recebe o endereço do
rendezvous, impresso por `rendezvous/deploy.sh up`).

## 9. Conclusão

O checkpoint 6 não muda os algoritmos: entrega o que faltava para demonstrá-los
e testá-los em conjunto. O harness distribuído roda o roteiro completo, com um
passo para cada item das listas dos dois alunos, em várias máquinas, com falhas
injetadas e métricas de tempo medidas num relógio único. O app com rendezvous
permite formar a rede entre máquinas atrás de NAT sem tocar nos Super Peers.
Cluster e app foram exercitados em máquinas reais, em sub-redes e países
diferentes.
