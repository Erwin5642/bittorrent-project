#include "superpeer/smr.h"
#include "utils/test_utils.h"

#include <stdlib.h>
#include <string.h>

/**
 * @file test_2pc.c
 * @brief Decisão do 2PC e slot PREPARED, sem socket.
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

static file_metadata_t make_meta(uint8_t seed, const char *name, uint32_t version, uint32_t chunks) {
  file_metadata_t meta;
  size_t i;

  metadata_init(&meta);
  fill_id(meta.object_id, seed);
  set_name(meta.filename, name);
  meta.size = 1000u + seed;
  meta.chunk_count = chunks;
  meta.version = version;
  fill_id(meta.owner.bytes, (uint8_t)(seed + 3));
  if (chunks > 0) {
    meta.chunk_hashes = malloc((size_t)chunks * METADATA_CHUNK_HASH_SIZE);
    for (i = 0; i < (size_t)chunks * METADATA_CHUNK_HASH_SIZE; i++) {
      meta.chunk_hashes[i] = (uint8_t)(0x40 + seed + i);
    }
  }
  return meta;
}

static int seal_entry(smr_entry_t *entry, smr_op_t op, uint64_t index, uint32_t version,
                      const char *name, const uint8_t object_id[METADATA_OBJECT_ID_SIZE],
                      const file_metadata_t *meta) {
  memset(entry, 0, sizeof *entry);
  entry->operation = op;
  entry->log_index = index;
  entry->timestamp = index;
  entry->version = version;
  set_name(entry->name, name);
  memcpy(entry->object_id, object_id, METADATA_OBJECT_ID_SIZE);
  if (meta) {
    entry->stored = *meta;
    entry->stored.version = version;
  }
  return smr_entry_seal_checksum(entry);
}

static smr_log_t *new_log(void) {
  return calloc(1, sizeof(smr_log_t));
}

static metadata_table_t *new_table(void) {
  metadata_table_t *table = malloc(sizeof *table);

  if (table) {
    metadata_table_init(table);
  }
  return table;
}

/**
 * @brief Todos YES aplicam uma vez. O segundo COMMIT não soma version.
 */
static void test_all_yes_applies_once(void) {
  smr_log_t *log = new_log();
  metadata_table_t *table = new_table();
  file_metadata_t meta;
  smr_entry_t entry;
  const file_metadata_t *found;
  int votes[3] = {1, 1, 1};

  meta = make_meta(4, "a.pdf", 4, 1);
  expect(log != NULL && table != NULL, "aloca");
  expect(seal_entry(&entry, SMR_OP_STORE, 1, 4, "a.pdf", meta.object_id, &meta) == 1, "sela");
  expect(smr_prepare_vote(log, &entry) == 1, "voto YES local");
  expect(log->prepared_phase == SMR_TX_PREPARED && log->count == 0, "slot fora da tabela");
  expect(metadata_table_find_name(table, "a.pdf") == NULL, "prepare nao instala");
  expect(smr_tx_decide_votes(votes, 3) == SMR_TX_COMMITTED, "todos YES");
  expect(smr_tx_finish(log, table, SMR_TX_COMMITTED) == 1, "commit");
  found = metadata_table_find_name(table, "a.pdf");
  expect(found != NULL && found->version == 4, "versao selada");
  expect(found->chunk_hashes != NULL && found->chunk_hashes != meta.chunk_hashes, "tabela tem copia");
  expect(log->count == 1 && log->last_applied == 1 && log->prepared_phase == SMR_TX_NONE, "slot saiu");
  expect(smr_tx_finish(log, table, SMR_TX_COMMITTED) == 1, "segundo commit sem slot");
  expect(smr_log_apply(log, table, &entry) == 1, "replay do mesmo indice");
  found = metadata_table_find_name(table, "a.pdf");
  expect(found != NULL && found->version == 4 && log->count == 1, "version quieta");

  metadata_release(&meta);
  smr_log_shutdown(log);
  metadata_table_clear(table);
  free(log);
  free(table);
}

/**
 * @brief Um NO esvazia o slot e não escreve na tabela.
 */
static void test_one_no_discards_slot(void) {
  smr_log_t *log = new_log();
  metadata_table_t *table = new_table();
  smr_entry_t entry;
  uint8_t id[METADATA_OBJECT_ID_SIZE];
  int votes[3] = {1, 0, 1};

  fill_id(id, 8);
  expect(seal_entry(&entry, SMR_OP_PUT, 1, 1, "fileB", id, NULL) == 1, "sela");
  expect(smr_prepare_vote(log, &entry) == 1, "slot ocupado");
  expect(smr_tx_decide_votes(votes, 3) == SMR_TX_ABORTED, "um NO aborta");
  expect(smr_tx_finish(log, table, SMR_TX_ABORTED) == 1, "abort");
  expect(log->prepared_phase == SMR_TX_NONE && log->count == 0, "slot vazio");
  expect(metadata_table_find_name(table, "fileB") == NULL, "tabela intacta");
  expect(smr_tx_decide_votes(NULL, 0) == SMR_TX_COMMITTED, "sem participante remoto comita");
  expect(smr_tx_decide_votes(NULL, 2) == SMR_TX_ABORTED, "votos ausentes abortam");
  expect(smr_tx_finish(NULL, table, SMR_TX_ABORTED) == 0, "finish nulo");
  expect(smr_tx_finish(log, table, SMR_TX_PREPARED) == 0, "decisao desconhecida");

  smr_log_shutdown(log);
  metadata_table_clear(table);
  free(log);
  free(table);
}

/**
 * @brief Coordenador morto no PREPARE: sem COMMIT a rodada aborta.
 * Com COMMIT em algum sobrevivente, quem ainda está PREPARED aplica.
 */
static void test_recover_from_prepare(void) {
  smr_log_t *behind = new_log();
  smr_log_t *ahead = new_log();
  metadata_table_t *table_behind = new_table();
  metadata_table_t *table_ahead = new_table();
  smr_entry_t entry;
  smr_tx_phase_t open_only[2] = {SMR_TX_PREPARED, SMR_TX_PREPARED};
  smr_tx_phase_t with_commit[2] = {SMR_TX_PREPARED, SMR_TX_COMMITTED};
  const file_metadata_t *found;
  uint8_t id[METADATA_OBJECT_ID_SIZE];

  fill_id(id, 3);
  expect(seal_entry(&entry, SMR_OP_PUT, 1, 2, "fileA", id, NULL) == 1, "sela");
  expect(smr_tx_decide_recover(open_only, 2) == SMR_TX_ABORTED, "ninguem tem COMMIT");
  expect(smr_tx_decide_recover(NULL, 0) == SMR_TX_ABORTED, "lista vazia aborta");
  expect(smr_prepare_vote(behind, &entry) == 1, "sobrevivente votou YES");
  expect(smr_tx_finish(behind, table_behind, SMR_TX_ABORTED) == 1, "abort da recuperacao");
  expect(metadata_table_find_name(table_behind, "fileA") == NULL, "upload interrompido some");
  expect(behind->count == 0, "log do abort nao anda");

  expect(smr_prepare_vote(behind, &entry) == 1, "de novo no slot");
  expect(smr_log_apply(ahead, table_ahead, &entry) == 1, "outro ja comitou");
  expect(smr_tx_decide_recover(with_commit, 2) == SMR_TX_COMMITTED, "um COMMIT fecha");
  expect(smr_tx_finish(behind, table_behind, SMR_TX_COMMITTED) == 1, "completa em quem faltava");
  found = metadata_table_find_name(table_behind, "fileA");
  expect(found != NULL && found->version == 2, "versao do COMMIT");
  expect(smr_tx_finish(ahead, table_ahead, SMR_TX_COMMITTED) == 1, "quem ja aplicou nao escreve de novo");
  found = metadata_table_find_name(table_ahead, "fileA");
  expect(found != NULL && found->version == 2 && ahead->count == 1, "version do adiantado quieta");

  smr_log_shutdown(behind);
  smr_log_shutdown(ahead);
  metadata_table_clear(table_behind);
  metadata_table_clear(table_ahead);
  free(behind);
  free(ahead);
  free(table_behind);
  free(table_ahead);
}

/**
 * @brief NO por checksum, buraco ou slot ocupado não guarda a entrada.
 */
static void test_vote_rejects(void) {
  smr_log_t *log = new_log();
  smr_entry_t entry;
  smr_entry_t other;
  uint8_t id[METADATA_OBJECT_ID_SIZE];

  fill_id(id, 9);
  expect(smr_prepare_vote(NULL, NULL) == 0, "log nulo");
  expect(seal_entry(&entry, SMR_OP_PUT, 1, 1, "ok", id, NULL) == 1, "sela 1");
  entry.checksum[0] ^= 0xff;
  expect(smr_prepare_vote(log, &entry) == 0, "checksum ruim e NO");
  expect(log->prepared_phase == SMR_TX_NONE, "slot continua vazio");
  entry.checksum[0] ^= 0xff;

  expect(seal_entry(&other, SMR_OP_PUT, 3, 1, "gap", id, NULL) == 1, "sela buraco");
  expect(smr_prepare_vote(log, &other) == 0, "indice com buraco e NO");

  expect(smr_prepare_vote(log, &entry) == 1, "YES");
  expect(smr_prepare_vote(log, &entry) == 0, "segundo PREPARED e NO");
  expect(log->prepared.log_index == 1, "o primeiro slot permanece");

  smr_log_shutdown(log);
  free(log);
}

/**
 * @brief Só o índice acima de last_applied + 1 é buraco. É o que dispara o IST.
 */
static void test_vote_hole_detection(void) {
  smr_log_t *log = new_log();
  metadata_table_t *table = new_table();
  smr_entry_t first;
  smr_entry_t next;
  smr_entry_t far;
  smr_entry_t stale;
  uint8_t id[METADATA_OBJECT_ID_SIZE];

  fill_id(id, 5);
  expect(smr_vote_hole(NULL, NULL) == 0, "argumentos nulos nao sao buraco");
  expect(seal_entry(&first, SMR_OP_PUT, 1, 1, "um", id, NULL) == 1, "sela 1");
  expect(seal_entry(&next, SMR_OP_PUT, 2, 1, "dois", id, NULL) == 1, "sela 2");
  expect(seal_entry(&far, SMR_OP_PUT, 5, 1, "cinco", id, NULL) == 1, "sela 5");
  expect(smr_vote_hole(log, &first) == 0, "indice seguinte nao e buraco");
  expect(smr_vote_hole(log, &next) == 1, "log vazio e indice 2 e buraco");
  expect(smr_vote_hole(log, &far) == 1, "indice 5 num log vazio e buraco");

  expect(smr_log_apply(log, table, &first) == 1, "aplica 1");
  expect(smr_vote_hole(log, &next) == 0, "depois de aplicar 1, o 2 e o seguinte");
  expect(smr_vote_hole(log, &far) == 1, "o 5 continua buraco");
  expect(seal_entry(&stale, SMR_OP_PUT, 1, 1, "um", id, NULL) == 1, "sela 1 de novo");
  expect(smr_vote_hole(log, &stale) == 0, "indice ja aplicado nao e buraco");

  smr_log_shutdown(log);
  free(log);
  free(table);
}

int main(void) {
  test_vote_hole_detection();
  test_all_yes_applies_once();
  test_one_no_discards_slot();
  test_recover_from_prepare();
  test_vote_rejects();
  return test_report();
}
