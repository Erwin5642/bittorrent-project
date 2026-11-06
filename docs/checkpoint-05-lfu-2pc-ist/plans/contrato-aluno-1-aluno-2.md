# Contrato de interface — Checkpoint 5 (Aluno 1 ↔ Aluno 2)

Prazo do checkpoint: **30/10/2026**. Fonte: [`docs/specification/trabalho_2026_SD.pdf`](../../specification/trabalho_2026_SD.pdf) (item 10, Checkpoint 5, e as seções de 2PC e IST).

Este documento existe para uma coisa só: registrar os pontos onde o 2PC e o IST
do Aluno 2 encostam no protocolo, no cliente e no cache LFU do Aluno 1. Cada
item abaixo quebra a integração se as duas pontas assumirem coisas diferentes.

O plano de implementação do lado do Aluno 2 está em
[`aluno-2.md`](aluno-2.md). Este documento não descreve *como* implementar,
apenas *o que precisa ficar acordado*. As caixas começam abertas: a decisão
recomendada está no texto, e a caixa fecha quando isso estiver no código ou
no plano do Aluno 1, não quando tiver sido apenas conversado.

## Como usar o checklist

Cada item tem uma ou duas caixas, com o responsável nomeado. Marque quando o
acordo estiver realmente refletido no código ou no documento. Se um item for
renegociado, edite o texto do item e desmarque as caixas.

| Item | Assunto | Situação |
| --- | --- | --- |
| C1 | Fronteira LFU / transferência × 2PC / IST | aberto |
| C2 | YES / NO no fio | aberto |
| C3 | Leitura do corpo em `protocol.c` | aberto |
| C4 | Layout de `PREPARE`, `COMMIT`, `ABORT`, `SNAPSHOT`, `STATE_TRANSFER` | aberto |
| C5 | Quando o `STORE` recebe `ACK` | aberto |
| C6 | Invalidação do LFU depois do apply | aberto |
| C7 | Cliente, `upload` e tokens de log | aberto |
| C8 | Pausa para falha na fase `PREPARE` | aberto |

---

## C1 — Fronteira LFU / transferência × 2PC / IST

O item 10 entrega ao Aluno 1 o LFU (`cache hit`, `cache miss`, `frequency`,
`eviction`) e a transferência de arquivo (`upload` / `download`). Entrega ao
Aluno 2 o 2PC (`PREPARE`, voto, `COMMIT`, `ABORT`) e o IST (`SNAPSHOT`,
`STATE_TRANSFER`).

O Aluno 2 não implementa eviction, hit nem miss. O Aluno 1 não implementa o
pedido de estado nem a rodada de commit. “Transferência” na linha do Aluno 1
é a de arquivo. A transferência de estado é o IST.

- [ ] **Aluno 1** — LFU e transferência de arquivo ficam no plano e no código dele
- [ ] **Aluno 2** — 2PC e IST ficam no plano e no código dele, sem módulo de cache

## C2 — YES / NO no fio

A tabela de mensagens da spec lista `PREPARE`, `COMMIT`, `ABORT`, `ACK` e
`ERROR`. Não lista tipo `YES` nem `NO`. O voto da fase 1 usa os tipos que já
existem, no mesmo socket do `PREPARE`:

- `ACK` significa YES
- `ERROR` significa NO

O stdout do participante imprime a palavra `YES` ou `NO`, para o roteiro de
avaliação. O enum `message_type` não ganha valor. Os números atuais de
`ACK`, `ERROR`, `PREPARE`, `COMMIT`, `ABORT`, `SNAPSHOT` e `STATE_TRANSFER`
permanecem.

- [ ] **Aluno 1** — não acrescenta `YES` nem `NO` ao enum
- [ ] **Aluno 2** — o voto no fio é `ACK` / `ERROR`, e o log imprime `YES` / `NO`

## C3 — Leitura do corpo em `protocol.c`

`src/common/protocol.c` é arquivo do Aluno 1. O Aluno 2 entra como apoio, só
nos cinco tipos deste checkpoint.

Hoje `recv_message` devolve `SNAPSHOT` e `STATE_TRANSFER` sem ler o corpo, e
`PREPARE`, `COMMIT` e `ABORT` têm `payload_sizes` `-1` e caem no `default`.
Os cinco passam a ser lidos por inteiro, com CRC conferido, no mesmo caminho
de `LOG_APPEND`. O retorno antecipado de `SNAPSHOT` e `STATE_TRANSFER` sai.

`PREPARE` e `STATE_TRANSFER` carregam o mesmo corpo de `LOG_APPEND` e podem
passar de `MAX_CONTROL_PAYLOAD_SZ`. Os dois entram em `payload_is_data`, ao
lado de `LOG_APPEND`. `COMMIT` e `ABORT` ficam no teto de controle: 24 bytes
fixos.

- [ ] **Aluno 1** — aceita o apoio do Aluno 2 nesses cinco tipos, sem mudar o número deles no enum
- [ ] **Aluno 2** — lê o corpo inteiro e tira o atalho que devolve `SNAPSHOT` e `STATE_TRANSFER` sem payload

## C4 — Layout dos payloads

Inteiros em big-endian. `PREPARE` e `STATE_TRANSFER` reusam `log_append_pack`
/ `log_append_unpack` de `include/common/protocol.h`. Não há um segundo
formato de entrada de log.

`PREPARE`: os bytes de `log_append_pack`. `log_index` já vem ≥ 1, com
checksum selado pelo coordenador. O participante não recalcula `version` nem
`log_index`.

`COMMIT` e `ABORT`: `transaction_id` (16 bytes) seguido de `log_index`
(`uint64`). Tamanho fixo 24. `payload_sizes` desses dois deixa de ser `-1`.

`SNAPSHOT` pedido (quem sincroniza → quem tem o log):

- `kind` = 1 (`uint8`)
- `last_applied` (`uint64`)

`SNAPSHOT` resposta, no mesmo socket:

- `kind` = 2 (`uint8`)
- `checkpoint_index` (`uint64`)
- `last_applied` do remetente (`uint64`)
- `missing_count` (`uint32`)

Em seguida, ainda no mesmo socket, `missing_count` mensagens
`STATE_TRANSFER`. Cada uma é uma `smr_entry_t`, com `log_index` de
`last_applied + 1` até o fim, em ordem crescente. `missing_count = 0`
encerra a RPC.

- [ ] **Aluno 1** — o framing aceita esses tamanhos; `PREPARE` e `STATE_TRANSFER` contam como payload de dados
- [ ] **Aluno 2** — pack e unpack seguem este layout, sem campo extra

## C5 — Quando o `STORE` recebe `ACK`

`superpeer_handle_store` em `src/superpeer/superpeer.c` já responde `ACK`
quando `smr_propose_store` retorna 1, e `ERROR` com `"replication failed"`
quando retorna 0. Isso permanece.

A partir deste checkpoint, retorno 1 significa que o `COMMIT` foi aplicado na
hash table **deste** processo. O `ACK` não sai no `PREPARE`. O cliente do
Aluno 1 não trata `PREPARE` como publicação feita.

Com um único Super Peer, o caso do C1 e do C2, não há voto remoto. O
coordenador comita local e o `ACK` sai como hoje.

- [ ] **Aluno 1** — o `upload` espera o `ACK` do `STORE` e trata `ERROR` como falha
- [ ] **Aluno 2** — `smr_propose_store` só retorna 1 depois do `COMMIT` local

## C6 — Invalidação do LFU depois do apply

A hash table muda no `COMMIT` e no apply do IST. O Gossip carrega membership,
não metadata. A invalidação do cache não espera `GOSSIP`.

O Aluno 1 acrescenta em `superpeer_t` um ponteiro:

```c
void (*lfu_invalidate)(struct superpeer *sp, const uint8_t object_id[32]);
```

Ele fica `NULL` até o cache existir. O Aluno 1 aponta a função quando o LFU
passar a servir lookup. O Aluno 2 chama o ponteiro, se não for nulo, depois
de cada apply, com `smr_lock` já solto. `NULL` não faz nada.

Sem essa chamada, um hit do LFU devolve o registro anterior ao `COMMIT`.

- [ ] **Aluno 1** — declara o ponteiro, deixa `NULL` enquanto não houver cache, e implementa a função quando o LFU entrar
- [ ] **Aluno 2** — chama `lfu_invalidate` depois do apply de `COMMIT` e do IST

## C7 — Cliente, `upload` e tokens de log

Não há `tests/c5/run.sh`. O roteiro do item 10 usa o `upload` que o cliente
já transforma em `STORE`. Não entra comando novo em `src/peer/peer.c`.

O Super Peer imprime `UPLOAD <nome>` no `COMMIT` que instala o registro.
Estes tokens saem só do código do Aluno 2:

```text
PREPARE
YES
NO
COMMIT
ABORT
IST
versions missing
receive logs
READY
```

`ELECTION` e `COORDINATOR` continuam com o Aluno 1. O Aluno 1 não imprime
`IST`, `PREPARE`, `COMMIT` nem `ABORT` a partir do cache.

- [ ] **Aluno 1** — o `upload` existente publica via `STORE`; o log do LFU não usa os tokens do 2PC e do IST
- [ ] **Aluno 2** — imprime `UPLOAD <nome>` no `COMMIT` e os tokens da lista acima

## C8 — Pausa para falha na fase `PREPARE`

A avaliação força uma falha durante o `PREPARE`. O coordenador lê a variável
de ambiente `BT_PAUSE_AFTER_PREPARE`, em segundos, e dorme esse tempo entre
o último YES e o envio do `COMMIT`. Sem a variável, a pausa é zero. O
`peer` não lê essa variável.

O `upload` hoje não arma timeout de socket. Se o Aluno 1 passar a armar um,
o valor tem de ser maior que a pausa usada na demonstração. Os dois lados
combinam o número antes de o timeout existir.

- [ ] **Aluno 1** — o cliente ignora `BT_PAUSE_AFTER_PREPARE`; um timeout futuro de `upload` fica acima da pausa combinada
- [ ] **Aluno 2** — a pausa existe só no coordenador, entre o último YES e o `COMMIT`
