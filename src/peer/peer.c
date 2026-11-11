/*
 * peer.c — cliente P2P de linha de comando (CP1).
 * Carrega a config, gera o NodeID, conecta a um Super Peer, envia uma mensagem
 * de controle (ping/join/leave) e imprime a resposta. Exercita a ponta cliente
 * do protocolo definido em protocol.c/network.c.
 */
#include <netinet/in.h>

#include "../../include/common/network.h"
#include "../../include/common/protocol.h"
#include "../../include/common/node.h"
#include "../../include/common/config.h"
#include "../../include/peer/upload.h"
#include "../../include/peer/download.h"
#include "../../include/peer/replica.h"
#include "../../include/peer/session.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

typedef struct {
    const char *cmd;
    uint16_t    type;
} cmd_entry_t;


/* Comandos aceitos na CLI mapeados para o tipo de mensagem correspondente. */
static const cmd_entry_t cmd_table[] = {
    { "ping",  PING  },
    { "join",  JOIN  },
    { "leave", LEAVE },
};

#define CMD_TABLE_LEN (sizeof cmd_table / sizeof cmd_table[0])

/* returns the message type, or -1 if the command is unknown */
static int32_t cmd_to_type(const char *cmd) {
    for (size_t i = 0; i < CMD_TABLE_LEN; i++)
        if (strcmp(cmd, cmd_table[i].cmd) == 0)
            return (int32_t)cmd_table[i].type;
    return -1;
}

typedef struct peer_t{
	node_id_t node_id;
	uint32_t ipv4;
	uint16_t port;
	node_type_t type;
}peer_t;

/* Carrega o .conf, gera UUID+NodeID e preenche o peer_t. Retorna 1 em sucesso. */
int peer_init(peer_t* peer, const char *conf_path){
	node_config_t cfg;
	node_uuid_t uuid;
	char ip[INET_ADDRSTRLEN];
	char hex[NODE_ID_HEX_SIZE];

	if (!peer || !conf_path) {
		return 0;
	}

	if (!node_config_load(conf_path, &cfg)) {
		fprintf(stderr, "peer_init: failed to load %s\n", conf_path);
		return 0;
	}
	if (cfg.node_type != PEER) {
		fprintf(stderr, "peer_init: type must be peer\n");
		return 0;
	}

	if (!node_uuid_random(&uuid) || !node_id_generate(cfg.ipv4, cfg.port, &uuid, &peer->node_id)) {
		fprintf(stderr, "peer_init: failed to generate NodeID\n");
		return 0;
	}

	if (!inet_ntop(AF_INET, &cfg.ipv4, ip, sizeof ip) ||
	    !node_id_to_hex(&peer->node_id, hex, sizeof hex)) {
		return 0;
	}
	printf("NodeID: %s\n", hex);
	fflush(stdout);

	peer->ipv4 = cfg.ipv4;
	peer->port = cfg.port;
	peer->type = cfg.node_type;
	return 1;
}

#define PEER_CONF "config/peer1.conf"
#define STORAGE_ROOT "storage"

/* Host/porta do Super Peer: --port explicito, senao o primeiro bootstrap que conectar. */
static int peer_endpoint(const node_config_t *cfg, const char *host_opt, long port_opt,
                         char *host_out, size_t host_cap, uint16_t *port_out) {
	int i;

	if (port_opt != 0) {
		snprintf(host_out, host_cap, "%s", host_opt ? host_opt : "127.0.0.1");
		*port_out = (uint16_t)port_opt;
		return 1;
	}
	if (!cfg || cfg->bootstrap_count <= 0)
		return 0;
	for (i = 0; i < cfg->bootstrap_count; i++) {
		char ip[INET_ADDRSTRLEN];
		int fd;

		if (cfg->bootstrap[i].port == 0)
			continue;
		if (!inet_ntop(AF_INET, &cfg->bootstrap[i].ipv4, ip, sizeof ip))
			continue;
		fd = net_connect(ip, cfg->bootstrap[i].port);
		if (fd < 0)
			continue;
		net_close(fd);
		snprintf(host_out, host_cap, "%s", ip);
		*port_out = cfg->bootstrap[i].port;
		return 1;
	}
	return 0;
}

/* Registra o metadado no Super Peer. 1 so com ACK. */
static int store_to_superpeer(const peer_t *self, const char *host, uint16_t port,
                              const file_metadata_t *meta) {
	size_t wire;
	uint8_t payload[METADATA_PAYLOAD_MAX];
	uint8_t buf[HEADER_SIZE + METADATA_PAYLOAD_MAX];
	uint8_t reply[MAX_CONTROL_PAYLOAD_SZ];
	pl_header h;
	msg_t r;
	int fd;
	int ok = 0;

	wire = metadata_wire_size(meta->chunk_count);
	if (wire == 0 || wire > sizeof payload) {
		fprintf(stderr, "STORE: metadado nao cabe no payload\n");
		return 0;
	}
	if (metadata_pack(meta, payload, sizeof payload) != (ssize_t)wire)
		return 0;

	fd = net_connect(host, port);
	if (fd < 0) {
		fprintf(stderr, "STORE: nao conectou ao Super Peer %s:%u\n", host, port);
		return 0;
	}

	memset(&h, 0, sizeof h);
	h.protocol_ver = PROTOCOL_VER;
	h.msg_type = STORE;
	h.time = (uint64_t)time(NULL);
	h.pl_size = (uint32_t)wire;
	memcpy(h.src_node, self->node_id.bytes, NODE_ID_SIZE);

	if (simple_send(fd, buf, payload, (uint32_t)wire, &h) == NET_OK) {
		printf("TX STORE\n");
		r = simple_recv(fd, reply, sizeof reply);
		if (r.status == NET_OK && r.header.msg_type == ACK) {
			printf("RX %s\n", message_type_name(r.header.msg_type));
			ok = 1;
		} else {
			fprintf(stderr, "STORE: Super Peer recusou o metadado\n");
		}
	}
	net_close(fd);
	return ok;
}

/* Modo sessão: a identidade nasce uma vez e vale para todos os comandos. */
static int g_session = 0;
static int g_have_identity = 0;
static peer_t g_identity;
static node_config_t g_identity_cfg;

/* Identidade do peer: usa o .conf se houver, senão gera um NodeID efêmero. */
static void peer_identity(peer_t *self, node_config_t *cfg, const char *conf) {
	node_uuid_t uuid;
	const char *path = conf ? conf : PEER_CONF;

	if (g_session && g_have_identity) {
		*self = g_identity;
		*cfg = g_identity_cfg;
		return;
	}

	if (peer_init(self, path) && node_config_load(path, cfg))
		goto done;

	/* Sem config: identidade efêmera, sem bootstrap. */
	memset(cfg, 0, sizeof *cfg);
	memset(self, 0, sizeof *self);
	self->type = PEER;
	if (node_uuid_random(&uuid))
		node_id_generate(0, 0, &uuid, &self->node_id);

done:
	if (g_session) {
		g_identity = *self;
		g_identity_cfg = *cfg;
		g_have_identity = 1;
	}
}

/* Estado da transferência (CP5), uma linha por transição. */
static void transfer_state(const char *name, const char *state) {
	printf("Transfer %s %s\n", name, state);
	fflush(stdout);
}

static void pause_ms(long ms) {
	struct timespec ts;

	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

/* Tentativas de enviar os chunks a uma réplica viva. O COMMIT do STORE pode
   chegar a ela depois do ACK, e até lá ela recusa o chunk. */
#define UPLOAD_REPLICA_TRIES 10
#define UPLOAD_REPLICA_PAUSE_MS 200

/* Envia todos os chunks a uma réplica. 1 se ela confirmou todos. */
static int send_to_replica(const peer_t *self, const file_metadata_t *meta,
                           const replica_endpoint_t *r) {
	int fd = net_connect(r->host, r->port);
	int i;

	if (fd < 0)
		return 0; /* conexão recusada: o nó está fora, não adianta insistir */
	net_close(fd);
	for (i = 0; i < UPLOAD_REPLICA_TRIES; i++) {
		if (upload_send_chunks(r->host, r->port, &self->node_id, meta, STORAGE_ROOT))
			return 1;
		pause_ms(UPLOAD_REPLICA_PAUSE_MS);
	}
	return 0;
}

/* Subcomando: --cmd upload --file <arquivo> */
static int cmd_upload(const char *file, const char *conf, const char *host_opt, long port_opt) {
	peer_t self;
	node_config_t cfg;
	file_metadata_t meta;
	replica_endpoint_t holders[REPLICATION_FACTOR];
	char host[INET_ADDRSTRLEN];
	uint16_t port = 0;
	size_t holder_count;
	size_t replicas = 0;
	size_t h;

	if (!file) {
		fprintf(stderr, "upload: faltou --file <arquivo>\n");
		return 1;
	}
	peer_identity(&self, &cfg, conf);
	if (!peer_endpoint(&cfg, host_opt, port_opt, host, sizeof host, &port)) {
		fprintf(stderr, "upload: sem Super Peer (use --port ou bootstrap no .conf)\n");
		return 1;
	}

	if (!upload_prepare(file, STORAGE_ROOT, &self.node_id, &meta)) {
		fprintf(stderr, "upload: falha ao processar %s\n", file);
		return 1;
	}

	upload_print_report(&meta);
	transfer_state(meta.filename, "CREATED");
	/* O Super Peer só aceita chunk de um registro que já está na tabela dele,
	   então o STORE vem antes dos chunks. */
	if (!store_to_superpeer(&self, host, port, &meta)) {
		fprintf(stderr, "upload: falha ao publicar %s\n", file);
		metadata_release(&meta);
		return 1;
	}
	transfer_state(meta.filename, "QUEUED");

	/* Réplicas: dono do ObjectID no anel e sucessores (CP5). */
	holder_count = replica_holders(host, port, &self.node_id, meta.object_id, 0, holders,
	                               REPLICATION_FACTOR);
	transfer_state(meta.filename, "STARTED");
	transfer_state(meta.filename, "TRANSFERRING");
	for (h = 0; h < holder_count; h++) {
		if (send_to_replica(&self, &meta, &holders[h])) {
			replicas++;
		} else {
			printf("Transfer %s replica %s:%u sem resposta; pulando\n", meta.filename,
			       holders[h].host, (unsigned)holders[h].port);
			fflush(stdout);
		}
	}
	transfer_state(meta.filename, "VERIFYING");
	if (replicas == 0) {
		fprintf(stderr, "upload: nenhuma replica confirmou os chunks de %s\n", file);
		metadata_release(&meta);
		return 1;
	}
	transfer_state(meta.filename, "FINISHED");
	printf("Transfer %s REPLICATED %zu\n", meta.filename, replicas);
	fflush(stdout);
	metadata_release(&meta);
	return 0;
}

/* Subcomando: --cmd download --name <nome> --output <saida> */
static int cmd_download(const char *name, const char *output, const char *conf,
                        const char *host_opt, long port_opt) {
	peer_t self;
	node_config_t cfg;
	char host[INET_ADDRSTRLEN];
	uint16_t port = 0;

	if (!name || !output) {
		fprintf(stderr, "download: faltou --name <nome> e/ou --output <saida>\n");
		return 1;
	}
	peer_identity(&self, &cfg, conf);
	if (!peer_endpoint(&cfg, host_opt, port_opt, host, sizeof host, &port)) {
		fprintf(stderr, "download: sem Super Peer (use --port ou bootstrap no .conf)\n");
		return 1;
	}
	if (!download_from_superpeer(host, port, &self.node_id, name, output)) {
		fprintf(stderr, "download: falha ao baixar %s\n", name);
		return 1;
	}
	return 0;
}

/* Subcomando (CP4): --cmd put --name <nome> --value <valor>
 * Manda PUT. O Super Peer sela a operação PUT; o valor só entra no ObjectID
 * SHA-256(nome || valor). Sai 0 só com o ACK. */
static int cmd_put(const char *name, const char *value, const char *conf,
                   const char *host_opt, long port_opt) {
	peer_t self;
	node_config_t cfg;
	node_id_t oid;
	pl_header hdr;
	msg_t reply;
	char host[INET_ADDRSTRLEN];
	char hex[NODE_ID_HEX_SIZE];
	uint8_t payload[MAX_CONTROL_PAYLOAD_SZ];
	uint8_t frame[HEADER_SIZE + MAX_CONTROL_PAYLOAD_SZ];
	uint8_t reply_buf[MAX_CONTROL_PAYLOAD_SZ];
	uint8_t *concat;
	uint16_t port = 0;
	size_t name_len;
	size_t value_len;
	ssize_t packed;
	int fd;
	int ok;

	if (!name || !value) {
		fprintf(stderr, "put: faltou --name <nome> e/ou --value <valor>\n");
		return 1;
	}
	name_len = strlen(name);
	value_len = strlen(value);
	if (name_len == 0 || name_len >= METADATA_FILENAME_MAX) {
		fprintf(stderr, "put: --name precisa de 1 a %d caracteres\n", METADATA_FILENAME_MAX - 1);
		return 1;
	}
	peer_identity(&self, &cfg, conf);
	if (!peer_endpoint(&cfg, host_opt, port_opt, host, sizeof host, &port)) {
		fprintf(stderr, "put: sem Super Peer (use --port ou bootstrap no .conf)\n");
		return 1;
	}

	concat = malloc(name_len + value_len);
	if (!concat)
		return 1;
	memcpy(concat, name, name_len);
	if (value_len > 0)
		memcpy(concat + name_len, value, value_len);
	ok = sha256(concat, name_len + value_len, oid.bytes);
	free(concat);
	if (!ok) {
		fprintf(stderr, "put: falha ao calcular o ObjectID\n");
		return 1;
	}
	node_id_to_hex(&oid, hex, sizeof hex);
	printf("PUT %s\nObjectID: %s\n", name, hex);
	fflush(stdout);

	packed = put_request_pack(name, (const uint8_t *)value, value_len, payload, sizeof payload);
	if (packed < 0) {
		fprintf(stderr, "put: valor nao cabe no pedido\n");
		return 1;
	}
	fd = net_connect(host, port);
	if (fd < 0) {
		fprintf(stderr, "put: nao conectou ao Super Peer %s:%u\n", host, port);
		return 1;
	}
	memset(&hdr, 0, sizeof hdr);
	hdr.protocol_ver = PROTOCOL_VER;
	hdr.msg_type = PUT;
	hdr.time = (uint64_t)time(NULL);
	hdr.pl_size = (uint32_t)packed;
	memcpy(hdr.src_node, self.node_id.bytes, NODE_ID_SIZE);
	ok = 0;
	if (simple_send(fd, frame, payload, (uint32_t)packed, &hdr) == NET_OK) {
		printf("TX PUT\n");
		reply = simple_recv(fd, reply_buf, sizeof reply_buf);
		if (reply.status == NET_OK && reply.header.msg_type == ACK) {
			printf("RX %s\n", message_type_name(reply.header.msg_type));
			ok = 1;
		}
	}
	net_close(fd);
	if (!ok) {
		fprintf(stderr, "put: Super Peer nao confirmou %s\n", name);
		return 1;
	}
	return 0;
}

/* Subcomando (CP4): --cmd state --host <ip> --port <porta>
 * Pede GET_STATE e imprime o texto que o Super Peer montou. */
static int cmd_state(const char *host, long port) {
	uint8_t frame[HEADER_SIZE];
	uint8_t reply[MAX_CONTROL_PAYLOAD_SZ + 1];
	pl_header hdr;
	msg_t msg;
	node_id_t self;
	int fd;

	if (!host || port < 1 || port > 65535) {
		fprintf(stderr, "state: faltou --host <ip> --port <porta>\n");
		return 1;
	}

	memset(&self, 0, sizeof self);
	fd = net_connect(host, (uint16_t)port);
	if (fd < 0)
		return 1;

	memset(&hdr, 0, sizeof hdr);
	hdr.protocol_ver = PROTOCOL_VER;
	hdr.msg_type = GET_STATE;
	hdr.time = (uint64_t)time(NULL);
	hdr.pl_size = 0;
	memcpy(hdr.src_node, self.bytes, NODE_ID_SIZE);
	if (simple_send(fd, frame, NULL, 0, &hdr) != NET_OK) {
		net_close(fd);
		return 1;
	}
	msg = simple_recv(fd, reply, MAX_CONTROL_PAYLOAD_SZ);
	net_close(fd);
	if (msg.status != NET_OK || msg.header.msg_type != GET_STATE) {
		fprintf(stderr, "state: Super Peer nao devolveu o estado\n");
		return 1;
	}
	reply[msg.header.pl_size] = '\0';
	printf("State %s:%ld\n%s", host, port, (const char *)reply);
	return 0;
}

static int hex_nibble(char c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* 64 caracteres hexadecimais, sem 0x. */
static int parse_object_id(const char *hex, uint8_t out[NODE_ID_SIZE]) {
	size_t i;

	if (!hex || !out || strlen(hex) != NODE_ID_SIZE * 2)
		return 0;
	for (i = 0; i < NODE_ID_SIZE; i++) {
		int hi = hex_nibble(hex[i * 2]);
		int lo = hex_nibble(hex[i * 2 + 1]);
		if (hi < 0 || lo < 0)
			return 0;
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return 1;
}

static void print_lookup_node(const chord_peer_t *node) {
	char ip[INET_ADDRSTRLEN];

	if (!inet_ntop(AF_INET, &node->ipv4, ip, sizeof ip)) {
		printf("?\n");
		return;
	}
	printf("%s:%u\n", ip, (unsigned)node->port);
}

/* Subcomando: --cmd lookup --name <arquivo> --host <ip> --port <porta> */
static int cmd_lookup_name(const char *name, const char *host, long port) {
	peer_t self;
	node_config_t cfg;
	uint8_t payload[METADATA_FILENAME_MAX];
	uint8_t frame[HEADER_SIZE + METADATA_FILENAME_MAX];
	uint8_t reply[METADATA_PAYLOAD_MAX];
	pl_header hdr;
	msg_t r;
	ssize_t n;
	int fd;

	if (!name || !host || port < 1 || port > 65535) {
		fprintf(stderr, "lookup: faltou --name, --host e --port\n");
		return 1;
	}
	peer_identity(&self, &cfg, NULL);
	n = lookup_pack(name, payload, sizeof payload);
	if (n < 0)
		return 1;
	fd = net_connect(host, (uint16_t)port);
	if (fd < 0)
		return 1;
	memset(&hdr, 0, sizeof hdr);
	hdr.protocol_ver = PROTOCOL_VER;
	hdr.msg_type = LOOKUP;
	hdr.time = (uint64_t)time(NULL);
	hdr.pl_size = (uint32_t)n;
	memcpy(hdr.src_node, self.node_id.bytes, NODE_ID_SIZE);
	if (simple_send(fd, frame, payload, (uint32_t)n, &hdr) != NET_OK) {
		net_close(fd);
		return 1;
	}
	r = simple_recv(fd, reply, sizeof reply);
	net_close(fd);
	if (r.status != NET_OK)
		return 1;
	printf("LOOKUP %s\n", name);
	return r.header.msg_type == STORE ? 0 : 1;
}

/* Subcomando: --cmd lookup --object-id <64 hex> --host <ip> --port <porta> */
static int cmd_lookup(const char *object_id, const char *host, long port) {
	uint8_t key[NODE_ID_SIZE];
	uint8_t frame[HEADER_SIZE + NODE_ID_SIZE];
	uint8_t reply[2u + CHORD_LOOKUP_PATH_MAX * CHORD_NODE_WIRE_SIZE];
	chord_peer_t path[CHORD_LOOKUP_PATH_MAX];
	pl_header hdr;
	msg_t msg;
	unsigned count = 0;
	unsigned i;
	int fd;
	node_id_t self;

	if (!object_id || !parse_object_id(object_id, key)) {
		fprintf(stderr, "lookup: --object-id precisa de 64 digitos hexadecimais\n");
		return 1;
	}
	if (!host || port < 1 || port > 65535) {
		fprintf(stderr, "lookup: faltou --host <ip> --port <porta>\n");
		return 1;
	}

	memset(&self, 0, sizeof self);
	fd = net_connect(host, (uint16_t)port);
	if (fd < 0)
		return 1;

	memset(&hdr, 0, sizeof hdr);
	hdr.protocol_ver = PROTOCOL_VER;
	hdr.msg_type = FIND_SUCCESSOR;
	hdr.time = (uint64_t)time(NULL);
	hdr.pl_size = NODE_ID_SIZE;
	memcpy(hdr.src_node, self.bytes, NODE_ID_SIZE);
	if (simple_send(fd, frame, key, NODE_ID_SIZE, &hdr) != NET_OK) {
		net_close(fd);
		return 1;
	}
	msg = simple_recv(fd, reply, sizeof reply);
	net_close(fd);
	if (msg.status != NET_OK || msg.header.msg_type != FIND_SUCCESSOR ||
	    !chord_lookup_path_unpack(path, CHORD_LOOKUP_PATH_MAX, &count, reply, msg.header.pl_size) ||
	    count == 0) {
		fprintf(stderr, "lookup: Super Peer nao devolveu o caminho\n");
		return 1;
	}

	printf("Lookup path:\n\n");
	for (i = 0; i < count; i++) {
		if (i > 0)
			printf("↓\n");
		print_lookup_node(&path[i]);
	}
	printf("\nOwner: ");
	print_lookup_node(&path[count - 1]);
	return 0;
}

/* Comandos de controle do CP1: ping, join e leave. Exigem host e porta. */
static int cmd_control(const char *cmd, const char *conf, const char *host, long port) {
    int32_t type;
    peer_t self;
    int fd;
    int rc;
    uint8_t buf[HEADER_SIZE + MAX_CONTROL_PAYLOAD_SZ];
    uint8_t reply_payload[MAX_CONTROL_PAYLOAD_SZ];
    char reply_struct[sizeof(ack_t) > sizeof(error_t)
                      ? sizeof(ack_t) : sizeof(error_t)];
    msg_t r;

    if (port == 0) {
        fprintf(stderr, "uso: --cmd <ping|join|leave> --host <ip> --port <porta>\n");
        return 1;
    }

    printf("cmd=%s host=%s port=%ld\n", cmd, host, port);

    type = cmd_to_type(cmd);
    if (type < 0) {
        fprintf(stderr, "cmd invalido: %s\n", cmd);
        return 1;
    }

    if (!peer_init(&self, conf ? conf : PEER_CONF))
        return 1;

    fd = net_connect(host, (uint16_t)port);
    if (fd < 0)
        return 1;

    /* Monta o payload conforme o comando e envia via send_message. */
    switch (type) {
    case PING: {
        pl_header h;
        memset(&h, 0, sizeof h);
        h.protocol_ver = PROTOCOL_VER;
        h.msg_type = PING;
        h.time = (uint64_t)time(NULL);
        h.pl_size = 0;
        memcpy(h.src_node, self.node_id.bytes, NODE_ID_SIZE);
        rc = send_message(fd, buf, sizeof buf, NULL, &h);
        break;
    }
    case JOIN: {
        join_t j;
        memset(&j, 0, sizeof j);
        j.ipv4 = self.ipv4;
        j.port = self.port;
        j.node_type = self.type;
        rc = send_join(fd, &self.node_id, &j);
        break;
    }
    case LEAVE: {
        leave_t l;
        memset(&l, 0, sizeof l);
        rc = send_leave(fd, &self.node_id, &l);
        break;
    }
    default:
        net_close(fd);
        return 1;
    }

    if (rc != NET_OK) {
        net_close(fd);
        return 1;
    }
    printf("TX %s\n", message_type_name((uint16_t)type));

    /* Le a resposta (ACK/ERROR/...) e imprime o tipo recebido. */
    r = recv_message(fd, reply_payload, sizeof reply_payload, reply_struct,
                     sizeof reply_struct);
    net_close(fd);

    if (r.status != NET_OK)
        return 1;

    printf("RX %s\n", message_type_name((uint16_t)r.header.msg_type));
    return 0;
}

/* ---- Modo sessão (CP6): o cliente fica aberto e lê comandos, um por linha ---- */

/* SIGTERM e SIGINT pedem o fim da sessão; o comando em curso termina antes. */
static volatile sig_atomic_t g_stop = 0;

static void on_stop_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

typedef struct {
    const char *conf;  /* .conf do peer (identidade e bootstrap) */
    const char *host;  /* Super Peer padrão, valido com port != 0 */
    long port;         /* 0 = usar o bootstrap do .conf */
} session_t;

/* Super Peer padrão da sessão: --host/--port ou o primeiro bootstrap que responder. */
static int session_endpoint(const session_t *s, char *host, size_t cap, long *port) {
    node_config_t cfg;
    uint16_t p = 0;

    memset(&cfg, 0, sizeof cfg);
    if (s->conf)
        node_config_load(s->conf, &cfg);
    if (!peer_endpoint(&cfg, s->host, s->port, host, cap, &p)) {
        fprintf(stderr, "sessao: sem Super Peer (use --port ou bootstrap no .conf)\n");
        return 0;
    }
    *port = p;
    return 1;
}

static int is_port_text(const char *text, long *out) {
    char *end;
    long v = strtol(text, &end, 10);

    if (*text == '\0' || *end != '\0' || v < 1 || v > 65535)
        return 0;
    *out = v;
    return 1;
}

/* host e porta explícitos (argv[from], argv[from+1]) ou o padrão da sessão. */
static int session_target(const session_t *s, int argc, char **argv, int from,
                          char *host, size_t cap, long *port) {
    if (argc == from)
        return session_endpoint(s, host, cap, port);
    if (argc == from + 2 && is_port_text(argv[from + 1], port)) {
        snprintf(host, cap, "%s", argv[from]);
        return 1;
    }
    fprintf(stderr, "sessao: esperava [host porta] depois de %s\n", argv[0]);
    return 0;
}

static void session_help(void) {
    printf("Comandos:\n"
           "  upload <arquivo>\n"
           "  download <nome> <saida>\n"
           "  lookup <nome|object-id>\n"
           "  put <nome> <valor...>\n"
           "  state [host porta]\n"
           "  ping|join|leave [host porta]\n"
           "  sleep <segundos>\n"
           "  help | quit\n");
}

static void session_sleep(long seconds) {
    long waited_ms = 0;

    while (waited_ms < seconds * 1000 && !g_stop) {
        pause_ms(100);
        waited_ms += 100;
    }
}

/* Executa um comando já separado em tokens. Devolve o código que iria no exit. */
static int session_exec(const session_t *s, int argc, char **argv) {
    const char *cmd = argv[0];
    char host[INET_ADDRSTRLEN];
    long port = 0;

    if (strcmp(cmd, "upload") == 0 && argc == 2)
        return cmd_upload(argv[1], s->conf, s->host, s->port);
    if (strcmp(cmd, "download") == 0 && argc == 3)
        return cmd_download(argv[1], argv[2], s->conf, s->host, s->port);
    if (strcmp(cmd, "lookup") == 0 && argc == 2) {
        uint8_t key[NODE_ID_SIZE];

        if (!session_endpoint(s, host, sizeof host, &port))
            return 1;
        if (parse_object_id(argv[1], key))
            return cmd_lookup(argv[1], host, port);
        return cmd_lookup_name(argv[1], host, port);
    }
    if (strcmp(cmd, "put") == 0 && argc >= 3) {
        char value[MAX_CONTROL_PAYLOAD_SZ];
        size_t used = 0;
        int i;

        value[0] = '\0';
        for (i = 2; i < argc; i++) {
            int n = snprintf(value + used, sizeof value - used, "%s%s", i > 2 ? " " : "", argv[i]);

            if (n < 0 || (size_t)n >= sizeof value - used) {
                fprintf(stderr, "put: valor longo demais\n");
                return 1;
            }
            used += (size_t)n;
        }
        return cmd_put(argv[1], value, s->conf, s->host, s->port);
    }
    if (strcmp(cmd, "state") == 0 && (argc == 1 || argc == 3)) {
        if (!session_target(s, argc, argv, 1, host, sizeof host, &port))
            return 1;
        return cmd_state(host, port);
    }
    if (cmd_to_type(cmd) >= 0 && (argc == 1 || argc == 3)) {
        if (!session_target(s, argc, argv, 1, host, sizeof host, &port))
            return 1;
        return cmd_control(cmd, s->conf, host, port);
    }
    if (strcmp(cmd, "sleep") == 0 && argc == 2) {
        long seconds;

        if (!is_port_text(argv[1], &seconds) && strcmp(argv[1], "0") != 0) {
            fprintf(stderr, "sleep: segundos invalidos: %s\n", argv[1]);
            return 1;
        }
        session_sleep(strcmp(argv[1], "0") == 0 ? 0 : seconds);
        return 0;
    }
    fprintf(stderr, "sessao: comando desconhecido ou argumentos errados: %s (digite help)\n", cmd);
    return 1;
}

/* Lê comandos de @p in até quit, EOF ou sinal. Cada comando termina com
   "[done N rc=R]", que é o sinal de fim e o resultado para quem controla a sessão.
   Com stop_on_error, o primeiro rc != 0 encerra (modo batch). */
static int run_session(FILE *in, const session_t *s, int prompt, int stop_on_error) {
    char line[SESSION_LINE_MAX];
    char *argv[SESSION_ARGV_MAX];
    unsigned seq = 0;
    int failed = 0;

    while (!g_stop) {
        int argc;
        int rc;

        if (prompt) {
            fputs("client> ", stdout);
            fflush(stdout);
        }
        if (!fgets(line, sizeof line, in)) {
            if (ferror(in) && errno == EINTR) {
                clearerr(in);
                continue;
            }
            break;
        }
        if (strchr(line, '\n') == NULL && !feof(in)) {
            int c;

            while ((c = fgetc(in)) != EOF && c != '\n')
                ;
            seq++;
            fprintf(stderr, "sessao: linha longa demais\n");
            printf("[done %u rc=1]\n", seq);
            fflush(stdout);
            continue;
        }

        argc = session_split(line, argv, SESSION_ARGV_MAX);
        if (argc == 0)
            continue;
        seq++;
        if (argc < 0) {
            fprintf(stderr, "sessao: linha invalida (aspas ou tokens demais)\n");
            rc = 1;
        } else if (strcmp(argv[0], "quit") == 0 || strcmp(argv[0], "exit") == 0) {
            break;
        } else if (strcmp(argv[0], "help") == 0) {
            session_help();
            rc = 0;
        } else {
            rc = session_exec(s, argc, argv);
        }
        fflush(stdout);
        fflush(stderr);
        printf("[done %u rc=%d]\n", seq, rc);
        fflush(stdout);
        if (rc != 0) {
            failed = 1;
            if (stop_on_error)
                break;
        }
    }
    printf("[bye]\n");
    fflush(stdout);
    return failed;
}

/* --interactive e --batch: identidade única, marcadores e laço de comandos. */
static int start_session(const char *conf, const char *host, long port,
                         const char *batch_file, int keep_going) {
    session_t s;
    FILE *in = stdin;
    peer_t self;
    node_config_t cfg;
    struct sigaction sa;
    int interactive = (batch_file == NULL);
    int rc;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    /* sem SA_RESTART: o fgets parado em leitura volta com EINTR e a sessão fecha */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    g_session = 1;
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (batch_file && strcmp(batch_file, "-") != 0) {
        in = fopen(batch_file, "r");
        if (!in) {
            fprintf(stderr, "batch: nao abriu %s\n", batch_file);
            return 1;
        }
    }

    s.conf = conf;
    s.host = host;
    s.port = port;
    peer_identity(&self, &cfg, conf);
    printf("[ready]\n");
    fflush(stdout);

    rc = run_session(in, &s, interactive && isatty(STDIN_FILENO), !interactive && !keep_going);
    if (in != stdin)
        fclose(in);
    return rc;
}

int main(int argc, char* argv[]){
	const char *cmd  = NULL;

	/* Peer fechado devolve EPIPE no send; nao mata o processo. */
	signal(SIGPIPE, SIG_IGN);

    const char *host = "127.0.0.1";
    const char *file = NULL;
    const char *name = NULL;
    const char *output = NULL;
    const char *conf = NULL;
    const char *object_id = NULL;
    const char *value = NULL;
    const char *batch_file = NULL;
    int interactive = 0;
    int keep_going = 0;
    long port = 0;

    static struct option long_opts[] = {
        {"cmd",         required_argument, NULL, 'c'},
        {"host",        required_argument, NULL, 'h'},
        {"port",        required_argument, NULL, 'p'},
        {"file",        required_argument, NULL, 'f'},
        {"name",        required_argument, NULL, 'n'},
        {"output",      required_argument, NULL, 'o'},
        {"config",      required_argument, NULL, 'g'},
        {"object-id",   required_argument, NULL, 'i'},
        {"value",       required_argument, NULL, 'v'},
        {"interactive", no_argument,       NULL, 'I'},
        {"batch",       required_argument, NULL, 'b'},
        {"keep-going",  no_argument,       NULL, 'k'},
        {NULL,          0,                 NULL,  0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:h:p:f:n:o:g:i:v:Ib:k", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'c': cmd = optarg; break;
        case 'h': host = optarg; break;
        case 'f': file = optarg; break;
        case 'n': name = optarg; break;
        case 'o': output = optarg; break;
        case 'g': conf = optarg; break;
        case 'i': object_id = optarg; break;
        case 'v': value = optarg; break;
        case 'I': interactive = 1; break;
        case 'b': batch_file = optarg; break;
        case 'k': keep_going = 1; break;
        case 'p': {
            char *end;
            port = strtol(optarg, &end, 10);
            if (*end != '\0' || port < 1 || port > 65535) {
                fprintf(stderr, "porta invalida: %s\n", optarg);
                return 1;
            }
            break;
        }
        default:
            fprintf(stderr, "uso: %s --cmd <upload|download|put|state|ping|join|leave|lookup> ...\n"
                            "     %s --interactive | --batch <arquivo|-> [--keep-going] [--config <conf>] [--port <porta>]\n",
                    argv[0], argv[0]);
            return 1;
        }
    }

    /* Modo sessão (CP6): fica aberto lendo comandos da stdin ou de um arquivo. */
    if (interactive || batch_file) {
        if (interactive && batch_file) {
            fprintf(stderr, "use --interactive ou --batch, nao os dois\n");
            return 1;
        }
        if (cmd) {
            fprintf(stderr, "--cmd nao se combina com --interactive/--batch\n");
            return 1;
        }
        return start_session(conf, host, port, batch_file, keep_going);
    }

    if (!cmd) {
        fprintf(stderr, "uso: %s --cmd <upload|download|put|state|ping|join|leave|lookup> ...\n", argv[0]);
        return 1;
    }

    /* Subcomandos de arquivo do CP2. Sem --port, usam o bootstrap do .conf. */
    if (strcmp(cmd, "upload") == 0)
        return cmd_upload(file, conf, host, port);
    if (strcmp(cmd, "download") == 0)
        return cmd_download(name, output, conf, host, port);
    if (strcmp(cmd, "lookup") == 0) {
        if (name && !object_id)
            return cmd_lookup_name(name, host, port);
        return cmd_lookup(object_id, host, port);
    }
    if (strcmp(cmd, "put") == 0)
        return cmd_put(name, value, conf, host, port);
    if (strcmp(cmd, "state") == 0)
        return cmd_state(host, port);

    /* Comandos de controle do CP1 (ping/join/leave) exigem host/porta. */
    return cmd_control(cmd, conf, host, port);
}
