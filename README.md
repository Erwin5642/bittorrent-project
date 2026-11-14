# BitTorrent P2P Híbrido

Rede P2P de compartilhamento de arquivos escrita em C para Linux, com `peer`s (clientes) e `superpeer`s (nós da overlay). Trabalho da disciplina de Programação Distribuída (UEMS, Ciência da Computação, 2026). A especificação oficial está em [`docs/specification/trabalho_2026_SD.pdf`](docs/specification/trabalho_2026_SD.pdf).

Um peer envia um arquivo à rede, e qualquer outro peer o baixa pelo nome ou pelo `ObjectID`. Os Super Peers guardam os metadados e mantêm a overlay de pé, mesmo quando um deles cai.

## O que o sistema faz

| Área | Como funciona |
| --- | --- |
| **Arquivos** | O arquivo é dividido em chunks de até 4 MiB, comprimido com LZ4 e conferido com SHA-256. O download baixa os chunks em paralelo e troca de réplica se uma falhar. |
| **Localização** | DHT **Chord** entre os Super Peers. `ObjectID = SHA-256(arquivo)`, com `lookup`, `join`, `stabilize`, `notify` e `fix_fingers`. |
| **Membership** | **Gossip** com detecção de falha (`ALIVE`, `SUSPECT`, `FAILED`) por heartbeat. |
| **Coordenador** | Eleição **Bully**. O coordenador não é um processo à parte: é um papel assumido por um Super Peer eleito. |
| **Consistência** | Máquina de estados replicada (**SMR**) com log de operações. Cada escrita passa por **2PC** (`PREPARE`, `YES`/`NO`, `COMMIT`/`ABORT`). Um nó que ficou para trás alcança o estado por **IST**, transferindo só o trecho do log que falta. |
| **Cache** | Cache **LFU** nas consultas de metadados, com invalidação quando o registro muda. |
| **Rede** | TCP (POSIX sockets e threads) com framing: cabeçalho de 99 bytes com `TransactionID`, timestamp, tamanho do payload e checksum. |

Não há banco de dados relacional: o estado dos metadados fica em hash tables em memória, replicadas pelo SMR.

## Processos

| Binário | Papel |
| --- | --- |
| `bin/superpeer` | Nó da overlay: membership, Chord, metadados, LFU, eleição, 2PC/SMR/IST. |
| `bin/client` | Peer: upload, download, consulta de estado. Roda um comando e sai (`--cmd`), ou fica aberto em modo sessão (`--interactive`, `--batch`). |

## Começando

Requisitos: Linux x86-64, `gcc`, `make` e as bibliotecas `libssl`/`libcrypto`, `zlib` e `liblz4` (com headers).

```bash
make            # gera bin/superpeer, bin/client e bin/node
make test       # testes unitários, todos devem terminar com "testes ok"
```

### Uma máquina

Os arquivos de `config/` são exemplos de configuração (`ip`, `port`, `type`, `bootstrap`):

```bash
./bin/superpeer config/sp1.conf                       # primeiro Super Peer, sem bootstrap
./bin/superpeer config/sp2.conf                       # segundo, entra pelo primeiro
./bin/client --cmd upload --file documento.pdf        # veja ./bin/client sem argumentos para a lista de comandos
./bin/client --interactive                            # sessão: upload, download, lookup, put, state, ping, join, leave
```

Para ver o roteiro completo de ponta a ponta numa máquina só (três Super Peers, uploads, queda e volta de um nó, IST, cache e download conferido byte a byte):

```bash
tests/flow/run.sh        # roteiro automatizado
tests/flow/watch.sh      # o mesmo, com painéis em tmux
```

### Várias máquinas (cluster)

O harness em [`tests/cluster/`](tests/cluster/README.md) sobe Super Peers e clientes em várias máquinas por ssh a partir de um `cluster.conf`, injeta falhas (`kill -9`), inspeciona os nós e roda os cenários de teste:

```bash
cp tests/cluster/cluster.conf.example tests/cluster/cluster.conf   # ajuste hosts e usuários
tests/cluster/cluster.sh copy-keys && tests/cluster/cluster.sh check
tests/cluster/cluster.sh deploy
tests/cluster/run.sh smoke      # rede, ssh, um upload e um download
tests/cluster/run.sh final      # uma fase por item da especificação, com relatório e métricas de tempo
tests/cluster/run.sh carga      # todos os clientes trabalhando ao mesmo tempo
```

Cada máquina sobe por padrão 3 Super Peers e 30 clientes. O cenário `final` grava `tests/cluster/out/relatorio.md`. Há também comandos de inspeção (`overview`, `chord`, `coordinator`, `state`, `events`…) e uma TUI em `tests/cluster/tui/run.sh`.

### App para o usuário final

[`app/`](app/README.md) é uma interface de terminal que roda um Super Peer e um cliente na máquina do usuário. Ela encontra os outros Super Peers por um servidor de **rendezvous** ([`rendezvous/`](rendezvous/README.md)) e liga as máquinas, mesmo atrás de NAT, por hole punching TCP, sem alterar os Super Peers. Permite enviar arquivos e pastas, baixar com barra de progresso e ver o estado do nó.

```bash
make && app/run.sh
```

O servidor de rendezvous pode ser publicado numa instância EC2 com `rendezvous/deploy.sh up`. O app e o túnel não cifram nem autenticam o tráfego; servem para o contexto do trabalho, não para uso real.

## Estrutura do repositório

```text
.
├── app/                 App de terminal (Textual): um Super Peer + um cliente por máquina
├── rendezvous/          Servidor de rendezvous, rdv.sh, nat-probe.py e deploy na AWS
├── src/                 Implementação em C
│   ├── common/          protocolo, rede, identidade do nó, configuração, compressão
│   ├── peer/            upload, download, chunks, réplica, storage, modo sessão do cliente
│   └── superpeer/       Chord, metadados, SMR (2PC, IST, eleição), LFU, Super Peer
├── include/             Headers públicos, espelhando src/ (documentados com Doxygen)
├── config/              Exemplos de .conf de nós
├── tests/
│   ├── common/ peer/ superpeer/    Testes unitários (make test)
│   ├── c1 … c5/                    Harnesses de cada checkpoint, numa máquina
│   ├── flow/                       Roteiro de ponta a ponta numa máquina
│   └── cluster/                    Harness distribuído por ssh, cenários e TUI
├── scripts/demo/        Script de demo do checkpoint 1
├── third_party/         Bibliotecas externas
└── docs/                Especificação e documentação de cada checkpoint
```

## Documentação

A documentação de cada fase fica em `docs/checkpoint-0N-*/`, com três subpastas: `descriptions/` (requisitos), `plans/` (planos de implementação) e `deliverable/` (material entregue).

| Checkpoint | Foco | Relatório |
| --- | --- | --- |
| 1 | Núcleo de rede: TCP, framing, `JOIN`/`ACK`, bootstrap | [`relatorio-tecnico.md`](docs/checkpoint-01-network/deliverable/relatorio-tecnico.md) |
| 2 | Pipeline de arquivos e metadados: chunks, LZ4, SHA-256, `ObjectID` | planos em `docs/checkpoint-02-files-metadata/plans/` |
| 3 | Membership Gossip e Chord | plano em `docs/checkpoint-03-membership-chord/plans/` |
| 4 | Eleição Bully e SMR | [`relatorio-tecnico.md`](docs/checkpoint-04-election-smr/deliverable/relatorio-tecnico.md) |
| 5 | Cache LFU, 2PC e IST | [`relatorio-tecnico.md`](docs/checkpoint-05-lfu-2pc-ist/deliverable/relatorio-tecnico.md) |
| 6 | Integração, harness distribuído, app com rendezvous e testes finais | [`relatorio-tecnico.md`](docs/checkpoint-06-integration/deliverable/relatorio-tecnico.md) |

O relatório do checkpoint 6 é o melhor ponto de partida para ver o sistema inteiro funcionando: ele mapeia cada item da especificação ao código e ao teste que o demonstra.

## Limitações conhecidas

- O log de operações do SMR fica em memória, com teto de 256 entradas, o que limita o que o IST consegue reconstruir.
- O envio de chunks no upload ainda não tem prazo (timeout).
- O harness de cluster e o app têm no máximo 64 Super Peers por roster.
- O rendezvous e o túnel do app não têm autenticação nem cifra, e NAT simétrico impede o hole punching.
