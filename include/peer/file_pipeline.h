#ifndef FILE_PIPELINE_H
#define FILE_PIPELINE_H

#include "../common/node.h"

#include <stddef.h>
#include <stdint.h>

/**
 * @file file_pipeline.h
 * @brief Fragmentação de arquivo do CP2: ObjectID e hashes de chunk.
 *
 * Primeira etapa do pipeline de upload (`arquivo → SHA-256 → fragmentação`).
 * Calcula o `ObjectID = SHA-256(arquivo)` e, para cada chunk de tamanho fixo,
 * o SHA-256 dos bytes **originais** (antes de qualquer compressão — contrato C2).
 * A compressão LZ4, o storage e a transferência são etapas posteriores.
 */

/** Tamanho de chunk fixado pela especificação: 4 MB (contrato C4). */
#define CHUNK_SIZE (4u * 1024u * 1024u)

/**
 * @brief Resultado da fragmentação de um arquivo/buffer.
 *
 * @c chunk_hashes é um bloco contíguo de @c chunk_count × @c NODE_ID_SIZE bytes
 * (o SHA-256 de cada chunk, em ordem), alocado por @c fragment_buffer /
 * @c file_fragment e liberado por @c file_fragmentation_release.
 */
typedef struct {
  uint8_t object_id[NODE_ID_SIZE]; /**< SHA-256 do arquivo inteiro. */
  uint64_t size;                   /**< Tamanho original em bytes. */
  uint32_t chunk_count;            /**< ceil(size / CHUNK_SIZE); 0 se vazio. */
  uint8_t *chunk_hashes;           /**< Hashes contíguos, ou NULL se chunk_count 0. */
} file_fragmentation_t;

/**
 * @brief Número de chunks para um arquivo de @p size bytes.
 * @param size Tamanho do arquivo em bytes.
 * @return ceil(size / CHUNK_SIZE); 0 se @p size for 0.
 */
uint32_t file_chunk_count(uint64_t size);

/**
 * @brief Fragmenta um buffer em memória: ObjectID e hash de cada chunk.
 * @param data Bytes do arquivo (pode ser NULL apenas se @p len for 0).
 * @param len Tamanho de @p data em bytes.
 * @param out Recebe o resultado; o caller libera com @c file_fragmentation_release.
 * @return 1 em sucesso, 0 em argumento inválido ou falha de alocação/hash.
 */
int fragment_buffer(const uint8_t *data, size_t len, file_fragmentation_t *out);

/**
 * @brief Lê um arquivo do disco e o fragmenta (ver @c fragment_buffer).
 * @param path Caminho do arquivo.
 * @param out Recebe o resultado; o caller libera com @c file_fragmentation_release.
 * @return 1 em sucesso, 0 se o arquivo não abrir/ler ou em falha de fragmentação.
 */
int file_fragment(const char *path, file_fragmentation_t *out);

/**
 * @brief Libera a cauda de hashes e zera o resultado.
 * @param out Resultado preenchido por @c fragment_buffer / @c file_fragment.
 */
void file_fragmentation_release(file_fragmentation_t *out);

#endif
