/*
 * download.c — pipeline de download do CP2 (Aluno 1).
 * metadado (índice local) -> chunks do storage -> LZ4 decode -> SHA-256 por
 * chunk -> remonta -> SHA-256(arquivo) == ObjectID -> grava a saída.
 * Os chunks são processados em paralelo (thread pool): cada worker cuida de um
 * subconjunto de índices, escrevendo em regiões disjuntas do buffer de saída.
 */
#include <pthread.h>
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

/** Número de threads na transferência paralela de chunks. */
#define TRANSFER_WORKERS 4

/* Trabalho de um worker: chunks start, start+workers, start+2*workers, ... */
typedef struct {
	const char *root;
	const file_metadata_t *meta;
	uint8_t *out_buf;
	uint32_t start;
	uint32_t workers;
	int ok;
} dl_task_t;

/* Descomprime e verifica os chunks do worker, escrevendo em out_buf por offset. */
static void *dl_worker(void *arg) {
	dl_task_t *t = arg;
	size_t comp_cap = lz4_compress_bound(CHUNK_SIZE);
	uint8_t *comp = comp_cap ? malloc(comp_cap) : NULL;
	uint32_t i;

	if (!comp) {
		t->ok = 0;
		return NULL;
	}
	for (i = t->start; i < t->meta->chunk_count; i += t->workers) {
		size_t off = (size_t)i * CHUNK_SIZE;
		size_t expect = t->meta->size - off < CHUNK_SIZE ? t->meta->size - off : CHUNK_SIZE;
		size_t comp_len = 0;
		size_t plain_len = 0;
		uint8_t digest[NODE_ID_SIZE];

		if (!storage_get_chunk(t->root, t->meta->object_id, i, comp, comp_cap, &comp_len) ||
		    lz4_decompress(comp, comp_len, t->out_buf + off, expect, &plain_len) != COMP_OK ||
		    plain_len != expect ||
		    !sha256(t->out_buf + off, plain_len, digest) ||
		    memcmp(digest, t->meta->chunk_hashes + (size_t)i * NODE_ID_SIZE, NODE_ID_SIZE) != 0) {
			free(comp);
			t->ok = 0;
			return NULL;
		}
	}
	free(comp);
	t->ok = 1;
	return NULL;
}

/* Processa os chunks em paralelo. Devolve 1 se todos os workers tiveram sucesso. */
static int transfer_chunks(const char *root, const file_metadata_t *meta, uint8_t *out_buf) {
	pthread_t th[TRANSFER_WORKERS];
	dl_task_t task[TRANSFER_WORKERS];
	uint32_t nw = meta->chunk_count < TRANSFER_WORKERS ? meta->chunk_count : TRANSFER_WORKERS;
	uint32_t spawned = 0;
	uint32_t w;
	int all_ok = 1;

	for (w = 0; w < nw; w++) {
		task[w] = (dl_task_t){root, meta, out_buf, w, nw, 0};
		if (pthread_create(&th[w], NULL, dl_worker, &task[w]) != 0) {
			all_ok = 0;
			break;
		}
		spawned++;
	}
	for (w = 0; w < spawned; w++) {
		pthread_join(th[w], NULL);
		if (!task[w].ok)
			all_ok = 0;
	}
	return all_ok;
}

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
	uint8_t digest[NODE_ID_SIZE];
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
	if (!out_buf)
		goto done;

	/* Transferência paralela: descomprime + verifica cada chunk. */
	if (!transfer_chunks(storage_root, &meta, out_buf)) {
		fprintf(stderr, "download: falha na transferencia/verificacao dos chunks\n");
		goto done;
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
	free(out_buf);
	metadata_release(&meta);
	return rc;
}
