#include "common/compression.h"
#include "common/node.h"
#include "common/protocol.h"
#include "peer/storage.h"
#include "peer/file_pipeline.h"
#include "peer/upload.h"
#include "utils/test_utils.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * @file test_upload.c
 * @brief Testes do pipeline de upload: metadado, storage e round-trip do chunk.
 */

/* Remove os chunks e diretórios criados sob root. */
static void cleanup(const char *root, const uint8_t *oid, uint32_t chunks) {
  char path[STORAGE_PATH_MAX];
  char dir[STORAGE_PATH_MAX];
  uint32_t i;

  for (i = 0; i < chunks; i++)
    if (storage_chunk_path(path, sizeof path, root, oid, i))
      unlink(path);
  if (storage_object_dir(dir, sizeof dir, root, oid))
    rmdir(dir);
  rmdir(root);
}

/* Escreve um arquivo temporário com @p len bytes; devolve o caminho em @p path. */
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

/* Upload de arquivo de 1 chunk: metadado, storage e round-trip conferem. */
static void test_upload_single_chunk(void) {
  char root[] = "/tmp/bt_up_XXXXXX";
  char file[] = "/tmp/bt_upfile_XXXXXX";
  const char *content = "conteudo do arquivo para upload no checkpoint 2";
  size_t len = strlen(content);
  node_id_t owner;
  file_metadata_t meta;
  uint8_t stored[512];
  uint8_t plain[512];
  uint8_t refobj[NODE_ID_SIZE];
  uint8_t refchunk[NODE_ID_SIZE];
  size_t stored_len = 0;
  size_t plain_len = 0;

  expect(mkdtemp(root) != NULL, "cria storage root");
  memset(owner.bytes, 0xA5, sizeof owner.bytes);
  expect(make_temp_file(file, (const uint8_t *)content, len) == 1, "cria arquivo");

  expect(upload_prepare(file, root, &owner, &meta) == 1, "upload_prepare OK");
  expect(meta.size == len, "size == len");
  expect(meta.chunk_count == 1, "1 chunk");
  expect(strstr(meta.filename, "bt_upfile_") != NULL, "filename = basename");
  expect(memcmp(meta.owner.bytes, owner.bytes, NODE_ID_SIZE) == 0, "owner gravado");
  expect(meta.version == 1, "version 1");

  expect(sha256((const uint8_t *)content, len, refobj) == 1, "sha256 arquivo");
  expect(memcmp(meta.object_id, refobj, NODE_ID_SIZE) == 0, "ObjectID confere");

  /* O chunk gravado está comprimido e descomprime para o conteúdo original. */
  expect(storage_get_chunk(root, meta.object_id, 0, stored, sizeof stored,
                           &stored_len) == 1,
         "chunk 0 no storage");
  expect(lz4_decompress(stored, stored_len, plain, sizeof plain, &plain_len) ==
             COMP_OK,
         "chunk descomprime");
  expect(plain_len == len && memcmp(plain, content, len) == 0,
         "chunk descomprimido == original");

  /* O hash do chunk no metadado é do conteúdo original (contrato C2). */
  expect(sha256((const uint8_t *)content, len, refchunk) == 1, "sha256 chunk");
  expect(memcmp(meta.chunk_hashes, refchunk, NODE_ID_SIZE) == 0,
         "hash de chunk sobre bytes originais");

  /* O metadado empacota no layout de STORE. */
  {
    uint8_t wire[METADATA_PAYLOAD_MAX];
    ssize_t n = metadata_pack(&meta, wire, sizeof wire);
    expect(n == (ssize_t)(METADATA_WIRE_PREFIX + NODE_ID_SIZE),
           "metadata_pack: prefixo + 1 hash");
  }

  cleanup(root, meta.object_id, meta.chunk_count);
  metadata_release(&meta);
  unlink(file);
}

/* Upload de arquivo de 2 chunks: último chunk (menor) também é gravado. */
static void test_upload_two_chunks(void) {
  char root[] = "/tmp/bt_up2_XXXXXX";
  char file[] = "/tmp/bt_upfile2_XXXXXX";
  size_t len = (size_t)CHUNK_SIZE + 10;
  uint8_t *data = malloc(len);
  file_metadata_t meta;
  node_id_t owner;
  uint8_t stored[64];
  uint8_t plain[64];
  size_t stored_len = 0;
  size_t plain_len = 0;
  size_t i;

  expect(data != NULL, "aloca dados de 2 chunks");
  if (!data)
    return;
  for (i = 0; i < len; i++)
    data[i] = (uint8_t)(i * 7 + 3);
  memset(owner.bytes, 0x11, sizeof owner.bytes);

  expect(mkdtemp(root) != NULL, "cria storage root (2 chunks)");
  expect(make_temp_file(file, data, len) == 1, "cria arquivo de 2 chunks");

  expect(upload_prepare(file, root, &owner, &meta) == 1, "upload_prepare 2 chunks");
  expect(meta.chunk_count == 2, "2 chunks");
  expect(storage_has_chunk(root, meta.object_id, 0) == 1, "chunk 0 gravado");
  expect(storage_has_chunk(root, meta.object_id, 1) == 1, "chunk 1 gravado");

  /* O chunk 1 (10 bytes originais) descomprime para os 10 bytes finais. */
  expect(storage_get_chunk(root, meta.object_id, 1, stored, sizeof stored,
                           &stored_len) == 1,
         "get chunk 1");
  expect(lz4_decompress(stored, stored_len, plain, sizeof plain, &plain_len) ==
             COMP_OK,
         "chunk 1 descomprime");
  expect(plain_len == 10 && memcmp(plain, data + CHUNK_SIZE, 10) == 0,
         "chunk 1 == cauda do arquivo");

  cleanup(root, meta.object_id, meta.chunk_count);
  metadata_release(&meta);
  unlink(file);
  free(data);
}

static void test_error_paths(void) {
  file_metadata_t meta;
  node_id_t owner;

  memset(owner.bytes, 0, sizeof owner.bytes);
  expect(upload_prepare(NULL, "root", &owner, &meta) == 0, "path NULL -> 0");
  expect(upload_prepare("/tmp/bt_nao_existe_zzz", "root", &owner, &meta) == 0,
         "arquivo inexistente -> 0");
}

int main(void) {
  test_upload_single_chunk();
  test_upload_two_chunks();
  test_error_paths();
  return test_report();
}
