#ifndef NODE_H
#define NODE_H

#include <stddef.h>
#include <stdint.h>

/**
 * @file node.h
 * @brief Identidade do nó e UUID.
 */

/** Tamanho do NodeID / digest SHA-256, em bytes. */
#define NODE_ID_SIZE 32

/** Tamanho do UUID aleatório usado na geração do NodeID, em bytes. */
#define NODE_UUID_SIZE 16

/** Tamanho do buffer de NodeID em hex, incluindo o NUL terminal. */
#define NODE_ID_HEX_SIZE ((NODE_ID_SIZE * 2) + 1)

/**
 * @brief Identificador SHA-256 do nó (32 bytes).
 *
 * Fórmula da spec: NodeID = SHA256(IP || Porta || UUID).
 */
typedef struct {
  uint8_t bytes[NODE_ID_SIZE]; /**< Digest em bruto, não string hex. */
} node_id_t;

/**
 * @brief UUID de 16 bytes concatenado na geração do NodeID.
 */
typedef struct {
  uint8_t bytes[NODE_UUID_SIZE]; /**< Bytes aleatórios (ex.: /dev/urandom). */
} node_uuid_t;

/**
 * @brief Papel do processo na rede híbrida.
 */
typedef enum {
  PEER,      /**< Cliente: upload/download e armazenamento local. */
  SUPERPEER  /**< Nó da overlay: membership, metadata, Chord, etc. */
} node_type_t;

/**
 * @brief Calcula SHA-256 de um buffer.
 * @param in Bytes de entrada.
 * @param len Tamanho de @p in em bytes.
 * @param out Digest de 32 bytes; o caller aloca.
 * @return 1 em sucesso, 0 se o OpenSSL falhar.
 */
int sha256(const uint8_t *in, const size_t len, uint8_t out[NODE_ID_SIZE]);

/**
 * @brief Preenche um UUID com bytes aleatórios.
 * @param uuid Destino; o caller aloca.
 * @return 1 em sucesso, 0 se a leitura aleatória falhar.
 */
int node_uuid_random(node_uuid_t *uuid);

/**
 * @brief Gera NodeID = SHA256(IP || Porta || UUID).
 * @param ipv4 IPv4 anunciado, network byte order.
 * @param port Porta TCP, host byte order (convertida no hash).
 * @param uuid UUID de 16 bytes já preenchido.
 * @param out NodeID resultante; o caller aloca.
 * @return 1 em sucesso, 0 se o hash falhar.
 */
int node_id_generate(const uint32_t ipv4, const uint16_t port, const node_uuid_t *uuid, node_id_t *out);

/**
 * @brief Compara dois NodeIDs em ordem lexicográfica byte a byte.
 * @param a Primeiro NodeID.
 * @param b Segundo NodeID.
 * @return Negativo se @p a for menor, 0 se iguais, positivo se @p a for maior.
 *         Ponteiro nulo conta como menor que um NodeID válido.
 * @note Base da ordem de Chord no CP3; no fio o ID continua sendo os 32 bytes crus.
 */
int node_id_cmp(const node_id_t *a, const node_id_t *b);

/**
 * @brief Escreve o NodeID como 64 caracteres hexadecimais minúsculos + NUL.
 * @param id NodeID de origem.
 * @param out Buffer de destino; o caller aloca.
 * @param out_len Tamanho de @p out em bytes (mínimo @c NODE_ID_HEX_SIZE).
 * @return 1 em sucesso, 0 se argumento nulo ou buffer curto.
 */
int node_id_to_hex(const node_id_t *id, char *out, size_t out_len);

#endif
