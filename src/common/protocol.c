#include <endian.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <arpa/inet.h>

#include "../../include/common/network.h"
#include "../../include/common/protocol.h"



static uint64_t my_ntohll(uint64_t val) {
    #if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        return val;
    #else
        return (((uint64_t)ntohl((uint32_t)(val & 0xFFFFFFFF))) << 32) | 
               ntohl((uint32_t)(val >> 32));
    #endif
}

static const int32_t payload_sizes[MSG_TYPE_MAX] = {
	[JOIN] = 39,
	[LEAVE] = 32,
	[ERROR] = 68,
	[ACK] = 32,

};

int pack_join(const join_t* in_st, char* out_msg){
	char* pt = out_msg;
	uint32_t temp_32;
	uint16_t temp_16;

	memcpy(pt, in_st->node_id, sizeof(uint8_t)*32);
	pt += sizeof(uint8_t)*32;
	
	temp_32 = htonl(in_st->ipv4);
	memcpy(pt, &temp_32, sizeof(uint32_t));
	pt += sizeof(uint32_t);

	temp_16 = htons(in_st->port);
	memcpy(pt, &temp_16, sizeof(uint16_t));
	pt += sizeof(uint16_t);

	memcpy(pt, &in_st->node_type, sizeof(uint8_t));
	pt += sizeof(uint8_t);	
	return 0;
}


int unpack_join(join_t* out_st, const char* in_msg){
	const char* pt = in_msg;
	uint32_t temp_32;
	uint16_t temp_16;

	memcpy(out_st->node_id, pt, sizeof(uint8_t)*32);
	pt += sizeof(uint8_t)*32;
	
	memcpy(&temp_32, pt, sizeof(uint32_t));
	out_st->ipv4 = ntohl(temp_32);
	pt += sizeof(uint32_t);

	memcpy(&temp_16, pt, sizeof(uint16_t));
	out_st->port = ntohs(temp_16);
	pt += sizeof(uint16_t);

	memcpy(&out_st->node_type, pt, sizeof(uint8_t));
	pt += sizeof(uint8_t);	
	return 0;
}

int pack_ack(const ack_t* in_st, char* out_msg){
	char* pt = out_msg;

	memcpy(pt, in_st->node_id, sizeof(uint8_t)*32);
	pt += sizeof(uint8_t)*32;
	
	return 0;
}

int unpack_ack(ack_t* out_st, const char* in_msg){
	const char* pt = in_msg;

	memcpy(out_st->node_id, pt, sizeof(uint8_t)*32);
	pt += sizeof(uint8_t)*32;

	return 0;
}


int pack_error(const error_t* in_st, char* out_msg){
	char* pt = out_msg;
	uint32_t temp_32 = htonl(in_st->code);
	memcpy(pt, &temp_32, sizeof(uint32_t));
	pt += sizeof(uint32_t);

	memcpy(pt, in_st->reason, sizeof(uint8_t)*64);
	pt += sizeof(uint8_t)*64;
	
	return 0;
}

int unpack_error(error_t* out_st, const char* in_msg){
	const char* pt = in_msg;
	uint32_t temp_32;
	memcpy(&temp_32, pt, sizeof(uint32_t));
	out_st->code = ntohl(temp_32);
	pt += sizeof(uint32_t);

	memcpy(out_st->reason, pt, sizeof(uint8_t)*64);
	pt += sizeof(uint8_t)*64;

	return 0;
}

int unpack_header(pl_header* out_st, const char* in_msg){
	const char* pt = in_msg;
	uint32_t temp_32;
	uint64_t temp_64;
	memcpy(&temp_32, pt, sizeof(uint32_t));
	out_st->protocol_ver = ntohl(temp_32);
	pt+=sizeof(uint32_t);
	memcpy(&temp_32, pt, sizeof(uint32_t));
	out_st->msg_type = ntohl(temp_32);
	pt+=sizeof(uint32_t);

	memcpy(out_st->src_node, pt, sizeof(uint8_t)*32);
	pt+=(sizeof(uint8_t)*32);
	memcpy(out_st->dst_node, pt, sizeof(uint8_t)*32);
	pt+=(sizeof(uint8_t)*32);
	memcpy(out_st->trsc_id, pt, sizeof(uint8_t)*16);
	pt+=(sizeof(uint8_t)*16);

	memcpy(&temp_64, pt, sizeof(uint64_t));
	out_st->time = my_ntohll(temp_64);
	pt+=sizeof(uint64_t);
	memcpy(&temp_32, pt, sizeof(uint32_t));
	out_st->pl_size = ntohl(temp_32);
	pt+=sizeof(uint32_t);
	memcpy(&temp_32, pt, sizeof(uint32_t));
	out_st->checksum = ntohl(temp_32);
	pt+=sizeof(uint32_t);
	
	return 0;
}

int pack_header(const pl_header* in_st, char* out_st){
	char* pt = out_st;
	uint32_t temp_32;
	uint64_t temp_64;
	temp_32 = htonl(in_st->protocol_ver);
	memcpy(pt, &temp_32, sizeof(uint32_t));
	pt+=sizeof(uint32_t);

	temp_32 = htonl(in_st->msg_type);
	memcpy(pt, &temp_32, sizeof(uint32_t));
	pt+=sizeof(uint32_t);

	memcpy(pt, in_st->src_node, sizeof(uint8_t)*32);
	pt+=(sizeof(uint8_t)*32);
	memcpy(pt, in_st->dst_node, sizeof(uint8_t)*32);
	pt+=(sizeof(uint8_t)*32);
	memcpy(pt, in_st->trsc_id, sizeof(uint8_t)*16);
	pt+=(sizeof(uint8_t)*16);

	temp_64 = my_ntohll(in_st->time);
	memcpy(pt, &temp_64, sizeof(uint64_t));
	pt+=sizeof(uint64_t);
	temp_32 = htonl(in_st->pl_size);
	memcpy(pt, &temp_32, sizeof(uint32_t));
	pt+=sizeof(uint32_t);
	temp_32 = htonl(in_st->checksum);
	memcpy(pt, &temp_32, sizeof(uint32_t));
	pt+=sizeof(uint32_t);
	
	return 0;

}

int send_message(int fd, char* out_msg_buffer, const void* msg_payload, const pl_header* msg_header){
		
	int status;
	char* buffer_pointer = out_msg_buffer;
	uint32_t message_type = msg_header->msg_type;
	uint32_t payload_size = msg_header->pl_size;

	pack_header(msg_header, buffer_pointer);
	buffer_pointer += HEADER_SIZE;

	if(message_type >= MSG_TYPE_MAX){
		fprintf(stderr, "recv_message # corrupted header");
		status = NET_ERROR; // TODO: add more error status types for logging
		return status;
	}

	if(message_type != DOWNLOAD_REP && message_type != DOWNLOAD_REQ && message_type != GOSSIP && message_type != STATE_TRANSFER && message_type != SNAPSHOT){
		if(payload_size > MAX_CONTROL_PAYLOAD_SZ || payload_sizes[message_type] != payload_size){
			fprintf(stderr, "recv_message # corrupted header");
			status = NET_ERROR; 
			return status;
		}
	
		switch(message_type){
			case JOIN: {
				pack_join(msg_payload, buffer_pointer);
				break;
			}
			case ACK: {
				pack_ack(msg_payload, buffer_pointer); 
				break;
			}
			case ERROR: {
				pack_error(msg_payload, buffer_pointer);
				break;
			}

			default:
				return NET_ERROR; 
		}
		if((status = send_all(fd, out_msg_buffer ,HEADER_SIZE + payload_size)) != NET_OK)
			return status;


	}
	return NET_OK;
}

msg_t recv_message(int fd, char* in_msg_buffer, void* struct_payload){

	char header_buffer[HEADER_SIZE]; 

	int status;
	
	if((status = recv_all(fd, header_buffer, HEADER_SIZE)) != NET_OK)
		return (msg_t){{0}, NULL, status};

	pl_header msg_header;

	unpack_header(&msg_header, header_buffer);
	
	uint32_t payload_size = msg_header.pl_size;
	uint32_t message_type = msg_header.msg_type;

	if(message_type >= MSG_TYPE_MAX){
		fprintf(stderr, "recv_message # corrupted header");
		status = NET_ERROR; // TODO: add more error status types for logging
		return (msg_t){{0}, NULL, status};
	}

	if(message_type != DOWNLOAD_REP && message_type != DOWNLOAD_REQ &&  message_type != GOSSIP && message_type != STATE_TRANSFER && message_type != SNAPSHOT){
		if(payload_size > MAX_CONTROL_PAYLOAD_SZ || payload_sizes[message_type] != payload_size){
			fprintf(stderr, "recv_message # corrupted header");
			status = NET_ERROR; 
			return (msg_t){{0}, NULL, status};
		}
		if((status = recv_all(fd, in_msg_buffer, payload_size))!= NET_OK)
			return (msg_t){{0}, NULL, status};
	
		switch(message_type){
			case JOIN: {
				join_t new_st;
				unpack_join(&new_st, in_msg_buffer);
				memcpy(struct_payload, &new_st, sizeof(join_t));
				break;
			}
			case ACK: {
				ack_t new_st;
				unpack_ack(&new_st, in_msg_buffer);
				memcpy(struct_payload, &new_st, sizeof(ack_t));
				break;
			}
			case ERROR: {
				error_t new_st;
				unpack_error(&new_st, in_msg_buffer);
				memcpy(struct_payload, &new_st, sizeof(error_t));
				return (msg_t){msg_header, struct_payload, NET_OK}; 
				break;
			}

			default:
				return (msg_t){msg_header, NULL, NET_OK}; 
		}

	}
	return (msg_t){msg_header, struct_payload, NET_OK}; 

}



