#include "common/compression.h"
#include "common/node.h"
#include "common/protocol.h"
#include "peer/download.h"
#include "peer/file_pipeline.h"
#include "peer/storage.h"
#include "peer/upload.h"
#include "utils/test_utils.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * @file test_download.c
 * @brief Testes do download: round-trip byte-idêntico, corrupção e ausência.
 */

static int make_temp_file(char *path, const uint8_t *data, size_t len) {
  int fd = mkstemp(path);
  if (fd < 0)
    return 0;
  if (len > 0 && write(fd, data, len) != (ssize_t)len) {
    close(fd);
    return 0;
  }
  close(fd);
  return 1;
}

static int read_all(const char *path, uint8_t *out, size_t cap, size_t *len) {
  FILE *fp = fopen(path, "rb");
  size_t n;
  if (!fp)
    return 0;
  n = fread(out, 1, cap, fp);
  *len = n;
  fclose(fp);
  return 1;
}

/* upload_prepare + grava o índice de metadado (como faz o peer no upload). */
static int do_upload(const char *root, const char *file, file_metadata_t *meta) {
  uint8_t wire[METADATA_PAYLOAD_MAX];
  node_id_t owner;
  ssize_t n;

  memset(owner.bytes, 0x33, sizeof owner.bytes);
  if (!upload_prepare(file, root, &owner, meta))
    return 0;
  n = metadata_pack(meta, wire, sizeof wire);
  if (n <= 0)
    return 0;
  return storage_put_meta(root, meta->filename, wire, (size_t)n);
}

static void cleanup(const char *root, const file_metadata_t *meta) {
  char path[STORAGE_PATH_MAX];
  char dir[STORAGE_PATH_MAX];
  uint32_t i;

  for (i = 0; i < meta->chunk_count; i++)
    if (storage_chunk_path(path, sizeof path, root, meta->object_id, i))
      unlink(path);
  if (storage_object_dir(dir, sizeof dir, root, meta->object_id))
    rmdir(dir);
  if (snprintf(path, sizeof path, "%s/index/%s.meta", root, meta->filename) > 0)
    unlink(path);
  if (snprintf(dir, sizeof dir, "%s/index", root) > 0)
    rmdir(dir);
  rmdir(root);
}

/* Round-trip: upload -> download reproduz o arquivo byte a byte. */
static void round_trip(const char *label, const uint8_t *data, size_t len) {
  char root[] = "/tmp/bt_dl_XXXXXX";
  char file[] = "/tmp/bt_dlin_XXXXXX";
  char out[] = "/tmp/bt_dlout_XXXXXX";
  file_metadata_t meta;
  uint8_t *back = malloc(len ? len : 1);
  size_t back_len = 0;
  char msg[128];

  expect(mkdtemp(root) != NULL, "cria storage root");
  expect(make_temp_file(file, data, len) == 1, "cria arquivo de entrada");
  /* out precisa ser um caminho; cria e reusa. */
  { int fd = mkstemp(out); if (fd >= 0) close(fd); }

  expect(do_upload(root, file, &meta) == 1, "upload + index");

  snprintf(msg, sizeof msg, "download OK (%s)", label);
  expect(download_file(meta.filename, out, root) == 1, msg);

  expect(read_all(out, back, len ? len : 1, &back_len) == 1, "le a saida");
  snprintf(msg, sizeof msg, "saida byte-identica (%s)", label);
  expect(back_len == len && (len == 0 || memcmp(back, data, len) == 0), msg);

  cleanup(root, &meta);
  metadata_release(&meta);
  unlink(file);
  unlink(out);
  free(back);
}

static void test_single_chunk(void) {
  const char *s = "arquivo pequeno para o round-trip de download do CP2";
  round_trip("1 chunk", (const uint8_t *)s, strlen(s));
}

static void test_two_chunks(void) {
  size_t len = (size_t)CHUNK_SIZE + 123;
  uint8_t *data = malloc(len);
  size_t i;
  if (!data) {
    expect(0, "aloca dados de 2 chunks");
    return;
  }
  for (i = 0; i < len; i++)
    data[i] = (uint8_t)(i * 13 + 5);
  round_trip("2 chunks", data, len);
  free(data);
}

/* Chunk corrompido no storage é detectado pelo SHA-256. */
static void test_corruption(void) {
  char root[] = "/tmp/bt_dlc_XXXXXX";
  char file[] = "/tmp/bt_dlcin_XXXXXX";
  const char *s = "conteudo que sera corrompido no storage antes do download";
  size_t len = strlen(s);
  file_metadata_t meta;
  char cpath[STORAGE_PATH_MAX];
  FILE *fp;

  expect(mkdtemp(root) != NULL, "cria root (corrupcao)");
  expect(make_temp_file(file, (const uint8_t *)s, len) == 1, "cria arquivo");
  expect(do_upload(root, file, &meta) == 1, "upload + index");

  /* Corrompe o chunk 0 gravado. */
  expect(storage_chunk_path(cpath, sizeof cpath, root, meta.object_id, 0) == 1,
         "caminho do chunk 0");
  fp = fopen(cpath, "r+b");
  expect(fp != NULL, "abre chunk 0");
  if (fp) {
    int c = fgetc(fp);
    fseek(fp, 0, SEEK_SET);
    fputc((c ^ 0xFF) & 0xFF, fp);
    fclose(fp);
  }

  expect(download_file(meta.filename, "/tmp/bt_dlc_out.bin", root) == 0,
         "download detecta corrupcao e falha");
  unlink("/tmp/bt_dlc_out.bin");

  cleanup(root, &meta);
  metadata_release(&meta);
  unlink(file);
}

static void test_missing(void) {
  char root[] = "/tmp/bt_dlm_XXXXXX";
  expect(mkdtemp(root) != NULL, "cria root (ausente)");
  expect(download_file("inexistente.bin", "/tmp/bt_dlm_out.bin", root) == 0,
         "download de nome inexistente falha");
  rmdir(root);
}

int main(void) {
  test_single_chunk();
  test_two_chunks();
  test_corruption();
  test_missing();
  return test_report();
}
