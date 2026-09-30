/*
 * download.c — pipeline de download do CP2 (Aluno 1).
 * metadado (índice local) -> chunks do storage -> LZ4 decode -> SHA-256 por
 * chunk -> remonta -> SHA-256(arquivo) == ObjectID -> grava a saída.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/peer/download.h"
#include "../../include/peer/file_pipeline.h"
#include "../../include/peer/storage.h"
#include "../../include/common/compression.h"
#include "../../include/common/node.h"
#include "../../include/common/protocol.h"

/* Grava len bytes de buf em path. Devolve 1/0. */
static int write_file(const char *path, const uint8_t *buf, size_t len) {
	FILE *fp = fopen(path, "wb");
	int ok;

	if (!fp)
		return 0;
	ok = (len == 0) || (fwrite(buf, 1, len, fp) == len);
	return (fclose(fp) == 0) && ok;
}

int download_file(const char *name, const char *output, const char *storage_root) {
	uint8_t meta_buf[METADATA_PAYLOAD_MAX];
	size_t meta_len = 0;
	file_metadata_t meta;
	uint8_t *out_buf = NULL;
	uint8_t *comp = NULL;
	size_t comp_cap;
	uint8_t digest[NODE_ID_SIZE];
	uint32_t i;
	int rc = 0;

	if (!name || !output || !storage_root)
		return 0;

	/* Resolve nome -> metadado (papel do LOOKUP, aqui via índice local). */
	if (!storage_get_meta(storage_root, name, meta_buf, sizeof meta_buf, &meta_len)) {
		fprintf(stderr, "download: metadado de '%s' nao encontrado\n", name);
		return 0;
	}
	if (!metadata_unpack(&meta, meta_buf, meta_len)) {
		fprintf(stderr, "download: metadado de '%s' invalido\n", name);
		return 0;
	}

	/* Arquivo vazio: nada a remontar, apenas confere o ObjectID. */
	if (meta.size == 0) {
		if (sha256(NULL, 0, digest) && memcmp(digest, meta.object_id, NODE_ID_SIZE) == 0 &&
		    write_file(output, NULL, 0)) {
			printf("Download completed\n");
			printf("SHA-256 verified\n");
			rc = 1;
		}
		metadata_release(&meta);
		return rc;
	}

	out_buf = malloc((size_t)meta.size);
	comp_cap = lz4_compress_bound(CHUNK_SIZE);
	if (!out_buf || comp_cap == 0 || !(comp = malloc(comp_cap)))
		goto done;

	for (i = 0; i < meta.chunk_count; i++) {
		size_t off = (size_t)i * CHUNK_SIZE;
		size_t expect = meta.size - off < CHUNK_SIZE ? meta.size - off : CHUNK_SIZE;
		size_t comp_len = 0;
		size_t plain_len = 0;

		if (!storage_get_chunk(storage_root, meta.object_id, i, comp, comp_cap, &comp_len)) {
			fprintf(stderr, "download: chunk %u ausente\n", i);
			goto done;
		}
		if (lz4_decompress(comp, comp_len, out_buf + off, expect, &plain_len) != COMP_OK ||
		    plain_len != expect) {
			fprintf(stderr, "download: falha ao descomprimir chunk %u\n", i);
			goto done;
		}
		/* Hash do chunk sobre os bytes originais (contrato C2). */
		if (!sha256(out_buf + off, plain_len, digest) ||
		    memcmp(digest, meta.chunk_hashes + (size_t)i * NODE_ID_SIZE, NODE_ID_SIZE) != 0) {
			fprintf(stderr, "download: SHA-256 do chunk %u nao confere\n", i);
			goto done;
		}
	}

	/* Verificação final: SHA-256 do arquivo remontado == ObjectID. */
	if (!sha256(out_buf, (size_t)meta.size, digest) ||
	    memcmp(digest, meta.object_id, NODE_ID_SIZE) != 0) {
		fprintf(stderr, "download: SHA-256 do arquivo nao confere com o ObjectID\n");
		goto done;
	}
	if (!write_file(output, out_buf, (size_t)meta.size)) {
		fprintf(stderr, "download: falha ao gravar %s\n", output);
		goto done;
	}

	printf("Download completed\n");
	printf("SHA-256 verified\n");
	fflush(stdout);
	rc = 1;

done:
	free(comp);
	free(out_buf);
	metadata_release(&meta);
	return rc;
}
