/*
 * compression.c — wrapper de compressão LZ4 (bloco único).
 * Isola a dependência da liblz4 do resto do código, como node.c faz com o
 * OpenSSL. O formato de bloco do LZ4 não guarda o tamanho original; o
 * descompressor recebe a capacidade de saída de quem chama.
 */
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include <lz4.h>

#include "../../include/common/compression.h"

/* A API do LZ4 usa int para tamanhos; recusa entradas que não caibam. */
size_t lz4_compress_bound(size_t in_len) {
	if (in_len == 0 || in_len > (size_t)LZ4_MAX_INPUT_SIZE)
		return 0;
	return (size_t)LZ4_compressBound((int)in_len);
}

int lz4_compress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                 size_t *out_len) {
	int written;

	if (!in || !out || !out_len || in_len == 0)
		return COMP_ERROR;
	if (in_len > (size_t)LZ4_MAX_INPUT_SIZE || out_cap > (size_t)INT_MAX)
		return COMP_ERROR;

	written = LZ4_compress_default((const char *)in, (char *)out, (int)in_len,
	                               (int)out_cap);
	if (written <= 0) /* 0 = não coube em out_cap */
		return COMP_ERROR;

	*out_len = (size_t)written;
	return COMP_OK;
}

int lz4_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                   size_t *out_len) {
	int produced;

	if (!in || !out || !out_len || in_len == 0)
		return COMP_ERROR;
	if (in_len > (size_t)INT_MAX || out_cap > (size_t)INT_MAX)
		return COMP_ERROR;

	produced = LZ4_decompress_safe((const char *)in, (char *)out, (int)in_len,
	                               (int)out_cap);
	if (produced < 0) /* dados corrompidos ou out_cap insuficiente */
		return COMP_ERROR;

	*out_len = (size_t)produced;
	return COMP_OK;
}
