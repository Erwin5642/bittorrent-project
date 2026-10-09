#include <arpa/inet.h>
#include <netinet/in.h>

#include "../../include/superpeer/superpeer.h"
#include "common/config.h"
#include "common/network.h"
#include "common/node.h"
#include "peer/storage.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

void member_table_init(member_table_t *table) {
  if (!table) {
    return;
  }
  memset(table->entries, 0, sizeof table->entries);
  table->count = 0;
}

int member_table_update(member_table_t *table, const member_t *member) {
  size_t i;

  if (!table || !member) {
    return 0;
  }

  for (i = 0; i < table->count; i++) {
    if (table->entries[i].ipv4 == member->ipv4 && table->entries[i].port == member->port) {
      table->entries[i] = *member;
      return 1;
    }
  }

  if (table->count >= MEMBER_TABLE_MAX_SIZE) {
    return 0;
  }
  table->entries[table->count++] = *member;
  return 1;
}

int member_table_insert_new(member_table_t *table, const member_t *member) {
  size_t i;

  if (!table || !member) {
    return 0;
  }
  for (i = 0; i < table->count; i++) {
    if (table->entries[i].ipv4 == member->ipv4 && table->entries[i].port == member->port) {
      return 1;
    }
  }
  return member_table_update(table, member);
}

const member_t *member_table_find_id(const member_table_t *table, const node_id_t *node_id) {
  size_t i;

  if (!table || !node_id) {
    return NULL;
  }
  for (i = 0; i < table->count; i++) {
    if (node_id_cmp(&table->entries[i].id, node_id) == 0) {
      return &table->entries[i];
    }
  }
  return NULL;
}

const member_t *member_table_find_addr(const member_table_t *table, uint32_t ipv4, uint16_t port) {
  size_t i;

  if (!table) {
    return NULL;
  }
  for (i = 0; i < table->count; i++) {
    if (table->entries[i].ipv4 == ipv4 && table->entries[i].port == port) {
      return &table->entries[i];
    }
  }
  return NULL;
}

void member_table_print(const member_table_t *table) {
  size_t i;
  static const char *const states[] = {"ALIVE", "SUSPECT", "FAILED", "REMOVED"};

  if (!table) {
    return;
  }

  for (i = 0; i < table->count; i++) {
    const member_t *m = &table->entries[i];
    char ip[INET_ADDRSTRLEN];
    char hex[NODE_ID_HEX_SIZE];
    const char *type = m->node_type == SUPERPEER ? "SUPERPEER" : "PEER";
    const char *state = ((size_t)m->state < 4) ? states[m->state] : "?";

    if (!inet_ntop(AF_INET, &m->ipv4, ip, sizeof ip) || !node_id_to_hex(&m->id, hex, sizeof hex)) {
      continue;
    }
    printf("%s %s %u %s %s\n", type, ip, (unsigned)m->port, hex, state);
  }
}

int superpeer_init(superpeer_t *sp, const char *conf_path, uint16_t port, const char *name) {
  node_config_t cfg;
  member_t self;
  char ip[INET_ADDRSTRLEN];
  char hex[NODE_ID_HEX_SIZE];
  char roster_name[SUPERPEER_NAME_MAX];
  int fd;

  if (!sp || !conf_path) {
    return 0;
  }

  memset(sp, 0, sizeof *sp);
  sp->listend_fd = -1;
  roster_name[0] = '\0';

  if (!node_config_load(conf_path, &cfg) &&
      (port == 0 || !node_roster_load(conf_path, port, &cfg, roster_name, sizeof roster_name))) {
    fprintf(stderr, "superpeer_init: failed to load %s\n", conf_path);
    return 0;
  }
  if (cfg.node_type != SUPERPEER) {
    fprintf(stderr, "superpeer_init: type must be superpeer\n");
    return 0;
  }

  if (port != 0) {
    cfg.port = port;
  }
  if (name && name[0]) {
    strncpy(sp->name, name, sizeof sp->name - 1);
  } else if (roster_name[0]) {
    strncpy(sp->name, roster_name, sizeof sp->name - 1);
  } else {
    strncpy(sp->name, "superpeer", sizeof sp->name - 1);
  }

  sp->cfg = cfg;

  if (!node_id_from_endpoint(cfg.ipv4, cfg.port, &sp->self_id)) {
    fprintf(stderr, "superpeer_init: failed to generate NodeID\n");
    return 0;
  }

  if (!inet_ntop(AF_INET, &cfg.ipv4, ip, sizeof ip) ||
      !node_id_to_hex(&sp->self_id, hex, sizeof hex)) {
    return 0;
  }
  printf("Node %s started\n", sp->name);
  printf("NodeID: %s\n", hex);
  fflush(stdout);

  member_table_init(&sp->members);
  metadata_table_init(&sp->metadata);
  self.id = sp->self_id;
  self.ipv4 = cfg.ipv4;
  self.port = cfg.port;
  self.node_type = SUPERPEER;
  self.state = MEMBER_ALIVE;
  self.last_heartbeat = 0;
  self.version = 0;
  if (!member_table_update(&sp->members, &self)) {
    fprintf(stderr, "superpeer_init: failed to self insert superpeer\n");
    return 0;
  }

  if (pthread_mutex_init(&sp->members_lock, NULL) != 0) {
    fprintf(stderr, "superpeer_init: failed to init mutex\n");
    return 0;
  }
  if (pthread_mutex_init(&sp->metadata_lock, NULL) != 0) {
    fprintf(stderr, "superpeer_init: failed to init metadata mutex\n");
    pthread_mutex_destroy(&sp->members_lock);
    return 0;
  }
  if (!chord_init(&sp->chord)) {
    fprintf(stderr, "superpeer_init: failed to init chord\n");
    pthread_mutex_destroy(&sp->metadata_lock);
    pthread_mutex_destroy(&sp->members_lock);
    return 0;
  }
  if (!chord_create(&sp->chord, &sp->self_id, cfg.ipv4, cfg.port)) {
    fprintf(stderr, "superpeer_init: failed to create chord ring\n");
    chord_shutdown(&sp->chord);
    pthread_mutex_destroy(&sp->metadata_lock);
    pthread_mutex_destroy(&sp->members_lock);
    return 0;
  }
  fd = net_listen(cfg.port);
  if (fd < 0) {
    chord_shutdown(&sp->chord);
    pthread_mutex_destroy(&sp->metadata_lock);
    pthread_mutex_destroy(&sp->members_lock);
    return 0;
  }
  sp->listend_fd = fd;
  return 1;
}

static void fill_join_error(error_t *err, uint32_t code, const char *reason) {
  memset(err, 0, sizeof *err);
  err->code = code;
  if (reason) {
    strncpy((char *)err->reason, reason, sizeof err->reason - 1);
  }
}

int superpeer_handle_join(superpeer_t *sp, const pl_header *hdr, const join_t *join, ack_t *ack, error_t *err) {
  node_id_t jid;
  member_t member;
  const member_t *existing;
  uint8_t zero_id[NODE_ID_SIZE];

  if (!sp || !hdr || !join || !ack || !err) {
    if (err) {
      fill_join_error(err, 1, "null argument");
    }
    return 0;
  }

  memset(ack, 0, sizeof *ack);
  memset(err, 0, sizeof *err);
  memset(zero_id, 0, sizeof zero_id);
  memcpy(jid.bytes, join->node_id, NODE_ID_SIZE);

  if (hdr->msg_type != JOIN) {
    fill_join_error(err, 3, "unsupported message type");
    return 0;
  }
  if (memcmp(join->node_id, hdr->src_node, NODE_ID_SIZE) != 0 ||
      memcmp(join->node_id, zero_id, NODE_ID_SIZE) == 0) {
    fill_join_error(err, 1, "malformed JOIN payload");
    return 0;
  }
  if (join->node_type != PEER && join->node_type != SUPERPEER) {
    fill_join_error(err, 4, "invalid node_type");
    return 0;
  }
  if (join->port == 0 || join->ipv4 == 0) {
    fill_join_error(err, 1, "malformed JOIN payload");
    return 0;
  }

  existing = member_table_find_addr(&sp->members, join->ipv4, join->port);
  member.id = jid;
  member.ipv4 = join->ipv4;
  member.port = join->port;
  member.node_type = join->node_type == SUPERPEER ? SUPERPEER : PEER;
  member.state = MEMBER_ALIVE;
  member.last_heartbeat = 0;
  if (existing) {
    member.version = existing->version + 1;
  } else {
    member.version = 0;
  }

  if (!member_table_update(&sp->members, &member)) {
    fill_join_error(err, 2, "member table full");
    return 0;
  }

  memcpy(ack->node_id, jid.bytes, NODE_ID_SIZE);
  return 1;
}

/* Saída de verificação do CP2: File, Size, ObjectID, Chunks, Chunk i. */
static void metadata_record_print(const file_metadata_t *meta) {
  node_id_t id;
  char hex[NODE_ID_HEX_SIZE];
  uint32_t i;

  if (!meta) {
    return;
  }
  memcpy(id.bytes, meta->object_id, NODE_ID_SIZE);
  if (!node_id_to_hex(&id, hex, sizeof hex)) {
    return;
  }
  printf("File: %s\n", meta->filename);
  printf("Size: %llu bytes\n", (unsigned long long)meta->size);
  printf("ObjectID: %s\n", hex);
  printf("Chunks: %u\n", meta->chunk_count);
  for (i = 0; i < meta->chunk_count; i++) {
    memcpy(id.bytes, meta->chunk_hashes + (size_t)i * METADATA_CHUNK_HASH_SIZE, NODE_ID_SIZE);
    if (!node_id_to_hex(&id, hex, sizeof hex)) {
      continue;
    }
    printf("Chunk %u: %s\n", i, hex);
  }
  fflush(stdout);
}

int superpeer_handle_store(superpeer_t *sp, const pl_header *hdr, const uint8_t *payload,
                           size_t payload_len, ack_t *ack, error_t *err) {
  file_metadata_t meta;

  if (!sp || !hdr || !payload || !ack || !err) {
    if (err) {
      fill_join_error(err, 1, "null argument");
    }
    return 0;
  }

  memset(ack, 0, sizeof *ack);
  memset(err, 0, sizeof *err);

  if (hdr->msg_type != STORE) {
    fill_join_error(err, 3, "unsupported message type");
    return 0;
  }

  metadata_init(&meta);
  if (!metadata_unpack(&meta, payload, payload_len)) {
    fill_join_error(err, 1, "malformed STORE payload");
    return 0;
  }
  if (metadata_id_is_zero(meta.owner.bytes) ||
      memcmp(meta.owner.bytes, hdr->src_node, NODE_ID_SIZE) != 0) {
    metadata_release(&meta);
    fill_join_error(err, 1, "malformed STORE payload");
    return 0;
  }

  if (!metadata_table_put(&sp->metadata, &meta)) {
    const int full = metadata_table_find_id(&sp->metadata, meta.object_id) == NULL &&
                     sp->metadata.count >= METADATA_TABLE_MAX;
    metadata_release(&meta);
    if (full) {
      fill_join_error(err, 6, "metadata table full");
    } else {
      fill_join_error(err, 1, "malformed STORE payload");
    }
    return 0;
  }

  const file_metadata_t* stored = metadata_table_find_id(&sp->metadata, meta.object_id);
  metadata_record_print(stored);
  metadata_release(&meta);
  memcpy(ack->node_id, hdr->src_node, NODE_ID_SIZE);
  return 1;
}

int superpeer_handle_leave(superpeer_t *sp, const pl_header *hdr, const leave_t *leave, ack_t *ack) {
  node_id_t nid;
  size_t i;

  if (!sp || !hdr || !leave || !ack) {
    return 0;
  }
  (void)hdr;

  memset(ack, 0, sizeof *ack);
  memcpy(nid.bytes, leave->node_id, NODE_ID_SIZE);
  memcpy(ack->node_id, leave->node_id, NODE_ID_SIZE);

  for (i = 0; i < sp->members.count; i++) {
    if (node_id_cmp(&sp->members.entries[i].id, &nid) == 0) {
      sp->members.entries[i].state = MEMBER_REMOVED;
      break;
    }
  }
  return 1;
}

static int send_pong(int fd, const pl_header *req, const node_id_t *self) {
  uint8_t buf[HEADER_SIZE];
  pl_header hdr;

  if (!req || !self) {
    return NET_ERROR;
  }
  fill_reply_header(&hdr, req, self, PONG, 0);
  return send_message(fd, buf, sizeof buf, NULL, &hdr);
}

static void sleep_ms(unsigned ms) {
  struct timespec ts;

  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

/** Chunks ficam no storage do processo; o peer só guarda o metadado na tabela. */
#define SUPERPEER_STORAGE_ROOT "data/storage"

typedef struct {
  superpeer_t *sp;
  int conn;
  struct sockaddr_in peer_addr;
} conn_job_t;

/* Tamanho original do chunk `index` a partir do tamanho do arquivo. */
static uint32_t chunk_plain_len(const file_metadata_t *meta, uint32_t index) {
  uint64_t off;
  uint64_t remain;

  if (!meta || (uint64_t)index * CHUNK_PLAIN_MAX >= meta->size)
    return 0;
  off = (uint64_t)index * CHUNK_PLAIN_MAX;
  remain = meta->size - off;
  if (remain > CHUNK_PLAIN_MAX)
    remain = CHUNK_PLAIN_MAX;
  return (uint32_t)remain;
}

/* Envia um payload cru (STORE ou DOWNLOAD_REP) como resposta. */
static int send_raw(int fd, const pl_header *req, const node_id_t *self, uint16_t type,
                    const uint8_t *payload, uint32_t len) {
  uint8_t *buf;
  pl_header hdr;
  int rc;

  buf = malloc((size_t)HEADER_SIZE + (size_t)len);
  if (!buf)
    return NET_ERROR;
  fill_reply_header(&hdr, req, self, type, len);
  rc = simple_send(fd, buf, payload, len, &hdr);
  free(buf);
  return rc;
}

/* LOOKUP: nome -> STORE com o metadado, ou erro. Quem chama segura metadata_lock. */
static int superpeer_handle_lookup(superpeer_t *sp, const uint8_t *payload, size_t payload_len,
                                   uint8_t *out, size_t out_cap, size_t *out_len, error_t *err) {
  char name[METADATA_FILENAME_MAX];
  const file_metadata_t *found;
  ssize_t n;

  if (!sp || !payload || !out || !out_len || !err) {
    if (err)
      fill_join_error(err, 1, "null argument");
    return 0;
  }
  if (!lookup_unpack(name, sizeof name, payload, payload_len)) {
    fill_join_error(err, 1, "malformed LOOKUP payload");
    return 0;
  }
  found = metadata_table_find_name(&sp->metadata, name);
  if (!found) {
    fill_join_error(err, 1, "not found");
    return 0;
  }
  n = metadata_pack(found, out, out_cap);
  if (n < 0) {
    fill_join_error(err, 1, "malformed STORE payload");
    return 0;
  }
  *out_len = (size_t)n;
  return 1;
}

/* DOWNLOAD_REP entrante: grava o bloco LZ4 e responde ACK. */
static int superpeer_handle_chunk_put(superpeer_t *sp, const pl_header *hdr, const uint8_t *payload,
                                      size_t payload_len, ack_t *ack, error_t *err) {
  uint8_t object_id[METADATA_OBJECT_ID_SIZE];
  uint32_t index = 0;
  uint32_t original_len = 0;
  uint32_t comp_len = 0;
  const uint8_t *comp = NULL;
  const file_metadata_t *meta;

  if (!sp || !hdr || !payload || !ack || !err) {
    if (err)
      fill_join_error(err, 1, "null argument");
    return 0;
  }
  memset(ack, 0, sizeof *ack);
  if (!download_rep_unpack(object_id, &index, &original_len, &comp, &comp_len, payload, payload_len)) {
    fill_join_error(err, 1, "malformed DOWNLOAD_REP payload");
    return 0;
  }
  meta = metadata_table_find_id(&sp->metadata, object_id);
  if (!meta || index >= meta->chunk_count || original_len != chunk_plain_len(meta, index)) {
    fill_join_error(err, 1, "not found");
    return 0;
  }
  if (!storage_put_chunk(SUPERPEER_STORAGE_ROOT, object_id, index, comp, comp_len)) {
    fill_join_error(err, 1, "storage write failed");
    return 0;
  }
  memcpy(ack->node_id, hdr->src_node, NODE_ID_SIZE);
  return 1;
}

/* DOWNLOAD_REQ: devolve o bloco gravado, ainda comprimido. */
static int superpeer_handle_chunk_get(superpeer_t *sp, const uint8_t *payload, size_t payload_len,
                                      uint8_t *out, size_t out_cap, size_t *out_len, error_t *err) {
  uint8_t object_id[METADATA_OBJECT_ID_SIZE];
  uint32_t index = 0;
  uint32_t plain;
  const file_metadata_t *meta;
  long stored;
  uint8_t *comp = NULL;
  size_t comp_len = 0;
  ssize_t n;
  int ok = 0;

  if (!sp || !payload || !out || !out_len || !err) {
    if (err)
      fill_join_error(err, 1, "null argument");
    return 0;
  }
  if (!download_req_unpack(object_id, &index, payload, payload_len)) {
    fill_join_error(err, 1, "malformed DOWNLOAD_REQ payload");
    return 0;
  }
  meta = metadata_table_find_id(&sp->metadata, object_id);
  plain = meta ? chunk_plain_len(meta, index) : 0;
  if (!meta || index >= meta->chunk_count || plain == 0) {
    fill_join_error(err, 1, "not found");
    return 0;
  }
  stored = storage_chunk_size(SUPERPEER_STORAGE_ROOT, object_id, index);
  if (stored <= 0) {
    fill_join_error(err, 1, "chunk missing");
    return 0;
  }
  comp = malloc((size_t)stored);
  if (!comp) {
    fill_join_error(err, 1, "out of memory");
    return 0;
  }
  if (!storage_get_chunk(SUPERPEER_STORAGE_ROOT, object_id, index, comp, (size_t)stored, &comp_len) ||
      comp_len != (size_t)stored || comp_len > UINT32_MAX) {
    fill_join_error(err, 1, "chunk missing");
    goto done;
  }
  n = download_rep_pack(object_id, index, plain, comp, (uint32_t)comp_len, out, out_cap);
  if (n < 0) {
    fill_join_error(err, 1, "malformed DOWNLOAD_REP payload");
    goto done;
  }
  *out_len = (size_t)n;
  ok = 1;
done:
  free(comp);
  return ok;
}

typedef union {
  join_t join;
  leave_t leave;
  ack_t ack;
  error_t error;
  chord_peer_t notify;
} control_payload_t;

static void peer_from_node(chord_peer_t *dst, const chord_node_t *src) {
  memset(dst, 0, sizeof *dst);
  memcpy(dst->id, src->id.bytes, NODE_ID_SIZE);
  dst->ipv4 = src->ipv4;
  dst->port = src->port;
}

static chord_node_t node_from_peer(const chord_peer_t *src) {
  chord_node_t node;

  memset(&node, 0, sizeof node);
  memcpy(node.id.bytes, src->id, NODE_ID_SIZE);
  node.ipv4 = src->ipv4;
  node.port = src->port;
  node.valid = 1;
  return node;
}

static void handle_connection(superpeer_t *sp, int conn, struct sockaddr_in peer_addr) {
  uint8_t *in_buf = malloc(MAX_DATA_PAYLOAD_SZ);
  control_payload_t payload;
  msg_t msg;

  if (!in_buf) {
    net_close(conn);
    return;
  }
  memset(&payload, 0, sizeof payload);
  msg = recv_message(conn, in_buf, MAX_DATA_PAYLOAD_SZ, &payload, sizeof payload);
  if (msg.status != NET_OK) {
    free(in_buf);
    net_close(conn);
    return;
  }

  if (msg.header.msg_type == PING) {
    printf("RX PING\n");
    fflush(stdout);
    send_pong(conn, &msg.header, &sp->self_id);
  } else if (msg.header.msg_type == JOIN) {
    ack_t ack;
    error_t err;
    int ok;

    if (payload.join.ipv4 == 0) {
      payload.join.ipv4 = peer_addr.sin_addr.s_addr;
    }

    memset(&ack, 0, sizeof ack);
    memset(&err, 0, sizeof err);
    pthread_mutex_lock(&sp->members_lock);
    ok = superpeer_handle_join(sp, &msg.header, &payload.join, &ack, &err);
    if (ok) {
      member_table_print(&sp->members);
    }
    pthread_mutex_unlock(&sp->members_lock);
    if (ok) {
      send_ack(conn, &msg.header, &sp->self_id, &ack);
    } else {
      if (err.code == 0) {
        err.code = 1;
      }
      send_error(conn, &msg.header, &sp->self_id, err.code,
                 err.reason[0] ? (const char *)err.reason : "JOIN rejected");
    }
  } else if (msg.header.msg_type == LEAVE) {
    ack_t ack;

    memset(&ack, 0, sizeof ack);
    pthread_mutex_lock(&sp->members_lock);
    superpeer_handle_leave(sp, &msg.header, &payload.leave, &ack);
    pthread_mutex_unlock(&sp->members_lock);
    send_ack(conn, &msg.header, &sp->self_id, &ack);
  } else if (msg.header.msg_type == STORE) {
    ack_t ack;
    error_t err;
    int ok;

    memset(&ack, 0, sizeof ack);
    memset(&err, 0, sizeof err);
    pthread_mutex_lock(&sp->metadata_lock);
    ok = superpeer_handle_store(sp, &msg.header, in_buf, msg.header.pl_size, &ack, &err);
    pthread_mutex_unlock(&sp->metadata_lock);
    if (ok) {
      send_ack(conn, &msg.header, &sp->self_id, &ack);
    } else {
      if (err.code == 0) {
        err.code = 1;
      }
      send_error(conn, &msg.header, &sp->self_id, err.code,
                 err.reason[0] ? (const char *)err.reason : "STORE rejected");
    }
  } else if (msg.header.msg_type == LOOKUP) {
    uint8_t reply[METADATA_PAYLOAD_MAX];
    size_t reply_len = 0;
    error_t err;
    int ok;

    memset(&err, 0, sizeof err);
    pthread_mutex_lock(&sp->metadata_lock);
    ok = superpeer_handle_lookup(sp, in_buf, msg.header.pl_size, reply, sizeof reply,
                                 &reply_len, &err);
    pthread_mutex_unlock(&sp->metadata_lock);
    if (ok) {
      send_raw(conn, &msg.header, &sp->self_id, STORE, reply, (uint32_t)reply_len);
    } else {
      send_error(conn, &msg.header, &sp->self_id, err.code ? err.code : 1,
                 err.reason[0] ? (const char *)err.reason : "LOOKUP rejected");
    }
  } else if (msg.header.msg_type == DOWNLOAD_REP) {
    ack_t ack;
    error_t err;
    int ok;

    memset(&ack, 0, sizeof ack);
    memset(&err, 0, sizeof err);
    pthread_mutex_lock(&sp->metadata_lock);
    ok = superpeer_handle_chunk_put(sp, &msg.header, in_buf, msg.header.pl_size, &ack, &err);
    pthread_mutex_unlock(&sp->metadata_lock);
    if (ok) {
      send_ack(conn, &msg.header, &sp->self_id, &ack);
    } else {
      send_error(conn, &msg.header, &sp->self_id, err.code ? err.code : 1,
                 err.reason[0] ? (const char *)err.reason : "DOWNLOAD_REP rejected");
    }
  } else if (msg.header.msg_type == DOWNLOAD_REQ) {
    uint8_t *reply = malloc(MAX_DATA_PAYLOAD_SZ);
    size_t reply_len = 0;
    error_t err;
    int ok = 0;

    memset(&err, 0, sizeof err);
    if (reply) {
      pthread_mutex_lock(&sp->metadata_lock);
      ok = superpeer_handle_chunk_get(sp, in_buf, msg.header.pl_size, reply, MAX_DATA_PAYLOAD_SZ,
                                     &reply_len, &err);
      pthread_mutex_unlock(&sp->metadata_lock);
    }
    if (ok) {
      send_raw(conn, &msg.header, &sp->self_id, DOWNLOAD_REP, reply, (uint32_t)reply_len);
    } else {
      send_error(conn, &msg.header, &sp->self_id, err.code ? err.code : 1,
                 err.reason[0] ? (const char *)err.reason : "DOWNLOAD_REQ rejected");
    }
    free(reply);
  } else if (msg.header.msg_type == CLOSEST_PRECEDING) {
    node_id_t key;
    chord_node_t step;
    chord_peer_t peer;
    uint8_t reply[CHORD_CLOSEST_REP_SIZE];
    int done = 0;
    ssize_t n;

    if (msg.header.pl_size != CHORD_CLOSEST_REQ_SIZE) {
      send_error(conn, &msg.header, &sp->self_id, 1, "malformed CLOSEST_PRECEDING payload");
    } else {
      memcpy(key.bytes, in_buf, NODE_ID_SIZE);
      if (!chord_lookup_step(&sp->chord, &key, &done, &step)) {
        send_error(conn, &msg.header, &sp->self_id, 1, "chord lookup failed");
      } else {
        peer_from_node(&peer, &step);
        n = chord_closest_reply_pack(done ? (uint8_t)CHORD_STEP_DONE : (uint8_t)CHORD_STEP_NEXT,
                                     &peer, reply, sizeof reply);
        if (n < 0) {
          send_error(conn, &msg.header, &sp->self_id, 1, "malformed CLOSEST_PRECEDING payload");
        } else {
          send_raw(conn, &msg.header, &sp->self_id, CLOSEST_PRECEDING, reply, (uint32_t)n);
        }
      }
    }
  } else if (msg.header.msg_type == GET_PREDECESSOR) {
    chord_node_t pred;

    if (msg.header.pl_size != 0) {
      send_error(conn, &msg.header, &sp->self_id, 1, "malformed GET_PREDECESSOR payload");
    } else if (!chord_copy_predecessor(&sp->chord, &pred)) {
      send_error(conn, &msg.header, &sp->self_id, 1, "chord lookup failed");
    } else if (!pred.valid) {
      send_raw(conn, &msg.header, &sp->self_id, GET_PREDECESSOR, NULL, 0);
    } else {
      chord_peer_t peer;
      uint8_t reply[CHORD_NODE_WIRE_SIZE];

      peer_from_node(&peer, &pred);
      if (chord_peer_pack(&peer, reply) != 0) {
        send_error(conn, &msg.header, &sp->self_id, 1, "malformed GET_PREDECESSOR payload");
      } else {
        send_raw(conn, &msg.header, &sp->self_id, GET_PREDECESSOR, reply, CHORD_NODE_WIRE_SIZE);
      }
    }
  } else if (msg.header.msg_type == GET_SUCCESSORS) {
    chord_node_t nodes[CHORD_SUCCESSORS_MAX];
    chord_peer_t peers[CHORD_SUCCESSORS_MAX];
    uint8_t reply[1u + CHORD_SUCCESSORS_MAX * CHORD_NODE_WIRE_SIZE];
    unsigned count = 0;
    unsigned i;
    ssize_t n;

    if (msg.header.pl_size != 0) {
      send_error(conn, &msg.header, &sp->self_id, 1, "malformed GET_SUCCESSORS payload");
    } else if (!chord_copy_successors(&sp->chord, nodes, CHORD_SUCCESSORS_MAX, &count)) {
      send_error(conn, &msg.header, &sp->self_id, 1, "chord lookup failed");
    } else {
      for (i = 0; i < count; i++) {
        peer_from_node(&peers[i], &nodes[i]);
      }
      n = chord_successors_pack(count ? peers : NULL, count, reply, sizeof reply);
      if (n < 0) {
        send_error(conn, &msg.header, &sp->self_id, 1, "malformed GET_SUCCESSORS payload");
      } else {
        send_raw(conn, &msg.header, &sp->self_id, GET_SUCCESSORS, reply, (uint32_t)n);
      }
    }
  } else if (msg.header.msg_type == NOTIFY) {
    ack_t ack;
    chord_node_t candidate;

    candidate = node_from_peer(&payload.notify);
    chord_apply_notify(&sp->chord, &candidate);
    memset(&ack, 0, sizeof ack);
    memcpy(ack.node_id, payload.notify.id, NODE_ID_SIZE);
    send_ack(conn, &msg.header, &sp->self_id, &ack);
  } else {
    send_error(conn, &msg.header, &sp->self_id, 3, "unsupported message type");
  }

  free(in_buf);
  net_close(conn);
}

static void *connection_worker(void *arg) {
  conn_job_t *job = arg;

  handle_connection(job->sp, job->conn, job->peer_addr);
  free(job);
  return NULL;
}

#define CHORD_MAINT_PERIOD_MS 1000u
#define CHORD_JOIN_RETRY_MS 500u
#define CHORD_JOIN_TIMEOUT_MS 10000u

static uint64_t mono_ms(void) {
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return (uint64_t)time(NULL) * 1000u;
  }
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int endpoint_is_self(const superpeer_t *sp, uint32_t ipv4, uint16_t port) {
  return sp && ipv4 == sp->cfg.ipv4 && port == sp->cfg.port;
}

static int connect_node(const chord_node_t *node) {
  char ip[INET_ADDRSTRLEN];

  if (!node || !node->valid || node->port == 0) {
    return -1;
  }
  if (!inet_ntop(AF_INET, &node->ipv4, ip, sizeof ip)) {
    return -1;
  }
  return net_connect(ip, node->port);
}

static void log_successor(const chord_node_t *node) {
  char ip[INET_ADDRSTRLEN];

  if (!node || !node->valid || !inet_ntop(AF_INET, &node->ipv4, ip, sizeof ip)) {
    return;
  }
  printf("Successor -> %s:%u\nFinger[0] -> %s:%u\n", ip, (unsigned)node->port, ip, (unsigned)node->port);
  fflush(stdout);
}

static void install_successor_list(superpeer_t *sp, const chord_node_t *list, unsigned count) {
  chord_node_t installed;
  unsigned n = 0;
  int changed = 0;

  if (!chord_install_successors(&sp->chord, list, count, &changed) || !changed) {
    return;
  }
  if (chord_copy_successors(&sp->chord, &installed, 1, &n) && n == 1) {
    log_successor(&installed);
  }
}

static void remember_superpeers(superpeer_t *sp, const chord_node_t *nodes, unsigned count) {
  unsigned i;

  pthread_mutex_lock(&sp->members_lock);
  for (i = 0; i < count; i++) {
    member_t member;
    const chord_node_t *node = &nodes[i];

    if (!node->valid || node->port == 0 || endpoint_is_self(sp, node->ipv4, node->port)) {
      continue;
    }
    if (member_table_find_addr(&sp->members, node->ipv4, node->port)) {
      continue;
    }
    memset(&member, 0, sizeof member);
    member.id = node->id;
    if (node_id_cmp(&member.id, &(node_id_t){0}) == 0 &&
        !node_id_from_endpoint(node->ipv4, node->port, &member.id)) {
      continue;
    }
    member.ipv4 = node->ipv4;
    member.port = node->port;
    member.node_type = SUPERPEER;
    member.state = MEMBER_ALIVE;
    member.last_heartbeat = time(NULL);
    member_table_insert_new(&sp->members, &member);
  }
  pthread_mutex_unlock(&sp->members_lock);
}

static int rpc_raw(const chord_node_t *dest, const node_id_t *self, uint16_t type, const uint8_t *payload,
                   uint32_t len, uint8_t *reply, uint32_t cap, msg_t *out) {
  uint8_t *frame;
  pl_header hdr;
  int fd;
  int ok;

  if (!dest || !self || !reply || !out || (len > 0 && !payload)) {
    return 0;
  }
  frame = malloc((size_t)HEADER_SIZE + len);
  if (!frame) {
    return 0;
  }
  fd = connect_node(dest);
  if (fd < 0) {
    free(frame);
    return 0;
  }
  memset(&hdr, 0, sizeof hdr);
  hdr.protocol_ver = PROTOCOL_VER;
  hdr.msg_type = type;
  hdr.time = (uint64_t)time(NULL);
  hdr.pl_size = len;
  memcpy(hdr.src_node, self->bytes, NODE_ID_SIZE);
  ok = simple_send(fd, frame, payload, len, &hdr) == NET_OK;
  if (ok) {
    *out = simple_recv(fd, reply, cap);
    ok = out->status == NET_OK;
  }
  net_close(fd);
  free(frame);
  return ok;
}

static int rpc_closest(superpeer_t *sp, const chord_node_t *dest, const node_id_t *key, int *done,
                       chord_node_t *out) {
  uint8_t reply[CHORD_CLOSEST_REP_SIZE];
  msg_t msg;
  uint8_t status = 0;
  chord_peer_t peer;

  if (endpoint_is_self(sp, dest->ipv4, dest->port)) {
    return chord_lookup_step(&sp->chord, key, done, out);
  }
  if (!rpc_raw(dest, &sp->self_id, CLOSEST_PRECEDING, key->bytes, NODE_ID_SIZE, reply, sizeof reply, &msg)) {
    return 0;
  }
  if (msg.header.msg_type != CLOSEST_PRECEDING ||
      !chord_closest_reply_unpack(&status, &peer, reply, msg.header.pl_size)) {
    return 0;
  }
  *done = status == CHORD_STEP_DONE;
  *out = node_from_peer(&peer);
  return 1;
}

static int find_successor_from(superpeer_t *sp, chord_node_t current, const node_id_t *key, chord_node_t *out) {
  unsigned hop;

  for (hop = 0; hop < CHORD_M; hop++) {
    int done = 0;
    chord_node_t step;

    if (!rpc_closest(sp, &current, key, &done, &step)) {
      return 0;
    }
    if (done) {
      *out = step;
      return step.valid;
    }
    if (!step.valid || endpoint_is_self(sp, step.ipv4, step.port) ||
        (step.ipv4 == current.ipv4 && step.port == current.port)) {
      return 0;
    }
    current = step;
  }
  return 0;
}

static int membership_join(superpeer_t *sp, const chord_node_t *dest) {
  join_t join;
  uint8_t raw[64];
  ack_t ack;
  msg_t msg;
  int fd;

  fd = connect_node(dest);
  if (fd < 0) {
    return 0;
  }
  memset(&join, 0, sizeof join);
  join.ipv4 = sp->cfg.ipv4;
  join.port = sp->cfg.port;
  join.node_type = SUPERPEER;
  if (send_join(fd, &sp->self_id, &join) != NET_OK) {
    net_close(fd);
    return 0;
  }
  msg = recv_message(fd, raw, sizeof raw, &ack, sizeof ack);
  net_close(fd);
  return msg.status == NET_OK && msg.header.msg_type == ACK;
}

static int try_join(superpeer_t *sp) {
  chord_node_t boot;
  chord_node_t succ;
  chord_node_t known[2];
  unsigned known_count = 0;

  memset(&boot, 0, sizeof boot);
  boot.ipv4 = sp->cfg.bootstrap[0].ipv4;
  boot.port = sp->cfg.bootstrap[0].port;
  boot.valid = 1;
  if (!node_id_from_endpoint(boot.ipv4, boot.port, &boot.id)) {
    return 0;
  }
  if (endpoint_is_self(sp, boot.ipv4, boot.port)) {
    return 1;
  }
  if (!find_successor_from(sp, boot, &sp->self_id, &succ) || !succ.valid) {
    return 0;
  }
  if (!endpoint_is_self(sp, succ.ipv4, succ.port)) {
    install_successor_list(sp, &succ, 1);
    known[known_count++] = succ;
  }
  if (!membership_join(sp, &boot)) {
    return 0;
  }
  known[known_count++] = boot;
  remember_superpeers(sp, known, known_count);
  return 1;
}

static int rpc_predecessor(superpeer_t *sp, const chord_node_t *dest, chord_node_t *out, int *present) {
  uint8_t reply[CHORD_NODE_WIRE_SIZE];
  msg_t msg;
  chord_peer_t peer;

  if (!rpc_raw(dest, &sp->self_id, GET_PREDECESSOR, NULL, 0, reply, sizeof reply, &msg)) {
    return 0;
  }
  if (msg.header.msg_type != GET_PREDECESSOR) {
    return 0;
  }
  if (msg.header.pl_size == 0) {
    *present = 0;
    return 1;
  }
  if (msg.header.pl_size != CHORD_NODE_WIRE_SIZE || chord_peer_unpack(&peer, reply) != 0) {
    return 0;
  }
  *out = node_from_peer(&peer);
  *present = out->valid;
  return 1;
}

static int same_endpoint(const chord_node_t *a, const chord_node_t *b) {
  return a->ipv4 == b->ipv4 && a->port == b->port;
}

static int pull_successors(superpeer_t *sp, const chord_node_t *succ, chord_node_t *list, unsigned *count) {
  uint8_t reply[1u + CHORD_SUCCESSORS_MAX * CHORD_NODE_WIRE_SIZE];
  msg_t msg;
  chord_peer_t peers[CHORD_SUCCESSORS_MAX];
  unsigned remote = 0;
  unsigned i;
  unsigned n;

  if (!rpc_raw(succ, &sp->self_id, GET_SUCCESSORS, NULL, 0, reply, sizeof reply, &msg)) {
    return 0;
  }
  if (msg.header.msg_type != GET_SUCCESSORS ||
      !chord_successors_unpack(peers, CHORD_SUCCESSORS_MAX, &remote, reply, msg.header.pl_size)) {
    return 0;
  }
  list[0] = *succ;
  n = 1;
  for (i = 0; i < remote && n < CHORD_R; i++) {
    chord_node_t node = node_from_peer(&peers[i]);
    unsigned j;
    int dup = endpoint_is_self(sp, node.ipv4, node.port);

    for (j = 0; j < n && !dup; j++) {
      dup = same_endpoint(&list[j], &node);
    }
    if (!dup && node.valid) {
      list[n++] = node;
    }
  }
  *count = n;
  return 1;
}

static int rpc_notify(superpeer_t *sp, const chord_node_t *dest) {
  chord_node_t self;
  chord_peer_t body;
  uint8_t frame[HEADER_SIZE + CHORD_NODE_WIRE_SIZE];
  uint8_t raw[64];
  ack_t ack;
  pl_header hdr;
  msg_t msg;
  int fd;

  pthread_mutex_lock(&sp->chord.lock);
  self = sp->chord.self;
  pthread_mutex_unlock(&sp->chord.lock);
  if (!self.valid) {
    return 0;
  }
  peer_from_node(&body, &self);
  fd = connect_node(dest);
  if (fd < 0) {
    return 0;
  }
  memset(&hdr, 0, sizeof hdr);
  hdr.protocol_ver = PROTOCOL_VER;
  hdr.msg_type = NOTIFY;
  hdr.time = (uint64_t)time(NULL);
  hdr.pl_size = CHORD_NODE_WIRE_SIZE;
  memcpy(hdr.src_node, sp->self_id.bytes, NODE_ID_SIZE);
  if (send_message(fd, frame, sizeof frame, &body, &hdr) != NET_OK) {
    net_close(fd);
    return 0;
  }
  msg = recv_message(fd, raw, sizeof raw, &ack, sizeof ack);
  net_close(fd);
  return msg.status == NET_OK && msg.header.msg_type == ACK;
}

/* Sucessor igual a si mesmo: o predecessor dele já está na memória, sem socket. */
static void stabilize(superpeer_t *sp) {
  chord_node_t self;
  chord_node_t succ;
  chord_node_t pred;
  chord_node_t learned;
  chord_node_t list[CHORD_R];
  unsigned n = 0;
  int present = 0;

  pthread_mutex_lock(&sp->chord.lock);
  self = sp->chord.self;
  succ = sp->chord.successors[0];
  pred = sp->chord.predecessor;
  pthread_mutex_unlock(&sp->chord.lock);
  if (!succ.valid) {
    return;
  }

  if (endpoint_is_self(sp, succ.ipv4, succ.port) && pred.valid &&
      !endpoint_is_self(sp, pred.ipv4, pred.port) &&
      chord_in_open(&self.id, &succ.id, &pred.id)) {
    install_successor_list(sp, &pred, 1);
    remember_superpeers(sp, &pred, 1);
    succ = pred;
  }
  if (endpoint_is_self(sp, succ.ipv4, succ.port)) {
    return;
  }
  if (!rpc_predecessor(sp, &succ, &learned, &present)) {
    return;
  }
  if (present && learned.valid && !endpoint_is_self(sp, learned.ipv4, learned.port) &&
      chord_in_open(&self.id, &succ.id, &learned.id)) {
    succ = learned;
    install_successor_list(sp, &succ, 1);
    remember_superpeers(sp, &succ, 1);
  }
  if (endpoint_is_self(sp, succ.ipv4, succ.port)) {
    return;
  }
  if (pull_successors(sp, &succ, list, &n)) {
    install_successor_list(sp, list, n);
    remember_superpeers(sp, list, n);
  }
  rpc_notify(sp, &succ);
}

static void *chord_maintenance(void *arg) {
  superpeer_t *sp = arg;
  const int alone = sp->cfg.bootstrap_count <= 0 ||
                    endpoint_is_self(sp, sp->cfg.bootstrap[0].ipv4, sp->cfg.bootstrap[0].port);
  int joined = alone;
  uint64_t start = mono_ms();
  uint64_t next_join = start;
  uint64_t next_stab = start;
  chord_node_t succ;
  unsigned n = 0;

  if (chord_copy_successors(&sp->chord, &succ, 1, &n) && n == 1) {
    log_successor(&succ);
  }

  while (1) {
    uint64_t now = mono_ms();
    uint64_t wake;

    if (!joined && now >= next_join) {
      if (now - start >= CHORD_JOIN_TIMEOUT_MS) {
        joined = 1;
      } else {
        if (try_join(sp)) {
          joined = 1;
        }
        next_join = mono_ms() + CHORD_JOIN_RETRY_MS;
      }
    }
    now = mono_ms();
    if (now >= next_stab) {
      stabilize(sp);
      next_stab = mono_ms() + CHORD_MAINT_PERIOD_MS;
    }
    now = mono_ms();
    wake = next_stab;
    if (!joined && next_join < wake) {
      wake = next_join;
    }
    if (wake > now) {
      sleep_ms((unsigned)(wake - now));
    }
  }
  return NULL;
}

int superpeer_run(superpeer_t *sp) {
  pthread_t maintenance;

  if (!sp || sp->listend_fd < 0) {
    return 0;
  }
  if (pthread_create(&maintenance, NULL, chord_maintenance, sp) == 0) {
    pthread_detach(maintenance);
  } else {
    fprintf(stderr, "superpeer_run: chord maintenance thread failed\n");
  }

  while (1) {
    pthread_t th;
    conn_job_t *job = malloc(sizeof *job);

    if (!job) {
      sleep_ms(50);
      continue;
    }
    job->sp = sp;
    memset(&job->peer_addr, 0, sizeof job->peer_addr);
    job->conn = net_accept(sp->listend_fd, &job->peer_addr);
    if (job->conn < 0) {
      free(job);
      sleep_ms(50);
      continue;
    }
    if (pthread_create(&th, NULL, connection_worker, job) != 0) {
      connection_worker(job);
      continue;
    }
    pthread_detach(th);
  }
}
