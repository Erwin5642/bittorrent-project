/*
 * storage.c — armazenamento local de chunks por ObjectID (CP2, Aluno 1).
 * Layout: <root>/<objectid_hex>/chunk_<i>.bin. Guarda bytes crus; a camada
 * não sabe (nem precisa) se o chunk está comprimido.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../../include/peer/storage.h"
#include "../../include/common/node.h"

/* Escreve o ObjectID como 64 caracteres hex minúsculos + NUL. */
static void hex_object_id(const uint8_t object_id[NODE_ID_SIZE], char out[NODE_ID_HEX_SIZE]) {
	static const char digits[] = "0123456789abcdef";
	size_t i;

	for (i = 0; i < NODE_ID_SIZE; i++) {
		out[i * 2] = digits[object_id[i] >> 4];
		out[i * 2 + 1] = digits[object_id[i] & 0x0F];
	}
	out[NODE_ID_SIZE * 2] = '\0';
}

/* mkdir tolerante: sucesso se criou ou se já existia. */
static int mkdir_ok(const char *path) {
	if (mkdir(path, 0777) == 0)
		return 1;
	return errno == EEXIST;
}

int storage_object_dir(char *out, size_t out_cap, const char *root,
                       const uint8_t object_id[NODE_ID_SIZE]) {
	char hex[NODE_ID_HEX_SIZE];
	int n;

	if (!out || !root || !object_id)
		return 0;

	hex_object_id(object_id, hex);
	n = snprintf(out, out_cap, "%s/%s", root, hex);
	if (n < 0 || (size_t)n >= out_cap)
		return 0;
	return 1;
}

int storage_chunk_path(char *out, size_t out_cap, const char *root,
                       const uint8_t object_id[NODE_ID_SIZE], uint32_t index) {
	char hex[NODE_ID_HEX_SIZE];
	int n;

	if (!out || !root || !object_id)
		return 0;

	hex_object_id(object_id, hex);
	n = snprintf(out, out_cap, "%s/%s/chunk_%u.bin", root, hex, index);
	if (n < 0 || (size_t)n >= out_cap)
		return 0;
	return 1;
}

int storage_put_chunk(const char *root, const uint8_t object_id[NODE_ID_SIZE],
                      uint32_t index, const uint8_t *data, size_t len) {
	char dir[STORAGE_PATH_MAX];
	char path[STORAGE_PATH_MAX];
	FILE *fp;

	if (!root || !object_id || (len > 0 && !data))
		return 0;
	if (!storage_object_dir(dir, sizeof dir, root, object_id))
		return 0;
	if (!storage_chunk_path(path, sizeof path, root, object_id, index))
		return 0;

	/* Cria a raiz e o diretório do objeto (um nível cada). */
	if (!mkdir_ok(root) || !mkdir_ok(dir))
		return 0;

	fp = fopen(path, "wb");
	if (!fp)
		return 0;
	if (len > 0 && fwrite(data, 1, len, fp) != len) {
		fclose(fp);
		return 0;
	}
	if (fclose(fp) != 0)
		return 0;
	return 1;
}

int storage_get_chunk(const char *root, const uint8_t object_id[NODE_ID_SIZE],
                      uint32_t index, uint8_t *out, size_t out_cap, size_t *out_len) {
	char path[STORAGE_PATH_MAX];
	FILE *fp;
	long size;
	int rc = 0;

	if (!root || !object_id || !out || !out_len)
		return 0;
	if (!storage_chunk_path(path, sizeof path, root, object_id, index))
		return 0;

	fp = fopen(path, "rb");
	if (!fp)
		return 0;

	if (fseek(fp, 0, SEEK_END) != 0)
		goto done;
	size = ftell(fp);
	if (size < 0 || (size_t)size > out_cap)
		goto done;
	if (fseek(fp, 0, SEEK_SET) != 0)
		goto done;
	if (size > 0 && fread(out, 1, (size_t)size, fp) != (size_t)size)
		goto done;

	*out_len = (size_t)size;
	rc = 1;

done:
	fclose(fp);
	return rc;
}

int storage_has_chunk(const char *root, const uint8_t object_id[NODE_ID_SIZE],
                      uint32_t index) {
	char path[STORAGE_PATH_MAX];

	if (!storage_chunk_path(path, sizeof path, root, object_id, index))
		return 0;
	return access(path, F_OK) == 0;
}

long storage_chunk_size(const char *root, const uint8_t object_id[NODE_ID_SIZE],
                        uint32_t index) {
	char path[STORAGE_PATH_MAX];
	struct stat st;

	if (!storage_chunk_path(path, sizeof path, root, object_id, index))
		return -1;
	if (stat(path, &st) != 0)
		return -1;
	return (long)st.st_size;
}
