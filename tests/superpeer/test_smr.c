#include "superpeer/superpeer.h"
#include "utils/test_utils.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "common/network.h"

/**
 * @file test_smr.c
 * @brief Testes do operation log, sem passar pelo harness de checkpoint.
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

static int object_id_of(const char *name, const uint8_t *value, size_t value_len,
                        uint8_t out[METADATA_OBJECT_ID_SIZE]) {
  size_t nlen = strlen(name);
  uint8_t *buf = malloc(nlen + value_len);
  int ok;

  if (!buf) {
    return 0;
  }
  memcpy(buf, name, nlen);
  if (value_len > 0) {
    memcpy(buf + nlen, value, value_len);
  }
  ok = sha256(buf, nlen + value_len, out);
  free(buf);
  return ok;
}

static superpeer_t *new_sp(void) {
  superpeer_t *sp = calloc(1, sizeof *sp);

  if (!sp) {
    return NULL;
  }
  metadata_table_init(&sp->metadata);
  smr_log_init(&sp->smr);
  fill_id(sp->self_id.bytes, 0x11);
  pthread_mutex_init(&sp->members_lock, NULL);
  pthread_mutex_init(&sp->metadata_lock, NULL);
  pthread_mutex_init(&sp->smr_lock, NULL);
  pthread_mutex_init(&sp->coord_lock, NULL);
  return sp;
}

static void free_sp(superpeer_t *sp) {
  if (!sp) {
    return;
  }
  smr_log_shutdown(&sp->smr);
  metadata_table_clear(&sp->metadata);
  pthread_mutex_destroy(&sp->smr_lock);
  pthread_mutex_destroy(&sp->coord_lock);
  pthread_mutex_destroy(&sp->metadata_lock);
  pthread_mutex_destroy(&sp->members_lock);
  free(sp);
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

/**
 * @brief O checksum é estável e muda quando o comando muda.
 */
static void test_checksum_covers_command(void) {
  smr_entry_t entry;
  uint8_t id[METADATA_OBJECT_ID_SIZE];
  uint8_t first[SMR_CHECKSUM_SIZE];
  uint8_t again[SMR_CHECKSUM_SIZE];

  fill_id(id, 4);
  expect(seal_entry(&entry, SMR_OP_PUT, 3, 2, "c4-file", id, NULL) == 1, "sela");
  expect(smr_entry_checksum_ok(&entry) == 1, "checksum fecha");
  memcpy(first, entry.checksum, SMR_CHECKSUM_SIZE);
  expect(smr_entry_seal_checksum(&entry) == 1, "sela de novo");
  memcpy(again, entry.checksum, SMR_CHECKSUM_SIZE);
  expect(memcmp(first, again, SMR_CHECKSUM_SIZE) == 0, "o mesmo comando gera o mesmo digest");

  entry.version = 9;
  expect(smr_entry_seal_checksum(&entry) == 1, "sela com outra versao");
  expect(memcmp(first, entry.checksum, SMR_CHECKSUM_SIZE) != 0, "versao entra no digest");
  expect(smr_entry_seal_checksum(NULL) == 0, "sela nulo");
  expect(smr_entry_checksum_ok(NULL) == 0, "confere nulo");
}

/**
 * @brief STORE vai e volta no fio. Checksum adulterado não reconstrói a entrada.
 */
static void test_pack_unpack_roundtrip(void) {
  smr_entry_t entry;
  smr_entry_t back;
  file_metadata_t meta;
  uint8_t *raw;
  ssize_t n;

  meta = make_meta(6, "wire.bin", 7, 2);
  raw = malloc((size_t)LOG_APPEND_PREFIX + METADATA_PAYLOAD_MAX);
  expect(raw != NULL, "buffer do fio");
  expect(seal_entry(&entry, SMR_OP_STORE, 1, 7, "wire.bin", meta.object_id, &meta) == 1, "sela STORE");
  n = log_append_pack(&entry, raw, (size_t)LOG_APPEND_PREFIX + METADATA_PAYLOAD_MAX);
  expect(n > LOG_APPEND_PREFIX, "STORE carrega o metadata");
  memset(&back, 0, sizeof back);
  expect(log_append_unpack(&back, raw, (size_t)n) == 1, "unpack");
  expect(back.log_index == 1 && back.version == 7 && back.operation == SMR_OP_STORE, "prefixo");
  expect(back.stored.chunk_count == 2 && back.stored.size == meta.size, "corpo");
  expect(back.stored.chunk_hashes != meta.chunk_hashes, "hashes do fio sao outra copia");
  expect(back.stored.chunk_hashes[0] == meta.chunk_hashes[0], "primeiro hash");
  expect(smr_entry_checksum_ok(&back) == 1, "digest do unpack");
  metadata_release(&back.stored);

  raw[69] ^= 0xff;
  expect(log_append_unpack(&back, raw, (size_t)n) == 0, "checksum adulterado falha");
  expect(log_append_pack(NULL, raw, 8) == -1, "pack nulo");
  expect(log_append_unpack(NULL, raw, (size_t)n) == 0, "unpack nulo");

  expect(seal_entry(&entry, SMR_OP_DELETE, 2, 1, "wire.bin", meta.object_id, NULL) == 1, "sela DELETE");
  n = log_append_pack(&entry, raw, (size_t)LOG_APPEND_PREFIX + METADATA_PAYLOAD_MAX);
  expect(n == LOG_APPEND_PREFIX, "DELETE nao leva metadata");

  metadata_release(&meta);
  free(raw);
}

/**
 * @brief Apply segue o índice. Replay não escreve de novo. Buraco e checksum ruim param.
 */
static void test_apply_order_replay_gap_and_delete(void) {
  smr_log_t *log = calloc(1, sizeof *log);
  metadata_table_t *table = malloc(sizeof *table);
  smr_entry_t entry;
  file_metadata_t meta;
  const file_metadata_t *found;
  uint8_t saved;

  metadata_table_init(table);
  meta = make_meta(8, "a.pdf", 4, 1);
  expect(seal_entry(&entry, SMR_OP_STORE, 1, 4, "a.pdf", meta.object_id, &meta) == 1, "sela 1");
  expect(smr_log_apply(log, table, &entry) == 1, "aplica 1");
  found = metadata_table_find_name(table, "a.pdf");
  expect(found != NULL && found->version == 4, "versao selada permanece");
  expect(log->count == 1 && log->last_applied == 1, "log andou");
  expect(log->entries[0].stored.chunk_hashes != meta.chunk_hashes, "o log copiou os hashes");
  saved = meta.chunk_hashes[0];
  meta.chunk_hashes[0] ^= 0xff;
  expect(log->entries[0].stored.chunk_hashes[0] == saved, "a copia do log nao segue o caller");

  entry.checksum[0] ^= 0xff;
  expect(smr_log_apply(log, table, &entry) == 1, "indice velho e no-op mesmo com checksum ruim");
  expect(log->count == 1, "replay nao duplica");
  entry.checksum[0] ^= 0xff;

  expect(seal_entry(&entry, SMR_OP_STORE, 3, 5, "a.pdf", meta.object_id, &meta) == 1, "sela buraco");
  expect(smr_log_apply(log, table, &entry) == 0, "buraco nao aplica");
  expect(log->last_applied == 1 && found->version == 4, "buraco nao avanca nem a versao");

  expect(seal_entry(&entry, SMR_OP_UPDATE, 2, 4, "a.pdf", meta.object_id, &meta) == 1, "sela update");
  entry.checksum[1] ^= 0x5a;
  expect(smr_log_apply(log, table, &entry) == 0, "checksum divergente nao aplica");
  expect(log->count == 1, "falha nao ocupa slot");

  expect(seal_entry(&entry, SMR_OP_UPDATE, 2, 4, "b.pdf", meta.object_id, &meta) == 1, "sela mesma versao");
  expect(smr_log_apply(log, table, &entry) == 1, "update aplica");
  found = metadata_table_find_name(table, "b.pdf");
  expect(found != NULL && found->version == 4, "install nao soma de novo");
  expect(metadata_table_find_name(table, "a.pdf") == NULL, "nome antigo sai");

  expect(seal_entry(&entry, SMR_OP_DELETE, 3, 5, "b.pdf", meta.object_id, NULL) == 1, "sela delete");
  expect(smr_log_apply(log, table, &entry) == 1, "delete aplica");
  expect(metadata_table_find_id(table, meta.object_id) == NULL, "object id saiu");
  expect(log->count == 3 && log->last_applied == 3, "delete fica no log");
  expect(smr_log_apply(NULL, table, &entry) == 0, "apply nulo");

  metadata_release(&meta);
  smr_log_shutdown(log);
  metadata_table_clear(table);
  free(log);
  free(table);
}

/**
 * @brief install grava a versão recebida. put soma um na chave que já existe.
 */
static void test_install_keeps_version(void) {
  metadata_table_t *table = malloc(sizeof *table);
  file_metadata_t meta;
  const file_metadata_t *found;

  metadata_table_init(table);
  meta = make_meta(3, "v.pdf", 3, 0);
  expect(metadata_table_install(table, &meta) == 1, "install novo");
  meta.version = 3;
  set_name(meta.filename, "v2.pdf");
  expect(metadata_table_install(table, &meta) == 1, "install da mesma chave");
  found = metadata_table_find_id(table, meta.object_id);
  expect(found != NULL && found->version == 3, "versao nao soma");
  expect(metadata_table_put(table, &meta) == 1, "put da mesma chave");
  found = metadata_table_find_id(table, meta.object_id);
  expect(found != NULL && found->version == 4, "put soma um");
  metadata_release(&meta);
  metadata_table_clear(table);
  free(table);
}

/**
 * @brief PUT usa SHA-256(nome || valor) e não guarda o valor.
 */
static void test_propose_put(void) {
  superpeer_t *sp = new_sp();
  const file_metadata_t *found;
  uint8_t expect_id[METADATA_OBJECT_ID_SIZE];
  uint8_t other_id[METADATA_OBJECT_ID_SIZE];
  const uint8_t value[] = {'v', '1'};
  const uint8_t other[] = {'v', '2'};

  expect(sp != NULL, "aloca superpeer");
  expect(smr_propose_put(sp, "c4-file", value, sizeof value) == 1, "put");
  expect(object_id_of("c4-file", value, sizeof value, expect_id) == 1, "digest do par");
  found = metadata_table_find_name(&sp->metadata, "c4-file");
  expect(found != NULL && found->version == 1 && found->chunk_count == 0 && found->size == 0, "registro sintetico");
  expect(found != NULL && memcmp(found->object_id, expect_id, METADATA_OBJECT_ID_SIZE) == 0, "object id");
  expect(sp->smr.count == 1 && sp->smr.entries[0].operation == SMR_OP_PUT, "operacao PUT");

  expect(smr_propose_put(sp, "c4-file", value, sizeof value) == 1, "mesmo par de novo");
  found = metadata_table_find_id(&sp->metadata, expect_id);
  expect(found != NULL && found->version == 2, "segunda versao");
  expect(sp->smr.last_applied == 2, "indice 2");

  expect(object_id_of("c4-file", other, sizeof other, other_id) == 1, "digest do outro valor");
  expect(memcmp(expect_id, other_id, METADATA_OBJECT_ID_SIZE) != 0, "valor diferente muda o id");
  expect(smr_propose_put(sp, "c4-file", other, sizeof other) == 1, "outro valor");
  expect(metadata_table_find_id(&sp->metadata, other_id) != NULL, "segundo objeto");
  expect(sp->metadata.count == 2, "nao substitui o par anterior");

  expect(smr_propose_put(sp, "", value, sizeof value) == 0, "nome vazio");
  expect(smr_propose_put(sp, "c4-file", NULL, sizeof value) == 0, "valor ausente");
  expect(smr_propose_put(NULL, "c4-file", value, sizeof value) == 0, "sp nulo");
  expect(smr_propose_put(sp, "vazio", NULL, 0) == 1, "valor vazio");
  free_sp(sp);
}

/**
 * @brief STORE sem coordenador sela localmente e não aliasa os hashes do caller.
 */
static void test_propose_store_local(void) {
  superpeer_t *sp = new_sp();
  file_metadata_t meta;
  const file_metadata_t *found;
  uint8_t saved;

  expect(sp != NULL, "aloca superpeer");
  meta = make_meta(9, "arquivo.pdf", 4, 1);
  saved = meta.chunk_hashes[0];
  expect(smr_propose_store(sp, &meta) == 1, "store novo");
  found = metadata_table_find_name(&sp->metadata, "arquivo.pdf");
  expect(found != NULL && found->version == 4, "primeira versao do STORE");
  expect(found->chunk_hashes != meta.chunk_hashes, "tabela tem copia");
  expect(sp->smr.entries[0].stored.chunk_hashes != meta.chunk_hashes, "log tem copia");
  meta.chunk_hashes[0] ^= 0xff;
  expect(found->chunk_hashes[0] == saved, "tabela estavel");
  expect(sp->smr.entries[0].stored.chunk_hashes[0] == saved, "log estavel");

  meta.chunk_hashes[0] = saved;
  meta.version = 99;
  set_name(meta.filename, "novo.pdf");
  expect(smr_propose_store(sp, &meta) == 1, "store da mesma chave");
  found = metadata_table_find_name(&sp->metadata, "novo.pdf");
  expect(found != NULL && found->version == 5, "versao armazenada mais um");
  expect(sp->metadata.count == 1 && sp->smr.count == 2, "um objeto, duas entradas");
  expect(smr_propose_store(sp, NULL) == 0, "meta nulo");
  expect(smr_propose_store(NULL, &meta) == 0, "sp nulo");

  metadata_release(&meta);
  free_sp(sp);
}

/**
 * @brief O teto do log recusa a entrada seguinte e não mexe na tabela.
 */
static void test_log_full(void) {
  superpeer_t *sp = new_sp();
  file_metadata_t meta;
  uint32_t i;

  expect(sp != NULL, "aloca superpeer");
  for (i = 0; i < SMR_LOG_MAX; i++) {
    char name[32];

    metadata_init(&meta);
    memset(meta.object_id, 0, sizeof meta.object_id);
    meta.object_id[0] = 1;
    meta.object_id[1] = (uint8_t)(i >> 8);
    meta.object_id[2] = (uint8_t)i;
    snprintf(name, sizeof name, "n-%u", i);
    set_name(meta.filename, name);
    meta.version = 1;
    fill_id(meta.owner.bytes, 3);
    if (!smr_propose_store(sp, &meta)) {
      expect(0, "enche o log");
      free_sp(sp);
      return;
    }
  }
  expect(sp->smr.count == SMR_LOG_MAX && sp->smr.last_applied == SMR_LOG_MAX, "log cheio");

  metadata_init(&meta);
  meta.object_id[0] = 2;
  set_name(meta.filename, "extra.pdf");
  meta.version = 1;
  fill_id(meta.owner.bytes, 3);
  expect(smr_propose_store(sp, &meta) == 0, "entrada extra falha");
  expect(sp->smr.count == SMR_LOG_MAX, "count parado");
  expect(metadata_table_find_name(&sp->metadata, "extra.pdf") == NULL, "extra nao entra na tabela");
  free_sp(sp);
}

/**
 * @brief Checkpoint marca o índice aplicado. Virar seguidor não reaplica o log.
 */
static void test_coordinator_checkpoint(void) {
  superpeer_t *sp = new_sp();
  file_metadata_t meta;
  node_id_t other;
  size_t count;
  uint64_t applied;
  uint64_t checkpoint;

  expect(sp != NULL, "aloca superpeer");
  meta = make_meta(5, "antes.pdf", 1, 0);
  expect(smr_propose_store(sp, &meta) == 1, "uma entrada");
  count = sp->smr.count;
  applied = sp->smr.last_applied;

  smr_on_coordinator(sp, &sp->self_id, 1);
  expect(sp->smr.checkpoint_index == applied, "checkpoint no ultimo indice");
  expect(sp->coord_role == COORD_LEADER && sp->has_coordinator == 1, "este no e o lider");
  expect(sp->smr.count == count, "checkpoint nao reaplica");

  fill_id(other.bytes, 0x22);
  checkpoint = sp->smr.checkpoint_index;
  smr_on_coordinator(sp, &other, 0);
  expect(sp->coord_role == COORD_FOLLOWER, "volta a seguidor");
  expect(sp->smr.count == count && sp->smr.last_applied == applied, "seguir nao reaplica");
  expect(sp->smr.checkpoint_index == checkpoint, "checkpoint do lider antigo fica");

  smr_on_coordinator(sp, NULL, 0);
  expect(sp->has_coordinator == 0, "coordenador limpo");
  smr_on_coordinator(NULL, &other, 1);
  metadata_release(&meta);
  free_sp(sp);
}

/**
 * @brief Endereço igual ao deste processo aplica localmente.
 */
static void test_self_endpoint_applies_locally(void) {
  superpeer_t *sp = new_sp();
  file_metadata_t meta;
  node_id_t other;

  expect(sp != NULL, "aloca superpeer");
  expect(inet_pton(AF_INET, "127.0.0.1", &sp->cfg.ipv4) == 1, "ipv4 local");
  sp->cfg.port = 1;
  fill_id(other.bytes, 0x44);
  sp->has_coordinator = 1;
  sp->coord_role = COORD_FOLLOWER;
  sp->coordinator_id = other;
  sp->members.entries[0].id = other;
  sp->members.entries[0].ipv4 = sp->cfg.ipv4;
  sp->members.entries[0].port = sp->cfg.port;
  sp->members.entries[0].node_type = SUPERPEER;
  sp->members.entries[0].state = MEMBER_ALIVE;
  sp->members.count = 1;

  meta = make_meta(12, "local.pdf", 2, 0);
  expect(smr_propose_store(sp, &meta) == 1, "nao conecta em si");
  expect(metadata_table_find_name(&sp->metadata, "local.pdf") != NULL, "aplicou local");
  expect(sp->smr.count == 1, "uma entrada local");
  metadata_release(&meta);
  free_sp(sp);
}

/**
 * @brief Coordenador que não aceita a conexão não deixa registro local.
 */
static void test_unreachable_coordinator(void) {
  superpeer_t *sp = new_sp();
  file_metadata_t meta;
  node_id_t other;

  expect(sp != NULL, "aloca superpeer");
  fill_id(other.bytes, 0x55);
  sp->has_coordinator = 1;
  sp->coord_role = COORD_FOLLOWER;
  sp->coordinator_id = other;
  expect(inet_pton(AF_INET, "127.0.0.1", &sp->members.entries[0].ipv4) == 1, "ipv4 do coordenador");
  sp->members.entries[0].id = other;
  sp->members.entries[0].port = 1;
  sp->members.entries[0].node_type = SUPERPEER;
  sp->members.entries[0].state = MEMBER_ALIVE;
  sp->members.count = 1;
  inet_pton(AF_INET, "127.0.0.1", &sp->cfg.ipv4);
  sp->cfg.port = 9;

  meta = make_meta(13, "remoto.pdf", 1, 0);
  expect(smr_propose_store(sp, &meta) == 0, "coordenador recusou");
  expect(sp->smr.count == 0 && sp->metadata.count == 0, "nada aplicado");
  metadata_release(&meta);
  free_sp(sp);
}

static int pack_proposal(smr_op_t op, const char *name, const uint8_t id[METADATA_OBJECT_ID_SIZE],
                         const file_metadata_t *meta, uint8_t *out, size_t cap, ssize_t *n) {
  smr_entry_t entry;

  if (!seal_entry(&entry, op, 0, 0, name, id, meta)) {
    return 0;
  }
  *n = log_append_pack(&entry, out, cap);
  return *n > 0;
}

/**
 * @brief O líder sela a proposta e o seguidor aplica o mesmo LOG_APPEND.
 */
static void test_ingest_proposal_and_replica(void) {
  superpeer_t *leader = new_sp();
  superpeer_t *follower = new_sp();
  int proposal_fd[2];
  int replica_fd[2];
  uint8_t id[METADATA_OBJECT_ID_SIZE];
  uint8_t *proposal;
  uint8_t *reply;
  ssize_t n = 0;
  pl_header hdr;
  msg_t msg;
  smr_entry_t got;
  const file_metadata_t *on_leader;
  const file_metadata_t *on_follower;
  const size_t cap = (size_t)LOG_APPEND_PREFIX + METADATA_PAYLOAD_MAX;

  expect(leader != NULL && follower != NULL, "aloca os dois");
  leader->has_coordinator = 1;
  leader->coord_role = COORD_LEADER;
  leader->coordinator_id = leader->self_id;
  fill_id(id, 0x42);
  proposal = malloc(cap);
  reply = malloc(cap);
  expect(proposal != NULL && reply != NULL, "buffers");
  expect(pack_proposal(SMR_OP_PUT, "c4-file", id, NULL, proposal, cap, &n) == 1, "proposta");

  memset(&hdr, 0, sizeof hdr);
  hdr.protocol_ver = PROTOCOL_VER;
  hdr.msg_type = LOG_APPEND;
  hdr.pl_size = (uint32_t)n;
  expect(socketpair(AF_UNIX, SOCK_STREAM, 0, proposal_fd) == 0, "par da proposta");
  expect(smr_ingest_append(leader, proposal_fd[0], &hdr, proposal, (size_t)n) == 1, "lider sela");
  msg = simple_recv(proposal_fd[1], reply, (uint32_t)cap);
  expect(msg.status == NET_OK && msg.header.msg_type == LOG_APPEND, "resposta e LOG_APPEND");
  memset(&got, 0, sizeof got);
  expect(log_append_unpack(&got, reply, msg.header.pl_size) == 1, "unpack da resposta");
  expect(got.log_index == 1 && got.version == 1 && strcmp(got.name, "c4-file") == 0, "entrada selada");
  expect(memcmp(got.object_id, id, METADATA_OBJECT_ID_SIZE) == 0, "object id preservado");
  on_leader = metadata_table_find_name(&leader->metadata, "c4-file");
  expect(on_leader != NULL && on_leader->version == 1, "lider instalou");

  follower->has_coordinator = 1;
  follower->coord_role = COORD_FOLLOWER;
  fill_id(follower->coordinator_id.bytes, 0x99);
  expect(socketpair(AF_UNIX, SOCK_STREAM, 0, replica_fd) == 0, "par da replica");
  hdr.pl_size = msg.header.pl_size;
  expect(smr_ingest_append(follower, replica_fd[0], &hdr, reply, msg.header.pl_size) == 1, "seguidor aplica");
  msg = simple_recv(replica_fd[1], proposal, (uint32_t)cap);
  expect(msg.status == NET_OK && msg.header.msg_type == ACK, "replica responde ACK");
  on_follower = metadata_table_find_name(&follower->metadata, "c4-file");
  expect(on_follower != NULL && on_follower->version == on_leader->version, "mesma versao");
  expect(memcmp(on_follower->object_id, on_leader->object_id, METADATA_OBJECT_ID_SIZE) == 0, "mesmo object id");
  expect(follower->smr.count == 1, "um registro no seguidor");

  expect(smr_ingest_append(follower, replica_fd[0], &hdr, reply, hdr.pl_size) == 1, "replay da replica");
  msg = simple_recv(replica_fd[1], proposal, (uint32_t)cap);
  expect(msg.status == NET_OK && msg.header.msg_type == ACK, "replay tambem da ACK");
  expect(follower->smr.count == 1 && follower->metadata.count == 1, "replay nao duplica");

  expect(pack_proposal(SMR_OP_PUT, "c4-file", id, NULL, proposal, cap, &n) == 1, "proposta de novo");
  proposal[10] ^= 0xff;
  hdr.pl_size = (uint32_t)n;
  expect(smr_ingest_append(leader, proposal_fd[0], &hdr, proposal, (size_t)n) == 0, "proposta ruim");
  expect(leader->smr.count == 1, "lider nao andou");

  net_close(proposal_fd[0]);
  net_close(proposal_fd[1]);
  net_close(replica_fd[0]);
  net_close(replica_fd[1]);
  metadata_release(&got.stored);
  free(proposal);
  free(reply);
  free_sp(leader);
  free_sp(follower);
}

/**
 * @brief Seguidor não sela proposta. Índice furado volta erro e não entra no log.
 */
static void test_ingest_rejects_follower_and_gap(void) {
  superpeer_t *sp = new_sp();
  int sv[2];
  uint8_t id[METADATA_OBJECT_ID_SIZE];
  uint8_t *raw;
  ssize_t n = 0;
  pl_header hdr;
  msg_t msg;
  smr_entry_t entry;
  const size_t cap = (size_t)LOG_APPEND_PREFIX + 64;

  expect(sp != NULL, "aloca superpeer");
  sp->has_coordinator = 1;
  sp->coord_role = COORD_FOLLOWER;
  fill_id(sp->coordinator_id.bytes, 0x77);
  fill_id(id, 0x21);
  raw = malloc(cap);
  expect(pack_proposal(SMR_OP_PUT, "nao.pdf", id, NULL, raw, cap, &n) == 1, "proposta");
  memset(&hdr, 0, sizeof hdr);
  hdr.protocol_ver = PROTOCOL_VER;
  hdr.msg_type = LOG_APPEND;
  hdr.pl_size = (uint32_t)n;
  expect(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "par");
  expect(smr_ingest_append(sp, sv[0], &hdr, raw, (size_t)n) == 0, "seguidor nao sela");
  msg = simple_recv(sv[1], raw, (uint32_t)cap);
  expect(msg.status == NET_OK && msg.header.msg_type == ERROR, "responde ERROR");
  expect(sp->smr.count == 0, "log do seguidor vazio");

  expect(seal_entry(&entry, SMR_OP_PUT, 4, 1, "gap.pdf", id, NULL) == 1, "sela indice 4");
  n = log_append_pack(&entry, raw, cap);
  expect(n > 0, "pack do buraco");
  hdr.pl_size = (uint32_t)n;
  expect(smr_ingest_append(sp, sv[0], &hdr, raw, (size_t)n) == 0, "buraco recusado");
  msg = simple_recv(sv[1], raw, (uint32_t)cap);
  expect(msg.status == NET_OK && msg.header.msg_type == ERROR, "buraco responde ERROR");
  expect(sp->smr.last_applied == 0, "last_applied parado");
  expect(metadata_table_find_name(&sp->metadata, "gap.pdf") == NULL, "gap nao instala");

  expect(smr_ingest_append(sp, -1, &hdr, raw, (size_t)n) == 0, "socket invalido");
  expect(smr_ingest_append(NULL, sv[0], &hdr, raw, (size_t)n) == 0, "sp nulo");
  net_close(sv[0]);
  net_close(sv[1]);
  free(raw);
  free_sp(sp);
}

/**
 * @brief Init e shutdown aceitam nulo. Shutdown libera os hashes do log.
 */
static void test_shutdown_frees_log_hashes(void) {
  smr_log_t *log = calloc(1, sizeof *log);
  metadata_table_t *table = malloc(sizeof *table);
  smr_entry_t entry;
  file_metadata_t meta;

  smr_log_init(NULL);
  smr_log_shutdown(NULL);
  metadata_table_init(table);
  meta = make_meta(15, "livre.pdf", 1, 1);
  expect(seal_entry(&entry, SMR_OP_STORE, 1, 1, "livre.pdf", meta.object_id, &meta) == 1, "sela");
  expect(smr_log_apply(log, table, &entry) == 1, "aplica");
  expect(log->entries[0].stored.chunk_hashes != NULL, "log segurou o bloco");
  smr_log_shutdown(log);
  expect(log->count == 0, "shutdown esvazia");
  metadata_release(&meta);
  metadata_table_clear(table);
  free(log);
  free(table);
}

int main(void) {
  test_checksum_covers_command();
  test_pack_unpack_roundtrip();
  test_apply_order_replay_gap_and_delete();
  test_install_keeps_version();
  test_propose_put();
  test_propose_store_local();
  test_log_full();
  test_coordinator_checkpoint();
  test_self_endpoint_applies_locally();
  test_unreachable_coordinator();
  test_ingest_proposal_and_replica();
  test_ingest_rejects_follower_and_gap();
  test_shutdown_frees_log_hashes();
  return test_report();
}
