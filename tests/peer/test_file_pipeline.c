#include "common/node.h"
#include "peer/file_pipeline.h"
#include "utils/test_utils.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * @file test_file_pipeline.c
 * @brief Testes de fragmentação: chunk_count, ObjectID e hash por chunk.
 */

/* ceil(size / CHUNK_SIZE), com 0 para arquivo vazio. */
static void test_chunk_count(void) {
  expect(file_chunk_count(0) == 0, "0 bytes -> 0 chunks");
  expect(file_chunk_count(1) == 1, "1 byte -> 1 chunk");
  expect(file_chunk_count(CHUNK_SIZE) == 1, "CHUNK_SIZE -> 1 chunk");
  expect(file_chunk_count(CHUNK_SIZE + 1) == 2, "CHUNK_SIZE+1 -> 2 chunks");
  expect(file_chunk_count((uint64_t)CHUNK_SIZE * 3) == 3, "3*CHUNK_SIZE -> 3 chunks");
}

/* Buffer menor que um chunk: 1 chunk, ObjectID e hash do chunk conferem. */
static void test_single_chunk(void) {
  const char *msg = "conteudo pequeno para fragmentar em um unico chunk";
  size_t len = strlen(msg);
  file_fragmentation_t frag;
  uint8_t ref[NODE_ID_SIZE];

  expect(fragment_buffer((const uint8_t *)msg, len, &frag) == 1,
         "fragment_buffer OK");
  expect(frag.size == len, "size == len");
  expect(frag.chunk_count == 1, "1 chunk");

  expect(sha256((const uint8_t *)msg, len, ref) == 1, "sha256 referencia");
  expect(memcmp(frag.object_id, ref, NODE_ID_SIZE) == 0,
         "ObjectID == sha256 do arquivo");
  expect(memcmp(frag.chunk_hashes, ref, NODE_ID_SIZE) == 0,
         "hash do unico chunk == sha256 do arquivo");

  file_fragmentation_release(&frag);
  expect(frag.chunk_hashes == NULL, "release zera chunk_hashes");
}

/* Dois chunks (último menor): offsets e hashes de fronteira conferem. */
static void test_two_chunks(void) {
  size_t len = (size_t)CHUNK_SIZE + 7;
  uint8_t *buf = malloc(len);
  file_fragmentation_t frag;
  uint8_t ref0[NODE_ID_SIZE];
  uint8_t ref1[NODE_ID_SIZE];
  uint8_t refobj[NODE_ID_SIZE];
  size_t i;

  expect(buf != NULL, "aloca buffer de 2 chunks");
  if (!buf)
    return;
  for (i = 0; i < len; i++)
    buf[i] = (uint8_t)(i * 31 + 7);

  expect(fragment_buffer(buf, len, &frag) == 1, "fragment_buffer 2 chunks OK");
  expect(frag.chunk_count == 2, "2 chunks");

  expect(sha256(buf, CHUNK_SIZE, ref0) == 1, "sha256 chunk 0");
  expect(sha256(buf + CHUNK_SIZE, 7, ref1) == 1, "sha256 chunk 1 (menor)");
  expect(sha256(buf, len, refobj) == 1, "sha256 arquivo");

  expect(memcmp(frag.chunk_hashes + 0 * NODE_ID_SIZE, ref0, NODE_ID_SIZE) == 0,
         "hash do chunk 0 confere");
  expect(memcmp(frag.chunk_hashes + 1 * NODE_ID_SIZE, ref1, NODE_ID_SIZE) == 0,
         "hash do chunk 1 (menor) confere");
  expect(memcmp(frag.object_id, refobj, NODE_ID_SIZE) == 0, "ObjectID confere");

  file_fragmentation_release(&frag);
  free(buf);
}

/* Mesmo conteúdo -> mesmo ObjectID; conteúdo alterado -> ObjectID diferente. */
static void test_object_id_stability(void) {
  const char *a = "documento identico";
  const char *b = "documento identicX"; /* 1 byte diferente */
  file_fragmentation_t fa;
  file_fragmentation_t fb;
  file_fragmentation_t fa2;

  expect(fragment_buffer((const uint8_t *)a, strlen(a), &fa) == 1, "frag a");
  expect(fragment_buffer((const uint8_t *)a, strlen(a), &fa2) == 1, "frag a de novo");
  expect(fragment_buffer((const uint8_t *)b, strlen(b), &fb) == 1, "frag b");

  expect(memcmp(fa.object_id, fa2.object_id, NODE_ID_SIZE) == 0,
         "mesmo conteudo -> mesmo ObjectID");
  expect(memcmp(fa.object_id, fb.object_id, NODE_ID_SIZE) != 0,
         "conteudo diferente -> ObjectID diferente");

  file_fragmentation_release(&fa);
  file_fragmentation_release(&fa2);
  file_fragmentation_release(&fb);
}

/* file_fragment lê do disco e produz o mesmo resultado de fragment_buffer. */
static void test_file_fragment(void) {
  char path[] = "/tmp/bt_frag_XXXXXX";
  const char *msg = "arquivo em disco para fragmentar";
  size_t len = strlen(msg);
  int fd;
  file_fragmentation_t frag;
  uint8_t ref[NODE_ID_SIZE];

  fd = mkstemp(path);
  expect(fd >= 0, "cria arquivo temporario");
  if (fd < 0)
    return;
  expect(write(fd, msg, len) == (ssize_t)len, "escreve o arquivo");
  close(fd);

  expect(file_fragment(path, &frag) == 1, "file_fragment OK");
  expect(frag.size == len, "size do arquivo");
  expect(frag.chunk_count == 1, "1 chunk");
  expect(sha256((const uint8_t *)msg, len, ref) == 1, "sha256 referencia");
  expect(memcmp(frag.object_id, ref, NODE_ID_SIZE) == 0, "ObjectID do arquivo");

  file_fragmentation_release(&frag);
  unlink(path);

  expect(file_fragment("/tmp/bt_frag_inexistente_zzz", &frag) == 0,
         "file_fragment falha em arquivo inexistente");
}

/* Argumentos inválidos retornam 0 sem crash. */
static void test_error_paths(void) {
  file_fragmentation_t frag;
  uint8_t byte = 0x42;

  expect(fragment_buffer(&byte, 1, NULL) == 0, "out NULL -> 0");
  expect(fragment_buffer(NULL, 5, &frag) == 0, "data NULL com len>0 -> 0");
  expect(file_fragment(NULL, &frag) == 0, "path NULL -> 0");
}

int main(void) {
  test_chunk_count();
  test_single_chunk();
  test_two_chunks();
  test_object_id_stability();
  test_file_fragment();
  test_error_paths();
  return test_report();
}
