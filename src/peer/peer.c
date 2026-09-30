/*
 * peer.c — cliente P2P de linha de comando (CP1).
 * Carrega a config, gera o NodeID, conecta a um Super Peer, envia uma mensagem
 * de controle (ping/join/leave) e imprime a resposta. Exercita a ponta cliente
 * do protocolo definido em protocol.c/network.c.
 */
#include "../../include/common/network.h"
#include "../../include/common/protocol.h"
#include "../../include/common/node.h"
#include "../../include/common/config.h"
#include "../../include/peer/upload.h"
#include "../../include/peer/download.h"
#include "../../include/peer/storage.h"

#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <time.h>
#include <netinet/in.h>
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

/* Registra o metadado no Super Peer de bootstrap via STORE. Best-effort. */
static void store_to_superpeer(const peer_t *self, const node_config_t *cfg,
                               const file_metadata_t *meta) {
	char host[INET_ADDRSTRLEN];
	uint16_t port;
	size_t wire;
	uint8_t payload[MAX_CONTROL_PAYLOAD_SZ];
	uint8_t buf[HEADER_SIZE + MAX_CONTROL_PAYLOAD_SZ];
	pl_header h;
	int fd;

	if (cfg->bootstrap_count <= 0) {
		printf("(sem bootstrap no .conf; metadado nao enviado ao Super Peer)\n");
		return;
	}
	port = cfg->bootstrap[0].port;
	if (!inet_ntop(AF_INET, &cfg->bootstrap[0].ipv4, host, sizeof host))
		return;

	wire = metadata_wire_size(meta->chunk_count);
	if (wire == 0 || wire > sizeof payload) {
		fprintf(stderr, "STORE: metadado nao cabe no payload de controle\n");
		return;
	}
	if (metadata_pack(meta, payload, sizeof payload) != (ssize_t)wire)
		return;

	fd = net_connect(host, port);
	if (fd < 0) {
		fprintf(stderr, "STORE: nao conectou ao Super Peer %s:%u\n", host, port);
		return;
	}

	memset(&h, 0, sizeof h);
	h.protocol_ver = PROTOCOL_VER;
	h.msg_type = STORE;
	h.time = (uint64_t)time(NULL);
	h.pl_size = (uint32_t)wire;
	memcpy(h.src_node, self->node_id.bytes, NODE_ID_SIZE);

	if (simple_send(fd, buf, payload, (uint32_t)wire, &h) == NET_OK) {
		uint8_t reply[MAX_CONTROL_PAYLOAD_SZ];
		msg_t r;
		printf("TX STORE\n");
		r = simple_recv(fd, reply, sizeof reply);
		if (r.status == NET_OK)
			printf("RX %s\n", message_type_name(r.header.msg_type));
		else
			fprintf(stderr, "STORE: sem resposta valida do Super Peer\n");
	}
	net_close(fd);
}

/* Identidade do peer: usa o .conf se houver, senão gera um NodeID efêmero. */
static void peer_identity(peer_t *self, node_config_t *cfg) {
	node_uuid_t uuid;

	if (peer_init(self, PEER_CONF) && node_config_load(PEER_CONF, cfg))
		return;

	/* Sem config: identidade efêmera, sem bootstrap. Pipeline local ainda roda. */
	memset(cfg, 0, sizeof *cfg);
	memset(self, 0, sizeof *self);
	self->type = PEER;
	if (node_uuid_random(&uuid))
		node_id_generate(0, 0, &uuid, &self->node_id);
}

/* Grava o metadado empacotado no índice local, para o download resolver por nome. */
static void index_metadata(const file_metadata_t *meta) {
	uint8_t wire[METADATA_PAYLOAD_MAX];
	ssize_t n = metadata_pack(meta, wire, sizeof wire);

	if (n > 0)
		storage_put_meta(STORAGE_ROOT, meta->filename, wire, (size_t)n);
}

/* Subcomando: --cmd upload --file <arquivo> */
static int cmd_upload(const char *file) {
	peer_t self;
	node_config_t cfg;
	file_metadata_t meta;

	if (!file) {
		fprintf(stderr, "upload: faltou --file <arquivo>\n");
		return 1;
	}
	peer_identity(&self, &cfg);

	if (!upload_prepare(file, STORAGE_ROOT, &self.node_id, &meta)) {
		fprintf(stderr, "upload: falha ao processar %s\n", file);
		return 1;
	}

	upload_print_report(&meta);
	index_metadata(&meta);
	store_to_superpeer(&self, &cfg, &meta);
	metadata_release(&meta);
	return 0;
}

/* Subcomando: --cmd download --name <nome> --output <saida> */
static int cmd_download(const char *name, const char *output) {
	if (!name || !output) {
		fprintf(stderr, "download: faltou --name <nome> e/ou --output <saida>\n");
		return 1;
	}
	if (!download_file(name, output, STORAGE_ROOT)) {
		fprintf(stderr, "download: falha ao baixar %s\n", name);
		return 1;
	}
	return 0;
}

int main(int argc, char* argv[]){
	const char *cmd  = NULL;

	/* Peer fechado devolve EPIPE no send; nao mata o processo. */
	signal(SIGPIPE, SIG_IGN);

    const char *host = "127.0.0.1";
    const char *file = NULL;
    const char *name = NULL;
    const char *output = NULL;
    long port = 0;

    static struct option long_opts[] = {
        {"cmd",    required_argument, NULL, 'c'},
        {"host",   required_argument, NULL, 'h'},
        {"port",   required_argument, NULL, 'p'},
        {"file",   required_argument, NULL, 'f'},
        {"name",   required_argument, NULL, 'n'},
        {"output", required_argument, NULL, 'o'},
        {NULL,     0,                 NULL,  0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:h:p:f:n:o:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'c': cmd = optarg; break;
        case 'h': host = optarg; break;
        case 'f': file = optarg; break;
        case 'n': name = optarg; break;
        case 'o': output = optarg; break;
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
            fprintf(stderr, "uso: %s --cmd <upload|download|ping|join|leave> ...\n", argv[0]);
            return 1;
        }
    }

    if (!cmd) {
        fprintf(stderr, "uso: %s --cmd <upload|download|ping|join|leave> ...\n", argv[0]);
        return 1;
    }

    /* Subcomandos de arquivo do CP2. */
    if (strcmp(cmd, "upload") == 0)
        return cmd_upload(file);
    if (strcmp(cmd, "download") == 0)
        return cmd_download(name, output);

    /* Comandos de controle do CP1 (ping/join/leave) exigem host/porta. */
    if (port == 0) {
        fprintf(stderr, "uso: %s --cmd <ping|join|leave> --host <ip> --port <porta>\n", argv[0]);
        return 1;
    }

    printf("cmd=%s host=%s port=%ld\n", cmd, host, port);

    int32_t type = cmd_to_type(cmd);
    if (type < 0) {
        fprintf(stderr, "cmd invalido: %s\n", cmd);
        return 1;
    }

    peer_t self;
    if (!peer_init(&self, "config/peer1.conf"))
        return 1;

    int fd = net_connect(host, (uint16_t)port);
    if (fd < 0)
        return 1;

    /* Monta o payload conforme o comando e envia via send_message. */
    uint8_t buf[HEADER_SIZE + MAX_CONTROL_PAYLOAD_SZ];
    int rc;

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
    uint8_t reply_payload[MAX_CONTROL_PAYLOAD_SZ];
    char reply_struct[sizeof(ack_t) > sizeof(error_t)
                      ? sizeof(ack_t) : sizeof(error_t)];

    msg_t r = recv_message(fd, reply_payload, sizeof reply_payload, reply_struct,
                           sizeof reply_struct);
    net_close(fd);

    if (r.status != NET_OK)
        return 1;

    printf("RX %s\n", message_type_name((uint16_t)r.header.msg_type));
    return 0;
}
