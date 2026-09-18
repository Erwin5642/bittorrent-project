#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <endian.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <arpa/inet.h>

#include "../../include/common/network.h"

#define HEADER_SIZE 104
#define MAX_CONTROL_PAYLOAD_SZ 4096

enum message_type{
	JOIN,
	LEAVE,
	LOOKUP,
	STORE,
	DOWNLOAD_REQ,
	DOWNLOAD_REP,
	PREPARE,
	COMMIT,
	ABORT,
	HEARTBEAT,
	GOSSIP,
	ELECTION,
	OK,
	COORDINATOR,
	SNAPSHOT,
	STATE_TRANSFER,
	ACK,
	ERROR,
	MSG_TYPE_MAX,
};

typedef enum compression_type{
	LZ4,
}compress_type;

typedef enum node_type{
	PEER,
	SUPERPEER,
}node_type;

typedef enum metadata_status{
	ATIVO,
	REPLICANDO,
	REMOVIDO,
}mtdata_status;

//TODO:  types of each attribute to be decided
typedef struct payloadHeader{
	uint32_t protocol_ver;
	uint32_t msg_type;
	uint8_t src_node[32];
	uint8_t dst_node[32];
	uint8_t trsc_id[16];
	uint64_t time;
	uint32_t pl_size;
	uint32_t checksum;
}pl_header;

typedef struct file_metadata{
	uint8_t obj_id[32];
	char* file_name;
	uint64_t version;
	uint64_t size;
	compress_type compression;
	uint8_t owner_peer[32];
	uint8_t* replica_peers[32];
	uint32_t chunk_count;
	uint8_t* chunk_hash[32];
	uint64_t upload_date;
	uint64_t last_access;
	uint64_t download_counter;
	mtdata_status status;
}fl_mtdata;

typedef struct message_struct{
	pl_header header;
	void* payload;
	uint8_t status;
}msg_t;

typedef struct joinPayload{
	uint8_t node_id[32];
	uint32_t ipv4;
	uint16_t port;
	uint8_t node_type;
}join_t;

typedef struct ackPayload{
	uint8_t node_id[32];
}ack_t;

typedef struct errorPayload{
	uint32_t code;
	uint8_t reason[64];
}error_t;
/*
int pack_join(const join_t* in_st, char* out_msg);
int unpack_join(join_t* out_st, const char* in_msg);
int pack_ack(const ack_t* in_st, char* out_msg);
int unpack_ack(ack_t* out_st, const char* in_msg);
int pack_error(const error_t* in_st, char* out_msg);
int unpack_error(error_t* out_st, const char* in_msg);
int unpack_header(pl_header* out_st, const char* in_msg);
int pack_header(const pl_header* in_st, char* out_st);
*/
int send_message(int fd, char* out_msg_buffer, const void* msg_payload, const pl_header* msg_header);
msg_t recv_message(int fd, char* in_msg_buffer, void* struct_payload);



#endif
