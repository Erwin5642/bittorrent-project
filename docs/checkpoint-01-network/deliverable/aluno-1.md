# Entrega — Checkpoint 1 (Aluno 1)

Checkpoint: **01 — Network**. Prazo: **18/09/2026**. Papel: Aluno 1. Fonte de requisitos: [`docs/specification/trabalho_2026_SD.pdf`](../../specification/trabalho_2026_SD.pdf) (seção 6, Checkpoint 1). Plano correspondente: [`plans/aluno-1.md`](../plans/aluno-1.md).

Este documento é o relatório final da parte do Aluno 1 no CP1: o que foi entregue, como compilar e executar, e como cada item de verificação da spec é atendido e demonstrado.

## Resumo do que foi entregue

O Aluno 1 é responsável pela **camada de rede e pelo formato de mensagens**: `network.c`, `protocol.c` e `peer.c`. Estão implementados e integrados:

- sockets TCP IPv4 (`socket`/`bind`/`listen`/`accept`/`connect`) e I/O de stream confiável (`send_all`/`recv_all`);
- header padrão de tamanho fixo (99 bytes, big-endian), com serialização explícita;
- framing de mensagem completa (header + payload) no envio e na recepção;
- checksum CRC32 do payload, validado na recepção;
- serialização dos payloads de controle `JOIN`, `ACK`, `ERROR`, `LEAVE` (e `PING`/`PONG` sem payload);
- um processo cliente `peer` que conecta a um Super Peer, envia uma mensagem de controle e lê a resposta;
- testes automatizados de framing em `tests/c1/`.

## Componentes implementados

### Camada de rede — `src/common/network.c` / `include/common/network.h`

API mínima de sockets, consumida por `peer` e `superpeer` (nenhum outro módulo chama `socket`/`bind`/... direto):

| Função | Papel |
| --- | --- |
| `net_listen(port)` | Socket TCP passivo: `SO_REUSEADDR`, `bind` em `INADDR_ANY`, `listen` backlog 128. |
| `net_connect(host, port)` | Cliente IPv4 (`inet_pton` + `connect`). |
| `net_accept(listen_fd, out_addr)` | Aceita conexão e devolve o endereço do remoto. |
| `net_close(fd)` | Fecha o socket. |
| `send_all(fd, buf, n)` | Envia `n` bytes por completo, tratando `send` parcial. |
| `recv_all(fd, buf, n)` | Recebe exatamente `n` bytes; distingue EOF (`NET_CLOSED`) de erro (`NET_ERROR`). |

### Header, framing e checksum — `src/common/protocol.c` / `include/common/protocol.h`

**Header** (`HEADER_SIZE = 99` bytes, big-endian):

| Offset | Tamanho | Campo |
| --- | --- | --- |
| 0 | 1 | `protocol_ver` (`PROTOCOL_VER = 1`) |
| 1 | 2 | `msg_type` |
| 3 | 32 | `src_node` (NodeID) |
| 35 | 32 | `dst_node` (NodeID) |
| 67 | 16 | `trsc_id` (TransactionID) |
| 83 | 8 | `time` |
| 91 | 4 | `pl_size` |
| 95 | 4 | `checksum` (CRC32 do payload) |

**Framing:** `send_message` empacota o payload conforme o tipo, calcula o CRC32 do payload, serializa o header (`pack_header`) e faz um único `send_all` de `HEADER_SIZE + pl_size` bytes. `recv_message` lê o header, valida `protocol_ver`/`msg_type`/`pl_size`, lê o payload, **recomputa e confere o CRC32** e desserializa para o struct do tipo. As variantes `simple_send`/`simple_recv` transportam um payload de bytes arbitrário com o mesmo header e CRC.

**Payloads de controle** (layout fixo, sem padding): `JOIN` (39 B), `ACK` (32 B), `ERROR` (68 B), `LEAVE` (32 B), `PING`/`PONG` (0 B). Helpers de resposta: `fill_reply_header`, `send_ack`, `send_error`, `send_join`, `send_leave`.

### Cliente P2P — `src/peer/peer.c`

Processo CLI que valida a ponta cliente do protocolo: carrega a config, gera o `NodeID`, conecta ao Super Peer, envia `ping`/`join`/`leave` e imprime `TX`/`RX` do tipo enviado/recebido.

## Como compilar

Na raiz do repositório:

```sh
make
```

Gera os binários em `bin/`: `bin/client` (o `peer`), `bin/superpeer` e `bin/node`. Dependências de sistema: `libcrypto` (OpenSSL, para SHA-256) e `libz` (zlib, para CRC32).

## Como executar (demonstração)

Dois processos conversando por TCP, em terminais separados:

1. **Super Peer** (entrega do Aluno 2) escutando numa porta, ex.:

   ```sh
   ./bin/superpeer config/sp1.conf
   ```

2. **Peer** (Aluno 1) enviando uma mensagem de controle ao Super Peer:

   ```sh
   ./bin/client --cmd join --host 127.0.0.1 --port <porta_do_superpeer>
   ./bin/client --cmd ping --host 127.0.0.1 --port <porta_do_superpeer>
   ./bin/client --cmd leave --host 127.0.0.1 --port <porta_do_superpeer>
   ```

Saída esperada no `peer`: as linhas `TX <TIPO>` (mensagem enviada) e `RX <TIPO>` (resposta lida). No `join` bem-sucedido, o Super Peer registra o membro e responde `ACK`; em payload inválido/tabela cheia, responde `ERROR`.

> Observação de execução: `peer_init` carrega `config/peer.conf`. Garanta que esse arquivo exista (há `config/peer1.conf` como referência de formato). Ver "Limitações conhecidas".

## Itens de verificação do CP1

A spec (seção 6) lista os itens de verificação abaixo; segue como cada um é atendido pela parte do Aluno 1:

| Item da spec | Como é atendido | Como demonstrar |
| --- | --- | --- |
| Conexão TCP funcionando | `net_listen`/`net_accept` no servidor, `net_connect` no cliente | `client` conecta ao `superpeer` sem erro |
| Mensagem chega corretamente | `send_message`/`recv_message` com `send_all`/`recv_all` | `TX JOIN` no peer → membro registrado no Super Peer |
| Header é interpretado | `pack_header`/`unpack_header` (99 B, big-endian) | teste `test_pack_unpack_header` reconstrói todos os campos |
| Checksum é validado | CRC32 do payload preenchido no envio e conferido no `recv` | CRC adulterado → `recv_message` retorna `NET_ERROR`, mensagem não é aceita |
| Dois processos podem conversar | `peer` ↔ `superpeer` por TCP | `client --cmd join` recebe `RX ACK` |
| Não pode ocorrer segmentation fault | payload inválido, tipo não suportado, tabela cheia e `NET_CLOSED` são tratados sem crash | processo segue após entradas inválidas/desconexão |

## Testes automatizados

Suíte de framing do CP1 em `tests/c1/`:

```sh
cd tests/c1
make
./test_protocol
```

`test_protocol.c` cobre, via asserções `expect(...)` (imprimem PASS/FAIL e o processo retorna código de erro se algo falhar):

- `HEADER_SIZE == 99` e `PROTOCOL_VER == 1`;
- valores dos tipos de controle (`JOIN=0`, `PING=1`, `PONG=2`, `LEAVE=3`);
- round-trip `pack_header`/`unpack_header` (byte 0 = versão, bytes 1–2 = `msg_type` em network order, e `src_node`/`dst_node`/`trsc_id`/`time`/`pl_size` reconstruídos);
- tamanhos no fio dos payloads (`JOIN` 39 B, `ACK` 32 B, `ERROR` 68 B).

### Evidência de execução

Saída real da suíte (compilação + execução em 18/09/2026):

```
$ cd tests/c1 && make
gcc -Wall -Wextra -pedantic -I../../include -I../../tests test_protocol.c \
    ../../src/common/protocol.c ../../src/common/network.c \
    ../../tests/utils/test_utils.c -o test_protocol -lz

$ ./test_protocol
20 testes ok
$ echo $?
0
```

As 20 asserções passam e o processo retorna `0`. A compilação emite um único *warning* pré-existente de `-Wsign-compare` em `recv_message` (comparação `int32_t` × `uint32_t` na validação de `pl_size`), que não afeta os testes; está listado em "Limitações conhecidas".

Há ainda testes dos módulos do Aluno 2 em `tests/common/` (`test_config.c`, `test_node.c`) e `tests/superpeer/`, que exercitam a integração ponta a ponta com esta camada de rede.

## Limitações conhecidas

- **Config do peer com caminho fixo:** `peer_init` lê `config/peer.conf` independentemente dos argumentos `--host`/`--port` da CLI; esses argumentos governam apenas o destino da conexão. Alinhar config e CLI fica para um refino futuro.
- **Payload de tamanho variável não implementado:** os tipos `DOWNLOAD_REP`, `GOSSIP`, `SNAPSHOT`, `STATE_TRANSFER` já existem no enum e são excluídos da validação de tamanho fixo em `recv_message`, mas seu framing será entregue a partir do CP2.
- **Marcadores `TEMP`/`TODO`:** há anotações remanescentes em `protocol.c`/`protocol.h` (nomes de tipo e logging de erro) para limpeza futura; não afetam os itens de verificação do CP1.
- **Warning de `-Wsign-compare`:** `recv_message` compara `payload_sizes[message_type]` (`int32_t`) com `pl_size` (`uint32_t`); a compilação emite um *warning*, mas o comportamento é correto para os tamanhos usados no CP1. Corrigir o tipo/cast é um refino futuro.
- **`bind` em `INADDR_ANY`:** o Super Peer escuta em todas as interfaces; o `ip` do `.conf` é usado apenas para o `NodeID` e o anúncio de endereço — adequado ao CP1 (localhost/LAN).

## Arquivos entregues (Aluno 1)

| Papel | Header | Fonte |
| --- | --- | --- |
| Sockets / I/O de stream | `include/common/network.h` | `src/common/network.c` |
| Header, framing, payloads, CRC32 | `include/common/protocol.h` | `src/common/protocol.c` |
| Cliente P2P | — | `src/peer/peer.c` |
| Testes de framing CP1 | — | `tests/c1/test_protocol.c` |
