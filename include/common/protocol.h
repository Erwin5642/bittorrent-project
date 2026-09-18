#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "node.h"

#include <stdint.h>

/**
 * @file protocol.h
 * @brief Framing TCP, header padrão e payloads de controle (JOIN/PING/PONG/LEAVE/ACK/ERROR).
 *
 * Toda mensagem no fio é @c HEADER_SIZE bytes de header (big-endian) seguidos de
 * @c pl_size bytes de payload. O header carrega um CRC32 do payload. A serialização
 * é explícita (pack/unpack), nunca @c send do struct em memória, para não vazar
 * padding nem endianness do host.
 */

/** Tamanho fixo do header serializado, em bytes (soma dos campos de @c pl_header). */
#define HEADER_SIZE 99
/** Teto de bytes de payload aceito para mensagens de controle. */
#define MAX_CONTROL_PAYLOAD_SZ 4096
/** Versão do framing no primeiro byte do header (`!BH` no fio). */
#define PROTOCOL_VER 1

/**
 * @brief Tipos de mensagem do protocolo.
 *
 * Cobre todos os checkpoints; o CP1 exercita apenas o subconjunto de controle
 * (@c JOIN, @c PING, @c PONG, @c LEAVE, @c ACK, @c ERROR). @c MSG_TYPE_MAX é
 * sentinela para validação de faixa e dimensionamento de tabelas.
 */
enum message_type{
	JOIN = 0,        /**< Entrada de nó na rede (payload @c join_t). */
	PING = 1,        /**< Sonda de conectividade, sem payload. */
	PONG = 2,        /**< Resposta a @c PING, sem payload. */
	LEAVE = 3,       /**< Saída de nó da rede (payload @c leave_t). */
	LOOKUP,          /**< Busca de recurso na DHT (CP3). */
	STORE,           /**< Armazenamento de metadado (CP2). */
	DOWNLOAD_REQ,    /**< Requisição de download (CP2, payload variável). */
	DOWNLOAD_REP,    /**< Resposta de download (CP2, payload variável). */
	PREPARE,         /**< Fase 1 do 2PC (CP5). */
	COMMIT,          /**< Confirmação do 2PC (CP5). */
	ABORT,           /**< Aborto do 2PC (CP5). */
	HEARTBEAT,       /**< Batimento de detecção de falhas (CP4). */
	GOSSIP,          /**< Disseminação Gossip (CP4, payload variável). */
	ELECTION,        /**< Mensagem de eleição Bully (CP4). */
	OK,              /**< Resposta positiva na eleição Bully (CP4). */
	COORDINATOR,     /**< Anúncio de coordenador eleito (CP4). */
	SNAPSHOT,        /**< Snapshot de estado (CP5, payload variável). */
	STATE_TRANSFER,  /**< Transferência de estado incremental (CP5, payload variável). */
	ACK,             /**< Confirmação (payload @c ack_t). */
	ERROR,           /**< Erro (payload @c error_t). */
	MSG_TYPE_MAX,    /**< Sentinela: número de tipos válidos. */
};

/**
 * @brief Algoritmo de compressão de um objeto (CP2).
 */
typedef enum compression_type{
	LZ4,  /**< Compressão LZ4. */
}compress_type;

/**
 * @brief Estado de um metadado no ciclo de replicação (CP2/CP5).
 */
typedef enum metadata_status{
	ACTIVE,       /**< Ativo e disponível. */
	REPLICATING,  /**< Em replicação. */
	REMOVED,      /**< Removido logicamente. */
}mtdata_status;

/**
 * @brief Nome legível de um tipo de mensagem, para log.
 * @param t Valor de @c message_type.
 * @return String estática (ex.: "JOIN"); "UNKNOWN" se fora de faixa.
 */
const char *message_type_name(uint16_t t);

/**
 * @brief Header padrão, comum a toda mensagem.
 *
 * Layout no fio (big-endian, @c HEADER_SIZE = 99 bytes):
 * off 0 `protocol_ver`(1), 1 `msg_type`(2), 3 `src_node`(32), 35 `dst_node`(32),
 * 67 `trsc_id`(16), 83 `time`(8), 91 `pl_size`(4), 95 `checksum`(4).
 */
typedef struct payloadHeader{
	uint8_t protocol_ver;  /**< Versão do protocolo; deve ser @c PROTOCOL_VER. */
	uint16_t msg_type;     /**< Tipo da mensagem (@c message_type). */
	uint8_t src_node[32];  /**< NodeID da origem (32 bytes crus). */
	uint8_t dst_node[32];  /**< NodeID do destino (32 bytes crus). */
	uint8_t trsc_id[16];   /**< TransactionID de 128 bits. */
	uint64_t time;         /**< Timestamp Unix da origem. */
	uint32_t pl_size;      /**< Bytes de payload após o header. */
	uint32_t checksum;     /**< CRC32 do payload. */
}pl_header;

/**
 * @brief Metadado de um objeto na Hash Table distribuída (CP2).
 */
typedef struct file_metadata{
	uint8_t obj_id[32];          /**< ObjectID = SHA-256 do conteúdo. */
	char* file_name;             /**< Nome do arquivo. */
	uint64_t version;            /**< Versão do metadado. */
	uint64_t size;               /**< Tamanho em bytes. */
	compress_type compression;   /**< Algoritmo de compressão. */
	uint8_t owner_peer[32];      /**< NodeID do dono. */
	uint8_t* replica_peers[32];  /**< NodeIDs das réplicas. */
	uint32_t chunk_count;        /**< Número de chunks. */
	uint8_t* chunk_hash[32];     /**< Hash de cada chunk. */
	uint64_t upload_date;        /**< Data de upload. */
	uint64_t last_access;        /**< Último acesso (LFU). */
	uint64_t download_counter;   /**< Contador de downloads (LFU). */
	mtdata_status status;        /**< Estado do metadado. */
}fl_mtdata;

/**
 * @brief Resultado de um @c recv: header, payload desserializado e status.
 */
typedef struct message_struct{
	pl_header header;  /**< Header recebido. */
	void* payload;     /**< Aponta para o struct de payload preenchido, ou NULL. */
	uint8_t status;    /**< @c NET_OK, @c NET_ERROR ou @c NET_CLOSED. */
}msg_t;

/**
 * @brief Payload de JOIN (39 bytes): identidade e endereço anunciado do joiner.
 */
typedef struct joinPayload{
	uint8_t node_id[32];  /**< NodeID do joiner; deve bater com @c src_node. */
	uint32_t ipv4;        /**< IPv4 anunciado, network byte order. */
	uint16_t port;        /**< Porta anunciada, big-endian no fio. */
	uint8_t node_type;    /**< 0 = peer, 1 = superpeer. */
}join_t;

/**
 * @brief Payload de LEAVE: NodeID de quem sai (32 bytes).
 */
typedef struct leavePayload{
	uint8_t node_id[32];  /**< NodeID de quem está saindo. */
}leave_t;

/**
 * @brief Payload de ACK: NodeID confirmado (32 bytes).
 */
typedef struct ackPayload{
	uint8_t node_id[32];  /**< NodeID confirmado (o do joiner). */
}ack_t;

/**
 * @brief Payload de ERROR (68 bytes): código e motivo.
 */
typedef struct errorPayload{
	uint32_t code;       /**< 1 malformado, 2 tabela cheia, 3 não suportado, 4 tipo inválido. */
	uint8_t reason[64];  /**< Texto UTF-8, NUL-padded. */
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

/**
 * @brief Serializa header+payload de controle e envia em um único @c send_all.
 *
 * Empacota o payload conforme @c msg_type, calcula o CRC32 do payload,
 * preenche @c checksum, serializa o header e envia @c HEADER_SIZE + @c pl_size bytes.
 * @param fd Socket conectado.
 * @param out_msg_buffer Buffer de trabalho; ao menos @c HEADER_SIZE + @c pl_size bytes.
 * @param msg_payload Struct do payload (@c join_t, @c ack_t, ...) ou NULL para PING/PONG.
 * @param msg_header Header com @c msg_type e @c pl_size já definidos; @c checksum é preenchido aqui.
 * @return @c NET_OK em sucesso, @c NET_ERROR em tipo/tamanho inválido ou erro de envio.
 */
int send_message(int fd, char* out_msg_buffer, const void* msg_payload, pl_header* msg_header);

/**
 * @brief Recebe header+payload de controle, valida e desserializa.
 *
 * Lê o header, confere @c PROTOCOL_VER, o tipo e @c pl_size, lê o payload,
 * valida o CRC32 e faz o unpack para @p struct_payload.
 * @param fd Socket conectado.
 * @param in_msg_buffer Buffer de trabalho para o payload cru.
 * @param struct_payload Destino do struct desserializado; o caller aloca.
 * @return @c msg_t com @c status @c NET_OK, @c NET_ERROR (versão/tipo/tamanho/CRC) ou @c NET_CLOSED.
 */
msg_t recv_message(int fd, char* in_msg_buffer, void* struct_payload);

/**
 * @brief Envia um header seguido de um payload de bytes arbitrário (string).
 * @param fd Socket conectado.
 * @param out_msg_buffer Buffer de trabalho; ao menos @c HEADER_SIZE + @p str_size bytes.
 * @param payload Bytes do payload.
 * @param str_size Tamanho do payload; deve ser igual a @c msg_header->pl_size.
 * @param msg_header Header com @c msg_type e @c pl_size; @c checksum é preenchido aqui.
 * @return @c NET_OK em sucesso, @c NET_ERROR em tipo/tamanho inválido ou erro de envio.
 */
int simple_send(int fd, char* out_msg_buffer, const char* payload, uint32_t str_size, pl_header* msg_header);

/**
 * @brief Recebe um header seguido de um payload de bytes arbitrário, validando o CRC.
 * @param fd Socket conectado.
 * @param payload Destino do payload; o caller aloca ao menos @p str_size bytes.
 * @param str_size Capacidade de @p payload; @c pl_size maior é rejeitado.
 * @return @c msg_t com @c status @c NET_OK, @c NET_ERROR (tipo/tamanho/CRC) ou @c NET_CLOSED.
 */
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
 * @brief Tamanho fixo do payload de um tipo de controle.
 * @param t Valor de @c message_type.
 * @return Bytes do payload (0 para PING/PONG), ou @c -1 se @p t estiver fora de faixa.
 */
int32_t payload_size_for(uint16_t t);

#endif
