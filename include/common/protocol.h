#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "node.h"

#include <stdint.h>

/**
 * @file protocol.h
 * @brief Framing TCP, header padrão e payloads de controle (JOIN/PING/PONG/LEAVE/ACK/ERROR).
 */

#define HEADER_SIZE 99
#define MAX_CONTROL_PAYLOAD_SZ 4096
/** Versão do framing no primeiro byte do header (`!BH` no fio). */
#define PROTOCOL_VER 1

enum message_type{
	JOIN = 0,
	PING = 1,
	PONG = 2,
	LEAVE = 3,
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

/**
 * @brief Payload de LEAVE: NodeID de quem sai (32 bytes).
 */
typedef struct leavePayload{
	uint8_t node_id[32];
}leave_t;

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

/**
 * @brief Monta um header de resposta: ecoa TransactionID, troca src/dst.
 * @param out Header de destino; o caller aloca.
 * @param in Header da mensagem recebida.
 * @param self NodeID de quem responde (vira @c src_node).
 * @param msg_type Tipo da resposta (`ACK`, `ERROR`, …).
 * @param pl_size Tamanho do payload em bytes.
 */
void fill_reply_header(pl_header *out, const pl_header *in, const node_id_t *self, uint16_t msg_type,
                       uint32_t pl_size);

/**
 * @brief Envia um ACK ecoando o TransactionID de @p req.
 * @param fd Socket conectado.
 * @param req Header da mensagem original.
 * @param self NodeID de quem responde.
 * @param ack Payload com o NodeID confirmado.
 * @return @c NET_OK em sucesso, @c NET_ERROR caso contrário.
 */
int send_ack(int fd, const pl_header *req, const node_id_t *self, const ack_t *ack);

/**
 * @brief Envia um ERROR ecoando o TransactionID de @p req.
 * @param fd Socket conectado.
 * @param req Header da mensagem original.
 * @param self NodeID de quem responde.
 * @param code Código CP1 (`1` malformado, `2` tabela cheia, `3` não suportado, `4` tipo inválido).
 * @param reason Texto UTF-8 (até 63 chars + NUL); pode ser NULL.
 * @return @c NET_OK em sucesso, @c NET_ERROR caso contrário.
 */
int send_error(int fd, const pl_header *req, const node_id_t *self, uint32_t code, const char *reason);

/**
 * @brief Envia um JOIN (mensagem de origem, não reply).
 * @param fd Socket conectado ao Super Peer.
 * @param self NodeID de quem entra (vira @c src_node e @c join.node_id).
 * @param join IPv4/porta anunciados e @c node_type.
 * @return @c NET_OK em sucesso, @c NET_ERROR caso contrário.
 */
int send_join(int fd, const node_id_t *self, const join_t *join);

/**
 * @brief Envia um LEAVE (mensagem de origem, não reply).
 * @param fd Socket conectado ao Super Peer.
 * @param self NodeID de quem sai (vira @c src_node).
 * @param leave Se @c node_id estiver zerado, usa @p self.
 * @return @c NET_OK em sucesso, @c NET_ERROR caso contrário.
 */
int send_leave(int fd, const node_id_t *self, const leave_t *leave);

/**
 * @brief Tamanho fixo do payload no fio, ou @c -1 se o tipo for inválido.
 * @param t Valor de @c message_type.
 */
int32_t payload_size_for(uint16_t t);

#endif
