#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "node.h"

#include <stdint.h>
#include <sys/types.h>

/**
 * @file protocol.h
 * @brief Framing TCP, header padrão, payloads de controle e o registro de metadata no fio.
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
/**
 * @brief Tamanho máximo do chunk original no CP2, em bytes (4 MiB).
 *
 * Igual a @c CHUNK_SIZE de @c file_pipeline.h. Os dois precisam coincidir:
 * o teto de dados abaixo cabe exatamente um chunk desse tamanho comprimido.
 */
#define CHUNK_PLAIN_MAX (4u * 1024u * 1024u)
/**
 * @brief Prefixo fixo de @c DOWNLOAD_REP, em bytes.
 *
 * object_id (32) + chunk_index (4) + original_len (4) + comp_len (4) = 44.
 * Os bytes LZ4 vêm em seguida.
 */
#define CHUNK_WIRE_PREFIX 44u
/**
 * @brief Teto de payload das mensagens de dados (STORE, LOOKUP, DOWNLOAD_*).
 *
 * Cabe um chunk de @c CHUNK_PLAIN_MAX após LZ4 (bound = n + n/255 + 16) mais
 * o prefixo de @c DOWNLOAD_REP. Mensagens de controle continuam em
 * @c MAX_CONTROL_PAYLOAD_SZ.
 */
#define MAX_DATA_PAYLOAD_SZ (CHUNK_WIRE_PREFIX + CHUNK_PLAIN_MAX + (CHUNK_PLAIN_MAX / 255u) + 16u)
/** Payload fixo de @c DOWNLOAD_REQ: object_id (32) + chunk_index (4). */
#define DOWNLOAD_REQ_WIRE_SIZE 36u
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
	uint32_t code;       /**< 1 malformado, 2 membros cheia, 3 não suportado, 4 tipo inválido, 6 metadata cheia. */
	uint8_t reason[64];  /**< Texto UTF-8, NUL-padded. */
}error_t;

/*
int pack_join(const join_t* in_st, uint8_t* out_msg);
int unpack_join(join_t* out_st, const uint8_t* in_msg);
int pack_ack(const ack_t* in_st, uint8_t* out_msg);
int unpack_ack(ack_t* out_st, const uint8_t* in_msg);
int pack_error(const error_t* in_st, uint8_t* out_msg);
int unpack_error(error_t* out_st, const uint8_t* in_msg);
int unpack_header(pl_header* out_st, const uint8_t* in_msg);
int pack_header(const pl_header* in_st, uint8_t* out_st);
*/

/**
 * @brief Serializa header e payload de controle e grava o CRC32 do payload.
 *
 * Empacota o payload conforme @c hdr->msg_type, calcula o CRC32 desses bytes,
 * preenche @c hdr->checksum e serializa o header no início de @p out_buf.
 * @param out_buf Destino da mensagem completa.
 * @param out_cap Capacidade de @p out_buf, em bytes. Precisa caber @c HEADER_SIZE + @c pl_size.
 * @param payload Struct do payload (@c join_t, @c ack_t, ...) ou NULL para PING/PONG.
 * @param hdr Header com @c msg_type e @c pl_size já definidos; @c checksum é preenchido aqui.
 * @return Bytes escritos (@c HEADER_SIZE + payload), ou @c -1 se tipo, tamanho ou buffer forem inválidos.
 */
ssize_t serialize_message(uint8_t *out_buf, size_t out_cap, const void *payload, pl_header *hdr);

/**
 * @brief Desserializa uma mensagem já recebida: header, CRC32 e payload.
 *
 * Lê o header em @p in_buf, confere o CRC32 do payload e só então faz o unpack
 * do tipo (@c JOIN, @c LEAVE, @c ACK, @c ERROR). PING/PONG não têm payload.
 * @c STORE, @c LOOKUP, @c DOWNLOAD_REQ e @c DOWNLOAD_REP passam com o payload
 * cru: o handler desserializa. O teto desses tipos é @c MAX_DATA_PAYLOAD_SZ.
 * @param in_buf Mensagem completa: @c HEADER_SIZE bytes de header e em seguida o payload.
 * @param buf_len Bytes válidos em @p in_buf. Precisa cobrir @c HEADER_SIZE + @c pl_size.
 * @param out_hdr Recebe o header; o caller aloca.
 * @param out_payload Recebe o struct do tipo (@c join_t, ...). NULL em PING/PONG.
 * @return @c NET_OK, @c NET_CORRUPTED_MSG se o CRC falhar, ou @c NET_ERROR.
 */
int deserialize_message(const uint8_t *in_buf, size_t buf_len, pl_header *out_hdr, void *out_payload);

/**
 * @brief Serializa header+payload de controle e envia em um único @c send_all.
 *
 * Empacota o payload conforme @c msg_type, calcula o CRC32 do payload,
 * preenche @c checksum, serializa o header e envia @c HEADER_SIZE + @c pl_size bytes.
 * @param fd Socket conectado.
 * @param out_msg_buffer Buffer de trabalho.
 * @param buf_size Capacidade de @p out_msg_buffer, em bytes. Precisa caber @c HEADER_SIZE + @c pl_size.
 * @param msg_payload Struct do payload (@c join_t, @c ack_t, ...) ou NULL para PING/PONG.
 * @param msg_header Header com @c msg_type e @c pl_size já definidos; @c checksum é preenchido aqui.
 * @return @c NET_OK em sucesso, @c NET_ERROR em tipo/tamanho inválido, buffer curto ou erro de envio.
 */
int send_message(int fd, uint8_t *out_msg_buffer, size_t buf_size, const void *msg_payload,
                 pl_header *msg_header);

/**
 * @brief Recebe uma mensagem, valida o CRC32 e desserializa o controle.
 *
 * Lê o header, confere @c PROTOCOL_VER, o tipo e @c pl_size, lê o payload
 * inteiro e valida o CRC32. Controle (@c JOIN, @c LEAVE, @c ACK, @c ERROR)
 * vai para @p struct_payload. @c STORE, @c LOOKUP, @c DOWNLOAD_REQ e
 * @c DOWNLOAD_REP ficam crus em @p in_msg_buffer, até @c MAX_DATA_PAYLOAD_SZ.
 * @param fd Socket conectado.
 * @param in_msg_buffer Buffer de trabalho para o payload cru. Para
 *        @c DOWNLOAD_REP precisa caber @c MAX_DATA_PAYLOAD_SZ.
 * @param in_buf_size Capacidade de @p in_msg_buffer, em bytes. @c pl_size maior é rejeitado.
 * @param struct_payload Destino do struct desserializado; o caller aloca. Ignorado em PING/PONG.
 * @param struct_size Capacidade de @p struct_payload, em bytes. Precisa caber o struct do tipo recebido.
 * @return @c msg_t com @c status @c NET_OK, @c NET_ERROR (versão/tipo/tamanho/CRC/buffer) ou @c NET_CLOSED.
 */
msg_t recv_message(int fd, uint8_t *in_msg_buffer, size_t in_buf_size, void *struct_payload,
                   size_t struct_size);

/**
 * @brief Envia um header seguido de um payload de bytes já empacotados.
 *
 * Controle fica no teto @c MAX_CONTROL_PAYLOAD_SZ. @c STORE, @c LOOKUP,
 * @c DOWNLOAD_REQ e @c DOWNLOAD_REP podem ir até @c MAX_DATA_PAYLOAD_SZ.
 * @param fd Socket conectado.
 * @param out_msg_buffer Buffer de trabalho; ao menos @c HEADER_SIZE + @p str_size bytes.
 * @param payload Bytes do payload.
 * @param str_size Tamanho do payload; deve ser igual a @c msg_header->pl_size.
 * @param msg_header Header com @c msg_type e @c pl_size; @c checksum é preenchido aqui.
 * @return @c NET_OK em sucesso, @c NET_ERROR em tipo/tamanho inválido ou erro de envio.
 */
int simple_send(int fd, uint8_t* out_msg_buffer, const uint8_t* payload, uint32_t str_size, pl_header* msg_header);

/**
 * @brief Recebe um header seguido de um payload de bytes arbitrário, validando o CRC.
 * @param fd Socket conectado.
 * @param payload Destino do payload; o caller aloca ao menos @p str_size bytes.
 * @param str_size Capacidade de @p payload; @c pl_size maior é rejeitado.
 * @return @c msg_t com @c status @c NET_OK, @c NET_ERROR (tipo/tamanho/CRC) ou @c NET_CLOSED.
 */
msg_t simple_recv(int fd, uint8_t* payload, uint32_t str_size);

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
 * @param code `1` malformado, `2` membros cheia, `3` não suportado, `4` tipo inválido, `6` metadata cheia.
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
 * @brief Tamanho fixo do payload de um tipo de controle.
 * @param t Valor de @c message_type.
 * @return Bytes do payload (0 para PING/PONG), ou @c -1 se @p t estiver fora de faixa.
 */
int32_t payload_size_for(uint16_t t);

/** Tamanho do ObjectID, em bytes. */
#define METADATA_OBJECT_ID_SIZE 32

/** Tamanho do hash de um chunk, em bytes. */
#define METADATA_CHUNK_HASH_SIZE 32

/** Nome lógico no fio, incluindo o NUL. No máximo 255 caracteres úteis. */
#define METADATA_FILENAME_MAX 256

/**
 * @brief Teto do registro serializado, em bytes.
 *
 * Distinto do payload de controle (4096). Um registro de metadata cabe
 * abaixo de um chunk de 4 MB.
 */
#define METADATA_PAYLOAD_MAX (64u * 1024u)

/**
 * @brief Prefixo fixo no fio, em bytes.
 *
 * object_id (32) + filename (256) + size (8) + chunk_count (4) +
 * version (4) + owner (32) = 336. A cauda são @c chunk_count hashes.
 */
#define METADATA_WIRE_PREFIX 336

/**
 * @brief Metadata de um arquivo, no fio e na tabela do Super Peer.
 *
 * Corresponde ao `FileMetadata` do checkpoint. O dono é o NodeID de 32
 * bytes já usado no JOIN. Os hashes formam um bloco contíguo, não um
 * vetor de ponteiros.
 */
typedef struct {
  uint8_t object_id[METADATA_OBJECT_ID_SIZE]; /**< ObjectID, 32 bytes crus. */
  char filename[METADATA_FILENAME_MAX];       /**< Nome lógico, NUL-terminated. */
  uint64_t size;                              /**< Tamanho do arquivo original, em bytes. */
  uint32_t chunk_count;                       /**< Quantidade de hashes em @c chunk_hashes. */
  uint32_t version;                           /**< Versão do registro. */
  node_id_t owner;                            /**< NodeID de quem publicou. */
  uint8_t *chunk_hashes;                      /**< @c chunk_count * 32 bytes, ou NULL se zero.
                                                   SHA-256 do chunk original, antes do LZ4. */
} file_metadata_t;

/**
 * @brief Diz se o ObjectID é 32 bytes zero.
 * @param id Chave de 32 bytes.
 * @return 1 se todos os bytes forem zero, 0 caso contrário.
 */
int metadata_id_is_zero(const uint8_t id[METADATA_OBJECT_ID_SIZE]);

/**
 * @brief Diz se o nome cabe no campo e não é vazio.
 * @param filename Nome a validar. Pode ser NULL.
 * @return 1 se houver ao menos um caractere e um NUL dentro de @c METADATA_FILENAME_MAX, 0 caso contrário.
 */
int metadata_name_ok(const char *filename);

/**
 * @brief Tamanho no fio de um registro com @p chunk_count hashes.
 * @param chunk_count Quantidade de chunks.
 * @return @c METADATA_WIRE_PREFIX + hashes, ou 0 se passar de @c METADATA_PAYLOAD_MAX.
 */
size_t metadata_wire_size(uint32_t chunk_count);

/**
 * @brief Zera um registro. Não libera @c chunk_hashes.
 * @param meta Destino; o caller aloca. NULL é ignorado.
 */
void metadata_init(file_metadata_t *meta);

/**
 * @brief Libera o bloco de hashes e zera o registro.
 * @param meta Registro cuja memória de @c chunk_hashes pertence a quem chama.
 */
void metadata_release(file_metadata_t *meta);

/**
 * @brief Serializa um registro em big-endian, sem ponteiros.
 *
 * Prefixo de @c METADATA_WIRE_PREFIX bytes e, em seguida, os hashes na
 * ordem dos chunks. Cada inteiro multibyte vai em network byte order.
 * @param meta Registro de origem.
 * @param out Destino; o caller aloca.
 * @param out_cap Capacidade de @p out, em bytes.
 * @return Bytes escritos, ou -1 se o registro ou o buffer forem inválidos.
 */
ssize_t metadata_pack(const file_metadata_t *meta, uint8_t *out, size_t out_cap);

/**
 * @brief Reconstrói um registro a partir do layout de @c metadata_pack.
 *
 * Aloca @c chunk_hashes quando @c chunk_count é maior que zero. O chamador
 * libera com @c metadata_release. ObjectID zerado, nome vazio ou cauda que
 * não fecha com @c chunk_count falham sem deixar memória pendente.
 * @param out Destino; o caller aloca o struct.
 * @param in Bytes do payload, sem o header de mensagem.
 * @param in_len Tamanho de @p in. Tem de ser igual ao tamanho calculado por @c chunk_count.
 * @return 1 em sucesso, 0 se o buffer, o nome ou a cauda forem inválidos.
 */
int metadata_unpack(file_metadata_t *out, const uint8_t *in, size_t in_len);

/**
 * @brief Serializa um LOOKUP: nome em campo fixo de @c METADATA_FILENAME_MAX bytes.
 * @param filename Nome lógico, NUL-terminated.
 * @param out Destino; o caller aloca ao menos @c METADATA_FILENAME_MAX bytes.
 * @param out_cap Capacidade de @p out.
 * @return Bytes escritos, ou -1 se o nome ou o buffer forem inválidos.
 */
ssize_t lookup_pack(const char *filename, uint8_t *out, size_t out_cap);

/**
 * @brief Lê o nome de um LOOKUP empacotado por @c lookup_pack.
 * @param filename Destino, com espaço para @c METADATA_FILENAME_MAX bytes.
 * @param filename_cap Capacidade de @p filename.
 * @param in Payload recebido.
 * @param in_len Tamanho de @p in. Tem de ser @c METADATA_FILENAME_MAX.
 * @return 1 em sucesso, 0 se o buffer ou o nome forem inválidos.
 */
int lookup_unpack(char *filename, size_t filename_cap, const uint8_t *in, size_t in_len);

/**
 * @brief Serializa um DOWNLOAD_REQ (object_id + índice, big-endian).
 * @param object_id ObjectID de 32 bytes.
 * @param chunk_index Índice do chunk.
 * @param out Destino; o caller aloca ao menos @c DOWNLOAD_REQ_WIRE_SIZE bytes.
 * @param out_cap Capacidade de @p out.
 * @return Bytes escritos, ou -1 se argumento ou buffer forem inválidos.
 */
ssize_t download_req_pack(const uint8_t object_id[METADATA_OBJECT_ID_SIZE], uint32_t chunk_index,
                          uint8_t *out, size_t out_cap);

/**
 * @brief Lê um DOWNLOAD_REQ empacotado por @c download_req_pack.
 * @param object_id Recebe o ObjectID; o caller aloca 32 bytes.
 * @param chunk_index Recebe o índice.
 * @param in Payload recebido.
 * @param in_len Tamanho de @p in. Tem de ser @c DOWNLOAD_REQ_WIRE_SIZE.
 * @return 1 em sucesso, 0 se o buffer for inválido ou o ObjectID for zero.
 */
int download_req_unpack(uint8_t object_id[METADATA_OBJECT_ID_SIZE], uint32_t *chunk_index,
                        const uint8_t *in, size_t in_len);

/**
 * @brief Tamanho no fio de um DOWNLOAD_REP com @p comp_len bytes LZ4.
 * @param comp_len Tamanho do bloco comprimido.
 * @return @c CHUNK_WIRE_PREFIX + @p comp_len, ou 0 se passar de @c MAX_DATA_PAYLOAD_SZ
 *         ou se @p comp_len for 0.
 */
size_t download_rep_wire_size(uint32_t comp_len);

/**
 * @brief Serializa um DOWNLOAD_REP: prefixo big-endian e em seguida os bytes LZ4.
 * @param object_id ObjectID de 32 bytes.
 * @param chunk_index Índice do chunk.
 * @param original_len Tamanho do chunk antes do LZ4. No máximo @c CHUNK_PLAIN_MAX.
 * @param comp Bytes comprimidos.
 * @param comp_len Tamanho de @p comp.
 * @param out Destino; o caller aloca.
 * @param out_cap Capacidade de @p out.
 * @return Bytes escritos, ou -1 se o registro ou o buffer forem inválidos.
 */
ssize_t download_rep_pack(const uint8_t object_id[METADATA_OBJECT_ID_SIZE], uint32_t chunk_index,
                          uint32_t original_len, const uint8_t *comp, uint32_t comp_len,
                          uint8_t *out, size_t out_cap);

/**
 * @brief Lê um DOWNLOAD_REP. @p comp aponta para dentro de @p in (não aloca).
 * @param object_id Recebe o ObjectID.
 * @param chunk_index Recebe o índice.
 * @param original_len Recebe o tamanho original.
 * @param comp Recebe o ponteiro para os bytes LZ4 dentro de @p in.
 * @param comp_len Recebe o tamanho do bloco comprimido.
 * @param in Payload recebido.
 * @param in_len Tamanho de @p in. Tem de bater com @c comp_len.
 * @return 1 em sucesso, 0 se o buffer, o ObjectID ou os tamanhos forem inválidos.
 */
int download_rep_unpack(uint8_t object_id[METADATA_OBJECT_ID_SIZE], uint32_t *chunk_index,
                        uint32_t *original_len, const uint8_t **comp, uint32_t *comp_len,
                        const uint8_t *in, size_t in_len);

#endif
