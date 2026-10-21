#include "superpeer/superpeer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/network.h"

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
  if (!smr_log_apply(&sp->smr, &sp->metadata, &entry)) {
    return 0;
  }
  if (!packed_out) {
    return 1;
  }

  buf = malloc((size_t)LOG_APPEND_PREFIX + METADATA_PAYLOAD_MAX);
  if (!buf) {
    return 1;
  }
  n = log_append_pack(&entry, buf, (size_t)LOG_APPEND_PREFIX + METADATA_PAYLOAD_MAX);
  if (n < 0) {
    free(buf);
    return 1;
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

static int send_packed(int fd, const superpeer_t *sp, const pl_header *req, const uint8_t *packed,
                       size_t packed_len) {
  uint8_t *frame;
  pl_header hdr;
  int rc;

  if (!packed || packed_len == 0 || packed_len > UINT32_MAX) {
    return 0;
  }
  frame = malloc(HEADER_SIZE + packed_len);
  if (!frame) {
    return 0;
  }
  if (req) {
    fill_reply_header(&hdr, req, &sp->self_id, LOG_APPEND, (uint32_t)packed_len);
  } else {
    memset(&hdr, 0, sizeof hdr);
    hdr.protocol_ver = PROTOCOL_VER;
    hdr.msg_type = LOG_APPEND;
    hdr.time = (uint64_t)time(NULL);
    hdr.pl_size = (uint32_t)packed_len;
    memcpy(hdr.src_node, sp->self_id.bytes, NODE_ID_SIZE);
  }
  rc = simple_send(fd, frame, packed, (uint32_t)packed_len, &hdr);
  free(frame);
  return rc == NET_OK;
}

static void broadcast_append(superpeer_t *sp, const uint8_t *packed, size_t packed_len) {
  member_t targets[MEMBER_TABLE_MAX_SIZE];
  size_t count = 0;
  size_t i;

  pthread_mutex_lock(&sp->members_lock);
  for (i = 0; i < sp->members.count; i++) {
    member_t *m = &sp->members.entries[i];

    if (m->node_type != SUPERPEER || m->state == MEMBER_FAILED || m->state == MEMBER_REMOVED) {
      continue;
    }
    if (node_id_cmp(&m->id, &sp->self_id) == 0 || endpoint_is_self(sp, m->ipv4, m->port)) {
      continue;
    }
    targets[count++] = *m;
  }
  pthread_mutex_unlock(&sp->members_lock);

  for (i = 0; i < count; i++) {
    char ip[INET_ADDRSTRLEN];
    uint8_t ackbuf[128];
    msg_t reply;
    int fd;

    if (!inet_ntop(AF_INET, &targets[i].ipv4, ip, sizeof ip)) {
      continue;
    }
    fd = net_connect(ip, targets[i].port);
    if (fd < 0) {
      continue;
    }
    if (send_packed(fd, sp, NULL, packed, packed_len)) {
      reply = simple_recv(fd, ackbuf, sizeof ackbuf);
      (void)reply;
    }
    net_close(fd);
  }
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
    goto done;
  }
  n = log_append_pack(&proposal, packed, cap);
  if (n < 0) {
    goto done;
  }
  if (!inet_ntop(AF_INET, &coord->ipv4, ip, sizeof ip)) {
    goto done;
  }
  fd = net_connect(ip, coord->port);
  if (fd < 0) {
    goto done;
  }
  frame = malloc(HEADER_SIZE + (size_t)n);
  if (!frame) {
    goto done;
  }
  memset(&hdr, 0, sizeof hdr);
  hdr.protocol_ver = PROTOCOL_VER;
  hdr.msg_type = LOG_APPEND;
  hdr.time = (uint64_t)time(NULL);
  hdr.pl_size = (uint32_t)n;
  memcpy(hdr.src_node, sp->self_id.bytes, NODE_ID_SIZE);
  if (simple_send(fd, frame, packed, (uint32_t)n, &hdr) != NET_OK) {
    goto done;
  }
  reply = simple_recv(fd, reply_buf, (uint32_t)cap);
  if (reply.status != NET_OK || reply.header.msg_type != LOG_APPEND) {
    goto done;
  }
  if (!log_append_unpack(&sealed, reply_buf, reply.header.pl_size)) {
    goto done;
  }
  pthread_mutex_lock(&sp->metadata_lock);
  pthread_mutex_lock(&sp->smr_lock);
  ok = smr_log_apply(&sp->smr, &sp->metadata, &sealed);
  pthread_mutex_unlock(&sp->smr_lock);
  pthread_mutex_unlock(&sp->metadata_lock);
  metadata_release(&sealed.stored);

done:
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
  if (!seal_local(sp, op, name, object_id, meta, &packed, &packed_len)) {
    return 0;
  }
  if (broadcast && packed) {
    broadcast_append(sp, packed, packed_len);
  }
  free(packed);
  return 1;
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
    return;
  }
  pthread_mutex_lock(&sp->smr_lock);
  sp->smr.checkpoint_index = sp->smr.last_applied;
  idx = sp->smr.checkpoint_index;
  pthread_mutex_unlock(&sp->smr_lock);
  printf("SMR checkpoint LogIndex %llu\n", (unsigned long long)idx);
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
    if (entry.operation == SMR_OP_STORE || entry.operation == SMR_OP_UPDATE) {
      ok = seal_local(sp, entry.operation, entry.stored.filename, entry.stored.object_id,
                      &entry.stored, &packed, &packed_len);
    } else {
      ok = seal_local(sp, entry.operation, entry.name, entry.object_id, NULL, &packed, &packed_len);
    }
    metadata_release(&entry.stored);
    if (!ok || !packed) {
      free(packed);
      send_error(conn, hdr, &sp->self_id, 1, "LOG_APPEND rejected");
      return 0;
    }
    if (broadcast) {
      broadcast_append(sp, packed, packed_len);
    }
    ok = send_packed(conn, sp, hdr, packed, packed_len);
    free(packed);
    return ok;
  }

  pthread_mutex_lock(&sp->metadata_lock);
  pthread_mutex_lock(&sp->smr_lock);
  ok = smr_log_apply(&sp->smr, &sp->metadata, &entry);
  pthread_mutex_unlock(&sp->smr_lock);
  pthread_mutex_unlock(&sp->metadata_lock);
  metadata_release(&entry.stored);
  if (!ok) {
    send_error(conn, hdr, &sp->self_id, 1, "LOG_APPEND rejected");
    return 0;
  }
  memset(&ack, 0, sizeof ack);
  memcpy(ack.node_id, sp->self_id.bytes, NODE_ID_SIZE);
  return send_ack(conn, hdr, &sp->self_id, &ack) == NET_OK;
}
