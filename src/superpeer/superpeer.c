#include <arpa/inet.h>
#include <netinet/in.h>

#include "../../include/superpeer/superpeer.h"
#include "common/config.h"
#include "common/network.h"
#include "common/node.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

  fd = net_listen(cfg.port);
  if (fd < 0) {
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
  char buf[HEADER_SIZE];
  pl_header hdr;

  if (!req || !self) {
    return NET_ERROR;
  }
  fill_reply_header(&hdr, req, self, PONG, 0);
  return send_message(fd, buf, NULL, &hdr);
}

typedef struct {
  superpeer_t *sp;
  int conn;
  struct sockaddr_in peer_addr;
} conn_job_t;

static void handle_connection(superpeer_t *sp, int conn, struct sockaddr_in peer_addr) {
  char in_buf[MAX_CONTROL_PAYLOAD_SZ];
  union {
    join_t join;
    leave_t leave;
  } payload;
  msg_t msg;

  memset(&payload, 0, sizeof payload);
  msg = recv_message(conn, in_buf, &payload);
  if (msg.status != NET_OK) {
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
    if (payload.join.port == 0) {
      payload.join.port = ntohs(peer_addr.sin_port);
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
  } else {
    send_error(conn, &msg.header, &sp->self_id, 3, "unsupported message type");
  }

  net_close(conn);
}

static void *connection_worker(void *arg) {
  conn_job_t *job = arg;

  handle_connection(job->sp, job->conn, job->peer_addr);
  free(job);
  return NULL;
}

int superpeer_run(superpeer_t *sp) {
  if (!sp || sp->listend_fd < 0) {
    return 0;
  }

  while (1) {
    pthread_t th;
    conn_job_t *job = malloc(sizeof *job);

    if (!job) {
      continue;
    }
    job->sp = sp;
    memset(&job->peer_addr, 0, sizeof job->peer_addr);
    job->conn = net_accept(sp->listend_fd, &job->peer_addr);
    if (job->conn < 0) {
      free(job);
      continue;
    }
    if (pthread_create(&th, NULL, connection_worker, job) != 0) {
      connection_worker(job);
      continue;
    }
    pthread_detach(th);
  }
}
