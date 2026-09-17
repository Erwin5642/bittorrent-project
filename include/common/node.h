#ifndef NODE_H
#define NODE_H

#include <stddef.h>
#include <stdint.h>

/**
 * @file node.h
 * @brief Identidade do nó, UUID e configuração local.
 */

/** Tamanho do NodeID / digest SHA-256, em bytes. */
#define NODE_ID_SIZE 32

/** Tamanho do UUID aleatório usado na geração do NodeID, em bytes. */
#define NODE_UUID_SIZE 16

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
  peer,      /**< Cliente: upload/download e armazenamento local. */
  superpeer  /**< Nó da overlay: membership, metadata, Chord, etc. */
} node_type_t;

/**
 * @brief Configuração local lida do arquivo .conf.
 */
typedef struct {
  uint32_t ipv4; /**< IPv4 anunciado, network byte order. */
  uint16_t port; /**< Porta TCP, host byte order. */
  node_type_t node_type; /**< peer ou superpeer. */
} node_config_t;

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

#endif
