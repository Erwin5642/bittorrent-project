#ifndef COMPRESSION_H
#define COMPRESSION_H

#include <stddef.h>
#include <stdint.h>

/**
 * @file compression.h
 * @brief Wrapper de compressão LZ4 (bloco único), isolando a dependência da lib.
 *
 * Usado no pipeline de arquivos do CP2: cada chunk é comprimido antes da
 * transferência/armazenamento e descomprimido na recepção. O formato de bloco
 * do LZ4 não guarda o tamanho original; quem descomprime precisa conhecê-lo
 * (no download vem do metadado) e passá-lo como capacidade de saída.
 */

/** Operação concluída com sucesso. */
#define COMP_OK 0
/** Argumento inválido, buffer de saída insuficiente ou falha da lib. */
#define COMP_ERROR -1

/**
 * @brief Limite superior do tamanho comprimido para uma entrada de @p in_len bytes.
 *
 * Útil para dimensionar o buffer de saída de @c lz4_compress.
 * @param in_len Tamanho da entrada em bytes.
 * @return Capacidade mínima segura para a saída, ou 0 se @p in_len for inválido
 *         (0 ou maior que o suportado pela lib).
 */
size_t lz4_compress_bound(size_t in_len);

/**
 * @brief Comprime um buffer com LZ4 (bloco único).
 * @param in Bytes de entrada.
 * @param in_len Tamanho de @p in em bytes (> 0).
 * @param out Destino; o caller aloca ao menos @c lz4_compress_bound(in_len) bytes.
 * @param out_cap Capacidade de @p out em bytes.
 * @param out_len Recebe o número de bytes escritos em @p out.
 * @return @c COMP_OK em sucesso, @c COMP_ERROR em argumento inválido, buffer
 *         curto ou falha da lib.
 */
int lz4_compress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                 size_t *out_len);

/**
 * @brief Descomprime um bloco LZ4 gerado por @c lz4_compress.
 * @param in Bytes comprimidos.
 * @param in_len Tamanho de @p in em bytes (> 0).
 * @param out Destino; o caller aloca ao menos o tamanho original.
 * @param out_cap Capacidade de @p out em bytes (>= tamanho original conhecido).
 * @param out_len Recebe o número de bytes descomprimidos.
 * @return @c COMP_OK em sucesso, @c COMP_ERROR em argumento inválido, buffer
 *         curto ou dados corrompidos.
 */
int lz4_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                   size_t *out_len);

#endif
