#include "common/node.h"
#include "peer/storage.h"
#include "utils/test_utils.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * @file test_storage.c
 * @brief Testes do storage local de chunks: put/get/has/size e caminhos.
 */

/* Remove os artefatos criados sob root para não deixar lixo em /tmp. */
static void cleanup(const char *root, const uint8_t *oid, uint32_t chunks) {
  char path[STORAGE_PATH_MAX];
  char dir[STORAGE_PATH_MAX];
  uint32_t i;

  for (i = 0; i < chunks; i++) {
    if (storage_chunk_path(path, sizeof path, root, oid, i))
      unlink(path);
  }
  if (storage_object_dir(dir, sizeof dir, root, oid))
    rmdir(dir);
  rmdir(root);
}

static void test_paths(void) {
  uint8_t oid[NODE_ID_SIZE];
  char path[STORAGE_PATH_MAX];

  memset(oid, 0, sizeof oid);
  oid[0] = 0xAB; /* primeiro byte -> "ab" no hex */

  expect(storage_chunk_path(path, sizeof path, "storage", oid, 5) == 1,
         "monta caminho do chunk");
  expect(strstr(path, "storage/ab") == path, "prefixo root/hex");
  expect(strstr(path, "/chunk_5.bin") != NULL, "sufixo chunk_5.bin");

  /* Buffer curto -> truncamento detectado. */
  char tiny[8];
  expect(storage_chunk_path(tiny, sizeof tiny, "storage", oid, 5) == 0,
         "rejeita truncamento");
}

static void test_put_get(void) {
  char root[] = "/tmp/bt_store_XXXXXX";
  uint8_t oid[NODE_ID_SIZE];
  const uint8_t c0[] = {1, 2, 3, 4, 5};
  const uint8_t c1[] = {0xFF, 0x00, 0x7F};
  uint8_t back[64];
  size_t back_len = 0;
  size_t i;

  expect(mkdtemp(root) != NULL, "cria root temporario");
  for (i = 0; i < NODE_ID_SIZE; i++)
    oid[i] = (uint8_t)(i + 1);

  expect(storage_put_chunk(root, oid, 0, c0, sizeof c0) == 1, "put chunk 0");
  expect(storage_put_chunk(root, oid, 1, c1, sizeof c1) == 1, "put chunk 1");

  expect(storage_has_chunk(root, oid, 0) == 1, "has chunk 0");
  expect(storage_has_chunk(root, oid, 1) == 1, "has chunk 1");
  expect(storage_has_chunk(root, oid, 2) == 0, "chunk 2 nao existe");

  expect(storage_chunk_size(root, oid, 0) == (long)sizeof c0, "size chunk 0");
  expect(storage_chunk_size(root, oid, 1) == (long)sizeof c1, "size chunk 1");
  expect(storage_chunk_size(root, oid, 2) == -1, "size de inexistente -> -1");

  expect(storage_get_chunk(root, oid, 0, back, sizeof back, &back_len) == 1,
         "get chunk 0");
  expect(back_len == sizeof c0, "len chunk 0");
  expect(memcmp(back, c0, sizeof c0) == 0, "conteudo chunk 0");

  expect(storage_get_chunk(root, oid, 1, back, sizeof back, &back_len) == 1,
         "get chunk 1");
  expect(back_len == sizeof c1, "len chunk 1");
  expect(memcmp(back, c1, sizeof c1) == 0, "conteudo chunk 1");

  cleanup(root, oid, 2);
}

static void test_error_paths(void) {
  char root[] = "/tmp/bt_store_XXXXXX";
  uint8_t oid[NODE_ID_SIZE];
  const uint8_t data[] = {9, 8, 7, 6};
  uint8_t back[64];
  uint8_t tiny[2];
  size_t back_len = 0;

  expect(mkdtemp(root) != NULL, "cria root temporario (erros)");
  memset(oid, 0x55, sizeof oid);

  expect(storage_get_chunk(root, oid, 0, back, sizeof back, &back_len) == 0,
         "get de inexistente -> 0");

  expect(storage_put_chunk(root, oid, 0, data, sizeof data) == 1, "put para teste");
  expect(storage_get_chunk(root, oid, 0, tiny, sizeof tiny, &back_len) == 0,
         "get rejeita out_cap insuficiente");

  expect(storage_put_chunk(NULL, oid, 0, data, sizeof data) == 0, "root NULL -> 0");

  cleanup(root, oid, 1);
}

int main(void) {
  test_paths();
  test_put_get();
  test_error_paths();
  return test_report();
}
