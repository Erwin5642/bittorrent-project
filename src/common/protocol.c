#include <endian.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <arpa/inet.h>

#include "network.h"

// TEMP
static const int FILE_NAME_MAX = 25;

// TEMP
#include <stdint.h>

uint64_t my_ntohll(uint64_t val) {
    #if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        return val;
    #else
        return (((uint64_t)ntohl((uint32_t)(val & 0xFFFFFFFF))) << 32) | 
               ntohl((uint32_t)(val >> 32));
    #endif
}



#define HEADER_SIZE 104

enum message_type{
	JOIN,
	LEAVE,
	LOOKUP,
	STORE,
	DOWN,
	LOAD_REQ,
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
	uint32_t checksun;
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


/* WRONG
void pack_message(const char* msg_header, const char* payload, int payload_size, char* out_msg){
	const int message_size = payload_size + HEADER_SIZE;
	if(!msg_header || !payload || !out_msg){
		if(!payload)
			fprintf(stderr, "pack_message # null payload");
		if(!msg_header)
			fprintf(stderr, "pack_message # null message header");
		if(!out_msg)
			fprintf(stderr, "pack_message # null message buffer");
		return;
	}

	void* msg_pointer = out_msg; 
	int i = 0;
	int remaining = message_size;
	while(remaining == 0){
		if(remaining >= 4){
			uint32_t buffer32 = htonl(*(uint32_t*)out_msg);
			memcpy(msg_pointer, &buffer32, sizeof(uint32_t));
			msg_pointer += 4;
			remaining -= 4;
		}
		else if(remaining >= 2){
			uint16_t buffer16 = htonl(*(uint16_t*)out_msg);
			memcpy(msg_pointer, &buffer16, sizeof(uint16_t));
			msg_pointer += 2;
			remaining -= 2;

		}
		else{
			remaining = 0;
		}

	}

}*/

int unpack_header(pl_header* out_header, const char* header_buf){
	const char* pt = header_buf;
	uint32_t temp_32;
	uint64_t temp_64;
	memcpy(&temp_32, pt, sizeof(uint32_t));
	temp_32 = ntohl(temp_32);
	out_header->protocol_ver = temp_32;
	pt+=sizeof(uint32_t);
	memcpy(&temp_32, pt, sizeof(uint32_t));
	temp_32 = ntohl(temp_32);
	out_header->msg_type = temp_32;
	pt+=sizeof(uint32_t);

	memcpy(out_header->src_node, pt, sizeof(uint8_t)*32);
	pt+=(sizeof(uint8_t)*32);
	memcpy(out_header->dst_node, pt, sizeof(uint8_t)*32);
	pt+=(sizeof(uint8_t)*32);
	memcpy(out_header->trsc_id, pt, sizeof(uint8_t)*16);
	pt+=(sizeof(uint8_t)*16);

	memcpy(&temp_64, pt, sizeof(uint64_t));
	temp_64 = my_ntohll(temp_64);
	out_header->time = temp_64;
	pt+=sizeof(uint64_t);
	memcpy(&temp_32, pt, sizeof(uint32_t));
	temp_32 = ntohl(temp_32);
	out_header->pl_size = temp_32;
	pt+=sizeof(uint32_t);
	memcpy(&temp_32, pt, sizeof(uint32_t));
	temp_32 = ntohl(temp_32);
	out_header->checksun = temp_32;
	pt+=sizeof(uint32_t);
	
	return 0;
}


msg_t recv_message(int fd){

	char header_buffer[HEADER_SIZE]; 

	int status;
	
	if((status = recv_all(fd, header_buffer, HEADER_SIZE)) != NET_OK)
		return (msg_t){{0}, NULL, status};
	
	// TODO: unpack

	pl_header msg_header;

	unpack_header(&msg_header, header_buffer);
	
	
	// TODO: get and unpack payload 
	
		
	return (msg_t){msg_header, NULL, NET_OK}; 

}



