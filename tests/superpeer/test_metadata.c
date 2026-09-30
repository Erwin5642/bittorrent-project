#include "superpeer/metadata.h"
#include "utils/test_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @file test_metadata.c
 * @brief Testes da hash table de metadata, sem socket.
 */

static void fill_id(uint8_t id[METADATA_OBJECT_ID_SIZE], uint8_t seed) {
  size_t i;

  for (i = 0; i < METADATA_OBJECT_ID_SIZE; i++) {
    id[i] = (uint8_t)(seed + i);
  }
}

static file_metadata_t make_meta(uint8_t seed, const char *name, uint32_t version, uint32_t chunks) {
  file_metadata_t meta;
  size_t i;

  metadata_init(&meta);
  fill_id(meta.object_id, seed);
  strncpy(meta.filename, name, METADATA_FILENAME_MAX - 1);
  meta.size = 1000 + seed;
  meta.chunk_count = chunks;
  meta.version = version;
  fill_id(meta.owner.bytes, (uint8_t)(seed + 3));
  if (chunks > 0) {
    meta.chunk_hashes = malloc((size_t)chunks * METADATA_CHUNK_HASH_SIZE);
    for (i = 0; i < (size_t)chunks * METADATA_CHUNK_HASH_SIZE; i++) {
      meta.chunk_hashes[i] = (uint8_t)(seed + i);
    }
  }
  return meta;
}

static void test_put_find_and_hashes(void) {
  metadata_table_t *table = malloc(sizeof *table);
  file_metadata_t meta;
  const file_metadata_t *found;
  uint8_t id[METADATA_OBJECT_ID_SIZE];

  metadata_table_init(table);
  meta = make_meta(1, "arquivo.pdf", 4, 2);
  expect(metadata_table_put(table, &meta) == 1, "insere registro");
  expect(table->count == 1, "count 1");
  fill_id(id, 1);
  found = metadata_table_find_id(table, id);
  expect(found != NULL && found->version == 4, "primeira versao permanece");
  expect(found != NULL && found->size == meta.size, "tamanho copiado");
  expect(found != NULL && found->chunk_hashes != meta.chunk_hashes, "hashes sao uma copia");
  expect(found != NULL && memcmp(found->chunk_hashes, meta.chunk_hashes, 2 * METADATA_CHUNK_HASH_SIZE) == 0,
         "bytes dos hashes batem");
  found = metadata_table_find_name(table, "arquivo.pdf");
  expect(found != NULL && memcmp(found->object_id, id, METADATA_OBJECT_ID_SIZE) == 0, "busca por nome");

  metadata_release(&meta);
  metadata_table_clear(table);
  expect(table->count == 0, "clear esvazia");
  expect(metadata_table_find_name(table, "arquivo.pdf") == NULL, "nome some no clear");
  free(table);
}

static void test_update_increments_version(void) {
  metadata_table_t *table = malloc(sizeof *table);
  file_metadata_t first;
  file_metadata_t second;
  const file_metadata_t *found;
  uint8_t id[METADATA_OBJECT_ID_SIZE];

  metadata_table_init(table);
  first = make_meta(8, "a.pdf", 1, 1);
  second = make_meta(8, "b.pdf", 99, 1);
  second.chunk_hashes[0] = 0xAB;
  expect(metadata_table_put(table, &first) == 1, "primeiro put");
  expect(metadata_table_put(table, &second) == 1, "put da mesma chave");
  expect(table->count == 1, "atualizacao nao duplica");
  fill_id(id, 8);
  found = metadata_table_find_id(table, id);
  expect(found != NULL && found->version == 2, "versao armazenada mais um");
  expect(found != NULL && strcmp(found->filename, "b.pdf") == 0, "nome substituido");
  expect(found != NULL && found->chunk_hashes[0] == 0xAB, "hash novo");
  found = metadata_table_find_name(table, "a.pdf");
  expect(found == NULL, "nome antigo sai");

  metadata_release(&first);
  metadata_release(&second);
  metadata_table_clear(table);
  free(table);
}

static void test_reject_invalid_and_full(void) {
  metadata_table_t *table = malloc(sizeof *table);
  file_metadata_t meta;
  file_metadata_t extra;
  uint8_t id[METADATA_OBJECT_ID_SIZE];
  size_t i;

  metadata_table_init(table);
  meta = make_meta(1, "ok.pdf", 0, 0);
  expect(metadata_table_put(NULL, &meta) == 0, "tabela NULL");
  expect(metadata_table_put(table, NULL) == 0, "registro NULL");
  memset(meta.object_id, 0, METADATA_OBJECT_ID_SIZE);
  expect(metadata_table_put(table, &meta) == 0, "ObjectID zerado");
  fill_id(meta.object_id, 1);
  meta.filename[0] = '\0';
  expect(metadata_table_put(table, &meta) == 0, "nome vazio");
  expect(table->count == 0, "rejeicao nao insere");

  for (i = 0; i < METADATA_TABLE_MAX; i++) {
    file_metadata_t item = make_meta((uint8_t)i, "n.pdf", 0, 0);
    item.object_id[0] = (uint8_t)i;
    item.object_id[1] = (uint8_t)(i >> 8);
    if (i == 0) {
      item.object_id[2] = 1;
    }
    snprintf(item.filename, METADATA_FILENAME_MAX, "f%zu.pdf", i);
    if (!metadata_table_put(table, &item)) {
      expect(0, "enche a tabela");
      metadata_release(&item);
      break;
    }
    metadata_release(&item);
  }
  expect(table->count == METADATA_TABLE_MAX, "tabela no limite");

  extra = make_meta(9, "extra.pdf", 0, 0);
  extra.object_id[0] = 0xFF;
  extra.object_id[1] = 0xFF;
  extra.object_id[2] = 0xFF;
  expect(metadata_table_put(table, &extra) == 0, "insercao nova em tabela cheia falha");
  expect(table->count == METADATA_TABLE_MAX, "count nao cresce");

  fill_id(id, 0);
  id[0] = 0;
  id[1] = 0;
  id[2] = 1;
  meta = make_meta(0, "atualizado.pdf", 7, 0);
  memcpy(meta.object_id, id, METADATA_OBJECT_ID_SIZE);
  expect(metadata_table_put(table, &meta) == 1, "atualizacao em tabela cheia sucede");
  expect(metadata_table_find_id(table, id)->version == 1, "versao incrementada no limite");

  metadata_release(&meta);
  metadata_release(&extra);
  metadata_table_clear(table);
  free(table);
}

static void test_two_names_same_label(void) {
  metadata_table_t *table = malloc(sizeof *table);
  file_metadata_t a;
  file_metadata_t b;
  const file_metadata_t *found;

  metadata_table_init(table);
  a = make_meta(1, "igual.pdf", 0, 0);
  b = make_meta(2, "igual.pdf", 0, 0);
  expect(metadata_table_put(table, &a) == 1, "primeiro nome");
  expect(metadata_table_put(table, &b) == 1, "segundo nome igual");
  found = metadata_table_find_name(table, "igual.pdf");
  expect(found != NULL && memcmp(found->object_id, a.object_id, METADATA_OBJECT_ID_SIZE) == 0,
         "o primeiro slot com o nome prevalece");

  metadata_release(&a);
  metadata_release(&b);
  metadata_table_clear(table);
  free(table);
}

int main(void) {
  test_put_find_and_hashes();
  test_update_increments_version();
  test_reject_invalid_and_full();
  test_two_names_same_label();
  return test_report();
}
