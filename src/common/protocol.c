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
	[LOOKUP] = METADATA_FILENAME_MAX,
	[STORE] = -1,
	[DOWNLOAD_REQ] = DOWNLOAD_REQ_WIRE_SIZE,
	[DOWNLOAD_REP] = -1,
	[PREPARE] = -1,
	[COMMIT] = -1,
	[ABORT] = -1,
	[HEARTBEAT] = 0,
	[GOSSIP] = -1,
	[ELECTION] = -1,
	[OK] = -1,
	[COORDINATOR] = -1,
	[SNAPSHOT] = -1,
	[STATE_TRANSFER] = -1,
	[ACK] = 32,
	[ERROR] = 68,
	[CLOSEST_PRECEDING] = -1,
	[GET_PREDECESSOR] = -1,
	[GET_SUCCESSORS] = -1,
	[NOTIFY] = CHORD_NODE_WIRE_SIZE,
	[FIND_SUCCESSOR] = -1,
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
    [LOOKUP] = "LOOKUP",
    [STORE] = "STORE",
    [DOWNLOAD_REQ] = "DOWNLOAD_REQ",
    [DOWNLOAD_REP] = "DOWNLOAD_REP",
    [ACK]   = "ACK",
    [ERROR] = "ERROR",
    [CLOSEST_PRECEDING] = "CLOSEST_PRECEDING",
    [GET_PREDECESSOR] = "GET_PREDECESSOR",
    [GET_SUCCESSORS] = "GET_SUCCESSORS",
    [NOTIFY] = "NOTIFY",
    [FIND_SUCCESSOR] = "FIND_SUCCESSOR",
    [GOSSIP] = "GOSSIP",
};

/* STORE/LOOKUP/DOWNLOAD_* carregam bytes crus e podem passar do teto de controle. */
static int payload_is_data(uint16_t t) {
	return t == STORE || t == LOOKUP || t == DOWNLOAD_REQ || t == DOWNLOAD_REP;
}

static uint32_t payload_ceiling(uint16_t t) {
	return payload_is_data(t) ? MAX_DATA_PAYLOAD_SZ : MAX_CONTROL_PAYLOAD_SZ;
}

static int payload_crc_ok(uint32_t expect, const uint8_t *payload, uint32_t len) {
	uLong crc = crc32(0L, Z_NULL, 0);

	if (len > 0)
		crc = crc32(crc, (const Bytef *)payload, len);
	return (uint32_t)crc == expect;
}

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

/* LOOKUP: filename em campo fixo de METADATA_FILENAME_MAX, resto zerado. */
ssize_t lookup_pack(const char *filename, uint8_t *out, size_t out_cap) {
	size_t n;

	if (!metadata_name_ok(filename) || !out || out_cap < METADATA_FILENAME_MAX)
		return -1;
	memset(out, 0, METADATA_FILENAME_MAX);
	n = strlen(filename);
	memcpy(out, filename, n + 1);
	return (ssize_t)METADATA_FILENAME_MAX;
}

int lookup_unpack(char *filename, size_t filename_cap, const uint8_t *in, size_t in_len) {
	if (!filename || filename_cap < METADATA_FILENAME_MAX || !in ||
	    in_len != METADATA_FILENAME_MAX)
		return 0;
	memcpy(filename, in, METADATA_FILENAME_MAX);
	filename[METADATA_FILENAME_MAX - 1] = '\0';
	return metadata_name_ok(filename);
}

int chord_peer_pack(const chord_peer_t *in, uint8_t *out) {
	uint16_t port_be;

	if (!in || !out)
		return -1;
	memcpy(out, in->id, NODE_ID_SIZE);
	memcpy(out + NODE_ID_SIZE, &in->ipv4, 4);
	port_be = htons(in->port);
	memcpy(out + NODE_ID_SIZE + 4, &port_be, 2);
	return 0;
}

int chord_peer_unpack(chord_peer_t *out, const uint8_t *in) {
	uint16_t port_be;

	if (!out || !in)
		return -1;
	memset(out, 0, sizeof *out);
	memcpy(out->id, in, NODE_ID_SIZE);
	memcpy(&out->ipv4, in + NODE_ID_SIZE, 4);
	memcpy(&port_be, in + NODE_ID_SIZE + 4, 2);
	out->port = ntohs(port_be);
	return 0;
}

ssize_t chord_closest_reply_pack(uint8_t done, const chord_peer_t *node, uint8_t *out, size_t out_cap) {
	if (!node || !out || out_cap < CHORD_CLOSEST_REP_SIZE)
		return -1;
	if (done != CHORD_STEP_NEXT && done != CHORD_STEP_DONE)
		return -1;
	out[0] = done;
	if (chord_peer_pack(node, out + 1) != 0)
		return -1;
	return (ssize_t)CHORD_CLOSEST_REP_SIZE;
}

int chord_closest_reply_unpack(uint8_t *done, chord_peer_t *node, const uint8_t *in, size_t in_len) {
	if (!done || !node || !in || in_len != CHORD_CLOSEST_REP_SIZE)
		return 0;
	if (in[0] != CHORD_STEP_NEXT && in[0] != CHORD_STEP_DONE)
		return 0;
	if (chord_peer_unpack(node, in + 1) != 0)
		return 0;
	*done = in[0];
	return 1;
}

ssize_t chord_successors_pack(const chord_peer_t *nodes, unsigned count, uint8_t *out, size_t out_cap) {
	unsigned i;
	size_t need;

	if (!out || count > CHORD_SUCCESSORS_MAX || (count > 0 && !nodes))
		return -1;
	need = 1u + (size_t)count * CHORD_NODE_WIRE_SIZE;
	if (out_cap < need)
		return -1;
	out[0] = (uint8_t)count;
	for (i = 0; i < count; i++) {
		if (chord_peer_pack(&nodes[i], out + 1u + (size_t)i * CHORD_NODE_WIRE_SIZE) != 0)
			return -1;
	}
	return (ssize_t)need;
}

int chord_successors_unpack(chord_peer_t *nodes, unsigned cap, unsigned *count, const uint8_t *in,
                            size_t in_len) {
	unsigned n;
	unsigned i;

	if (!count || !in || in_len < 1)
		return 0;
	n = in[0];
	if (n > CHORD_SUCCESSORS_MAX || n > cap)
		return 0;
	if (in_len != 1u + (size_t)n * CHORD_NODE_WIRE_SIZE)
		return 0;
	if (n > 0 && !nodes)
		return 0;
	for (i = 0; i < n; i++) {
		if (chord_peer_unpack(&nodes[i], in + 1u + (size_t)i * CHORD_NODE_WIRE_SIZE) != 0)
			return 0;
	}
	*count = n;
	return 1;
}

ssize_t chord_lookup_path_pack(const chord_peer_t *nodes, unsigned count, uint8_t *out, size_t out_cap) {
	unsigned i;
	size_t need;
	uint16_t be;

	if (!out || count > CHORD_LOOKUP_PATH_MAX || (count > 0 && !nodes))
		return -1;
	need = 2u + (size_t)count * CHORD_NODE_WIRE_SIZE;
	if (out_cap < need)
		return -1;
	be = htons((uint16_t)count);
	memcpy(out, &be, 2);
	for (i = 0; i < count; i++) {
		if (chord_peer_pack(&nodes[i], out + 2u + (size_t)i * CHORD_NODE_WIRE_SIZE) != 0)
			return -1;
	}
	return (ssize_t)need;
}

int chord_lookup_path_unpack(chord_peer_t *nodes, unsigned cap, unsigned *count, const uint8_t *in,
                             size_t in_len) {
	uint16_t be;
	unsigned n;
	unsigned i;

	if (!count || !in || in_len < 2)
		return 0;
	memcpy(&be, in, 2);
	n = ntohs(be);
	if (n > CHORD_LOOKUP_PATH_MAX || n > cap)
		return 0;
	if (in_len != 2u + (size_t)n * CHORD_NODE_WIRE_SIZE)
		return 0;
	if (n > 0 && !nodes)
		return 0;
	for (i = 0; i < n; i++) {
		if (chord_peer_unpack(&nodes[i], in + 2u + (size_t)i * CHORD_NODE_WIRE_SIZE) != 0)
			return 0;
	}
	*count = n;
	return 1;
}

_Static_assert(GOSSIP_ENTRY_WIRE_SIZE == 52u, "entrada de gossip");
_Static_assert(2u + GOSSIP_MAX_ENTRIES * GOSSIP_ENTRY_WIRE_SIZE <= MAX_CONTROL_PAYLOAD_SZ,
               "gossip cabe no teto de controle");

/* Uma linha: id, ipv4 (já em network order), porta, tipo, estado, heartbeat, versão. */
static int gossip_entry_pack(const gossip_member_t *row, uint8_t *out) {
	uint8_t *pt = out;
	uint16_t port_be;
	uint32_t ver_be;
	uint64_t hb_be;

	if (!row || !out)
		return -1;
	/* 1 = SUPERPEER, 3 = MEMBER_REMOVED. O protocolo não inclui superpeer.h. */
	if (row->node_type > 1 || row->state > 3)
		return -1;
	memcpy(pt, row->id, NODE_ID_SIZE);
	pt += NODE_ID_SIZE;
	memcpy(pt, &row->ipv4, 4);
	pt += 4;
	port_be = htons(row->port);
	memcpy(pt, &port_be, 2);
	pt += 2;
	*pt++ = row->node_type;
	*pt++ = row->state;
	hb_be = my_ntohll(row->last_heartbeat);
	memcpy(pt, &hb_be, 8);
	pt += 8;
	ver_be = htonl(row->version);
	memcpy(pt, &ver_be, 4);
	return 0;
}

static int gossip_entry_unpack(gossip_member_t *row, const uint8_t *in) {
	const uint8_t *pt = in;
	uint16_t port_be;
	uint32_t ver_be;
	uint64_t hb_be;

	if (!row || !in)
		return -1;
	memset(row, 0, sizeof *row);
	memcpy(row->id, pt, NODE_ID_SIZE);
	pt += NODE_ID_SIZE;
	memcpy(&row->ipv4, pt, 4);
	pt += 4;
	memcpy(&port_be, pt, 2);
	row->port = ntohs(port_be);
	pt += 2;
	row->node_type = *pt++;
	row->state = *pt++;
	if (row->node_type > 1 || row->state > 3)
		return -1;
	memcpy(&hb_be, pt, 8);
	row->last_heartbeat = my_ntohll(hb_be);
	pt += 8;
	memcpy(&ver_be, pt, 4);
	row->version = ntohl(ver_be);
	return 0;
}

ssize_t gossip_members_pack(const gossip_member_t *rows, unsigned count, uint8_t *out, size_t out_cap) {
	unsigned i;
	size_t need;
	uint16_t be;

	if (!out || count > GOSSIP_MAX_ENTRIES || (count > 0 && !rows))
		return -1;
	need = 2u + (size_t)count * GOSSIP_ENTRY_WIRE_SIZE;
	if (out_cap < need)
		return -1;
	be = htons((uint16_t)count);
	memcpy(out, &be, 2);
	for (i = 0; i < count; i++) {
		if (gossip_entry_pack(&rows[i], out + 2u + (size_t)i * GOSSIP_ENTRY_WIRE_SIZE) != 0)
			return -1;
	}
	return (ssize_t)need;
}

int gossip_members_unpack(gossip_member_t *rows, unsigned cap, unsigned *count, const uint8_t *in,
                          size_t in_len) {
	uint16_t be;
	unsigned n;
	unsigned i;

	if (!count || !in || in_len < 2)
		return 0;
	memcpy(&be, in, 2);
	n = ntohs(be);
	if (n > GOSSIP_MAX_ENTRIES || n > cap)
		return 0;
	if (in_len != 2u + (size_t)n * GOSSIP_ENTRY_WIRE_SIZE)
		return 0;
	if (n > 0 && !rows)
		return 0;
	for (i = 0; i < n; i++) {
		if (gossip_entry_unpack(&rows[i], in + 2u + (size_t)i * GOSSIP_ENTRY_WIRE_SIZE) != 0)
			return 0;
	}
	*count = n;
	return 1;
}

/* DOWNLOAD_REQ: 32 bytes de ObjectID + uint32 big-endian. */
ssize_t download_req_pack(const uint8_t object_id[METADATA_OBJECT_ID_SIZE], uint32_t chunk_index,
                          uint8_t *out, size_t out_cap) {
	uint32_t temp_32;

	if (!object_id || metadata_id_is_zero(object_id) || !out ||
	    out_cap < DOWNLOAD_REQ_WIRE_SIZE)
		return -1;
	memcpy(out, object_id, METADATA_OBJECT_ID_SIZE);
	temp_32 = htonl(chunk_index);
	memcpy(out + METADATA_OBJECT_ID_SIZE, &temp_32, sizeof temp_32);
	return (ssize_t)DOWNLOAD_REQ_WIRE_SIZE;
}

int download_req_unpack(uint8_t object_id[METADATA_OBJECT_ID_SIZE], uint32_t *chunk_index,
                        const uint8_t *in, size_t in_len) {
	uint32_t temp_32;

	if (!object_id || !chunk_index || !in || in_len != DOWNLOAD_REQ_WIRE_SIZE)
		return 0;
	memcpy(object_id, in, METADATA_OBJECT_ID_SIZE);
	if (metadata_id_is_zero(object_id))
		return 0;
	memcpy(&temp_32, in + METADATA_OBJECT_ID_SIZE, sizeof temp_32);
	*chunk_index = ntohl(temp_32);
	return 1;
}

/* 0 se o bloco comprimido nao cabe no teto de dados. */
size_t download_rep_wire_size(uint32_t comp_len) {
	size_t total;

	if (comp_len == 0)
		return 0;
	total = (size_t)CHUNK_WIRE_PREFIX + (size_t)comp_len;
	if (total > MAX_DATA_PAYLOAD_SZ)
		return 0;
	return total;
}

/*
 * DOWNLOAD_REP: object_id, indice, tamanho original, tamanho comprimido
 * (uint32 big-endian) e, em seguida, os bytes LZ4.
 */
ssize_t download_rep_pack(const uint8_t object_id[METADATA_OBJECT_ID_SIZE], uint32_t chunk_index,
                          uint32_t original_len, const uint8_t *comp, uint32_t comp_len,
                          uint8_t *out, size_t out_cap) {
	size_t need;
	uint32_t temp_32;
	uint8_t *pt;

	if (!object_id || metadata_id_is_zero(object_id) || !comp || !out)
		return -1;
	if (original_len == 0 || original_len > CHUNK_PLAIN_MAX)
		return -1;
	need = download_rep_wire_size(comp_len);
	if (need == 0 || out_cap < need)
		return -1;

	pt = out;
	memcpy(pt, object_id, METADATA_OBJECT_ID_SIZE);
	pt += METADATA_OBJECT_ID_SIZE;
	temp_32 = htonl(chunk_index);
	memcpy(pt, &temp_32, sizeof temp_32);
	pt += sizeof temp_32;
	temp_32 = htonl(original_len);
	memcpy(pt, &temp_32, sizeof temp_32);
	pt += sizeof temp_32;
	temp_32 = htonl(comp_len);
	memcpy(pt, &temp_32, sizeof temp_32);
	pt += sizeof temp_32;
	memcpy(pt, comp, comp_len);
	return (ssize_t)need;
}

int download_rep_unpack(uint8_t object_id[METADATA_OBJECT_ID_SIZE], uint32_t *chunk_index,
                        uint32_t *original_len, const uint8_t **comp, uint32_t *comp_len,
                        const uint8_t *in, size_t in_len) {
	uint32_t temp_32;
	uint32_t index;
	uint32_t plain;
	uint32_t clen;
	const uint8_t *pt;

	if (!object_id || !chunk_index || !original_len || !comp || !comp_len || !in ||
	    in_len < CHUNK_WIRE_PREFIX)
		return 0;

	pt = in;
	memcpy(object_id, pt, METADATA_OBJECT_ID_SIZE);
	pt += METADATA_OBJECT_ID_SIZE;
	if (metadata_id_is_zero(object_id))
		return 0;

	memcpy(&temp_32, pt, sizeof temp_32);
	index = ntohl(temp_32);
	pt += sizeof temp_32;
	memcpy(&temp_32, pt, sizeof temp_32);
	plain = ntohl(temp_32);
	pt += sizeof temp_32;
	memcpy(&temp_32, pt, sizeof temp_32);
	clen = ntohl(temp_32);
	pt += sizeof temp_32;

	if (plain == 0 || plain > CHUNK_PLAIN_MAX)
		return 0;
	if (download_rep_wire_size(clen) != in_len)
		return 0;

	*chunk_index = index;
	*original_len = plain;
	*comp_len = clen;
	*comp = pt;
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
    case NOTIFY: pack_rc = chord_peer_pack((const chord_peer_t *)payload, payload_area); break;
    case PING:
    case PONG:
    case HEARTBEAT:
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
    if (payload_size > payload_ceiling(message_type))
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
    case NOTIFY: unpack_rc = chord_peer_unpack(out_payload, payload_area); break;
    case PING:
    case PONG:
    case HEARTBEAT:
        break;
    case STORE:
    case LOOKUP:
    case DOWNLOAD_REQ:
    case DOWNLOAD_REP:
    case CLOSEST_PRECEDING:
    case GET_PREDECESSOR:
    case GET_SUCCESSORS:
    case FIND_SUCCESSOR:
    case GOSSIP:
        break;   /* payload cru; o handler desserializa */
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
 *
 * STORE/LOOKUP/DOWNLOAD_* sao lidos por inteiro (ate MAX_DATA_PAYLOAD_SZ) e
 * devolvidos crus em in_msg_buffer. GOSSIP também. STATE_TRANSFER e SNAPSHOT
 * continuam sem leitura do corpo: o layout deles pertence aos checkpoints seguintes.
 */
msg_t recv_message(int fd, uint8_t *in_msg_buffer, size_t in_buf_size,
                   void *struct_payload, size_t struct_size){

	uint8_t header_buf[HEADER_SIZE];
	pl_header msg_header;
	int status;
	int decoded;
	uint32_t payload_size;
	uint16_t message_type;
	size_t struct_need = 0;

	if((status = recv_all(fd, header_buf, HEADER_SIZE)) != NET_OK)
		return (msg_t){{0}, NULL, status};

	if (unpack_header(&msg_header, header_buf) != 0)
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

	/* Layout ainda indefinido (CP5): nao consome o corpo. */
	if (message_type == STATE_TRANSFER || message_type == SNAPSHOT)
		return (msg_t){msg_header, struct_payload, NET_OK};

	if (payload_size > payload_ceiling(message_type) ||
	    (payload_sizes[message_type] >= 0 &&
	     payload_size != (uint32_t)payload_sizes[message_type])) {
		fprintf(stderr, "recv_message # corrupted header");
		return (msg_t){{0}, NULL, NET_ERROR};
	}
	if (payload_size > 0 && (!in_msg_buffer || in_buf_size < payload_size)) {
		fprintf(stderr, "recv_message # buffer curto");
		return (msg_t){msg_header, NULL, NET_ERROR};
	}

	/*
	 * Acima do teto de controle o frame nao cabe na pilha. O corpo vai direto
	 * para o buffer do caller; o CRC e conferido aqui, sem deserialize_message.
	 */
	if (payload_size > MAX_CONTROL_PAYLOAD_SZ) {
		if ((status = recv_all(fd, in_msg_buffer, payload_size)) != NET_OK)
			return (msg_t){{0}, NULL, status};
		if (!payload_crc_ok(msg_header.checksum, in_msg_buffer, payload_size))
			return (msg_t){msg_header, in_msg_buffer, NET_CORRUPTED_MSG};
		return (msg_t){msg_header, in_msg_buffer, NET_OK};
	}

	{
		uint8_t frame[HEADER_SIZE + MAX_CONTROL_PAYLOAD_SZ];

		memcpy(frame, header_buf, HEADER_SIZE);
		if((status = recv_all(fd, frame + HEADER_SIZE, payload_size))!= NET_OK)
			return (msg_t){{0}, NULL, status};
		if (payload_size > 0)
			memcpy(in_msg_buffer, frame + HEADER_SIZE, payload_size);

		switch (message_type) {
		case JOIN:  struct_need = sizeof(join_t); break;
		case LEAVE: struct_need = sizeof(leave_t); break;
		case ACK:   struct_need = sizeof(ack_t); break;
		case ERROR: struct_need = sizeof(error_t); break;
		case NOTIFY: struct_need = sizeof(chord_peer_t); break;
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

/* Variante de send_message para um payload de bytes ja empacotados. */
int simple_send(int fd, uint8_t* out_msg_buffer, const uint8_t* payload, uint32_t str_size, pl_header* msg_header){

	int status;
	uint8_t* buffer_pointer;
	uint16_t message_type;
	uint32_t payload_size;
	uLong crc;

	if (!out_msg_buffer || !msg_header)
		return NET_ERROR;

	message_type = msg_header->msg_type;
	payload_size = msg_header->pl_size;

	if(message_type >= MSG_TYPE_MAX){
		fprintf(stderr, "simple_send # tipo invalido\n");
		return NET_ERROR;
	}
	if(payload_size > payload_ceiling(message_type) || str_size != payload_size){
		fprintf(stderr, "simple_send # tamanho invalido\n");
		return NET_ERROR;
	}
	if (payload_size > 0 && !payload)
		return NET_ERROR;

	crc = crc32(0L, Z_NULL, 0);
	if (payload_size > 0)
		crc = crc32(crc, (const Bytef *)payload, payload_size);
	msg_header->checksum = (uint32_t)crc;

	buffer_pointer = out_msg_buffer;
	if (pack_header(msg_header, buffer_pointer) != 0)
		return NET_ERROR;
	buffer_pointer += HEADER_SIZE;
	if (payload_size > 0)
		memcpy(buffer_pointer, payload, str_size);

	if((status = send_all(fd, out_msg_buffer, HEADER_SIZE + payload_size)) != NET_OK)
		return status;

	return NET_OK;
}

/* Variante de recv_message para um payload de bytes arbitrario. */
msg_t simple_recv(int fd, uint8_t* payload, uint32_t str_size){
	uint8_t header_buffer[HEADER_SIZE];
	pl_header msg_header;
	int status;
	uint32_t payload_size;
	uint16_t message_type;

	if((status = recv_all(fd, header_buffer, HEADER_SIZE)) != NET_OK)
		return (msg_t){{0}, NULL, status};

	if (unpack_header(&msg_header, header_buffer) != 0)
		return (msg_t){{0}, NULL, NET_ERROR};

	payload_size = msg_header.pl_size;
	message_type = msg_header.msg_type;

	if(message_type >= MSG_TYPE_MAX){
		fprintf(stderr, "simple_recv # tipo invalido\n");
		return (msg_t){{0}, NULL, NET_ERROR};
	}

	if(payload_size > payload_ceiling(message_type) || payload_size > str_size){
		fprintf(stderr, "simple_recv # tamanho invalido\n");
		return (msg_t){{0}, NULL, NET_ERROR};
	}
	if (payload_size > 0 && !payload)
		return (msg_t){msg_header, NULL, NET_ERROR};
	if((status = recv_all(fd, payload, payload_size))!= NET_OK)
		return (msg_t){{0}, NULL, status};

	if (!payload_crc_ok(msg_header.checksum, payload, payload_size))
		return (msg_t){msg_header, payload, NET_CORRUPTED_MSG};
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

/* Atalho: envia HEARTBEAT como mensagem de origem (TransactionID novo), sem payload. */
int send_heartbeat(int fd, const node_id_t *self) {
	uint8_t buf[HEADER_SIZE];
	pl_header hdr;

	if (!self) {
		return NET_ERROR;
	}
	if (!fill_origin_header(&hdr, self, HEARTBEAT, 0)) {
		return NET_ERROR;
	}
	return send_message(fd, buf, sizeof buf, NULL, &hdr);
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
