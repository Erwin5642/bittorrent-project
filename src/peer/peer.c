#include "../../include/common/network.h"
#include "../../include/common/protocol.h"
//#include "../../include/common/node.h"

#include <getopt.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

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

    /* cmd, host, port are ready here */
    printf("cmd=%s host=%s port=%ld\n", cmd, host, port);
	
	


	int32_t type = cmd_to_type(cmd);
	if (type < 0) { fprintf(stderr, "cmd invalido: %s\n", cmd); return 1; }


	const char *name = message_type_name((uint16_t)type);

	int fd = net_connect(host, (uint16_t)port);
	if (fd < 0) return 1;

	pl_header h = {0};
	h.protocol_ver = 1;
	h.msg_type = (uint32_t)type;
	h.time = (uint64_t)time(NULL);
	h.pl_size = (uint32_t)strlen(name);

	char buf[HEADER_SIZE + MAX_CONTROL_PAYLOAD_SZ];
	if (simple_send(fd, buf, name, (uint32_t)strlen(name), &h) != NET_OK) {
		net_close(fd);
		return 1;
	}

	printf("TX %s\n", name);

		
	char reply[64];
	msg_t r = simple_recv(fd, reply, sizeof reply);
	net_close(fd);
	if (r.status != NET_OK) return 1;

	printf("RX %s\n", message_type_name((uint16_t)r.header.msg_type));

	return 0;
}
