#include "../../include/common/network.h"
#include "../../include/common/protocol.h"
#include "../../include/common/node.h"
#include "../../include/common/config.h"

#include <getopt.h>
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

int main(int argc, char* argv[]){
	const char *cmd  = NULL;
    const char *host = "127.0.0.1";
    long port = 0;
	
    static struct option long_opts[] = {
        {"cmd",  required_argument, NULL, 'c'},
        {"host", required_argument, NULL, 'h'},
        {"port", required_argument, NULL, 'p'},
        {NULL,   0,                 NULL,  0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:h:p:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'c':
            cmd = optarg;
            break;
        case 'h':
            host = optarg;
            break;
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
            fprintf(stderr, "uso: %s --cmd <ping|join|leave> "
                            "--host <ip> --port <porta>\n", argv[0]);
            return 1;
        }
    }

    if (!cmd || port == 0) {
        fprintf(stderr, "uso: %s --cmd <ping|join|leave> "
                        "--host <ip> --port <porta>\n", argv[0]);
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

    char buf[HEADER_SIZE + MAX_CONTROL_PAYLOAD_SZ];
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
        rc = send_message(fd, buf, NULL, &h);
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

    char reply_payload[MAX_CONTROL_PAYLOAD_SZ];
    char reply_struct[sizeof(ack_t) > sizeof(error_t)
                      ? sizeof(ack_t) : sizeof(error_t)];

    msg_t r = recv_message(fd, reply_payload, reply_struct);
    net_close(fd);

    if (r.status != NET_OK)
        return 1;

    printf("RX %s\n", message_type_name((uint16_t)r.header.msg_type));
    return 0;
}
