/*
 * file_pipeline.c — fragmentação de arquivo do CP2 (Aluno 1).
 * Calcula o ObjectID (SHA-256 do arquivo) e o SHA-256 de cada chunk de 4 MB,
 * sobre os bytes originais, antes de qualquer compressão (contrato C2).
 * Reusa o wrapper sha256() de node.c; não adiciona dependência de cripto.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/peer/file_pipeline.h"
#include "../../include/common/node.h"

uint32_t file_chunk_count(uint64_t size) {
	if (size == 0)
		return 0;
	return (uint32_t)((size + CHUNK_SIZE - 1) / CHUNK_SIZE);
}

int fragment_buffer(const uint8_t *data, size_t len, file_fragmentation_t *out) {
	uint32_t i;
	uint32_t count;

	if (!out)
		return 0;
	if (len > 0 && !data)
		return 0;

	memset(out, 0, sizeof *out);
	out->size = (uint64_t)len;

	/* ObjectID = SHA-256 do arquivo inteiro. */
	if (!sha256(data, len, out->object_id))
		return 0;

	count = file_chunk_count((uint64_t)len);
	out->chunk_count = count;
	if (count == 0)
		return 1; /* arquivo vazio: sem cauda de hashes */

	out->chunk_hashes = malloc((size_t)count * NODE_ID_SIZE);
	if (!out->chunk_hashes) {
		memset(out, 0, sizeof *out);
		return 0;
	}

	/* Hash de cada chunk sobre os bytes originais; o último pode ser menor. */
	for (i = 0; i < count; i++) {
		size_t off = (size_t)i * CHUNK_SIZE;
		size_t clen = len - off < CHUNK_SIZE ? len - off : CHUNK_SIZE;
		if (!sha256(data + off, clen, out->chunk_hashes + (size_t)i * NODE_ID_SIZE)) {
			file_fragmentation_release(out);
			return 0;
		}
	}

	return 1;
}

int file_fragment(const char *path, file_fragmentation_t *out) {
	FILE *fp;
	long size;
	uint8_t *buf = NULL;
	int rc = 0;

	if (!path || !out)
		return 0;

	fp = fopen(path, "rb");
	if (!fp)
		return 0;

	if (fseek(fp, 0, SEEK_END) != 0)
		goto done;
	size = ftell(fp);
	if (size < 0)
		goto done;
	if (fseek(fp, 0, SEEK_SET) != 0)
		goto done;

	if (size > 0) {
		buf = malloc((size_t)size);
		if (!buf)
			goto done;
		if (fread(buf, 1, (size_t)size, fp) != (size_t)size)
			goto done;
	}

	rc = fragment_buffer(buf, (size_t)size, out);

done:
	free(buf);
	fclose(fp);
	return rc;
}

void file_fragmentation_release(file_fragmentation_t *out) {
	if (!out)
		return;
	free(out->chunk_hashes);
	memset(out, 0, sizeof *out);
}
