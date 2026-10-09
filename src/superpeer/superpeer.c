#include <arpa/inet.h>
#include <netinet/in.h>

#include "../../include/superpeer/superpeer.h"
#include "common/config.h"
#include "common/network.h"
#include "common/node.h"
#include "common/protocol.h"
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
  node_uuid_t uuid;
  member_t self;
  char ip[INET_ADDRSTRLEN];
  char hex[NODE_ID_HEX_SIZE];
  int fd;

  if (!sp || !conf_path) {
    return 0;
  }

  memset(sp, 0, sizeof *sp);
  sp->listend_fd = -1;

  if (!node_config_load(conf_path, &cfg)) {
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
  } else {
    strncpy(sp->name, "superpeer", sizeof sp->name - 1);
  }

  sp->cfg = cfg;

  if (!node_uuid_random(&uuid) || !node_id_generate(cfg.ipv4, cfg.port, &uuid, &sp->self_id)) {
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
  self.last_heartbeat = (time_t)time(NULL);
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
  fd = net_listen(cfg.port);
  if (fd < 0) {
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
  member.last_heartbeat = (time_t)time(NULL);
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
} control_payload_t;

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
  } else if (msg.header.msg_type == HEARTBEAT){
	/* RX de heartbeat (CP3): so atualiza o emissor, nao responde (fire-and-forget). */
	node_id_t sender;
	memcpy(&sender, msg.header.src_node, NODE_ID_SIZE);
	pthread_mutex_lock(&sp->members_lock);
	for(size_t i = 0; i<sp->members.count; i++){
		if(node_id_cmp(&sp->members.entries[i].id, &sender) == 0){
			sp->members.entries[i].last_heartbeat = (time_t)time(NULL);
			sp->members.entries[i].state = MEMBER_ALIVE; /* revive de SUSPECT se o silencio tinha sido so uma falsa suspeita */
			break;
		}
	}
	pthread_mutex_unlock(&sp->members_lock);
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

/*
 * Thread de heartbeat (CP3): roda para sempre, detached, junto do accept loop.
 * Cada despertar (1s) faz duas coisas sobre a membership table:
 *   1. Julga: compara 'now - last_heartbeat' de cada membro contra
 *      HEARTBEAT_TIMEOUT_SEC (15s) e rebaixa quem ficou em silencio para
 *      MEMBER_SUSPECT. O retorno a MEMBER_ALIVE acontece no RX de HEARTBEAT,
 *      em handle_connection. A promocao SUSPECT->FAILED fica para o Gossip (CP4).
 *   2. Envia: a cada HEARTBEAT_SEC (5s) emite um batimento para os demais
 *      Super Peers ainda considerados vivos (node_type == SUPERPEER e
 *      state != MEMBER_FAILED).
 * O julgamento acontece a cada 1s (mais fino que o envio, 5s) para garantir
 * que a transicao para SUSPECT seja detectada bem antes do timeout do
 * harness do professor (FAILURE_TIMEOUT_SEC + 3s).
 * Os alvos do envio sao copiados para 'targets' ainda dentro de members_lock;
 * o envio em si (net_connect/send_heartbeat, bloqueante) roda depois do
 * unlock, para nao travar JOIN/LEAVE/STORE/etc. enquanto conecta.
 */
static void *heartbeat_worker(void *arg) {
	superpeer_t* sp = arg;
	time_t last_hb = 0;
	unsigned sleep_interval_ms = 1000;

	char ip[INET_ADDRSTRLEN];
	char hex[NODE_ID_HEX_SIZE];
	while(1){
		time_t now = (time_t)time(NULL);
		int send_now = (now - last_hb >= HEARTBEAT_SEC);
		member_t targets[MEMBER_TABLE_MAX_SIZE];
		size_t target_count = 0;
		pthread_mutex_lock(&sp->members_lock);
		for(size_t i = 0; i < sp->members.count; i++){
			member_t* sel_member = &sp->members.entries[i];

			if(node_id_cmp(&sel_member->id, &sp->self_id) == 0) continue; /* nunca se autossuspeita */
			time_t age = now - sel_member->last_heartbeat;

			if(age >= HEARTBEAT_TIMEOUT_SEC && sel_member->state == MEMBER_ALIVE){
				sel_member->state = MEMBER_SUSPECT;
				inet_ntop(AF_INET, &sel_member->ipv4, ip, INET_ADDRSTRLEN);
				node_id_to_hex(&sel_member->id, hex, NODE_ID_HEX_SIZE);
				printf("SUSPECT %s %s:%d\n", hex, ip, sel_member->port);
			}
			if(send_now && sel_member->node_type == SUPERPEER && sel_member->state != MEMBER_FAILED && sel_member->state != MEMBER_REMOVED){
				targets[target_count++] = *sel_member;
			}
		}
		pthread_mutex_unlock(&sp->members_lock);

		if(send_now){
			for(size_t i = 0; i < target_count; i++){
				inet_ntop(AF_INET, &targets[i].ipv4, ip, INET_ADDRSTRLEN);
				int fd = net_connect(ip, targets[i].port);
				if(fd >= 0){
					if(send_heartbeat(fd, &sp->self_id) == NET_OK)
						printf("TX HEARTBEAT -> %s:%d\n", ip, targets[i].port);
					net_close(fd);
				}
			}
			last_hb = now;
		}

		sleep_ms(sleep_interval_ms);
	}

	return NULL;
}

int superpeer_run(superpeer_t *sp) {
  if (!sp || sp->listend_fd < 0) {
    return 0;
  }
	/* Thread de heartbeat (CP3): detached, igual as threads de conexao;
	 * nao ha shutdown gracioso (o harness usa kill -9), entao nao ha o que
	 * join-ar. Falha ao criar so deixa o Super Peer sem deteccao de falhas,
	 * mas nao impede o accept loop abaixo. */
	pthread_t hb;
	if(pthread_create(&hb, 0, heartbeat_worker, sp) == 0){
		pthread_detach(hb);
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
