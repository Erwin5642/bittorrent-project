#ifndef SUPERPEER_H
#define SUPERPEER_H

#include "common/config.h"
#include "common/node.h"
#include "common/protocol.h"
#include "superpeer/chord.h"
#include "superpeer/metadata.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/**
 * @file superpeer.h
 * @brief Processo Super Peer: membership local, índice de metadata e ciclo de JOIN.
 */

/** Capacidade da tabela de membros em memória (sem persistência). */
#define MEMBER_TABLE_MAX_SIZE 256

/** Tamanho do nome lógico do nó (CLI `--name`), incluindo o NUL. */
#define SUPERPEER_NAME_MAX 64

/** Intervalo entre batimentos enviados pela thread de heartbeat (CP3), em segundos. */
#define HEARTBEAT_SEC 5

/** Silêncio a partir do qual um membro ALIVE é rebaixado para SUSPECT, em segundos. */
#define HEARTBEAT_TIMEOUT_SEC 15

/**
 * @brief Estado de um membro na tabela local.
 */
typedef enum {
  MEMBER_ALIVE,   /**< Nó ativo; recebeu heartbeat há menos de @c HEARTBEAT_TIMEOUT_SEC. */
  MEMBER_SUSPECT, /**< Silencioso por mais de @c HEARTBEAT_TIMEOUT_SEC (CP3); confirmação via Gossip é CP4. */
  MEMBER_FAILED,  /**< Reservado (Gossip/Election, CP4). */
  MEMBER_REMOVED  /**< Saiu via LEAVE; não é mais alvo de heartbeat. */
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
  time_t last_heartbeat;     /**< Instante (`time(NULL)`) do último heartbeat. Stampado no self-insert, no JOIN, na inserção pelo anel e a cada RX de HEARTBEAT. */
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
  metadata_table_t metadata;         /**< Índice local por ObjectID. */
  chord_t chord;                     /**< Anel local. Começa com o sucessor igual a este nó. */
  int listend_fd;                    /**< fd de `net_listen`, ou -1. */
  char name[SUPERPEER_NAME_MAX];     /**< Nome lógico (`--name`), para logs do harness. */
  pthread_mutex_t members_lock;      /**< Serializa JOIN/LEAVE na tabela. */
  pthread_mutex_t metadata_lock;     /**< Serializa o acesso à tabela de metadata. */
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
 * @brief Insere um membro se o IP+porta ainda não existe.
 * @param table Tabela de destino.
 * @param member Entrada a copiar numa inserção nova.
 * @return 1 se inseriu ou se o endereço já existia, 0 se argumento nulo ou tabela cheia.
 * @note Entrada existente não é reescrita. Um `FAILED` permanece `FAILED`.
 */
int member_table_insert_new(member_table_t *table, const member_t *member);

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
 * @return 1 em sucesso, 0 se config, identidade, tabela, lock ou bind falhar.
 * @note Inclui o próprio nó como @c MEMBER_ALIVE. O bind usa @p port (ou a do
 *       .conf) em todas as interfaces; o `ip` da config só entra no NodeID.
 *       A tabela de metadata começa vazia. @c metadata_lock é independente de
 *       @c members_lock. O anel começa com um nó só: o sucessor é este processo.
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
 * @brief Valida um STORE e grava o registro na tabela de metadata.
 *
 * O payload é o layout de @c metadata_pack, ainda cru. @c owner tem de ser o
 * @c src_node do header. Tabela cheia numa inserção nova responde código 6.
 * @param sp Estado do Super Peer.
 * @param hdr Header da mensagem recebida.
 * @param payload Bytes do registro, sem o header de mensagem.
 * @param payload_len Tamanho de @p payload, em geral @c hdr->pl_size.
 * @param ack Preenchido com o NodeID de quem publicou, se o STORE for aceito.
 * @param err Preenchido se o STORE for rejeitado.
 * @return 1 para responder ACK, 0 para responder ERROR.
 * @note Quem chama segura @c metadata_lock. Sucesso imprime o registro em stdout
 *       (`File`, `Size`, `ObjectID`, `Chunks`, `Chunk i`).
 */
int superpeer_handle_store(superpeer_t *sp, const pl_header *hdr, const uint8_t *payload,
                           size_t payload_len, ack_t *ack, error_t *err);

/**
 * @brief Loop de `net_accept`: cada conexão vai para uma thread destacada (sem pool).
 * @param sp Super Peer já inicializado (`listend_fd` válido).
 * @return 0 se @p sp ou o listen fd for inválido. Não retorna no caminho feliz.
 * @note PING → PONG (log `RX PING`); JOIN → ACK/ERROR; LEAVE → ACK;
 *       STORE → ACK/ERROR; LOOKUP → STORE/ERROR; DOWNLOAD_REP → ACK/ERROR;
 *       DOWNLOAD_REQ → DOWNLOAD_REP/ERROR; CLOSEST_PRECEDING, GET_PREDECESSOR
 *       e GET_SUCCESSORS → o mesmo tipo, lido do anel; NOTIFY → ACK.
 *       FIND_SUCCESSOR → o caminho até o dono do ObjectID, no mesmo tipo.
 *       Chunks ficam em `data/storage`.
 *       Versão inválida ou @c NET_CLOSED: fecha o fd sem responder.
 *       Outros tipos: ERROR 3.
 *       Sem limite de conexões simultâneas nem timeout de I/O (MVP). Se
 *       `pthread_create` falhar, a conexão é tratada na thread de accept.
 *       Antes do laço, uma thread de manutenção faz `join`, `stabilize`,
 *       `notify`, `fix_fingers` e `check_predecessor` sem atrasar o `accept`.
 *       Sem bootstrap ela não abre socket. `fix_fingers` avança um índice por
 *       segundo. RPC do anel que falha tira o nó com `chord_drop_node`.
 *       Outra thread envia HEARTBEAT e marca SUSPECT quem ficou em silêncio.
 */
int superpeer_run(superpeer_t *sp);

#endif
