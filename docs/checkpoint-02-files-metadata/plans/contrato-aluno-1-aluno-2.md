# Contrato de interface — Checkpoint 2 (Aluno 1 ↔ Aluno 2)

Prazo do checkpoint: **30/09/2026**. Fonte: [`docs/specification/trabalho_2026_SD.pdf`](../../specification/trabalho_2026_SD.pdf) (seção 7, Checkpoint 2).

Este documento existe para uma coisa só: registrar os pontos onde o pipeline de
arquivos do Aluno 1 encosta no banco de metadados do Aluno 2. Cada item abaixo
quebra a integração se as duas pontas assumirem coisas diferentes — e quase
sempre quebra **longe** da causa, o que torna o debug caro.

O plano de implementação do lado do Aluno 2 está em
[`aluno-2.md`](aluno-2.md). Este documento não descreve *como* implementar,
apenas *o que foi acordado*.

## Como usar o checklist

Cada item tem uma ou duas caixas, com o responsável nomeado. Marque quando o
acordo estiver realmente refletido no código ou no documento, não quando ele
tiver sido apenas conversado. Se um item for renegociado, edite o texto do item
e desmarque as caixas.

| Item | Assunto | Situação |
| --- | --- | --- |
| C1 | Quem calcula o ObjectID | aberto |
| C2 | Semântica do hash de chunk | aberto |
| C3 | O que é o campo `size` | aberto |
| C4 | Chunk size é do Aluno 1 | aberto |
| C5 | Duas linhas em `protocol.c` | aberto |
| C6 | Layout do payload de `STORE` | aberto |
| C7 | Teto de payload de controle | aberto |
| C8 | Payload e resposta de `LOOKUP` | aberto |
| C9 | Códigos de erro novos | aberto |
| C10 | Divergências do struct do PDF | aberto |

---

## C1 — Quem calcula o ObjectID

`ObjectID = SHA256(arquivo)` é responsabilidade do pipeline do Aluno 1. Para o
Aluno 2 o ObjectID é uma chave opaca de 32 bytes: o Super Peer nunca recalcula o
hash nem valida o conteúdo do arquivo, porque não tem o arquivo.

As duas pontas usam o mesmo wrapper `sha256()` já exposto em
`include/common/node.h`. Ninguém reimplementa SHA-256 e ninguém adiciona uma
segunda dependência de cripto.

- [ ] **Aluno 1** — o `upload` calcula o ObjectID e o envia no payload
- [ ] **Aluno 2** — o Super Peer trata o ObjectID como chave opaca

## C2 — Semântica do hash de chunk

**É o item mais perigoso da lista.** Cada `chunk_hash` tem de ser o SHA-256 do
chunk **original, antes da compressão LZ4**.

Isso decorre do fluxo de download da especificação:

```
Compressed Chunk → LZ4 Decode → SHA-256 → Validação → Entrega
```

O receptor descomprime primeiro e só então compara o hash. Se o emissor hashear
o chunk já comprimido, a comparação falha para todo chunk, e o sintoma aparece
no download — não no upload, onde está a causa. Pior: LZ4 não garante saída
byte-idêntica entre versões ou níveis, então hash de dado comprimido não é
estável nem entre execuções.

- [ ] **Aluno 1** — o hash de chunk é calculado antes do LZ4
- [ ] **Aluno 2** — a semântica está registrada no Doxygen de `file_metadata_t`, em `include/common/protocol.h`

## C3 — O que é o campo `size`

`size` é o tamanho do arquivo **original, não comprimido**, em bytes. A saída de
verificação do PDF imprime `Size: 12458291 bytes` logo depois do nome do arquivo
e antes de mencionar `Compression: LZ4`, o que fixa a interpretação.

O tamanho comprimido não entra no registro de metadata no CP2. Se fizer falta
depois (estatística, cota), entra como campo novo, não reinterpretando este.

- [ ] **Aluno 1** — envia o tamanho original
- [ ] **Aluno 2** — não deriva nada do `size` que pressuponha compressão

## C4 — Chunk size é do Aluno 1

A especificação fixa `Chunk Size = 4 MB`, e a constante pertence ao módulo de
fragmentação do Aluno 1. O Super Peer é **agnóstico** a ela: recebe
`chunk_count` no payload e valida apenas que a cauda de hashes fecha com esse
número.

Em particular, o Aluno 2 **não** valida
`chunk_count == ceil(size / CHUNK_SIZE)`. Essa checagem acoplaria o banco de
metadados à política de fragmentação e obrigaria a mexer no Super Peer se o
tamanho de chunk mudar.

- [ ] **Aluno 1** — a constante de chunk vive no módulo de fragmentação
- [ ] **Aluno 2** — o Super Peer não referencia o tamanho de chunk

## C5 — Duas linhas em `protocol.c`

`src/common/protocol.c` é arquivo do Aluno 1. O pedido é pequeno e é o único
necessário.

`recv_message` **já aceita** `STORE`: como `payload_sizes[STORE]` vale `-1`, a
validação de tamanho fixo não se aplica, e o payload **é copiado** para o buffer
do chamador. A recusa acontece só no fim, porque o `switch` de
`deserialize_message` cai no `default`. Basta acrescentar ali:

```c
case STORE:
case LOOKUP:
    break;                      /* payload variável; o handler desserializa */
```

Com isso `recv_message` devolve `NET_OK`, os bytes crus ficam em
`in_msg_buffer` e o tamanho vem em `pl_size` no header. O Super Peer chama
`metadata_unpack` por conta própria. Nenhuma outra mudança em `protocol.c`.

### Armadilha a evitar

**Não** adicionar `STORE` à lista de exceções de `recv_message` — a condição que
hoje dispensa `DOWNLOAD_REQ`, `DOWNLOAD_REP`, `GOSSIP`, `SNAPSHOT` e
`STATE_TRANSFER` da validação de tamanho. Esse ramo retorna o header **sem nunca
ler o payload do socket**, deixando os bytes na conexão e dessincronizando o
stream.

Isso é um bug já existente, e vai atingir o Aluno 1 quando ele ligar
`DOWNLOAD_REQ`/`DOWNLOAD_REP`, que estão nessa mesma lista. Fica registrado aqui
porque foi descoberto ao desenhar o caminho do `STORE`.

- [ ] **Aluno 1** — os dois `case` entraram em `deserialize_message`
- [ ] **Aluno 1** — ciente do bug do ramo de payload variável, a corrigir antes do download
- [ ] **Aluno 2** — o handler de `STORE` desserializa a partir do buffer cru

## C6 — Layout do payload de `STORE`

O payload de `STORE` é a saída de `metadata_pack`, em
`include/common/protocol.h`: um prefixo de `METADATA_WIRE_PREFIX` (336 bytes)
seguido de `chunk_count` hashes de 32 bytes. As duas pontas usam essa função e
`metadata_unpack`; ninguém escreve um empacotador paralelo.

Ordem do prefixo, tudo big-endian:

| Campo | Tamanho | Notas |
| --- | --- | --- |
| `object_id` | 32 | Chave. Zerado é inválido. |
| `filename` | 256 | Nome lógico, NUL-terminated. Vazio é inválido. |
| `size` | 8 | Tamanho original (ver C3). |
| `chunk_count` | 4 | Define o tamanho da cauda. |
| `version` | 4 | O Super Peer sobrescreve num ObjectID já conhecido. |
| `owner` | 32 | NodeID de quem publicou (ver C10). |

- [ ] **Aluno 2** — `metadata_pack` e `metadata_unpack` implementados e testados
- [ ] **Aluno 1** — o peer monta o payload com `metadata_pack`

## C7 — Teto de payload de controle

`simple_send` e `recv_message` recusam `pl_size` acima de
`MAX_CONTROL_PAYLOAD_SZ` (4096 bytes). Fazendo a conta: sobram
`4096 - 336 = 3760` bytes de cauda, ou 117 chunks, ou um arquivo de cerca de
468 MB com chunks de 4 MB. O PDF de exemplo tem 3 chunks, isto é 432 bytes.

Portanto **nada a fazer no CP2**. Em particular, não mexer em
`METADATA_PAYLOAD_MAX` (64 KB, em `protocol.h`) nem no teto de 4096: é trabalho
para um problema que o checkpoint não tem. As duas constantes ficam no mesmo
header de propósito, para a divergência ficar visível.

- [ ] **Aluno 1** — ciente do teto de 117 chunks para o `STORE`
- [ ] **Aluno 2** — o limite de 4096 permanece intocado no CP2

## C8 — Payload e resposta de `LOOKUP`

Necessário para `./peer download arquivo.pdf` resolver o nome até o registro.
Proposta a fechar:

**Requisição** — payload fixo de 257 bytes:

| Campo | Tamanho | Notas |
| --- | --- | --- |
| `key_kind` | 1 | `0` = busca por ObjectID, `1` = busca por nome. |
| `key` | 256 | Nome NUL-terminated; num ObjectID, os 32 primeiros bytes e o resto zerado. |

**Resposta** — acerto responde `STORE` com o registro empacotado (mesmo layout
de C6); falha responde `ERROR` com código `5`.

Discriminar a chave já no CP2 evita mexer no formato de fio no CP3, onde o teste
do PDF é `lookup <ObjectID>`. Reusar `STORE` como tipo da resposta evita
inventar um tipo fora do enum fixado pela especificação.

- [ ] **Aluno 1** — o `download` envia `LOOKUP` neste formato
- [ ] **Aluno 2** — o Super Peer responde `STORE` no acerto e `ERROR 5` na falha

## C9 — Códigos de erro novos

Somam-se aos códigos `1` a `4` já usados no CP1:

| Código | Significado |
| --- | --- |
| 5 | Objeto não encontrado (`LOOKUP` sem acerto) |
| 6 | Tabela de metadata cheia |

A tabela completa de códigos é compartilhada; quem adicionar um código novo
registra aqui.

- [ ] **Aluno 2** — códigos 5 e 6 emitidos pelo Super Peer
- [ ] **Aluno 1** — o peer distingue os códigos 5 e 6 dos anteriores

## C10 — Divergências do struct do PDF

O struct `FileMetadata` da seção 7 do PDF foi adaptado como `file_metadata_t`,
em `include/common/protocol.h`. Duas diferenças deliberadas, a registrar no
`deliverable/`:

| Campo | No PDF | No repositório | Por quê |
| --- | --- | --- | --- |
| `owner` | `uint32_t` | `node_id_t` (32 bytes) | Um NodeID é um SHA-256; 4 bytes não identificam um nó. Mantém coerência com o `JOIN` e com `src_node` no header. |
| `chunk_hashes` | `uint8_t **` | `uint8_t *` contíguo | Uma alocação em vez de N, e serialização direta sem percorrer ponteiros. |

Os outros campos seguem o PDF em nome, tipo e ordem. Os sete campos do struct de
verificação são os implementados; os demais campos da tabela "Estrutura da
Entrada de Metadados" do documento de arquitetura (`Compression`,
`ReplicaPeers`, `UploadDate`, `LastAccess`, `DownloadCounter`, `Status`) ficam
**fora** do CP2 e não devem ser "completados" por engano.

- [ ] **Aluno 2** — divergências explicadas no `deliverable/` do CP2
- [ ] **Aluno 1** — ciente de que `owner` tem 32 bytes no fio
