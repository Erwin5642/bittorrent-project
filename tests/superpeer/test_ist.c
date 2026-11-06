#include "superpeer/smr.h"
#include "utils/test_utils.h"

#include <stdlib.h>
#include <string.h>

/**
 * @file test_ist.c
 * @brief Delta do IST, sem socket.
 */

static void fill_id(uint8_t id[METADATA_OBJECT_ID_SIZE], uint8_t seed) {
  size_t i;

  for (i = 0; i < METADATA_OBJECT_ID_SIZE; i++) {
    id[i] = (uint8_t)(seed + i);
  }
}

static void set_name(char dst[METADATA_FILENAME_MAX], const char *src) {
  size_t n = strlen(src);

  memset(dst, 0, METADATA_FILENAME_MAX);
  if (n >= METADATA_FILENAME_MAX) {
    n = METADATA_FILENAME_MAX - 1;
  }
  memcpy(dst, src, n);
}

static int seal_put(smr_entry_t *entry, uint64_t index, const char *name) {
  uint8_t id[METADATA_OBJECT_ID_SIZE];

  fill_id(id, (uint8_t)index);
  memset(entry, 0, sizeof *entry);
  entry->operation = SMR_OP_PUT;
  entry->log_index = index;
  entry->timestamp = index;
  entry->version = 1;
  set_name(entry->name, name);
  memcpy(entry->object_id, id, METADATA_OBJECT_ID_SIZE);
  return smr_entry_seal_checksum(entry);
}

static void apply_named(smr_log_t *log, metadata_table_t *table, uint64_t index, const char *name) {
  smr_entry_t entry;

  expect(seal_put(&entry, index, name) == 1, "sela");
  expect(smr_log_apply(log, table, &entry) == 1, "aplica");
}

/**
 * @brief Pedinte em 3 recebe 4 e 5. Pedinte em 0 recebe o log. Pedinte em dia recebe zero.
 */
static void test_delta_bounds(void) {
  smr_log_t *log = calloc(1, sizeof *log);
  metadata_table_t *table = malloc(sizeof *table);
  smr_ist_offer_t offer;
  const smr_entry_t *missing[8];
  const char *names[] = {"A", "B", "C", "D", "E"};
  uint64_t i;

  metadata_table_init(table);
  for (i = 0; i < 5; i++) {
    apply_named(log, table, i + 1, names[i]);
  }
  log->checkpoint_index = 3;
  expect(log->last_applied == 5, "lider em 5");

  expect(smr_ist_offer(log, 3, &offer, missing, 8) == 1, "oferta do 3");
  expect(offer.missing_count == 2 && offer.checkpoint_index == 3 && offer.source_last_applied == 5,
         "faltam 4 e 5");
  expect(missing[0]->log_index == 4 && missing[1]->log_index == 5, "ordem");
  expect(strcmp(missing[0]->name, "D") == 0 && strcmp(missing[1]->name, "E") == 0, "nomes");

  expect(smr_ist_offer(log, 0, &offer, missing, 8) == 1, "oferta do zero");
  expect(offer.missing_count == 5 && missing[0]->log_index == 1 && missing[4]->log_index == 5,
         "log inteiro");

  expect(smr_ist_offer(log, 5, &offer, missing, 8) == 1, "oferta em dia");
  expect(offer.missing_count == 0, "nada a transferir");
  expect(smr_ist_offer(NULL, 0, &offer, missing, 8) == 0, "log nulo");
  expect(smr_ist_offer(log, 0, NULL, missing, 8) == 0, "oferta nula");

  smr_log_shutdown(log);
  metadata_table_clear(table);
  free(log);
  free(table);
}

/**
 * @brief O slot PREPARED não entra no delta. Aplicar o delta em ordem replica a version.
 */
static void test_delta_apply_and_ignores_prepared(void) {
  smr_log_t *source = calloc(1, sizeof *source);
  smr_log_t *dest = calloc(1, sizeof *dest);
  metadata_table_t *source_table = malloc(sizeof *source_table);
  metadata_table_t *dest_table = malloc(sizeof *dest_table);
  smr_entry_t extra;
  smr_ist_offer_t offer;
  const smr_entry_t *missing[8];
  const file_metadata_t *found;
  size_t i;

  metadata_table_init(source_table);
  metadata_table_init(dest_table);
  apply_named(source, source_table, 1, "A");
  apply_named(source, source_table, 2, "B");
  expect(seal_put(&extra, 3, "C") == 1, "sela C");
  expect(smr_prepare_vote(source, &extra) == 1, "C fica so no slot");
  expect(smr_ist_offer(source, 0, &offer, missing, 8) == 1, "oferta");
  expect(offer.missing_count == 2, "PREPARED nao entra");

  for (i = 0; i < offer.missing_count; i++) {
    expect(smr_log_apply(dest, dest_table, missing[i]) == 1, "aplica delta");
  }
  expect(dest->last_applied == 2, "destino alcanca o commit");
  found = metadata_table_find_name(dest_table, "B");
  expect(found != NULL && found->version == 1, "version da entrada");
  expect(metadata_table_find_name(dest_table, "C") == NULL, "C nao foi transferido");

  smr_log_shutdown(source);
  smr_log_shutdown(dest);
  metadata_table_clear(source_table);
  metadata_table_clear(dest_table);
  free(source);
  free(dest);
  free(source_table);
  free(dest_table);
}

/**
 * @brief Buraco não avança last_applied. Cap menor que o delta não estoura o array.
 */
static void test_gap_and_short_cap(void) {
  smr_log_t *log = calloc(1, sizeof *log);
  metadata_table_t *table = malloc(sizeof *table);
  smr_entry_t hole;
  smr_ist_offer_t offer;
  const smr_entry_t *only[1];
  uint8_t id[METADATA_OBJECT_ID_SIZE];

  metadata_table_init(table);
  fill_id(id, 2);
  memset(&hole, 0, sizeof hole);
  hole.operation = SMR_OP_PUT;
  hole.log_index = 2;
  hole.timestamp = 2;
  hole.version = 1;
  set_name(hole.name, "gap");
  memcpy(hole.object_id, id, METADATA_OBJECT_ID_SIZE);
  expect(smr_entry_seal_checksum(&hole) == 1, "sela buraco");
  expect(smr_log_apply(log, table, &hole) == 0, "buraco nao aplica");
  expect(log->last_applied == 0 && log->count == 0, "last_applied parado");

  apply_named(log, table, 1, "A");
  apply_named(log, table, 2, "B");
  apply_named(log, table, 3, "C");
  expect(smr_ist_offer(log, 0, &offer, only, 1) == 1, "cap de um");
  expect(offer.missing_count == 3 && only[0]->log_index == 1, "conta as tres e escreve a primeira");

  smr_log_shutdown(log);
  metadata_table_clear(table);
  free(log);
  free(table);
}

int main(void) {
  test_delta_bounds();
  test_delta_apply_and_ignores_prepared();
  test_gap_and_short_cap();
  return test_report();
}
