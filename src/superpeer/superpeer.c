#include <arpa/inet.h>
#include <netinet/in.h>

#include "../../include/superpeer/superpeer.h"
#include "common/config.h"
#include "common/network.h"
#include "common/node.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

int superpeer_init(superpeer_t *sp, const char *conf_path) {
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

  sp->cfg = cfg;

  if (!node_uuid_random(&uuid) || !node_id_generate(cfg.ipv4, cfg.port, &uuid, &sp->self_id)) {
    fprintf(stderr, "superpeer_init: failed to generate NodeID\n");
    return 0;
  }

  if (!inet_ntop(AF_INET, &cfg.ipv4, ip, sizeof ip) ||
      !node_id_to_hex(&sp->self_id, hex, sizeof hex)) {
    return 0;
  }
  printf("SUPERPEER %s %u %s\n", ip, (unsigned)cfg.port, hex);

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

  fd = net_listen(cfg.port);
  if (fd < 0) {
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

int superpeer_run(superpeer_t *sp) {
  if (!sp || sp->listend_fd < 0) {
    return 0;
  }

  while (1) {
    struct sockaddr_in peer_addr;
    int conn;
    char in_buf[MAX_CONTROL_PAYLOAD_SZ];
    join_t join;
    msg_t msg;

    memset(&peer_addr, 0, sizeof(peer_addr));
    conn = net_accept(sp->listend_fd, &peer_addr);
    if (conn < 0) {
      continue;
    }

    memset(&join, 0, sizeof join);
    msg = recv_message(conn, in_buf, &join);
    if (msg.status != NET_OK) {
      net_close(conn);
      continue;
    }

    if (msg.header.msg_type == JOIN) {
      ack_t ack;
      error_t err;

      if (join.ipv4 == 0) {
        join.ipv4 = peer_addr.sin_addr.s_addr;
      }
      if (join.port == 0) {
        join.port = ntohs(peer_addr.sin_port);
      }

      memset(&ack, 0, sizeof ack);
      memset(&err, 0, sizeof err);
      if (superpeer_handle_join(sp, &msg.header, &join, &ack, &err)) {
        send_ack(conn, &msg.header, &sp->self_id, &ack);
        member_table_print(&sp->members);
      } else {
        if (err.code == 0) {
          err.code = 1;
        }
        send_error(conn, &msg.header, &sp->self_id, err.code,
                   err.reason[0] ? (const char *)err.reason : "JOIN rejected");
      }
    } else {
      send_error(conn, &msg.header, &sp->self_id, 3, "unsupported message type");
    }

    net_close(conn);
  }
}
