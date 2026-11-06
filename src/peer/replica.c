/*
 * replica.c — onde ficam os chunks de um ObjectID (CP5, Aluno 1).
 * Dono no anel + sucessores, até REPLICATION_FACTOR nós, descobertos com as
 * RPCs do Chord que o Super Peer já serve (FIND_SUCCESSOR e GET_SUCCESSORS).
 */
#include "../../include/peer/replica.h"
#include "../../include/common/network.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

void replica_set_timeout(int fd, int seconds) {
	struct timeval tv;

	tv.tv_sec = seconds;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

/* Acrescenta host:port se couber e ainda não estiver na lista. */
static void push_unique(replica_endpoint_t *out, size_t cap, size_t *n, const char *host,
                        uint16_t port) {
	size_t i;

	if (*n >= cap || !host || port == 0)
		return;
	for (i = 0; i < *n; i++) {
		if (out[i].port == port && strcmp(out[i].host, host) == 0)
			return;
	}
	snprintf(out[*n].host, sizeof out[*n].host, "%s", host);
	out[*n].port = port;
	(*n)++;
}

static void push_peer(replica_endpoint_t *out, size_t cap, size_t *n, const chord_peer_t *p) {
	char ip[INET_ADDRSTRLEN];

	if (!p || p->port == 0 || !inet_ntop(AF_INET, &p->ipv4, ip, sizeof ip))
		return;
	push_unique(out, cap, n, ip, p->port);
}

size_t replica_merge(const chord_peer_t *owner, const chord_peer_t *succ, unsigned succ_count,
                     size_t factor, const char *entry_host, uint16_t entry_port,
                     replica_endpoint_t *out, size_t cap) {
	size_t n = 0;
	size_t ring_cap;
	unsigned i;

	if (!out || cap == 0)
		return 0;
	ring_cap = factor < cap ? factor : cap;
	push_peer(out, ring_cap, &n, owner);
	for (i = 0; succ && i < succ_count; i++)
		push_peer(out, ring_cap, &n, &succ[i]);
	if (entry_host)
		push_unique(out, cap, &n, entry_host, entry_port);
	return n;
}

/* Uma RPC de pedido e resposta do mesmo tipo, com timeout. 1 se veio a resposta. */
static int ring_rpc(const char *host, uint16_t port, const node_id_t *self, uint16_t type,
                    const uint8_t *payload, uint32_t len, uint8_t *reply, uint32_t reply_cap,
                    uint32_t *reply_len) {
	uint8_t frame[HEADER_SIZE + NODE_ID_SIZE];
	pl_header hdr;
	msg_t r;
	int fd;
	int ok = 0;

	if (len > NODE_ID_SIZE)
		return 0;
	fd = net_connect(host, port);
	if (fd < 0)
		return 0;
	replica_set_timeout(fd, REPLICA_TIMEOUT_SEC);
	memset(&hdr, 0, sizeof hdr);
	hdr.protocol_ver = PROTOCOL_VER;
	hdr.msg_type = type;
	hdr.time = (uint64_t)time(NULL);
	hdr.pl_size = len;
	memcpy(hdr.src_node, self->bytes, NODE_ID_SIZE);
	if (simple_send(fd, frame, payload, len, &hdr) == NET_OK) {
		r = simple_recv(fd, reply, reply_cap);
		if (r.status == NET_OK && r.header.msg_type == type) {
			*reply_len = r.header.pl_size;
			ok = 1;
		}
	}
	net_close(fd);
	return ok;
}

size_t replica_holders(const char *host, uint16_t port, const node_id_t *self,
                       const uint8_t object_id[METADATA_OBJECT_ID_SIZE], int include_entry,
                       replica_endpoint_t *out, size_t cap) {
	uint8_t path_buf[2u + CHORD_LOOKUP_PATH_MAX * CHORD_NODE_WIRE_SIZE];
	uint8_t succ_buf[1u + CHORD_SUCCESSORS_MAX * CHORD_NODE_WIRE_SIZE];
	chord_peer_t path[CHORD_LOOKUP_PATH_MAX];
	chord_peer_t succ[CHORD_SUCCESSORS_MAX];
	const chord_peer_t *owner = NULL;
	char owner_ip[INET_ADDRSTRLEN];
	unsigned path_count = 0;
	unsigned succ_count = 0;
	uint32_t len = 0;

	if (!host || port == 0 || !self || !object_id || !out || cap == 0)
		return 0;

	if (ring_rpc(host, port, self, FIND_SUCCESSOR, object_id, METADATA_OBJECT_ID_SIZE, path_buf,
	             sizeof path_buf, &len) &&
	    chord_lookup_path_unpack(path, CHORD_LOOKUP_PATH_MAX, &path_count, path_buf, len) &&
	    path_count > 0) {
		owner = &path[path_count - 1];
		if (inet_ntop(AF_INET, &owner->ipv4, owner_ip, sizeof owner_ip) &&
		    ring_rpc(owner_ip, owner->port, self, GET_SUCCESSORS, NULL, 0, succ_buf,
		             sizeof succ_buf, &len) &&
		    !chord_successors_unpack(succ, CHORD_SUCCESSORS_MAX, &succ_count, succ_buf, len)) {
			succ_count = 0;
		}
	}
	if (!owner)
		return replica_merge(NULL, NULL, 0, REPLICATION_FACTOR, host, port, out, cap);
	return replica_merge(owner, succ, succ_count, REPLICATION_FACTOR,
	                     include_entry ? host : NULL, port, out, cap);
}
