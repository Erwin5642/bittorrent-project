#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define HEADER_SIZE 99 
#define MAX_CONTROL_PAYLOAD_SZ 4096

enum message_type{
	PING,
	PONG,
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
	ACTIVE,
	REPLICATING,	
	REMOVED,
}mtdata_status;

// TEMP

const char *message_type_name(uint16_t t);

// TEMP


//TODO:  types of each attribute to be decided
typedef struct payloadHeader{
	uint8_t protocol_ver;
	uint16_t msg_type;
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

int send_message(int fd, char* out_msg_buffer, const void* msg_payload, pl_header* msg_header);
msg_t recv_message(int fd, char* in_msg_buffer, void* struct_payload);
int simple_send(int fd, char* out_msg_buffer, const char* payload, uint32_t str_size, pl_header* msg_header);
msg_t simple_recv(int fd, char* payload, uint32_t str_size);


#endif
