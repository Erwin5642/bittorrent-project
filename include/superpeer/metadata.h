#ifndef METADATA_H
#define METADATA_H

#include "common/node.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/**
 * @file metadata.h
 * @brief Registro de metadata, layout no fio e hash table por ObjectID.
 *
 * O ObjectID é a chave de 32 bytes que chega no registro. O cálculo
 * SHA-256 do arquivo fica no pipeline do peer. Este módulo guarda o
 * registro e os hashes dos chunks.
 */

/** Tamanho do ObjectID, em bytes. */
#define METADATA_OBJECT_ID_SIZE 32

/** Tamanho do hash de um chunk, em bytes. */
#define METADATA_CHUNK_HASH_SIZE 32

/** Nome lógico no fio, incluindo o NUL. No máximo 255 caracteres úteis. */
#define METADATA_FILENAME_MAX 256

/** Número máximo de registros na tabela. */
#define METADATA_TABLE_MAX 256

/**
 * @brief Teto do registro serializado, em bytes.
 *
 * Distinto do payload de controle (4096). Um registro de metadata cabe
 * abaixo de um chunk de 4 MB.
 */
#define METADATA_PAYLOAD_MAX (64u * 1024u)

/**
 * @brief Prefixo fixo no fio, em bytes.
 *
 * object_id (32) + filename (256) + size (8) + chunk_count (4) +
 * version (4) + owner (32) = 336. A cauda são @c chunk_count hashes.
 */
#define METADATA_WIRE_PREFIX 336

/**
 * @brief Metadata de um arquivo na tabela do Super Peer.
 *
 * Corresponde ao `FileMetadata` do checkpoint. O dono é o NodeID de 32
 * bytes já usado no JOIN. Os hashes formam um bloco contíguo dono da
 * entrada, não um vetor de ponteiros.
 */
typedef struct {
  uint8_t object_id[METADATA_OBJECT_ID_SIZE]; /**< ObjectID, 32 bytes crus. */
  char filename[METADATA_FILENAME_MAX];       /**< Nome lógico, NUL-terminated. */
  uint64_t size;                              /**< Tamanho do arquivo, em bytes. */
  uint32_t chunk_count;                       /**< Quantidade de hashes em @c chunk_hashes. */
  uint32_t version;                           /**< Versão do registro. */
  node_id_t owner;                            /**< NodeID de quem publicou. */
  uint8_t *chunk_hashes;                      /**< @c chunk_count * 32 bytes, ou NULL se zero. */
} file_metadata_t;

/**
 * @brief Slot da hash table. A lista de cada balde é encadeada por @c next.
 */
typedef struct {
  file_metadata_t meta; /**< Cópia do registro. A tabela é dona de @c chunk_hashes. */
  int in_use;           /**< 1 se o slot guarda um registro. */
  int next;             /**< Próximo slot da lista, ou -1. */
} metadata_slot_t;

/**
 * @brief Hash table em memória, encadeada pela chave ObjectID.
 *
 * Capacidade fixa. A busca por nome percorre os slots em uso. Quem chama
 * serializa o acesso; a tabela não tem lock próprio.
 */
typedef struct {
  metadata_slot_t slots[METADATA_TABLE_MAX]; /**< Pool de registros. */
  int buckets[METADATA_TABLE_MAX];           /**< Cabeça de cada lista, ou -1. */
  size_t count;                              /**< Registros em uso. */
} metadata_table_t;

/**
 * @brief Tamanho no fio de um registro com @p chunk_count hashes.
 * @param chunk_count Quantidade de chunks.
 * @return @c METADATA_WIRE_PREFIX + hashes, ou 0 se passar de @c METADATA_PAYLOAD_MAX.
 */
size_t metadata_wire_size(uint32_t chunk_count);

/**
 * @brief Zera um registro. Não libera @c chunk_hashes.
 * @param meta Destino; o caller aloca. NULL é ignorado.
 */
void metadata_init(file_metadata_t *meta);

/**
 * @brief Libera o bloco de hashes e zera o registro.
 * @param meta Registro cuja memória de @c chunk_hashes pertence a quem chama.
 */
void metadata_release(file_metadata_t *meta);

/**
 * @brief Zera a tabela. Não libera hashes; a tabela precisa estar vazia.
 * @param table Destino; o caller aloca.
 */
void metadata_table_init(metadata_table_t *table);

/**
 * @brief Libera os hashes de cada slot e deixa a tabela vazia.
 * @param table Tabela a esvaziar.
 */
void metadata_table_clear(metadata_table_t *table);

/**
 * @brief Insere ou atualiza um registro pela chave ObjectID.
 *
 * Copia o nome, os identificadores e o bloco de hashes. Na primeira
 * inserção, @c version permanece a do registro. Se o ObjectID já existe,
 * os campos são substituídos e @c version vira o valor armazenado mais um.
 * @param table Tabela de destino.
 * @param meta Registro a copiar. @c filename é string não vazia.
 *             @c chunk_hashes é obrigatório quando @c chunk_count é maior que zero.
 * @return 1 em sucesso, 0 se argumento inválido, ObjectID zerado, nome vazio
 *         ou tabela cheia numa inserção nova.
 * @note Tabela cheia não escreve fora do pool e não altera o registro existente.
 */
int metadata_table_put(metadata_table_t *table, const file_metadata_t *meta);

/**
 * @brief Busca um registro pelo ObjectID.
 * @param table Tabela a consultar.
 * @param object_id Chave de 32 bytes.
 * @return Ponteiro para o registro interno, ou NULL se não houver.
 * @note O ponteiro vale até o próximo @c metadata_table_put ou @c metadata_table_clear.
 */
const file_metadata_t *metadata_table_find_id(const metadata_table_t *table,
                                              const uint8_t object_id[METADATA_OBJECT_ID_SIZE]);

/**
 * @brief Busca um registro pelo nome lógico.
 * @param table Tabela a consultar.
 * @param filename Nome NUL-terminated. A comparação é exata.
 * @return Ponteiro para o primeiro registro com esse nome, ou NULL.
 * @note O ponteiro vale até o próximo @c metadata_table_put ou @c metadata_table_clear.
 */
const file_metadata_t *metadata_table_find_name(const metadata_table_t *table, const char *filename);

/**
 * @brief Serializa um registro em big-endian, sem ponteiros.
 *
 * Prefixo de @c METADATA_WIRE_PREFIX bytes e, em seguida, os hashes na
 * ordem dos chunks. Cada inteiro multibyte vai em network byte order.
 * @param meta Registro de origem.
 * @param out Destino; o caller aloca.
 * @param out_cap Capacidade de @p out, em bytes.
 * @return Bytes escritos, ou -1 se o registro ou o buffer forem inválidos.
 */
ssize_t metadata_pack(const file_metadata_t *meta, uint8_t *out, size_t out_cap);

/**
 * @brief Reconstrói um registro a partir do layout de @c metadata_pack.
 *
 * Aloca @c chunk_hashes quando @c chunk_count é maior que zero. O chamador
 * libera com @c metadata_release. ObjectID zerado, nome vazio ou cauda que
 * não fecha com @c chunk_count falham sem deixar memória pendente.
 * @param out Destino; o caller aloca o struct.
 * @param in Bytes do payload, sem o header de mensagem.
 * @param in_len Tamanho de @p in. Tem de ser igual ao tamanho calculado por @c chunk_count.
 * @return 1 em sucesso, 0 se o buffer, o nome ou a cauda forem inválidos.
 */
int metadata_unpack(file_metadata_t *out, const uint8_t *in, size_t in_len);

#endif
