#include "superpeer/superpeer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common/network.h"

static pthread_mutex_t two_pc_mu = PTHREAD_MUTEX_INITIALIZER;

#define DECISION_WIRE 24
#define SNAPSHOT_REQ_LEN 9
#define SNAPSHOT_REP_LEN 21

static void put_be64(uint8_t *p, uint64_t v) {
  int i;

  for (i = 7; i >= 0; i--) {
    p[i] = (uint8_t)(v & 0xffu);
    v >>= 8;
  }
}

static uint64_t get_be64(const uint8_t *p) {
  uint64_t v = 0;
  int i;

  for (i = 0; i < 8; i++) {
    v = (v << 8) | p[i];
  }
  return v;
}

static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static const char *op_label(smr_op_t op) {
  switch (op) {
  case SMR_OP_STORE:
    return "STORE";
  case SMR_OP_DELETE:
    return "DELETE";
  case SMR_OP_UPDATE:
    return "UPDATE";
  case SMR_OP_PUT:
    return "PUT";
  default:
    return "OP";
  }
}

static int copy_name(char dst[METADATA_FILENAME_MAX], const char *src) {
  size_t n;

  if (!metadata_name_ok(src)) {
    return 0;
  }
  n = strlen(src);
  if (n >= METADATA_FILENAME_MAX) {
    return 0;
  }
  memset(dst, 0, METADATA_FILENAME_MAX);
  memcpy(dst, src, n + 1);
  return 1;
}

static int endpoint_is_self(const superpeer_t *sp, uint32_t ipv4, uint16_t port) {
  return sp && ipv4 == sp->cfg.ipv4 && port == sp->cfg.port;
}

/* O log fica dono do bloco novo. O ponteiro de origem continua com quem chamou. */
static int entry_clone(smr_entry_t *dst, const smr_entry_t *src) {
  size_t bytes;

  *dst = *src;
  dst->stored.chunk_hashes = NULL;
  if (src->stored.chunk_count == 0) {
    return 1;
  }
  if (!src->stored.chunk_hashes) {
    return 0;
  }
  bytes = (size_t)src->stored.chunk_count * METADATA_CHUNK_HASH_SIZE;
  dst->stored.chunk_hashes = malloc(bytes);
  if (!dst->stored.chunk_hashes) {
    return 0;
  }
  memcpy(dst->stored.chunk_hashes, src->stored.chunk_hashes, bytes);
  return 1;
}

static int apply_body(metadata_table_t *table, const smr_entry_t *entry) {
  file_metadata_t body;

  if (entry->operation == SMR_OP_DELETE) {
    metadata_table_delete(table, entry->object_id);
    return 1;
  }
  if (entry->operation == SMR_OP_PUT) {
    metadata_init(&body);
    memcpy(body.object_id, entry->object_id, METADATA_OBJECT_ID_SIZE);
    memcpy(body.filename, entry->name, METADATA_FILENAME_MAX);
    body.version = entry->version;
    return metadata_table_install(table, &body);
  }
  if (entry->operation != SMR_OP_STORE && entry->operation != SMR_OP_UPDATE) {
    return 0;
  }
  body = entry->stored;
  body.version = entry->version;
  memcpy(body.object_id, entry->object_id, METADATA_OBJECT_ID_SIZE);
  memcpy(body.filename, entry->name, METADATA_FILENAME_MAX);
  return metadata_table_install(table, &body);
}

void smr_log_init(smr_log_t *log) {
  if (!log) {
    return;
  }
  memset(log, 0, sizeof *log);
  printf("SMR log ready LogIndex 0\n");
  fflush(stdout);
}

void smr_log_shutdown(smr_log_t *log) {
  size_t n;
  size_t i;

  if (!log) {
    return;
  }
  n = log->count;
  if (n > SMR_LOG_MAX) {
    n = SMR_LOG_MAX;
  }
  for (i = 0; i < n; i++) {
    metadata_release(&log->entries[i].stored);
  }
  if (log->prepared_phase == SMR_TX_PREPARED) {
    metadata_release(&log->prepared.stored);
  }
  memset(log, 0, sizeof *log);
}

int smr_entry_seal_checksum(smr_entry_t *entry) {
  smr_entry_t tmp;
  uint8_t *buf;
  ssize_t n;

  if (!entry) {
    return 0;
  }
  tmp = *entry;
  memset(tmp.checksum, 0, SMR_CHECKSUM_SIZE);
  buf = malloc((size_t)LOG_APPEND_PREFIX + METADATA_PAYLOAD_MAX);
  if (!buf) {
    return 0;
  }
  n = log_append_pack(&tmp, buf, (size_t)LOG_APPEND_PREFIX + METADATA_PAYLOAD_MAX);
  if (n < 0 || !sha256(buf, (size_t)n, entry->checksum)) {
    free(buf);
    return 0;
  }
  free(buf);
  return 1;
}

int smr_entry_checksum_ok(const smr_entry_t *entry) {
  smr_entry_t tmp;
  uint8_t digest[SMR_CHECKSUM_SIZE];
  uint8_t *buf;
  ssize_t n;

  if (!entry) {
    return 0;
  }
  tmp = *entry;
  memset(tmp.checksum, 0, SMR_CHECKSUM_SIZE);
  buf = malloc((size_t)LOG_APPEND_PREFIX + METADATA_PAYLOAD_MAX);
  if (!buf) {
    return 0;
  }
  n = log_append_pack(&tmp, buf, (size_t)LOG_APPEND_PREFIX + METADATA_PAYLOAD_MAX);
  if (n < 0 || !sha256(buf, (size_t)n, digest)) {
    free(buf);
    return 0;
  }
  free(buf);
  return memcmp(digest, entry->checksum, SMR_CHECKSUM_SIZE) == 0;
}

int smr_log_apply(smr_log_t *log, metadata_table_t *table, const smr_entry_t *entry) {
  if (!log || !table || !entry || entry->log_index == 0) {
    return 0;
  }
  if (entry->log_index <= log->last_applied) {
    return 1;
  }
  if (!smr_entry_checksum_ok(entry)) {
    return 0;
  }
  if (entry->log_index != log->last_applied + 1 || log->count >= SMR_LOG_MAX) {
    return 0;
  }
  if (!entry_clone(&log->entries[log->count], entry)) {
    return 0;
  }
  if (!apply_body(table, entry)) {
    metadata_release(&log->entries[log->count].stored);
    memset(&log->entries[log->count], 0, sizeof log->entries[log->count]);
    return 0;
  }
  log->count++;
  log->last_applied = entry->log_index;
  printf("LOG #%llu\n%s %s\nLogIndex %llu\n",
         (unsigned long long)entry->log_index, op_label(entry->operation), entry->name,
         (unsigned long long)entry->log_index);
  fflush(stdout);
  /* Quem tem o superpeer_t chama lfu_invalidate depois de soltar smr_lock. */
  return 1;
}

static void prepared_clear(smr_log_t *log) {
  if (!log) {
    return;
  }
  if (log->prepared_phase == SMR_TX_PREPARED) {
    metadata_release(&log->prepared.stored);
  }
  memset(&log->prepared, 0, sizeof log->prepared);
  log->prepared_phase = SMR_TX_NONE;
}

int smr_prepare_vote(smr_log_t *log, const smr_entry_t *entry) {
  smr_entry_t copy;

  if (!log || !entry || entry->log_index == 0) {
    return 0;
  }
  if (log->prepared_phase == SMR_TX_PREPARED) {
    return 0;
  }
  if (entry->log_index != log->last_applied + 1 || log->count >= SMR_LOG_MAX) {
    return 0;
  }
  if (!smr_entry_checksum_ok(entry)) {
    return 0;
  }
  if (!entry_clone(&copy, entry)) {
    return 0;
  }
  log->prepared = copy;
  log->prepared_phase = SMR_TX_PREPARED;
  return 1;
}

smr_tx_phase_t smr_tx_decide_votes(const int *yes, size_t n) {
  size_t i;

  if (n == 0) {
    return SMR_TX_COMMITTED;
  }
  if (!yes) {
    return SMR_TX_ABORTED;
  }
  for (i = 0; i < n; i++) {
    if (!yes[i]) {
      return SMR_TX_ABORTED;
    }
  }
  return SMR_TX_COMMITTED;
}

smr_tx_phase_t smr_tx_decide_recover(const smr_tx_phase_t *phases, size_t n) {
  size_t i;

  if (!phases || n == 0) {
    return SMR_TX_ABORTED;
  }
  for (i = 0; i < n; i++) {
    if (phases[i] == SMR_TX_COMMITTED) {
      return SMR_TX_COMMITTED;
    }
  }
  return SMR_TX_ABORTED;
}

int smr_tx_finish(smr_log_t *log, metadata_table_t *table, smr_tx_phase_t decision) {
  int ok;

  if (!log || !table) {
    return 0;
  }
  if (decision == SMR_TX_ABORTED) {
    prepared_clear(log);
    return 1;
  }
  if (decision != SMR_TX_COMMITTED) {
    return 0;
  }
  if (log->prepared_phase != SMR_TX_PREPARED) {
    return 1;
  }
  ok = smr_log_apply(log, table, &log->prepared);
  prepared_clear(log);
  if (!ok) {
    return 0;
  }
  return 1;
}

int smr_ist_offer(const smr_log_t *log, uint64_t requester_last, smr_ist_offer_t *offer,
                  const smr_entry_t **missing, size_t missing_cap) {
  size_t i;
  size_t n;
  size_t total = 0;

  if (!log || !offer) {
    return 0;
  }
  n = log->count;
  if (n > SMR_LOG_MAX) {
    n = SMR_LOG_MAX;
  }
  offer->checkpoint_index = log->checkpoint_index;
  offer->source_last_applied = log->last_applied;
  for (i = 0; i < n; i++) {
    if (log->entries[i].log_index <= requester_last) {
      continue;
    }
    if (missing && total < missing_cap) {
      missing[total] = &log->entries[i];
    }
    total++;
  }
  offer->missing_count = total;
  return 1;
}

static int find_member(superpeer_t *sp, const node_id_t *id, member_t *out) {
  size_t i;

  pthread_mutex_lock(&sp->members_lock);
  for (i = 0; i < sp->members.count; i++) {
    if (node_id_cmp(&sp->members.entries[i].id, id) == 0) {
      *out = sp->members.entries[i];
      pthread_mutex_unlock(&sp->members_lock);
      return 1;
    }
  }
  pthread_mutex_unlock(&sp->members_lock);
  return 0;
}

/* 1 aplica aqui. 0 encaminha para remote. -1 não achou o coordenador. */
static int coordinator_local(superpeer_t *sp, int *broadcast, member_t *remote) {
  node_id_t id;
  int has;
  int leader;

  *broadcast = 0;
  memset(remote, 0, sizeof *remote);
  pthread_mutex_lock(&sp->coord_lock);
  has = sp->has_coordinator;
  id = sp->coordinator_id;
  leader = has && (sp->coord_role == COORD_LEADER || node_id_cmp(&id, &sp->self_id) == 0);
  if (leader) {
    *broadcast = 1;
  }
  pthread_mutex_unlock(&sp->coord_lock);
  if (!has || leader) {
    return 1;
  }
  if (!find_member(sp, &id, remote) || remote->port == 0) {
    return -1;
  }
  if (endpoint_is_self(sp, remote->ipv4, remote->port)) {
    return 1;
  }
  return 0;
}

/* Quem chama já segura metadata_lock e smr_lock. O pacote, se pedido, é cópia. */
static int seal_locked(superpeer_t *sp, smr_op_t op, const char *name,
                       const uint8_t object_id[METADATA_OBJECT_ID_SIZE], const file_metadata_t *meta,
                       uint8_t **packed_out, size_t *packed_len) {
  smr_entry_t entry;
  const file_metadata_t *cur;
  node_uuid_t trsc;
  uint32_t version;
  uint8_t *buf;
  ssize_t n;

  if (packed_out) {
    *packed_out = NULL;
    *packed_len = 0;
  }
  if ((op == SMR_OP_STORE || op == SMR_OP_UPDATE) && !meta) {
    return 0;
  }
  if (metadata_id_is_zero(object_id) || !metadata_name_ok(name)) {
    return 0;
  }
  if (sp->smr.count >= SMR_LOG_MAX) {
    return 0;
  }

  cur = metadata_table_find_id(&sp->metadata, object_id);
  if (op == SMR_OP_STORE || op == SMR_OP_UPDATE) {
    version = cur ? cur->version + 1 : meta->version;
  } else {
    version = cur ? cur->version + 1 : 1;
  }
  if (!node_uuid_random(&trsc)) {
    return 0;
  }

  memset(&entry, 0, sizeof entry);
  if (!copy_name(entry.name, name)) {
    return 0;
  }
  entry.operation = op;
  entry.log_index = sp->smr.last_applied + 1;
  entry.timestamp = entry.log_index;
  entry.version = version;
  memcpy(entry.transaction_id, trsc.bytes, SMR_TRSC_ID_SIZE);
  memcpy(entry.object_id, object_id, METADATA_OBJECT_ID_SIZE);
  if (op == SMR_OP_STORE || op == SMR_OP_UPDATE) {
    entry.stored = *meta;
    entry.stored.version = version;
  }
  if (!smr_entry_seal_checksum(&entry)) {
    return 0;
  }
  if (!smr_prepare_vote(&sp->smr, &entry)) {
    return 0;
  }
  if (!packed_out) {
    prepared_clear(&sp->smr);
    return 0;
  }

  buf = malloc((size_t)LOG_APPEND_PREFIX + METADATA_PAYLOAD_MAX);
  if (!buf) {
    prepared_clear(&sp->smr);
    return 0;
  }
  n = log_append_pack(&entry, buf, (size_t)LOG_APPEND_PREFIX + METADATA_PAYLOAD_MAX);
  if (n < 0) {
    free(buf);
    prepared_clear(&sp->smr);
    return 0;
  }
  *packed_out = buf;
  *packed_len = (size_t)n;
  return 1;
}

static int seal_local(superpeer_t *sp, smr_op_t op, const char *name,
                      const uint8_t object_id[METADATA_OBJECT_ID_SIZE], const file_metadata_t *meta,
                      uint8_t **packed_out, size_t *packed_len) {
  int ok;

  pthread_mutex_lock(&sp->metadata_lock);
  pthread_mutex_lock(&sp->smr_lock);
  ok = seal_locked(sp, op, name, object_id, meta, packed_out, packed_len);
  pthread_mutex_unlock(&sp->smr_lock);
  pthread_mutex_unlock(&sp->metadata_lock);
  return ok;
}

static int send_frame(int fd, const superpeer_t *sp, const pl_header *req, uint16_t type,
                      const uint8_t *body, size_t len) {
  uint8_t *frame;
  pl_header hdr;
  int rc;

  if (len > UINT32_MAX || (len > 0 && !body)) {
    return 0;
  }
  frame = malloc(HEADER_SIZE + len);
  if (!frame) {
    return 0;
  }
  if (req) {
    fill_reply_header(&hdr, req, &sp->self_id, type, (uint32_t)len);
  } else {
    memset(&hdr, 0, sizeof hdr);
    hdr.protocol_ver = PROTOCOL_VER;
    hdr.msg_type = type;
    hdr.time = (uint64_t)time(NULL);
    hdr.pl_size = (uint32_t)len;
    memcpy(hdr.src_node, sp->self_id.bytes, NODE_ID_SIZE);
  }
  rc = simple_send(fd, frame, len ? body : NULL, (uint32_t)len, &hdr);
  free(frame);
  return rc == NET_OK;
}

/* smr_lock já está solto. Tira o registro do cache e anuncia o nome instalado. */
static void notify_applied(superpeer_t *sp, const uint8_t object_id[METADATA_OBJECT_ID_SIZE],
                           const char *name) {
  if (sp && sp->lfu_invalidate && object_id && !metadata_id_is_zero(object_id)) {
    sp->lfu_invalidate(sp, object_id);
  }
  if (name && name[0] != '\0') {
    printf("UPLOAD %s\n", name);
    fflush(stdout);
  }
}

static int finish_locked(superpeer_t *sp, smr_tx_phase_t decision) {
  uint8_t object_id[METADATA_OBJECT_ID_SIZE];
  char name[METADATA_FILENAME_MAX];
  uint64_t before;
  uint64_t after;
  int ok;

  memset(object_id, 0, sizeof object_id);
  name[0] = '\0';
  pthread_mutex_lock(&sp->metadata_lock);
  pthread_mutex_lock(&sp->smr_lock);
  before = sp->smr.last_applied;
  if (sp->smr.prepared_phase == SMR_TX_PREPARED) {
    memcpy(object_id, sp->smr.prepared.object_id, METADATA_OBJECT_ID_SIZE);
    memcpy(name, sp->smr.prepared.name, METADATA_FILENAME_MAX);
    name[METADATA_FILENAME_MAX - 1] = '\0';
  }
  ok = smr_tx_finish(&sp->smr, &sp->metadata, decision);
  after = sp->smr.last_applied;
  pthread_mutex_unlock(&sp->smr_lock);
  pthread_mutex_unlock(&sp->metadata_lock);
  if (ok && after > before) {
    notify_applied(sp, object_id, name);
  }
  return ok;
}

/* Pausa só no coordenador, entre o último YES e o COMMIT. Sem a variável, é zero. */
static void pause_after_prepare(void) {
  const char *env = getenv("BT_PAUSE_AFTER_PREPARE");
  char *end = NULL;
  unsigned long seconds;

  if (!env || env[0] == '\0') {
    return;
  }
  seconds = strtoul(env, &end, 10);
  if (end == env || seconds == 0 || seconds > 3600UL) {
    return;
  }
  sleep((unsigned int)seconds);
}

/* Quem chama segura two_pc_mu. O slot já está PREPARED. */
static int round_two_pc(superpeer_t *sp, const uint8_t *packed, size_t packed_len) {
  member_t targets[MEMBER_TABLE_MAX_SIZE];
  int fds[MEMBER_TABLE_MAX_SIZE];
  int votes[MEMBER_TABLE_MAX_SIZE];
  smr_entry_t entry;
  uint8_t decision_body[DECISION_WIRE];
  size_t count = 0;
  size_t n_votes = 0;
  size_t i;
  smr_tx_phase_t decision;
  int ok;

  memset(&entry, 0, sizeof entry);
  for (i = 0; i < MEMBER_TABLE_MAX_SIZE; i++) {
    fds[i] = -1;
  }
  if (!log_append_unpack(&entry, packed, packed_len)) {
    finish_locked(sp, SMR_TX_ABORTED);
    printf("ABORT\n");
    fflush(stdout);
    return 0;
  }

  pthread_mutex_lock(&sp->members_lock);
  for (i = 0; i < sp->members.count; i++) {
    member_t *m = &sp->members.entries[i];

    if (m->node_type != SUPERPEER || m->state == MEMBER_FAILED || m->state == MEMBER_REMOVED ||
        m->state == MEMBER_SUSPECT) {
      continue;
    }
    if (node_id_cmp(&m->id, &sp->self_id) == 0 || endpoint_is_self(sp, m->ipv4, m->port)) {
      continue;
    }
    targets[count++] = *m;
  }
  pthread_mutex_unlock(&sp->members_lock);

  printf("PREPARE\n");
  fflush(stdout);
  for (i = 0; i < count; i++) {
    char ip[INET_ADDRSTRLEN];
    uint8_t reply[128];
    msg_t msg;
    int fd;
    int yes = 0;

    if (!inet_ntop(AF_INET, &targets[i].ipv4, ip, sizeof ip)) {
      printf("NO\n");
      votes[n_votes++] = 0;
      continue;
    }
    fd = net_connect(ip, targets[i].port);
    if (fd < 0 || !send_frame(fd, sp, NULL, PREPARE, packed, packed_len)) {
      if (fd >= 0) {
        net_close(fd);
      }
      printf("NO\n");
      votes[n_votes++] = 0;
      continue;
    }
    msg = simple_recv(fd, reply, sizeof reply);
    yes = msg.status == NET_OK && msg.header.msg_type == ACK;
    printf("%s\n", yes ? "YES" : "NO");
    votes[n_votes++] = yes;
    if (yes) {
      fds[i] = fd;
    } else {
      net_close(fd);
    }
  }
  fflush(stdout);

  decision = smr_tx_decide_votes(votes, n_votes);
  if (decision == SMR_TX_COMMITTED) {
    pause_after_prepare();
  }
  memcpy(decision_body, entry.transaction_id, SMR_TRSC_ID_SIZE);
  put_be64(decision_body + SMR_TRSC_ID_SIZE, entry.log_index);
  for (i = 0; i < count; i++) {
    uint8_t ackbuf[128];
    msg_t ack;
    uint16_t type = decision == SMR_TX_COMMITTED ? COMMIT : ABORT;

    if (fds[i] < 0) {
      continue;
    }
    if (send_frame(fds[i], sp, NULL, type, decision_body, DECISION_WIRE)) {
      ack = simple_recv(fds[i], ackbuf, sizeof ackbuf);
      (void)ack;
    }
    net_close(fds[i]);
  }

  ok = finish_locked(sp, decision);
  metadata_release(&entry.stored);
  if (decision == SMR_TX_COMMITTED && ok) {
    printf("COMMIT\n");
    fflush(stdout);
    return 1;
  }
  printf("ABORT\n");
  fflush(stdout);
  return 0;
}

/* Quem chama segura two_pc_mu e já deixou o slot PREPARED. */
static int finish_decision(superpeer_t *sp, const uint8_t *packed, size_t packed_len, int replicate) {
  int ok;

  if (replicate && packed && packed_len > 0) {
    return round_two_pc(sp, packed, packed_len);
  }
  printf("PREPARE\n");
  fflush(stdout);
  ok = finish_locked(sp, SMR_TX_COMMITTED);
  printf("%s\n", ok ? "COMMIT" : "ABORT");
  fflush(stdout);
  return ok;
}

static int coordinator_endpoint(superpeer_t *sp, uint32_t *ipv4, uint16_t *port) {
  node_id_t want;
  member_t member;
  size_t i;
  int has = 0;

  pthread_mutex_lock(&sp->coord_lock);
  if (sp->has_coordinator) {
    want = sp->coordinator_id;
    has = 1;
  }
  pthread_mutex_unlock(&sp->coord_lock);
  if (!has || node_id_cmp(&want, &sp->self_id) == 0) {
    return 0;
  }
  for (i = 0; i < sp->roster_count; i++) {
    node_id_t id;

    if (!node_id_from_endpoint(sp->roster[i].ipv4, sp->roster[i].port, &id)) {
      continue;
    }
    if (node_id_cmp(&id, &want) == 0) {
      *ipv4 = sp->roster[i].ipv4;
      *port = sp->roster[i].port;
      return *port != 0;
    }
  }
  if (!find_member(sp, &want, &member) || member.port == 0) {
    return 0;
  }
  *ipv4 = member.ipv4;
  *port = member.port;
  return 1;
}

static int apply_unlocked(superpeer_t *sp, const smr_entry_t *entry) {
  uint64_t before;
  uint64_t after;
  int ok;

  pthread_mutex_lock(&sp->metadata_lock);
  pthread_mutex_lock(&sp->smr_lock);
  before = sp->smr.last_applied;
  ok = smr_log_apply(&sp->smr, &sp->metadata, entry);
  after = sp->smr.last_applied;
  pthread_mutex_unlock(&sp->smr_lock);
  pthread_mutex_unlock(&sp->metadata_lock);
  if (ok && after > before) {
    notify_applied(sp, entry->object_id, entry->name);
  }
  return ok;
}

static void ist_from(superpeer_t *sp, const char *ip, uint16_t port) {
  uint64_t mine = 0;
  uint64_t checkpoint;
  uint64_t source_last;
  uint32_t missing;
  uint32_t i;
  uint8_t req[SNAPSHOT_REQ_LEN];
  uint8_t rep[64];
  uint8_t *frame = NULL;
  uint8_t *body = NULL;
  pl_header hdr;
  msg_t msg;
  int fd = -1;
  int attempt;
  int synced = 1;
  const size_t cap = (size_t)LOG_APPEND_PREFIX + METADATA_PAYLOAD_MAX;

  if (!sp || !ip || port == 0) {
    return;
  }
  pthread_mutex_lock(&sp->smr_lock);
  mine = sp->smr.last_applied;
  pthread_mutex_unlock(&sp->smr_lock);
  req[0] = 1;
  put_be64(req + 1, mine);

  for (attempt = 0; attempt < 5; attempt++) {
    fd = net_connect(ip, port);
    if (fd >= 0) {
      break;
    }
    usleep(200000);
  }
  if (fd < 0) {
    return;
  }
  frame = malloc(HEADER_SIZE + SNAPSHOT_REQ_LEN);
  memset(&hdr, 0, sizeof hdr);
  hdr.protocol_ver = PROTOCOL_VER;
  hdr.msg_type = SNAPSHOT;
  hdr.time = (uint64_t)time(NULL);
  hdr.pl_size = SNAPSHOT_REQ_LEN;
  memcpy(hdr.src_node, sp->self_id.bytes, NODE_ID_SIZE);
  if (!frame || simple_send(fd, frame, req, SNAPSHOT_REQ_LEN, &hdr) != NET_OK) {
    free(frame);
    net_close(fd);
    return;
  }
  free(frame);
  msg = simple_recv(fd, rep, sizeof rep);
  if (msg.status != NET_OK || msg.header.msg_type != SNAPSHOT || msg.header.pl_size < SNAPSHOT_REP_LEN ||
      rep[0] != 2) {
    net_close(fd);
    return;
  }
  checkpoint = get_be64(rep + 1);
  source_last = get_be64(rep + 9);
  missing = get_be32(rep + 17);
  if (missing > SMR_LOG_MAX) {
    net_close(fd);
    return;
  }
  printf("IST\n");
  printf("checkpoint LogIndex %llu\n", (unsigned long long)checkpoint);
  printf("version %llu -> %llu\n", (unsigned long long)mine, (unsigned long long)source_last);
  if (missing > 0) {
    printf("versions missing\n");
    printf("receive logs\n");
    printf("STATE_TRANSFER\n");
  }
  fflush(stdout);

  body = malloc(cap);
  if (!body) {
    net_close(fd);
    return;
  }
  for (i = 0; i < missing; i++) {
    smr_entry_t entry;
    int applied;

    msg = simple_recv(fd, body, (uint32_t)cap);
    if (msg.status != NET_OK || msg.header.msg_type != STATE_TRANSFER) {
      synced = 0;
      break;
    }
    memset(&entry, 0, sizeof entry);
    if (!log_append_unpack(&entry, body, msg.header.pl_size)) {
      synced = 0;
      break;
    }
    applied = apply_unlocked(sp, &entry);
    metadata_release(&entry.stored);
    if (!applied) {
      synced = 0;
      break;
    }
  }
  free(body);
  net_close(fd);
  if (missing > 0 && synced) {
    printf("READY\n");
    fflush(stdout);
  }
}

static void ist_sync(superpeer_t *sp) {
  uint32_t ipv4 = 0;
  uint16_t port = 0;
  char ip[INET_ADDRSTRLEN];

  if (!coordinator_endpoint(sp, &ipv4, &port) || !inet_ntop(AF_INET, &ipv4, ip, sizeof ip)) {
    return;
  }
  ist_from(sp, ip, port);
}

static int put_object_id(const char *name, const uint8_t *value, size_t value_len,
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

/* Índice 0 pede ao coordenador para selar. A entrada que volta é a que entra no log. */
static int forward_proposal(superpeer_t *sp, const member_t *coord, smr_op_t op, const char *name,
                            const uint8_t object_id[METADATA_OBJECT_ID_SIZE],
                            const file_metadata_t *meta) {
  smr_entry_t proposal;
  node_uuid_t trsc;
  uint8_t *packed = NULL;
  uint8_t *reply_buf = NULL;
  uint8_t *frame = NULL;
  smr_entry_t sealed;
  char ip[INET_ADDRSTRLEN];
  pl_header hdr;
  msg_t reply;
  ssize_t n;
  int fd = -1;
  int ok = 0;
  const size_t cap = (size_t)LOG_APPEND_PREFIX + METADATA_PAYLOAD_MAX;

  memset(&proposal, 0, sizeof proposal);
  memset(&sealed, 0, sizeof sealed);
  if (!copy_name(proposal.name, name) || !node_uuid_random(&trsc)) {
    return 0;
  }
  proposal.operation = op;
  memcpy(proposal.transaction_id, trsc.bytes, SMR_TRSC_ID_SIZE);
  memcpy(proposal.object_id, object_id, METADATA_OBJECT_ID_SIZE);
  if (op == SMR_OP_STORE || op == SMR_OP_UPDATE) {
    proposal.stored = *meta;
    proposal.stored.version = meta->version;
  }
  if (!smr_entry_seal_checksum(&proposal)) {
    return 0;
  }

  packed = malloc(cap);
  reply_buf = malloc(cap);
  if (!packed || !reply_buf) {
    if (fd >= 0) {
      net_close(fd);
    }
    free(frame);
    free(packed);
    free(reply_buf);
    return ok;
  }
  n = log_append_pack(&proposal, packed, cap);
  if (n < 0) {
    if (fd >= 0) {
      net_close(fd);
    }
    free(frame);
    free(packed);
    free(reply_buf);
    return ok;
  }
  if (!inet_ntop(AF_INET, &coord->ipv4, ip, sizeof ip)) {
    if (fd >= 0) {
      net_close(fd);
    }
    free(frame);
    free(packed);
    free(reply_buf);
    return ok;
  }
  fd = net_connect(ip, coord->port);
  if (fd < 0) {
    if (fd >= 0) {
      net_close(fd);
    }
    free(frame);
    free(packed);
    free(reply_buf);
    return ok;
  }
  frame = malloc(HEADER_SIZE + (size_t)n);
  if (!frame) {
    if (fd >= 0) {
      net_close(fd);
    }
    free(frame);
    free(packed);
    free(reply_buf);
    return ok;
  }
  memset(&hdr, 0, sizeof hdr);
  hdr.protocol_ver = PROTOCOL_VER;
  hdr.msg_type = LOG_APPEND;
  hdr.time = (uint64_t)time(NULL);
  hdr.pl_size = (uint32_t)n;
  memcpy(hdr.src_node, sp->self_id.bytes, NODE_ID_SIZE);
  if (simple_send(fd, frame, packed, (uint32_t)n, &hdr) != NET_OK) {
    if (fd >= 0) {
      net_close(fd);
    }
    free(frame);
    free(packed);
    free(reply_buf);
    return ok;
  }
  reply = simple_recv(fd, reply_buf, (uint32_t)cap);
  if (reply.status != NET_OK || reply.header.msg_type != LOG_APPEND) {
    if (fd >= 0) {
      net_close(fd);
    }
    free(frame);
    free(packed);
    free(reply_buf);
    return ok;
  }
  if (!log_append_unpack(&sealed, reply_buf, reply.header.pl_size)) {
    if (fd >= 0) {
      net_close(fd);
    }
    free(frame);
    free(packed);
    free(reply_buf);
    return ok;
  }
  ok = apply_unlocked(sp, &sealed);
  metadata_release(&sealed.stored);

  if (fd >= 0) {
    net_close(fd);
  }
  free(frame);
  free(packed);
  free(reply_buf);
  return ok;
}

static int propose(superpeer_t *sp, smr_op_t op, const char *name,
                   const uint8_t object_id[METADATA_OBJECT_ID_SIZE], const file_metadata_t *meta) {
  member_t remote;
  uint8_t *packed = NULL;
  size_t packed_len = 0;
  int broadcast = 0;
  int where;

  where = coordinator_local(sp, &broadcast, &remote);
  if (where < 0) {
    return 0;
  }
  if (where == 0) {
    return forward_proposal(sp, &remote, op, name, object_id, meta);
  }
  pthread_mutex_lock(&two_pc_mu);
  if (!seal_local(sp, op, name, object_id, meta, &packed, &packed_len)) {
    pthread_mutex_unlock(&two_pc_mu);
    return 0;
  }
  where = finish_decision(sp, packed, packed_len, broadcast);
  free(packed);
  pthread_mutex_unlock(&two_pc_mu);
  return where;
}

int smr_propose_store(struct superpeer *sp, const file_metadata_t *meta) {
  if (!sp || !meta || metadata_id_is_zero(meta->object_id) || !metadata_name_ok(meta->filename)) {
    return 0;
  }
  return propose(sp, SMR_OP_STORE, meta->filename, meta->object_id, meta);
}

int smr_propose_put(struct superpeer *sp, const char *name, const uint8_t *value, size_t value_len) {
  uint8_t object_id[METADATA_OBJECT_ID_SIZE];

  if (!sp || !metadata_name_ok(name) || (value_len > 0 && !value)) {
    return 0;
  }
  if (!put_object_id(name, value, value_len, object_id) || metadata_id_is_zero(object_id)) {
    return 0;
  }
  return propose(sp, SMR_OP_PUT, name, object_id, NULL);
}

static int peer_last_applied(superpeer_t *sp, const char *ip, uint16_t port, uint64_t *out) {
  uint64_t mine;
  uint8_t req[SNAPSHOT_REQ_LEN];
  uint8_t rep[64];
  uint8_t *frame;
  pl_header hdr;
  msg_t msg;
  int fd;
  int ok = 0;

  pthread_mutex_lock(&sp->smr_lock);
  mine = sp->smr.last_applied;
  pthread_mutex_unlock(&sp->smr_lock);
  req[0] = 1;
  put_be64(req + 1, mine);
  fd = net_connect(ip, port);
  if (fd < 0) {
    return 0;
  }
  frame = malloc(HEADER_SIZE + SNAPSHOT_REQ_LEN);
  memset(&hdr, 0, sizeof hdr);
  hdr.protocol_ver = PROTOCOL_VER;
  hdr.msg_type = SNAPSHOT;
  hdr.time = (uint64_t)time(NULL);
  hdr.pl_size = SNAPSHOT_REQ_LEN;
  memcpy(hdr.src_node, sp->self_id.bytes, NODE_ID_SIZE);
  if (frame && simple_send(fd, frame, req, SNAPSHOT_REQ_LEN, &hdr) == NET_OK) {
    msg = simple_recv(fd, rep, sizeof rep);
    if (msg.status == NET_OK && msg.header.msg_type == SNAPSHOT && msg.header.pl_size >= SNAPSHOT_REP_LEN &&
        rep[0] == 2) {
      *out = get_be64(rep + 9);
      ok = 1;
    }
  }
  free(frame);
  net_close(fd);
  return ok;
}

static void copy_live_peers(superpeer_t *sp, member_t *out, size_t *count) {
  size_t i;

  *count = 0;
  pthread_mutex_lock(&sp->members_lock);
  for (i = 0; i < sp->members.count; i++) {
    member_t *m = &sp->members.entries[i];

    if (m->node_type != SUPERPEER || m->state == MEMBER_FAILED || m->state == MEMBER_REMOVED ||
        m->state == MEMBER_SUSPECT) {
      continue;
    }
    if (node_id_cmp(&m->id, &sp->self_id) == 0 || endpoint_is_self(sp, m->ipv4, m->port)) {
      continue;
    }
    out[(*count)++] = *m;
  }
  pthread_mutex_unlock(&sp->members_lock);
}

/* Se algum vivo já aplicou mais longe, puxa o trecho. Senão não imprime IST. */
static void catch_up_peers(superpeer_t *sp) {
  member_t targets[MEMBER_TABLE_MAX_SIZE];
  size_t count = 0;
  size_t i;
  uint64_t mine;
  uint64_t best;
  char best_ip[INET_ADDRSTRLEN];
  uint16_t best_port = 0;

  pthread_mutex_lock(&sp->smr_lock);
  mine = sp->smr.last_applied;
  pthread_mutex_unlock(&sp->smr_lock);
  best = mine;
  copy_live_peers(sp, targets, &count);
  for (i = 0; i < count; i++) {
    char ip[INET_ADDRSTRLEN];
    uint64_t last = 0;

    if (!inet_ntop(AF_INET, &targets[i].ipv4, ip, sizeof ip)) {
      continue;
    }
    if (!peer_last_applied(sp, ip, targets[i].port, &last) || last <= best) {
      continue;
    }
    best = last;
    snprintf(best_ip, sizeof best_ip, "%s", ip);
    best_port = targets[i].port;
  }
  if (best_port != 0 && best > mine) {
    ist_from(sp, best_ip, best_port);
  }
}

static void broadcast_abort(superpeer_t *sp, const uint8_t tid[SMR_TRSC_ID_SIZE], uint64_t index) {
  member_t targets[MEMBER_TABLE_MAX_SIZE];
  uint8_t body[DECISION_WIRE];
  size_t count = 0;
  size_t i;

  memcpy(body, tid, SMR_TRSC_ID_SIZE);
  put_be64(body + SMR_TRSC_ID_SIZE, index);
  copy_live_peers(sp, targets, &count);
  for (i = 0; i < count; i++) {
    char ip[INET_ADDRSTRLEN];
    uint8_t reply[128];
    msg_t msg;
    int fd;

    if (!inet_ntop(AF_INET, &targets[i].ipv4, ip, sizeof ip)) {
      continue;
    }
    fd = net_connect(ip, targets[i].port);
    if (fd < 0) {
      continue;
    }
    if (send_frame(fd, sp, NULL, ABORT, body, DECISION_WIRE)) {
      msg = simple_recv(fd, reply, sizeof reply);
      (void)msg;
    }
    net_close(fd);
  }
}

/* O processo morto levou o slot. Sem COMMIT nos vivos, a rodada aborta. */
static void recover_prepared(superpeer_t *sp) {
  uint8_t tid[SMR_TRSC_ID_SIZE];
  uint64_t index = 0;
  int pending = 0;

  catch_up_peers(sp);
  pthread_mutex_lock(&sp->metadata_lock);
  pthread_mutex_lock(&sp->smr_lock);
  if (sp->smr.prepared_phase == SMR_TX_PREPARED && sp->smr.prepared.log_index <= sp->smr.last_applied) {
    prepared_clear(&sp->smr);
  } else if (sp->smr.prepared_phase == SMR_TX_PREPARED) {
    pending = 1;
    memcpy(tid, sp->smr.prepared.transaction_id, SMR_TRSC_ID_SIZE);
    index = sp->smr.prepared.log_index;
  }
  pthread_mutex_unlock(&sp->smr_lock);
  pthread_mutex_unlock(&sp->metadata_lock);
  if (!pending) {
    return;
  }
  broadcast_abort(sp, tid, index);
  finish_locked(sp, SMR_TX_ABORTED);
  printf("ABORT\n");
  fflush(stdout);
}

void smr_on_coordinator(struct superpeer *sp, const node_id_t *coordinator_id, int i_am_leader) {
  uint64_t idx;

  if (!sp) {
    return;
  }
  pthread_mutex_lock(&sp->coord_lock);
  if (!coordinator_id) {
    memset(&sp->coordinator_id, 0, sizeof sp->coordinator_id);
    sp->has_coordinator = 0;
    sp->coord_role = COORD_FOLLOWER;
    pthread_mutex_unlock(&sp->coord_lock);
    return;
  }
  sp->coordinator_id = *coordinator_id;
  sp->has_coordinator = 1;
  sp->coord_role = i_am_leader ? COORD_LEADER : COORD_FOLLOWER;
  pthread_mutex_unlock(&sp->coord_lock);
  if (!i_am_leader) {
    ist_sync(sp);
    return;
  }
  recover_prepared(sp);
  pthread_mutex_lock(&sp->smr_lock);
  sp->smr.checkpoint_index = sp->smr.last_applied;
  idx = sp->smr.checkpoint_index;
  pthread_mutex_unlock(&sp->smr_lock);
  printf("SMR log resumed LogIndex %llu\n", (unsigned long long)idx);
  printf("resume operations\n");
  fflush(stdout);
}

int smr_ingest_append(struct superpeer *sp, int conn, const pl_header *hdr, const uint8_t *payload,
                      size_t len) {
  smr_entry_t entry;
  ack_t ack;
  int ok;

  if (!sp || !hdr || !payload || conn < 0) {
    return 0;
  }
  memset(&entry, 0, sizeof entry);
  if (len != hdr->pl_size || !log_append_unpack(&entry, payload, len)) {
    send_error(conn, hdr, &sp->self_id, 1, "malformed LOG_APPEND");
    return 0;
  }

  if (entry.log_index == 0) {
    member_t remote;
    uint8_t *packed = NULL;
    size_t packed_len = 0;
    int broadcast = 0;
    int local = coordinator_local(sp, &broadcast, &remote);

    if (local != 1) {
      metadata_release(&entry.stored);
      send_error(conn, hdr, &sp->self_id, 1, "not coordinator");
      return 0;
    }
    pthread_mutex_lock(&two_pc_mu);
    if (entry.operation == SMR_OP_STORE || entry.operation == SMR_OP_UPDATE) {
      ok = seal_local(sp, entry.operation, entry.stored.filename, entry.stored.object_id,
                      &entry.stored, &packed, &packed_len);
    } else {
      ok = seal_local(sp, entry.operation, entry.name, entry.object_id, NULL, &packed, &packed_len);
    }
    metadata_release(&entry.stored);
    if (!ok || !packed) {
      free(packed);
      pthread_mutex_unlock(&two_pc_mu);
      send_error(conn, hdr, &sp->self_id, 1, "LOG_APPEND rejected");
      return 0;
    }
    ok = finish_decision(sp, packed, packed_len, broadcast);
    if (!ok) {
      free(packed);
      pthread_mutex_unlock(&two_pc_mu);
      send_error(conn, hdr, &sp->self_id, 1, "LOG_APPEND rejected");
      return 0;
    }
    ok = send_frame(conn, sp, hdr, LOG_APPEND, packed, packed_len);
    free(packed);
    pthread_mutex_unlock(&two_pc_mu);
    return ok;
  }

  ok = apply_unlocked(sp, &entry);
  metadata_release(&entry.stored);
  if (!ok) {
    send_error(conn, hdr, &sp->self_id, 1, "LOG_APPEND rejected");
    return 0;
  }
  memset(&ack, 0, sizeof ack);
  memcpy(ack.node_id, sp->self_id.bytes, NODE_ID_SIZE);
  return send_ack(conn, hdr, &sp->self_id, &ack) == NET_OK;
}

static int close_slot(superpeer_t *sp, const uint8_t *payload, size_t len, int commit) {
  uint8_t tid[SMR_TRSC_ID_SIZE];
  uint8_t object_id[METADATA_OBJECT_ID_SIZE];
  char name[METADATA_FILENAME_MAX];
  uint64_t index;
  uint64_t before;
  uint64_t after;
  int match;
  int ok;

  if (!payload || len != DECISION_WIRE) {
    return 0;
  }
  memset(object_id, 0, sizeof object_id);
  name[0] = '\0';
  memcpy(tid, payload, SMR_TRSC_ID_SIZE);
  index = get_be64(payload + SMR_TRSC_ID_SIZE);
  pthread_mutex_lock(&sp->metadata_lock);
  pthread_mutex_lock(&sp->smr_lock);
  before = sp->smr.last_applied;
  match = sp->smr.prepared_phase == SMR_TX_PREPARED && sp->smr.prepared.log_index == index &&
          memcmp(sp->smr.prepared.transaction_id, tid, SMR_TRSC_ID_SIZE) == 0;
  if (match) {
    memcpy(object_id, sp->smr.prepared.object_id, METADATA_OBJECT_ID_SIZE);
    memcpy(name, sp->smr.prepared.name, METADATA_FILENAME_MAX);
    name[METADATA_FILENAME_MAX - 1] = '\0';
  }
  ok = match && smr_tx_finish(&sp->smr, &sp->metadata, commit ? SMR_TX_COMMITTED : SMR_TX_ABORTED);
  after = sp->smr.last_applied;
  pthread_mutex_unlock(&sp->smr_lock);
  pthread_mutex_unlock(&sp->metadata_lock);
  if (ok && after > before) {
    notify_applied(sp, object_id, name);
  }
  return ok;
}

int smr_ingest_prepare(struct superpeer *sp, int conn, const pl_header *hdr, const uint8_t *payload,
                       size_t len) {
  smr_entry_t entry;
  ack_t ack;
  uint8_t next[MAX_CONTROL_PAYLOAD_SZ];
  msg_t msg;
  int ok;
  int commit;

  if (!sp || !hdr || !payload || conn < 0) {
    return 0;
  }
  memset(&entry, 0, sizeof entry);
  if (len != hdr->pl_size || !log_append_unpack(&entry, payload, len)) {
    send_error(conn, hdr, &sp->self_id, 1, "malformed PREPARE");
    return 0;
  }
  pthread_mutex_lock(&sp->metadata_lock);
  pthread_mutex_lock(&sp->smr_lock);
  ok = smr_prepare_vote(&sp->smr, &entry);
  pthread_mutex_unlock(&sp->smr_lock);
  pthread_mutex_unlock(&sp->metadata_lock);
  metadata_release(&entry.stored);
  if (!ok) {
    printf("NO\n");
    fflush(stdout);
    send_error(conn, hdr, &sp->self_id, 1, "NO");
    return 0;
  }
  memset(&ack, 0, sizeof ack);
  memcpy(ack.node_id, sp->self_id.bytes, NODE_ID_SIZE);
  if (send_ack(conn, hdr, &sp->self_id, &ack) != NET_OK) {
    return 0;
  }
  printf("YES\n");
  fflush(stdout);

  msg = recv_message(conn, next, sizeof next, NULL, 0);
  if (msg.status != NET_OK || (msg.header.msg_type != COMMIT && msg.header.msg_type != ABORT)) {
    return 0;
  }
  commit = msg.header.msg_type == COMMIT;
  if (!close_slot(sp, next, msg.header.pl_size, commit)) {
    send_error(conn, &msg.header, &sp->self_id, 1, "decision rejected");
    return 0;
  }
  memset(&ack, 0, sizeof ack);
  memcpy(ack.node_id, sp->self_id.bytes, NODE_ID_SIZE);
  return send_ack(conn, &msg.header, &sp->self_id, &ack) == NET_OK;
}

int smr_ingest_decision(struct superpeer *sp, int conn, const pl_header *hdr, const uint8_t *payload,
                        size_t len) {
  ack_t ack;
  int commit;

  if (!sp || !hdr || !payload || conn < 0 || (hdr->msg_type != COMMIT && hdr->msg_type != ABORT)) {
    return 0;
  }
  commit = hdr->msg_type == COMMIT;
  if (!close_slot(sp, payload, len, commit)) {
    send_error(conn, hdr, &sp->self_id, 1, "decision rejected");
    return 0;
  }
  printf("%s\n", commit ? "COMMIT" : "ABORT");
  fflush(stdout);
  memset(&ack, 0, sizeof ack);
  memcpy(ack.node_id, sp->self_id.bytes, NODE_ID_SIZE);
  return send_ack(conn, hdr, &sp->self_id, &ack) == NET_OK;
}

int smr_ingest_snapshot(struct superpeer *sp, int conn, const pl_header *hdr, const uint8_t *payload,
                        size_t len) {
  smr_ist_offer_t offer;
  const smr_entry_t *missing[SMR_LOG_MAX];
  uint8_t *packed[SMR_LOG_MAX];
  size_t packed_len[SMR_LOG_MAX];
  uint8_t reply[SNAPSHOT_REP_LEN];
  uint64_t requester_last;
  size_t ncopy = 0;
  size_t i;
  int ok = 0;

  if (!sp || !hdr || !payload || conn < 0) {
    return 0;
  }
  for (i = 0; i < SMR_LOG_MAX; i++) {
    packed[i] = NULL;
  }
  if (len < SNAPSHOT_REQ_LEN || payload[0] != 1) {
    send_error(conn, hdr, &sp->self_id, 1, "malformed SNAPSHOT");
    return 0;
  }
  requester_last = get_be64(payload + 1);
  pthread_mutex_lock(&sp->smr_lock);
  ok = smr_ist_offer(&sp->smr, requester_last, &offer, missing, SMR_LOG_MAX);
  if (ok) {
    ncopy = offer.missing_count;
    if (ncopy > SMR_LOG_MAX) {
      ok = 0;
    }
  }
  for (i = 0; ok && i < ncopy; i++) {
    uint8_t *buf = malloc((size_t)LOG_APPEND_PREFIX + METADATA_PAYLOAD_MAX);
    ssize_t n;

    if (!buf) {
      ok = 0;
      break;
    }
    n = log_append_pack(missing[i], buf, (size_t)LOG_APPEND_PREFIX + METADATA_PAYLOAD_MAX);
    if (n < 0) {
      free(buf);
      ok = 0;
      break;
    }
    packed[i] = buf;
    packed_len[i] = (size_t)n;
  }
  pthread_mutex_unlock(&sp->smr_lock);
  if (!ok) {
    for (i = 0; i < SMR_LOG_MAX; i++) {
      free(packed[i]);
    }
    send_error(conn, hdr, &sp->self_id, 1, "SNAPSHOT rejected");
    return 0;
  }

  reply[0] = 2;
  put_be64(reply + 1, offer.checkpoint_index);
  put_be64(reply + 9, offer.source_last_applied);
  put_be32(reply + 17, (uint32_t)offer.missing_count);
  if (!send_frame(conn, sp, hdr, SNAPSHOT, reply, SNAPSHOT_REP_LEN)) {
    for (i = 0; i < ncopy; i++) {
      free(packed[i]);
    }
    return 0;
  }
  for (i = 0; i < ncopy; i++) {
    int sent = send_frame(conn, sp, hdr, STATE_TRANSFER, packed[i], packed_len[i]);

    free(packed[i]);
    packed[i] = NULL;
    if (!sent) {
      for (; ++i < ncopy;) {
        free(packed[i]);
      }
      return 0;
    }
  }
  return 1;
}
