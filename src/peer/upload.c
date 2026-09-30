/*
 * upload.c — pipeline de upload do CP2 (Aluno 1).
 * arquivo -> SHA-256 (ObjectID + hash por chunk) -> LZ4 -> storage local
 * -> file_metadata_t (para o peer enviar via STORE). Sem rede aqui.
 * A compressão + gravação dos chunks é paralela (thread pool): cada worker
 * cuida de um subconjunto de índices, gravando arquivos de chunk distintos.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/peer/upload.h"
#include "../../include/peer/file_pipeline.h"
#include "../../include/peer/storage.h"
#include "../../include/common/compression.h"
#include "../../include/common/node.h"
#include "../../include/common/protocol.h"

/** Número de threads na compressão/gravação paralela de chunks. */
#define TRANSFER_WORKERS 4

/* Último componente de um caminho ("dir/sub/a.pdf" -> "a.pdf"). */
static const char *base_name(const char *path) {
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

/* Lê o arquivo inteiro para um buffer alocado; preenche out e len. Devolve 1/0. */
static int read_file(const char *path, uint8_t **out, size_t *len) {
	FILE *fp;
	long size;
	uint8_t *buf = NULL;
	int rc = 0;

	fp = fopen(path, "rb");
	if (!fp)
		return 0;
	if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) < 0 ||
	    fseek(fp, 0, SEEK_SET) != 0)
		goto done;
	if (size > 0) {
		buf = malloc((size_t)size);
		if (!buf || fread(buf, 1, (size_t)size, fp) != (size_t)size)
			goto done;
	}
	*out = buf;
	*len = (size_t)size;
	buf = NULL; /* ownership transferida */
	rc = 1;
done:
	free(buf);
	fclose(fp);
	return rc;
}

/* Trabalho de um worker: chunks start, start+workers, ... */
typedef struct {
	const char *root;
	const uint8_t *data;
	size_t len;
	const uint8_t *object_id;
	uint32_t start;
	uint32_t workers;
	uint32_t chunk_count;
	int ok;
} ul_task_t;

/* Comprime e grava no storage os chunks do worker. */
static void *ul_worker(void *arg) {
	ul_task_t *t = arg;
	size_t comp_cap = lz4_compress_bound(CHUNK_SIZE);
	uint8_t *comp = comp_cap ? malloc(comp_cap) : NULL;
	uint32_t i;

	if (!comp) {
		t->ok = 0;
		return NULL;
	}
	for (i = t->start; i < t->chunk_count; i += t->workers) {
		size_t off = (size_t)i * CHUNK_SIZE;
		size_t clen = t->len - off < CHUNK_SIZE ? t->len - off : CHUNK_SIZE;
		size_t comp_len = 0;

		if (lz4_compress(t->data + off, clen, comp, comp_cap, &comp_len) != COMP_OK ||
		    !storage_put_chunk(t->root, t->object_id, i, comp, comp_len)) {
			free(comp);
			t->ok = 0;
			return NULL;
		}
	}
	free(comp);
	t->ok = 1;
	return NULL;
}

/* Comprime+grava todos os chunks em paralelo. 1 se todos os workers ok. */
static int store_chunks(const char *root, const uint8_t *data, size_t len,
                        const uint8_t *object_id, uint32_t chunk_count) {
	pthread_t th[TRANSFER_WORKERS];
	ul_task_t task[TRANSFER_WORKERS];
	uint32_t nw = chunk_count < TRANSFER_WORKERS ? chunk_count : TRANSFER_WORKERS;
	uint32_t spawned = 0;
	uint32_t w;
	int all_ok = 1;

	for (w = 0; w < nw; w++) {
		task[w] = (ul_task_t){root, data, len, object_id, w, nw, chunk_count, 0};
		if (pthread_create(&th[w], NULL, ul_worker, &task[w]) != 0) {
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

int upload_prepare(const char *path, const char *storage_root,
                   const node_id_t *owner, file_metadata_t *out_meta) {
	uint8_t *buf = NULL;
	size_t len = 0;
	file_fragmentation_t frag;
	int rc = 0;

	if (!path || !storage_root || !owner || !out_meta)
		return 0;

	memset(&frag, 0, sizeof frag);
	if (!read_file(path, &buf, &len))
		return 0;

	/* ObjectID + hash de cada chunk (bytes originais). */
	if (!fragment_buffer(buf, len, &frag))
		goto done;

	/* Comprime e grava os chunks em paralelo. */
	if (frag.chunk_count > 0 &&
	    !store_chunks(storage_root, buf, len, frag.object_id, frag.chunk_count))
		goto done;

	/* Monta o metadado; assume a posse da cauda de hashes da fragmentação. */
	metadata_init(out_meta);
	memcpy(out_meta->object_id, frag.object_id, METADATA_OBJECT_ID_SIZE);
	snprintf(out_meta->filename, METADATA_FILENAME_MAX, "%s", base_name(path));
	out_meta->size = (uint64_t)len;
	out_meta->chunk_count = frag.chunk_count;
	out_meta->version = 1;
	memcpy(out_meta->owner.bytes, owner->bytes, NODE_ID_SIZE);
	out_meta->chunk_hashes = frag.chunk_hashes;
	frag.chunk_hashes = NULL; /* posse transferida para out_meta */

	rc = 1;

done:
	free(buf);
	file_fragmentation_release(&frag); /* no-op se a posse já foi transferida */
	return rc;
}

void upload_print_report(const file_metadata_t *meta) {
	char hex[NODE_ID_HEX_SIZE];
	node_id_t oid;
	uint32_t i;

	if (!meta)
		return;

	memcpy(oid.bytes, meta->object_id, NODE_ID_SIZE);
	node_id_to_hex(&oid, hex, sizeof hex);

	printf("File: %s\n", meta->filename);
	printf("Size: %llu bytes\n", (unsigned long long)meta->size);
	printf("ObjectID: %s\n", hex);
	printf("Chunks: %u\n", meta->chunk_count);
	for (i = 0; i < meta->chunk_count; i++) {
		node_id_t ch;
		memcpy(ch.bytes, meta->chunk_hashes + (size_t)i * NODE_ID_SIZE, NODE_ID_SIZE);
		node_id_to_hex(&ch, hex, sizeof hex);
		printf("Chunk %u: %s\n", i, hex);
	}
	printf("Compression: LZ4\n");
	fflush(stdout);
}
