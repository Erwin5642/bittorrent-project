#include <endian.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <time.h>
#include <zconf.h>
#include <zlib.h>

#include "../../include/common/network.h"
#include "../../include/common/protocol.h"


/*
 * protocol.c — header padrao, serializacao (pack/unpack), CRC32 e framing.
 * Tudo no fio vai em big-endian; o CRC32 (zlib) cobre apenas o payload.
 * Uma mensagem = HEADER_SIZE bytes de header + pl_size bytes de payload.
 */


/* ntohll portavel: converte uint64 de network para host byte order. */
static uint64_t my_ntohll(uint64_t val) {
    #if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        return val;
    #else
        return (((uint64_t)ntohl((uint32_t)(val & 0xFFFFFFFF))) << 32) | 
               ntohl((uint32_t)(val >> 32));
    #endif
}




/* Tamanho fixo do payload de cada tipo de controle (bytes). */
static const int32_t payload_sizes[MSG_TYPE_MAX] = {
	[JOIN] = 39,
	[PING] = 0,
	[PONG] = 0,
	[LEAVE] = 32,
	[ERROR] = 68,
	[ACK] = 32,
};

/* Tamanho do payload de um tipo, ou -1 se fora de faixa. */
int32_t payload_size_for(uint16_t t) {
    if (t >= MSG_TYPE_MAX) return -1;
    return payload_sizes[t];
}

//TEMP
static const char *const type_names[MSG_TYPE_MAX] = {
    [PING]  = "PING",
    [PONG]  = "PONG",
    [JOIN]  = "JOIN",
    [LEAVE] = "LEAVE",
    [ACK]   = "ACK",
    [ERROR] = "ERROR",
};

const char *message_type_name(uint16_t t) {
    if (t >= MSG_TYPE_MAX || !type_names[t])
        return "UNKNOWN";
    return type_names[t];
}

//TEMP

/* Serializa join_t no buffer (32B node_id + 4B ipv4 + 2B port + 1B tipo = 39B). */
int pack_join(const join_t* in_st, char* out_msg){
	char* pt = out_msg;
	//uint32_t temp_32;
	uint16_t temp_16;
	uint32_t temp_32;

	memcpy(pt, in_st->node_id, sizeof(uint8_t)*32);
	pt += sizeof(uint8_t)*32;

	/* ipv4 ja vem em network byte order da config; nao reconverter. */
	//temp_32 = htonl(in_st->ipv4);
	temp_32 = in_st->ipv4;
	memcpy(pt, &temp_32, sizeof(uint32_t));
	pt += sizeof(uint32_t);

	temp_16 = htons(in_st->port);
	memcpy(pt, &temp_16, sizeof(uint16_t));
	pt += sizeof(uint16_t);

	memcpy(pt, &in_st->node_type, sizeof(uint8_t));
	pt += sizeof(uint8_t);	
	return 0;
}


/* Le join_t do buffer recebido (inverso de pack_join). */
int unpack_join(join_t* out_st, const char* in_msg){
	const char* pt = in_msg;
	//uint32_t temp_32;
	uint16_t temp_16;
	uint32_t temp_32;
	memcpy(out_st->node_id, pt, sizeof(uint8_t)*32);
	pt += sizeof(uint8_t)*32;
	
	memcpy(&temp_32, pt, sizeof(uint32_t));
	out_st->ipv4 = temp_32;
	//out_st->ipv4 = ntohl(temp_32);
	pt += sizeof(uint32_t);
	 

	memcpy(&temp_16, pt, sizeof(uint16_t));
	out_st->port = ntohs(temp_16);
	pt += sizeof(uint16_t);

	memcpy(&out_st->node_type, pt, sizeof(uint8_t));
	pt += sizeof(uint8_t);	
	return 0;
}

/* Serializa ack_t (32B node_id). */
int pack_ack(const ack_t* in_st, char* out_msg){
	char* pt = out_msg;

	memcpy(pt, in_st->node_id, sizeof(uint8_t)*32);
	pt += sizeof(uint8_t)*32;

	return 0;
}

/* Le ack_t (32B node_id). */
int unpack_ack(ack_t* out_st, const char* in_msg){
	const char* pt = in_msg;

	memcpy(out_st->node_id, pt, sizeof(uint8_t)*32);
	pt += sizeof(uint8_t)*32;

	return 0;
}


/* Serializa error_t (4B code big-endian + 64B reason = 68B). */
int pack_error(const error_t* in_st, char* out_msg){
	char* pt = out_msg;
	uint32_t temp_32 = htonl(in_st->code);
	memcpy(pt, &temp_32, sizeof(uint32_t));
	pt += sizeof(uint32_t);

	memcpy(pt, in_st->reason, sizeof(uint8_t)*64);
	pt += sizeof(uint8_t)*64;
	
	return 0;
}

/* Le error_t (inverso de pack_error). */
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

/* Serializa leave_t (32B node_id). */
int pack_leave(const leave_t* in_st, char* out_msg){
	memcpy(out_msg, in_st->node_id, sizeof(uint8_t)*32);
	return 0;
}

/* Le leave_t (32B node_id). */
int unpack_leave(leave_t* out_st, const char* in_msg){
	memcpy(out_st->node_id, in_msg, sizeof(uint8_t)*32);
	return 0;
}

/* Le o header de 99 bytes do buffer, convertendo os campos de big-endian. */
int unpack_header(pl_header* out_st, const char* in_msg){
	const char* pt = in_msg;
	uint16_t temp_16;
	uint32_t temp_32;
	uint64_t temp_64;

	memcpy(&out_st->protocol_ver, pt, 1);
	pt+=sizeof(uint8_t);
	memcpy(&temp_16, pt, sizeof(uint16_t));
	out_st->msg_type = ntohs(temp_16);
	pt+=sizeof(uint16_t);

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

/* Serializa o header de 99 bytes no buffer, em big-endian. */
int pack_header(const pl_header* in_st, char* out_st){
	char* pt = out_st;
	uint16_t temp_16;
	uint32_t temp_32;
	uint64_t temp_64;

	memcpy(pt, &in_st->protocol_ver, 1);
	pt+=sizeof(uint8_t);
	temp_16 = htons(in_st->msg_type);
	memcpy(pt, &temp_16, sizeof(uint16_t));
	pt+=sizeof(uint16_t);

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


/*
 * Empacota o payload conforme o tipo, calcula o CRC32 do payload, serializa o
 * header e envia HEADER_SIZE + pl_size bytes num unico send_all.
 */
int send_message(int fd, char *out_msg_buffer, const void *msg_payload,
                 pl_header *msg_header) {
    int status;
    uint16_t message_type = msg_header->msg_type;
    uint32_t payload_size = msg_header->pl_size;
    char *payload_area = out_msg_buffer + HEADER_SIZE;

    if (message_type >= MSG_TYPE_MAX) {
        fprintf(stderr, "send_message # tipo invalido: %u\n", message_type);
        return NET_ERROR;
    }

    if (payload_sizes[message_type] < 0 &&
        payload_size != (uint32_t)payload_sizes[message_type]) {
        fprintf(stderr, "send_message # tamanho invalido para %s\n",
                message_type_name(message_type));
        return NET_ERROR;
    }
    if (payload_size > MAX_CONTROL_PAYLOAD_SZ)
        return NET_ERROR;

    switch (message_type) {
    case JOIN:  pack_join((const join_t *)msg_payload, payload_area);   break;
    case LEAVE: pack_leave((const leave_t *)msg_payload, payload_area); break;
    case ACK:   pack_ack((const ack_t *)msg_payload, payload_area);     break;
    case ERROR: pack_error((const error_t *)msg_payload, payload_area); break;
    case PING:
    case PONG:
        break;                      /* sem payload */
    default:
        return NET_ERROR;
    }

    /* Checksum cobre apenas o payload; o header ja empacotado o carrega. */
    uLong crc = crc32(0L, Z_NULL, 0);
    if (payload_size > 0)
        crc = crc32(crc, (const Bytef *)payload_area, payload_size);
    msg_header->checksum = (uint32_t)crc;

    pack_header(msg_header, out_msg_buffer);

    if ((status = send_all(fd, out_msg_buffer,
                           HEADER_SIZE + payload_size)) != NET_OK)
        return status;

    return NET_OK;
}

/*
 * Le o header, valida versao/tipo/pl_size, le o payload, confere o CRC32 e
 * desserializa para struct_payload. Devolve msg_t com header, payload e status.
 */
msg_t recv_message(int fd, char* in_msg_buffer, void* struct_payload){

	char header_buffer[HEADER_SIZE];

	int status;
	uLong crc = crc32(0L, Z_NULL, 0);

	/* 1) header de tamanho fixo */
	if((status = recv_all(fd, header_buffer, HEADER_SIZE)) != NET_OK)
		return (msg_t){{0}, NULL, status};

	pl_header msg_header;

	unpack_header(&msg_header, header_buffer);

	if (msg_header.protocol_ver != PROTOCOL_VER) {
		return (msg_t){msg_header, NULL, NET_ERROR};
	}

	uint32_t payload_size = msg_header.pl_size;
	uint16_t message_type = msg_header.msg_type;

	if(message_type >= MSG_TYPE_MAX){
		fprintf(stderr, "recv_message # corrupted header");
		status = NET_ERROR; // TODO: add more error status types for logging
		return (msg_t){{0}, NULL, status};
	}

	/* Tipos de payload variavel (CP2+) ficam fora da validacao de tamanho fixo. */
	if(message_type != DOWNLOAD_REP && message_type != DOWNLOAD_REQ &&  message_type != GOSSIP && message_type != STATE_TRANSFER && message_type != SNAPSHOT){
		/* pl_size precisa casar com o tamanho esperado do tipo. */
		if(payload_size > MAX_CONTROL_PAYLOAD_SZ || payload_sizes[message_type] != payload_size){
			fprintf(stderr, "recv_message # corrupted header");
			status = NET_ERROR;
			return (msg_t){{0}, NULL, status};
		}
		/* 2) payload; 3) valida o CRC antes de aceitar a mensagem. */
		if((status = recv_all(fd, in_msg_buffer, payload_size))!= NET_OK)
			return (msg_t){{0}, NULL, status};
		crc = crc32(crc, (const Bytef *)in_msg_buffer, payload_size);
		if(msg_header.checksum != (uint32_t)crc)
			return (msg_t){msg_header, in_msg_buffer, NET_ERROR};
		/* 4) desserializa para o struct do tipo recebido. */
		switch(message_type){
			case JOIN: {
				join_t new_st;
				unpack_join(&new_st, in_msg_buffer);
				memcpy(struct_payload, &new_st, sizeof(join_t));
				break;
			}
			case LEAVE: {
				leave_t new_st;
				unpack_leave(&new_st, in_msg_buffer);
				memcpy(struct_payload, &new_st, sizeof(leave_t));
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
			case PING:
				break;
			case PONG:
				break;
			default:
				return (msg_t){msg_header, NULL, NET_OK}; 
		}

	}
	return (msg_t){msg_header, struct_payload, NET_OK}; 
}

/* Variante de send_message para um payload de bytes/string arbitrario. */
int simple_send(int fd, char* out_msg_buffer, const char* payload, uint32_t str_size, pl_header* msg_header){

	int status;
	char* buffer_pointer = out_msg_buffer;
	uint16_t message_type = msg_header->msg_type;
	uint32_t payload_size = msg_header->pl_size;
	uLong crc = crc32(0L, Z_NULL, 0);
	crc = crc32(crc, (const Bytef *)payload, payload_size);
	msg_header->checksum = crc;

	pack_header(msg_header, buffer_pointer);
	buffer_pointer += HEADER_SIZE;

	if(message_type >= MSG_TYPE_MAX){
		fprintf(stderr, "recv_message # corrupted header");
		status = NET_ERROR; // TODO: add more error status types for logging
		return status;
	}

	if(payload_size > MAX_CONTROL_PAYLOAD_SZ || str_size != payload_size){
		fprintf(stderr, "recv_message # corrupted header");
		status = NET_ERROR; 
		return status;
	}
	
	memcpy(buffer_pointer, payload, sizeof(char)*str_size);

	if((status = send_all(fd, out_msg_buffer, HEADER_SIZE + payload_size)) != NET_OK)
		return status;

	return NET_OK;
}

/* Variante de recv_message para um payload de bytes/string arbitrario. */
msg_t simple_recv(int fd, char* payload, uint32_t str_size){
	char header_buffer[HEADER_SIZE];

	int status;
	
	if((status = recv_all(fd, header_buffer, HEADER_SIZE)) != NET_OK)
		return (msg_t){{0}, NULL, status};

	pl_header msg_header;

	unpack_header(&msg_header, header_buffer);
	
	uint32_t payload_size = msg_header.pl_size;
	uint16_t message_type = msg_header.msg_type;

	if(message_type >= MSG_TYPE_MAX){
		fprintf(stderr, "recv_message # corrupted header");
		status = NET_ERROR; // TODO: add more error status types for logging
		return (msg_t){{0}, NULL, status};
	}

	if(payload_size > str_size){ 
		fprintf(stderr, "recv_message # corrupted header");
		status = NET_ERROR; 
		return (msg_t){{0}, NULL, status};
	}
	if((status = recv_all(fd, payload, payload_size))!= NET_OK)
		return (msg_t){{0}, NULL, status};
			
	uLong crc = crc32(0L, Z_NULL, 0);
	crc = crc32(crc, (const Bytef *)payload, payload_size);
	if(msg_header.checksum != (uint32_t)crc)
		return (msg_t){msg_header, payload, NET_ERROR};
	return (msg_t){msg_header, payload, NET_OK}; 
}

/* Monta um header de resposta: ecoa o TransactionID e troca src/dst. */
void fill_reply_header(pl_header *out, const pl_header *in, const node_id_t *self, uint16_t msg_type,
                       uint32_t pl_size) {
	if (!out || !in || !self) {
		return;
	}
	memset(out, 0, sizeof *out);
	out->protocol_ver = PROTOCOL_VER;
	out->msg_type = msg_type;
	memcpy(out->src_node, self->bytes, NODE_ID_SIZE);
	memcpy(out->dst_node, in->src_node, NODE_ID_SIZE);
	memcpy(out->trsc_id, in->trsc_id, 16);
	out->time = (uint64_t)time(NULL);
	out->pl_size = pl_size;
}


/* Atalho: responde LEAVE ecoando o TransactionID de req. */
int send_leave(int fd, const pl_header *req, const node_id_t *self, const leave_t *leave){
	char buf[HEADER_SIZE + payload_sizes[LEAVE]];
	pl_header hdr;

	if(!req || !self || !leave)
		return NET_ERROR;

	fill_reply_header(&hdr, req, self, LEAVE, payload_sizes[LEAVE]);
	return send_message(fd, buf, buf, &hdr);
}

/* Atalho: responde JOIN ecoando o TransactionID de req. */
int send_join(int fd, const pl_header *req, const node_id_t *self, const join_t *join){
	char buf[HEADER_SIZE + payload_sizes[JOIN]];
	pl_header hdr;

	if(!req || !self || !join)
		return NET_ERROR;

	fill_reply_header(&hdr, req, self, JOIN, payload_sizes[JOIN]);
	return send_message(fd, buf, buf, &hdr);
}

/* Atalho: responde ACK ecoando o TransactionID de req. */
int send_ack(int fd, const pl_header *req, const node_id_t *self, const ack_t *ack) {
	char buf[HEADER_SIZE + payload_sizes[ACK]];
	pl_header hdr;

	if (!req || !self || !ack) {
		return NET_ERROR;
	}
	fill_reply_header(&hdr, req, self, ACK, payload_sizes[ACK]);
	return send_message(fd, buf, ack, &hdr);
}

/* Atalho: responde ERROR (code + reason) ecoando o TransactionID de req. */
int send_error(int fd, const pl_header *req, const node_id_t *self, uint32_t code, const char *reason) {
	char buf[HEADER_SIZE + payload_sizes[ERROR]];
	pl_header hdr;
	error_t err;

	if (!req || !self) {
		return NET_ERROR;
	}
	memset(&err, 0, sizeof err);
	err.code = code;
	if (reason) {
		strncpy((char *)err.reason, reason, sizeof err.reason - 1);
	}
	fill_reply_header(&hdr, req, self, ERROR, payload_sizes[ERROR]);
	return send_message(fd, buf, &err, &hdr);
}

int32_t payload_size_for(uint16_t t);
