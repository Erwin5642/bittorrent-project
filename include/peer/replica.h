#ifndef REPLICA_H
#define REPLICA_H

#include "../common/node.h"
#include "../common/protocol.h"

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file replica.h
 * @brief Réplicas dos chunks de um arquivo (CP5).
 *
 * Os chunks de um ObjectID ficam no dono desse ObjectID no anel Chord e nos
 * sucessores dele, até @c REPLICATION_FACTOR Super Peers distintos. Qualquer
 * cliente acha esses nós a partir de um Super Peer de entrada, com
 * @c FIND_SUCCESSOR e @c GET_SUCCESSORS, sem saber quem recebeu o upload.
 */

/** Cópias de cada chunk em Super Peers distintos (`ReplicationFactor` da spec). */
#define REPLICATION_FACTOR 3

/** Espera máxima por uma resposta de Super Peer nas RPCs da réplica, em segundos. */
#define REPLICA_TIMEOUT_SEC 3

/**
 * @brief Endereço de um Super Peer que guarda (ou deve guardar) os chunks.
 */
typedef struct {
  char host[INET_ADDRSTRLEN]; /**< IPv4 em texto. */
  uint16_t port;              /**< Porta TCP, host byte order. */
} replica_endpoint_t;

/**
 * @brief Monta a lista de réplicas: o dono, depois os sucessores dele.
 *
 * Endereços repetidos entram uma vez só. Com @p entry_host, o Super Peer de
 * entrada vai no fim se ainda não estiver na lista: no download ele é a última
 * tentativa, para os chunks gravados antes da réplica existir.
 * @param owner Dono do ObjectID, ou NULL se a busca falhou.
 * @param succ Sucessores do dono, ou NULL.
 * @param succ_count Quantidade em @p succ.
 * @param factor Máximo de nós vindos do anel (dono + sucessores).
 * @param entry_host Super Peer de entrada, ou NULL para não incluir.
 * @param entry_port Porta do Super Peer de entrada.
 * @param out Destino; o caller aloca @p cap entradas.
 * @param cap Capacidade de @p out.
 * @return Quantidade escrita em @p out.
 */
size_t replica_merge(const chord_peer_t *owner, const chord_peer_t *succ, unsigned succ_count,
                     size_t factor, const char *entry_host, uint16_t entry_port,
                     replica_endpoint_t *out, size_t cap);

/**
 * @brief Pergunta ao anel quem guarda os chunks de um ObjectID.
 *
 * @c FIND_SUCCESSOR no Super Peer de entrada dá o dono; @c GET_SUCCESSORS no
 * dono dá os sucessores. Se a busca falhar, a lista é só o Super Peer de
 * entrada, que é o comportamento de antes da réplica.
 * @param host Super Peer de entrada.
 * @param port Porta do Super Peer de entrada.
 * @param self NodeID de quem pergunta (vira @c src_node).
 * @param object_id ObjectID do arquivo.
 * @param include_entry 1 para pôr o Super Peer de entrada no fim (download).
 * @param out Destino; o caller aloca @p cap entradas.
 * @param cap Capacidade de @p out.
 * @return Quantidade escrita em @p out; 0 só com argumento inválido.
 */
size_t replica_holders(const char *host, uint16_t port, const node_id_t *self,
                       const uint8_t object_id[METADATA_OBJECT_ID_SIZE], int include_entry,
                       replica_endpoint_t *out, size_t cap);

/**
 * @brief Arma timeout de envio e de recepção num socket já conectado.
 * @param fd Socket.
 * @param seconds Tempo máximo de cada @c send / @c recv.
 */
void replica_set_timeout(int fd, int seconds);

#endif
