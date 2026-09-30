#ifndef STORAGE_H
#define STORAGE_H

#include "../common/node.h"

#include <stddef.h>
#include <stdint.h>

/**
 * @file storage.h
 * @brief Armazenamento local de chunks por ObjectID (CP2).
 *
 * Cada objeto vira um diretório `<root>/<objectid_hex>/` com um arquivo por
 * chunk (`chunk_<i>.bin`). A camada é agnóstica ao conteúdo do chunk: guarda e
 * devolve bytes crus, sejam comprimidos (LZ4) ou não — a compressão é etapa
 * separada do pipeline.
 */

/** Tamanho máximo de um caminho montado por este módulo, incluindo o NUL. */
#define STORAGE_PATH_MAX 512

/**
 * @brief Monta o caminho do diretório de um objeto: `<root>/<objectid_hex>`.
 * @param out Buffer de destino; o caller aloca.
 * @param out_cap Capacidade de @p out em bytes.
 * @param root Diretório raiz do storage.
 * @param object_id ObjectID de 32 bytes.
 * @return 1 em sucesso, 0 em argumento inválido ou truncamento.
 */
int storage_object_dir(char *out, size_t out_cap, const char *root,
                       const uint8_t object_id[NODE_ID_SIZE]);

/**
 * @brief Monta o caminho de um chunk: `<root>/<objectid_hex>/chunk_<index>.bin`.
 * @param out Buffer de destino; o caller aloca.
 * @param out_cap Capacidade de @p out em bytes.
 * @param root Diretório raiz do storage.
 * @param object_id ObjectID de 32 bytes.
 * @param index Índice do chunk.
 * @return 1 em sucesso, 0 em argumento inválido ou truncamento.
 */
int storage_chunk_path(char *out, size_t out_cap, const char *root,
                       const uint8_t object_id[NODE_ID_SIZE], uint32_t index);

/**
 * @brief Grava um chunk no storage, criando os diretórios necessários.
 * @param root Diretório raiz do storage.
 * @param object_id ObjectID de 32 bytes.
 * @param index Índice do chunk.
 * @param data Bytes do chunk.
 * @param len Tamanho de @p data em bytes.
 * @return 1 em sucesso, 0 em erro de I/O ou argumento inválido.
 */
int storage_put_chunk(const char *root, const uint8_t object_id[NODE_ID_SIZE],
                      uint32_t index, const uint8_t *data, size_t len);

/**
 * @brief Lê um chunk do storage.
 * @param root Diretório raiz do storage.
 * @param object_id ObjectID de 32 bytes.
 * @param index Índice do chunk.
 * @param out Destino; o caller aloca.
 * @param out_cap Capacidade de @p out em bytes.
 * @param out_len Recebe o número de bytes lidos.
 * @return 1 em sucesso, 0 se não existir, não couber em @p out ou em erro de I/O.
 */
int storage_get_chunk(const char *root, const uint8_t object_id[NODE_ID_SIZE],
                      uint32_t index, uint8_t *out, size_t out_cap, size_t *out_len);

/**
 * @brief Indica se um chunk existe no storage.
 * @return 1 se existir, 0 caso contrário.
 */
int storage_has_chunk(const char *root, const uint8_t object_id[NODE_ID_SIZE],
                      uint32_t index);

/**
 * @brief Tamanho em bytes de um chunk armazenado.
 * @return Tamanho do chunk, ou -1 se não existir/erro.
 */
long storage_chunk_size(const char *root, const uint8_t object_id[NODE_ID_SIZE],
                        uint32_t index);

/**
 * @brief Grava o metadado empacotado indexado por nome: `<root>/index/<name>.meta`.
 *
 * Permite ao `download` resolver `name → metadado` localmente (o mesmo que o
 * `LOOKUP` faria pela rede), já que upload e download rodam na mesma máquina.
 * @param root Diretório raiz do storage.
 * @param name Nome lógico do arquivo; não pode conter '/'.
 * @param data Bytes do metadado (saída de @c metadata_pack).
 * @param len Tamanho de @p data.
 * @return 1 em sucesso, 0 em nome inválido ou erro de I/O.
 */
int storage_put_meta(const char *root, const char *name, const uint8_t *data, size_t len);

/**
 * @brief Lê o metadado empacotado indexado por nome (ver @c storage_put_meta).
 * @param root Diretório raiz do storage.
 * @param name Nome lógico do arquivo.
 * @param out Destino; o caller aloca.
 * @param out_cap Capacidade de @p out.
 * @param out_len Recebe o número de bytes lidos.
 * @return 1 em sucesso, 0 se não existir, não couber ou em erro de I/O.
 */
int storage_get_meta(const char *root, const char *name, uint8_t *out, size_t out_cap,
                     size_t *out_len);

#endif
