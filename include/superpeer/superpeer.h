#ifndef SUPERPEER_H
#define SUPERPEER_H

#include "common/config.h"
#include "common/node.h"
#include "common/protocol.h"

#include <stddef.h>
#include <stdint.h>

/**
 * @file superpeer.h
 * @brief Processo Super Peer: membership local e ciclo de JOIN.
 */

/** Capacidade da tabela de membros em memória (sem persistência). */
#define MEMBER_TABLE_MAX_SIZE 256

/** Tamanho do nome lógico do nó (CLI `--name`), incluindo o NUL. */
#define SUPERPEER_NAME_MAX 64

/**
 * @brief Estado de um membro na tabela local.
 */
typedef enum {
  MEMBER_ALIVE,   /**< Nó ativo. Único estado usado no CP1. */
  MEMBER_SUSPECT, /**< Reservado (Gossip). */
  MEMBER_FAILED,  /**< Reservado (Gossip). */
  MEMBER_REMOVED  /**< Reservado (saída). */
} member_state_t;

/**
 * @brief Entrada da tabela de membros.
 */
typedef struct {
  node_id_t id;              /**< NodeID do membro. */
  uint32_t ipv4;             /**< IPv4 anunciado, network byte order. */
  uint16_t port;             /**< Porta TCP, host byte order. */
  node_type_t node_type;     /**< PEER ou SUPERPEER. */
  member_state_t state;      /**< Estado local. */
  uint16_t last_heartbeat;   /**< Último heartbeat (CP1: 0). */
  uint32_t version;          /**< Versão da entrada. */
} member_t;

/**
 * @brief Tabela fixa de membros do Super Peer.
 */
typedef struct {
  member_t entries[MEMBER_TABLE_MAX_SIZE]; /**< Slots ocupados em [0, count). */
  size_t count;                            /**< Quantidade de entradas válidas. */
} member_table_t;

/**
 * @brief Estado do processo Super Peer.
 */
typedef struct {
  node_config_t cfg;                 /**< Configuração lida do .conf. */
  node_id_t self_id;                 /**< NodeID deste Super Peer. */
  member_table_t members;            /**< Membership local. */
  int listend_fd;                    /**< fd de `net_listen`, ou -1. */
  char name[SUPERPEER_NAME_MAX];     /**< Nome lógico (`--name`), para logs do harness. */
} superpeer_t;

/**
 * @brief Zera a tabela.
 * @param table Destino; o caller aloca.
 */
void member_table_init(member_table_t *table);

/**
 * @brief Insere ou atualiza um membro pela chave IP+porta.
 * @param table Tabela de destino.
 * @param member Entrada a copiar.
 * @return 1 em sucesso, 0 se argumento nulo ou tabela cheia numa inserção nova.
 * @note JOIN repetido do mesmo IP+porta substitui a entrada, não duplica.
 */
int member_table_update(member_table_t *table, const member_t *member);

/**
 * @brief Busca um membro pelo NodeID.
 * @param table Tabela a consultar.
 * @param node_id Identificador de 32 bytes.
 * @return Ponteiro para a entrada, ou NULL se não houver.
 */
const member_t *member_table_find_id(const member_table_t *table, const node_id_t *node_id);

/**
 * @brief Busca um membro pelo endereço anunciado.
 * @param table Tabela a consultar.
 * @param ipv4 IPv4 em network byte order.
 * @param port Porta TCP em host byte order.
 * @return Ponteiro para a entrada, ou NULL se não houver.
 */
const member_t *member_table_find_addr(const member_table_t *table, uint32_t ipv4, uint16_t port);

/**
 * @brief Imprime a tabela em stdout (tipo, IP, porta, NodeID hex, estado).
 * @param table Tabela a imprimir.
 */
void member_table_print(const member_table_t *table);

/**
 * @brief Carrega o .conf, gera o NodeID, imprime a identidade e abre o listen.
 * @param sp Estado do Super Peer; o caller aloca.
 * @param conf_path Caminho do arquivo (ex.: `config/sp1.conf`).
 * @param port Porta de listen; `0` usa a porta do .conf. Também entra no NodeID.
 * @param name Nome lógico para logs (`Node <name> started`); NULL vira `"superpeer"`.
 * @return 1 em sucesso, 0 se config, identidade, tabela ou bind falhar.
 * @note Inclui o próprio nó como @c MEMBER_ALIVE. O bind usa @p port (ou a do
 *       .conf) em todas as interfaces; o `ip` da config só entra no NodeID.
 */
int superpeer_init(superpeer_t *sp, const char *conf_path, uint16_t port, const char *name);

/**
 * @brief Valida um JOIN e preenche ACK ou ERROR.
 * @param sp Estado do Super Peer.
 * @param hdr Header da mensagem recebida.
 * @param join Payload já unpackado.
 * @param ack Preenchido se o JOIN for aceito.
 * @param err Preenchido se o JOIN for rejeitado.
 * @return 1 para responder ACK, 0 para responder ERROR.
 */
int superpeer_handle_join(superpeer_t *sp, const pl_header *hdr, const join_t *join, ack_t *ack, error_t *err);

/**
 * @brief Processa LEAVE: marca o membro como @c MEMBER_REMOVED (se existir) e preenche ACK.
 * @param sp Estado do Super Peer.
 * @param hdr Header da mensagem recebida.
 * @param leave Payload já unpackado.
 * @param ack Preenchido com o NodeID de quem saiu.
 * @return 1 para responder ACK (sempre, se os ponteiros forem válidos).
 */
int superpeer_handle_leave(superpeer_t *sp, const pl_header *hdr, const leave_t *leave, ack_t *ack);

/**
 * @brief Loop de `net_accept` + `recv_message`.
 * @param sp Super Peer já inicializado (`listend_fd` válido).
 * @return 0 se @p sp ou o listen fd for inválido. Não retorna no caminho feliz.
 * @note PING → PONG (log `RX PING`); JOIN → ACK/ERROR; LEAVE → ACK.
 *       Versão inválida ou @c NET_CLOSED: fecha o fd sem responder. Outros tipos: ERROR 3.
 */
int superpeer_run(superpeer_t *sp);

#endif
