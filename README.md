# Trabalho de BitTorrent — Programação Distribuída 2026

Sistema P2P híbrido de compartilhamento de documentos para a disciplina de Programação Distribuída da UEMS (Ciência da Computação). A especificação do trabalho está em [`docs/specification/trabalho_2026_SD.pdf`](docs/specification/trabalho_2026_SD.pdf). Prazo final: **06/11/2026**.

Peers compartilham documentos PDF por uma overlay de Super Peers. A localização usa uma DHT Chord (`ObjectID = SHA-256(arquivo)`). O membership usa Gossip. A metadata se mantém consistente com eleição Bully, replicação de máquina de estados, two-phase commit e transferência incremental de estado.

## Restrições

- Linguagem: C em Linux
- POSIX Sockets e POSIX Threads
- Sem banco de dados relacional
- Mensagens TCP com framing, um header padrão, `TransactionID`, timestamp, tamanho do payload e checksum
- PDFs divididos em chunks de 4 MB, comprimidos com LZ4, hasheados com SHA-256, transferidos em paralelo
- Cache LFU para lookups
- O Coordinator **não** é um terceiro processo: é um papel assumido por um Super Peer eleito (Bully, maior `NodeID` vence)

## Processos

| Processo | Papel |
| --- | --- |
| `peer` | Cliente: CLI, upload/download, armazenamento local, chunking, LZ4, SHA-256 |
| `superpeer` | Nó da overlay: membership, metadata, Chord, Gossip, LFU, eleição, replicação |
| Coordinator | Papel lógico assumido por um Super Peer após eleição Bully (2PC, SMR, IST) |

Módulos compartilhados (`protocol`, `network`, `node`) pertencem aos dois processos.

## Checkpoints

| Checkpoint | Prazo | Foco |
| --- | --- | --- |
| 1 | 18/09 | Núcleo de rede: TCP, framing, `JOIN`/`ACK`, identidade do nó, bootstrap do Super Peer |
| 2 | 30/09 | Pipeline de arquivos e metadata: chunks, LZ4, SHA-256, hash table, `ObjectID` |
| 3 | 09/10 | Membership Gossip e Chord (`lookup`, `join`, `stabilize`, `notify`, `fix_fingers`) |
| 4 | 21/10 | Eleição Bully e log de operações replicável (SMR) |
| 5 | 30/10 | Cache LFU, 2PC, transferência incremental de estado |
| 6 | 06/11 | Integração, demos, testes e documentação final |

O grupo só avança quando o checkpoint atual está funcionando. Cada checkpoint também exige um deliverable escrito.

## Estrutura do repositório

```
.
├── config/                 Arquivos .conf dos nós (IP, porta, tipo, Super Peers de bootstrap)
├── data/                   Saída em tempo de execução (não é código-fonte)
│   ├── logs/               Logs de operação / persistência do SMR
│   └── storage/            Chunks locais após upload
├── docs/
│   ├── specification/      PDF da especificação do trabalho
│   └── checkpoint-0N-*/    Documentação de cada fase
│       ├── deliverable/    Documento exigido ao final do checkpoint
│       ├── descriptions/   Notas de módulos e protocolo
│       └── plans/          Planos de implementação
├── include/                Headers públicos, espelhando src/
│   ├── common/             protocol.h, network.h, node.h
│   ├── peer/               peer, storage, chunk, compression, sha256
│   └── superpeer/          metadata, hashtable, chord, gossip, cache, election, replication
├── scripts/demo/           Scripts reprodutíveis de demo do Super Peer / Peer
├── src/                    Implementação (.c), mesma divisão de include/
├── tests/                  Testes unitários e de integração
│   ├── common/
│   ├── peer/
│   ├── superpeer/
│   └── integration/
└── third_party/            Bibliotecas de terceiros (LZ4, SHA-256), não é código do projeto
```

Diretórios vazios mantêm um `.gitkeep` para o Git rastreá-los.

### `docs/`

Uma pasta por checkpoint. Coloque o material a ser avaliado em `deliverable/`, notas de pesquisa e design em `descriptions/`, e o plano de implementação em `plans/`.

### `src/` e `include/`

Agrupados pela divisão de processos definida na especificação:

- **common** — `protocol`, `network`, `node` (identidade, sockets, framing)
- **peer** — CLI, armazenamento local, chunking, compressão, hashing
- **superpeer** — hash table de metadata, Chord, Gossip, cache, eleição, replicação

Os headers ficam em `include/`; as implementações ficam em `src/`. A lógica do Coordinator fica dentro do Super Peer (`election`, `replication`).

### `config/`, `data/`, `third_party/`

- **config** — configurações de inicialização para não deixar endereços fixos no código (`sp1.conf`, `peer1.conf`, …)
- **data** — arquivos escritos pelos processos em execução (chunks, logs). Trate como descartável; não commite conteúdo real
- **third_party** — bibliotecas C externas permitidas pela especificação (LZ4, SHA-256). O código do projeto fica em `src/`

### `tests/` e `scripts/`

Os testes seguem a mesma divisão common / peer / superpeer, mais `tests/integration/` para cenários multi-processo (lookup do Chord, `kill -9`, eleição, IST). Os scripts de demo do checkpoint 6 ficam em `scripts/demo/`.
