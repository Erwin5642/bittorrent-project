#include <endian.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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




/* Tamanho fixo do payload, em bytes. -1 = layout ainda indefinido. */
static const int32_t payload_sizes[MSG_TYPE_MAX] = {
	[JOIN] = 39,
	[PING] = 0,
	[PONG] = 0,
	[LEAVE] = 32,
	[LOOKUP] = -1,
	[STORE] = -1,
	[DOWNLOAD_REQ] = -1,
	[DOWNLOAD_REP] = -1,
	[PREPARE] = -1,
	[COMMIT] = -1,
	[ABORT] = -1,
	[HEARTBEAT] = -1,
	[GOSSIP] = -1,
	[ELECTION] = -1,
	[OK] = -1,
	[COORDINATOR] = -1,
	[SNAPSHOT] = -1,
	[STATE_TRANSFER] = -1,
	[ACK] = 32,
	[ERROR] = 68,
};

/* Tamanho do payload de um tipo, ou -1 se fora de faixa ou ainda sem layout. */
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


/* ===== PACK e UNPACK ===== */

/* Serializa join_t no buffer (32B node_id + 4B ipv4 + 2B port + 1B tipo = 39B). */
int pack_join(const join_t* in_st, uint8_t* out_msg){
	uint8_t* pt = out_msg;
	if (!in_st || !out_msg)
		return -1;
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
int unpack_join(join_t* out_st, const uint8_t* in_msg){
	const uint8_t* pt = in_msg;
	if (!out_st || !in_msg)
		return -1;
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
int pack_ack(const ack_t* in_st, uint8_t* out_msg){
	uint8_t* pt = out_msg;
	if (!in_st || !out_msg)
		return -1;

	memcpy(pt, in_st->node_id, sizeof(uint8_t)*32);
	pt += sizeof(uint8_t)*32;

	return 0;
}

/* Le ack_t (32B node_id). */
int unpack_ack(ack_t* out_st, const uint8_t* in_msg){
	const uint8_t* pt = in_msg;
	if (!out_st || !in_msg)
		return -1;

	memcpy(out_st->node_id, pt, sizeof(uint8_t)*32);
	pt += sizeof(uint8_t)*32;

	return 0;
}


/* Serializa error_t (4B code big-endian + 64B reason = 68B). */
int pack_error(const error_t* in_st, uint8_t* out_msg){
	uint8_t* pt = out_msg;
	if (!in_st || !out_msg)
		return -1;
	uint32_t temp_32 = htonl(in_st->code);
	memcpy(pt, &temp_32, sizeof(uint32_t));
	pt += sizeof(uint32_t);

	memcpy(pt, in_st->reason, sizeof(uint8_t)*64);
	pt += sizeof(uint8_t)*64;
	
	return 0;
}

/* Le error_t (inverso de pack_error). */
int unpack_error(error_t* out_st, const uint8_t* in_msg){
	const uint8_t* pt = in_msg;
	if (!out_st || !in_msg)
		return -1;
	uint32_t temp_32;
	memcpy(&temp_32, pt, sizeof(uint32_t));
	out_st->code = ntohl(temp_32);
	pt += sizeof(uint32_t);

	memcpy(out_st->reason, pt, sizeof(uint8_t)*64);
	pt += sizeof(uint8_t)*64;

	return 0;
}

/* Serializa leave_t (32B node_id). */
int pack_leave(const leave_t* in_st, uint8_t* out_msg){
	if (!in_st || !out_msg)
		return -1;
	memcpy(out_msg, in_st->node_id, sizeof(uint8_t)*32);
	return 0;
}

/* Le leave_t (32B node_id). */
int unpack_leave(leave_t* out_st, const uint8_t* in_msg){
	if (!out_st || !in_msg)
		return -1;
	memcpy(out_st->node_id, in_msg, sizeof(uint8_t)*32);
	return 0;
}

/* ObjectID zerado nao identifica arquivo. */
int metadata_id_is_zero(const uint8_t id[METADATA_OBJECT_ID_SIZE]) {
	size_t i;

	for (i = 0; i < METADATA_OBJECT_ID_SIZE; i++) {
		if (id[i] != 0)
			return 0;
	}
	return 1;
}

/* Nome logico: nao vazio e com NUL dentro dos 256 bytes do campo. */
int metadata_name_ok(const char *filename) {
	size_t i;

	if (!filename || filename[0] == '\0')
		return 0;
	for (i = 0; i < METADATA_FILENAME_MAX; i++) {
		if (filename[i] == '\0')
			return 1;
	}
	return 0;
}

/* Prefixo fixo mais a cauda de hashes. 0 se passar de METADATA_PAYLOAD_MAX. */
size_t metadata_wire_size(uint32_t chunk_count) {
	size_t tail;

	if (chunk_count > (METADATA_PAYLOAD_MAX - METADATA_WIRE_PREFIX) / METADATA_CHUNK_HASH_SIZE)
		return 0;
	tail = (size_t)chunk_count * METADATA_CHUNK_HASH_SIZE;
	return (size_t)METADATA_WIRE_PREFIX + tail;
}

/* Zera o registro. O bloco de hashes, se houver, continua com quem chama. */
void metadata_init(file_metadata_t *meta) {
	if (!meta)
		return;
	memset(meta, 0, sizeof *meta);
}

/* Libera a cauda de hashes e zera o registro. */
void metadata_release(file_metadata_t *meta) {
	if (!meta)
		return;
	free(meta->chunk_hashes);
	metadata_init(meta);
}

/*
 * Prefixo de METADATA_WIRE_PREFIX bytes e, em seguida, chunk_count hashes
 * crus de 32 bytes. O ponteiro chunk_hashes nao vai para o fio.
 */
ssize_t metadata_pack(const file_metadata_t *meta, uint8_t *out, size_t out_cap) {
	uint64_t temp_64;
	uint32_t temp_32;

	if (!meta || !out)
		return -1;
	if (metadata_id_is_zero(meta->object_id) || !metadata_name_ok(meta->filename))
		return -1;
	if (meta->chunk_count > 0 && !meta->chunk_hashes)
		return -1;

	const size_t need = metadata_wire_size(meta->chunk_count);
	if (need == 0 || out_cap < need)
		return -1;

	uint8_t* pt = out;
	memcpy(pt, meta->object_id, METADATA_OBJECT_ID_SIZE);
	pt += METADATA_OBJECT_ID_SIZE;

	memcpy(pt, meta->filename, METADATA_FILENAME_MAX);
	pt += METADATA_FILENAME_MAX;

	temp_64 = my_ntohll(meta->size);
	memcpy(pt, &temp_64, sizeof temp_64);
	pt += sizeof temp_64;

	temp_32 = htonl(meta->chunk_count);
	memcpy(pt, &temp_32, sizeof temp_32);
	pt += sizeof temp_32;

	temp_32 = htonl(meta->version);
	memcpy(pt, &temp_32, sizeof temp_32);
	pt += sizeof temp_32;

	memcpy(pt, meta->owner.bytes, NODE_ID_SIZE);
	pt += NODE_ID_SIZE;

	/* Cada hash e um digest cru; nao ha endianness dentro do SHA-256. */
	if (meta->chunk_count > 0) {
		const size_t tail = (size_t)meta->chunk_count * METADATA_CHUNK_HASH_SIZE;
		memcpy(pt, meta->chunk_hashes, tail);
	}
	return (ssize_t)need;
}

/*
 * Inverso de metadata_pack. A cauda so e alocada depois de validar o
 * prefixo; falha devolve 0 sem deixar bloco pendente e sem mexer em out.
 */
int metadata_unpack(file_metadata_t *out, const uint8_t *in, size_t in_len) {
	file_metadata_t tmp;
	uint64_t temp_64;
	uint32_t temp_32;

	if (!out || !in || in_len < METADATA_WIRE_PREFIX)
		return 0;

	metadata_init(&tmp);
	const uint8_t* pt = in;

	memcpy(tmp.object_id, pt, METADATA_OBJECT_ID_SIZE);
	pt += METADATA_OBJECT_ID_SIZE;

	memcpy(tmp.filename, pt, METADATA_FILENAME_MAX);
	pt += METADATA_FILENAME_MAX;

	memcpy(&temp_64, pt, sizeof temp_64);
	tmp.size = my_ntohll(temp_64);
	pt += sizeof temp_64;

	memcpy(&temp_32, pt, sizeof temp_32);
	tmp.chunk_count = ntohl(temp_32);
	pt += sizeof temp_32;

	memcpy(&temp_32, pt, sizeof temp_32);
	tmp.version = ntohl(temp_32);
	pt += sizeof temp_32;

	memcpy(tmp.owner.bytes, pt, NODE_ID_SIZE);
	pt += NODE_ID_SIZE;

	if (metadata_id_is_zero(tmp.object_id) || !metadata_name_ok(tmp.filename))
		return 0;

	const size_t need = metadata_wire_size(tmp.chunk_count);
	if (need == 0 || in_len != need)
		return 0;

	if (tmp.chunk_count > 0) {
		const size_t tail = (size_t)tmp.chunk_count * METADATA_CHUNK_HASH_SIZE;
		tmp.chunk_hashes = malloc(tail);
		if (!tmp.chunk_hashes)
			return 0;
		memcpy(tmp.chunk_hashes, pt, tail);
	}

	*out = tmp;
	return 1;
}

/* Le o header de 99 bytes do buffer, convertendo os campos de big-endian. */
int unpack_header(pl_header* out_st, const uint8_t* in_msg){
	const uint8_t* pt = in_msg;
	if (!out_st || !in_msg)
		return -1;
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
int pack_header(const pl_header* in_st, uint8_t* out_st){
	uint8_t* pt = out_st;
	if (!in_st || !out_st)
		return -1;
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
 * Empacota o payload do tipo, calcula o CRC32 desses bytes e serializa o header
 * ja com o checksum. Nao envia.
 */
ssize_t serialize_message(uint8_t *out_buf, size_t out_cap, const void *payload,
                          pl_header *hdr) {
    uint16_t message_type;
    uint32_t payload_size;
    uint8_t *payload_area;
    int pack_rc = 0;
    size_t payload_bytes;
    uLong crc;

    if (!out_buf || !hdr)
        return -1;

    message_type = hdr->msg_type;
    payload_size = hdr->pl_size;

    if (message_type >= MSG_TYPE_MAX) {
        fprintf(stderr, "serialize_message # tipo invalido: %u\n", message_type);
        return -1;
    }
    if (payload_sizes[message_type] >= 0 &&
        payload_size != (uint32_t)payload_sizes[message_type]) {
        fprintf(stderr, "serialize_message # tamanho invalido para %s\n",
                message_type_name(message_type));
        return -1;
    }
    if (payload_size > MAX_CONTROL_PAYLOAD_SZ)
        return -1;
    if (out_cap < (size_t)HEADER_SIZE + (size_t)payload_size)
        return -1;

    payload_area = out_buf + HEADER_SIZE;

    switch (message_type) {
    case JOIN:  pack_rc = pack_join((const join_t *)payload, payload_area); break;
    case LEAVE: pack_rc = pack_leave((const leave_t *)payload, payload_area); break;
    case ACK:   pack_rc = pack_ack((const ack_t *)payload, payload_area); break;
    case ERROR: pack_rc = pack_error((const error_t *)payload, payload_area); break;
    case PING:
    case PONG:
        break;                      /* sem payload */
    default:
        return -1;
    }
    if (pack_rc != 0)
        return -1;

    payload_bytes = payload_size;

    /* Checksum cobre apenas o payload; o header ja empacotado o carrega. */
    crc = crc32(0L, Z_NULL, 0);
    if (payload_bytes > 0)
        crc = crc32(crc, (const Bytef *)payload_area, payload_bytes);
    hdr->checksum = (uint32_t)crc;

    if (pack_header(hdr, out_buf) != 0)
        return -1;

    return (ssize_t)((size_t)HEADER_SIZE + payload_bytes);
}

/*
 * Le o header, confere o CRC32 do payload e so entao faz o unpack do tipo.
 * CRC invalido devolve NET_CORRUPTED_MSG sem chamar unpack_*.
 */
int deserialize_message(const uint8_t *in_buf, const size_t buf_len, pl_header *out_hdr,
                        void *out_payload) {
    const uint8_t *payload_area;
    uint16_t message_type;
    uint32_t payload_size;
    uLong actual_crc;
    int unpack_rc = 0;

    if (!in_buf || !out_hdr || buf_len < HEADER_SIZE)
        return NET_ERROR;
    if (unpack_header(out_hdr, in_buf) != 0)
        return NET_ERROR;
    if (out_hdr->protocol_ver != PROTOCOL_VER)
        return NET_ERROR;

    message_type = out_hdr->msg_type;
    payload_size = out_hdr->pl_size;

    if (message_type >= MSG_TYPE_MAX) {
        fprintf(stderr, "deserialize_message # tipo invalido: %u\n", message_type);
        return NET_ERROR;
    }
    if (payload_sizes[message_type] >= 0 &&
        payload_size != (uint32_t)payload_sizes[message_type]) {
        fprintf(stderr, "deserialize_message # tamanho invalido para %s\n",
                message_type_name(message_type));
        return NET_ERROR;
    }
    if (payload_size > MAX_CONTROL_PAYLOAD_SZ)
        return NET_ERROR;
    if (buf_len < (size_t)HEADER_SIZE + (size_t)payload_size)
        return NET_ERROR;

    payload_area = in_buf + HEADER_SIZE;

    actual_crc = crc32(0L, Z_NULL, 0);
    if (payload_size > 0)
        actual_crc = crc32(actual_crc, (const Bytef *)payload_area, payload_size);
    if ((uint32_t)actual_crc != out_hdr->checksum)
        return NET_CORRUPTED_MSG;

    switch (message_type) {
    case JOIN:  unpack_rc = unpack_join(out_payload, payload_area); break;
    case LEAVE: unpack_rc = unpack_leave(out_payload, payload_area); break;
    case ACK:   unpack_rc = unpack_ack(out_payload, payload_area); break;
    case ERROR: unpack_rc = unpack_error(out_payload, payload_area); break;
    case PING:
    case PONG:
        break;
    default:
        return NET_ERROR;
    }
    if (unpack_rc != 0)
        return NET_ERROR;

    return NET_OK;
}

/* Serializa com serialize_message e envia o resultado num unico send_all. */
int send_message(const int fd, uint8_t *out_msg_buffer, const size_t buf_size,
                 const void *msg_payload, pl_header *msg_header) {
    ssize_t n;

    n = serialize_message(out_msg_buffer, buf_size, msg_payload, msg_header);
    if (n < 0)
        return NET_ERROR;

    return send_all(fd, out_msg_buffer, (uint32_t)n);
}

/*
 * Le o header, valida versao/tipo/pl_size, le o payload, confere o CRC32 e
 * desserializa para struct_payload. Devolve msg_t com header, payload e status.
 */
msg_t recv_message(int fd, uint8_t *in_msg_buffer, size_t in_buf_size,
                   void *struct_payload, size_t struct_size){

	uint8_t frame[HEADER_SIZE + MAX_CONTROL_PAYLOAD_SZ];
	pl_header msg_header;
	int status;
	int decoded;
	uint32_t payload_size;
	uint16_t message_type;
	size_t struct_need = 0;

	/* 1) header de tamanho fixo; o payload entra logo depois, no mesmo frame. */
	if((status = recv_all(fd, frame, HEADER_SIZE)) != NET_OK)
		return (msg_t){{0}, NULL, status};

	if (unpack_header(&msg_header, frame) != 0)
		return (msg_t){{0}, NULL, NET_ERROR};

	if (msg_header.protocol_ver != PROTOCOL_VER) {
		return (msg_t){msg_header, NULL, NET_ERROR};
	}

	payload_size = msg_header.pl_size;
	message_type = msg_header.msg_type;

	if(message_type >= MSG_TYPE_MAX){
		fprintf(stderr, "recv_message # corrupted header");
		return (msg_t){{0}, NULL, NET_ERROR};
	}

	/* Tipos de payload variavel (CP2+) ficam fora da validacao de tamanho fixo. */
	if(message_type != DOWNLOAD_REP && message_type != DOWNLOAD_REQ &&  message_type != GOSSIP && message_type != STATE_TRANSFER && message_type != SNAPSHOT){
		if (payload_size > MAX_CONTROL_PAYLOAD_SZ ||
		    (payload_sizes[message_type] >= 0 &&
		     payload_size != (uint32_t)payload_sizes[message_type])) {
			fprintf(stderr, "recv_message # corrupted header");
			return (msg_t){{0}, NULL, NET_ERROR};
		}
		if (payload_size > 0 && (!in_msg_buffer || in_buf_size < payload_size)) {
			fprintf(stderr, "recv_message # buffer curto");
			return (msg_t){msg_header, NULL, NET_ERROR};
		}
		if((status = recv_all(fd, frame + HEADER_SIZE, payload_size))!= NET_OK)
			return (msg_t){{0}, NULL, status};
		if (payload_size > 0)
			memcpy(in_msg_buffer, frame + HEADER_SIZE, payload_size);

		switch (message_type) {
		case JOIN:  struct_need = sizeof(join_t); break;
		case LEAVE: struct_need = sizeof(leave_t); break;
		case ACK:   struct_need = sizeof(ack_t); break;
		case ERROR: struct_need = sizeof(error_t); break;
		default:    break;
		}
		if (struct_need > 0 && (!struct_payload || struct_size < struct_need))
			return (msg_t){msg_header, NULL, NET_ERROR};

		decoded = deserialize_message(frame, (size_t)HEADER_SIZE + payload_size,
		                              &msg_header, struct_payload);
		if (decoded != NET_OK)
			return (msg_t){msg_header, NULL, decoded};
	}
	return (msg_t){msg_header, struct_payload, NET_OK};
}

/* Variante de send_message para um payload de bytes arbitrario. */
int simple_send(int fd, uint8_t* out_msg_buffer, const uint8_t* payload, uint32_t str_size, pl_header* msg_header){

	int status;
	uint8_t* buffer_pointer = out_msg_buffer;
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
	
	memcpy(buffer_pointer, payload, str_size);

	if((status = send_all(fd, out_msg_buffer, HEADER_SIZE + payload_size)) != NET_OK)
		return status;

	return NET_OK;
}

/* Variante de recv_message para um payload de bytes arbitrario. */
msg_t simple_recv(int fd, uint8_t* payload, uint32_t str_size){
	uint8_t header_buffer[HEADER_SIZE];

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

/* Monta um header de origem: gera TransactionID e deixa dst_node zerado. */
static int fill_origin_header(pl_header *out, const node_id_t *self, uint16_t msg_type, uint32_t pl_size) {
	node_uuid_t trsc;

	if (!out || !self || !node_uuid_random(&trsc)) {
		return 0;
	}
	memset(out, 0, sizeof *out);
	out->protocol_ver = PROTOCOL_VER;
	out->msg_type = msg_type;
	memcpy(out->src_node, self->bytes, NODE_ID_SIZE);
	memcpy(out->trsc_id, trsc.bytes, NODE_UUID_SIZE);
	out->time = (uint64_t)time(NULL);
	out->pl_size = pl_size;
	return 1;
}

/* Atalho: envia LEAVE como mensagem de origem (TransactionID novo). */
int send_leave(int fd, const node_id_t *self, const leave_t *leave) {
	uint8_t buf[HEADER_SIZE + 32];
	pl_header hdr;
	leave_t body;
	uint8_t zero_id[NODE_ID_SIZE];

	if (!self || !leave) {
		return NET_ERROR;
	}
	memset(zero_id, 0, sizeof zero_id);
	body = *leave;
	if (memcmp(body.node_id, zero_id, NODE_ID_SIZE) == 0) {
		memcpy(body.node_id, self->bytes, NODE_ID_SIZE);
	}
	if (!fill_origin_header(&hdr, self, LEAVE, (uint32_t)payload_sizes[LEAVE])) {
		return NET_ERROR;
	}
	return send_message(fd, buf, sizeof buf, &body, &hdr);
}

/* Atalho: envia JOIN como mensagem de origem (TransactionID novo). */
int send_join(int fd, const node_id_t *self, const join_t *join) {
	uint8_t buf[HEADER_SIZE + 39];
	pl_header hdr;
	join_t body;

	if (!self || !join) {
		return NET_ERROR;
	}
	body = *join;
	memcpy(body.node_id, self->bytes, NODE_ID_SIZE);
	if (!fill_origin_header(&hdr, self, JOIN, (uint32_t)payload_sizes[JOIN])) {
		return NET_ERROR;
	}
	return send_message(fd, buf, sizeof buf, &body, &hdr);
}

/* Atalho: responde ACK ecoando o TransactionID de req. */
int send_ack(int fd, const pl_header *req, const node_id_t *self, const ack_t *ack) {
	uint8_t buf[HEADER_SIZE + payload_sizes[ACK]];
	pl_header hdr;

	if (!req || !self || !ack) {
		return NET_ERROR;
	}
	fill_reply_header(&hdr, req, self, ACK, payload_sizes[ACK]);
	return send_message(fd, buf, sizeof buf, ack, &hdr);
}

/* Atalho: responde ERROR (code + reason) ecoando o TransactionID de req. */
int send_error(int fd, const pl_header *req, const node_id_t *self, uint32_t code, const char *reason) {
	uint8_t buf[HEADER_SIZE + payload_sizes[ERROR]];
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
	return send_message(fd, buf, sizeof buf, &err, &hdr);
}
