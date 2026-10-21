#ifndef SMR_H
#define SMR_H

#include "superpeer/metadata.h"

#include <stddef.h>
#include <stdint.h>

/**
 * @file smr.h
 * @brief Operation log do Super Peer e aplicação determinística do SMR.
 *
 * Uma entrada é um registro. O coordenador define @c log_index, @c version e
 * checksum, enquanto isso, os outros instalam a mesma entrada. O lock do log fica em
 * @c superpeer_t e não é segurado durante connect ou send.
 */

/** Teto de entradas do log em memória. Não é limite do fio. */
#define SMR_LOG_MAX 256

struct superpeer;

/**
 * @brief Log local, em ordem crescente de @c log_index.
 *
 * @c count é o número de entradas seladas. @c last_applied é 0 enquanto
 * nenhuma foi instalada na hash table; a próxima entrada aplicável é
 * @c last_applied + 1. Buraco não avança @c last_applied.
 */
typedef struct {
  smr_entry_t entries[SMR_LOG_MAX]; /**< Entradas em [0, count). O log é dono de @c stored.chunk_hashes. */
  size_t count;                     /**< Entradas seladas. */
  uint64_t last_applied;            /**< Maior índice já instalado, ou 0. */
  uint64_t checkpoint_index;        /**< Último índice marcado como checkpoint, ou 0. */
} smr_log_t;

/**
 * @brief Zera o log. Não libera hashes; o log precisa estar vazio.
 * @param log Destino; o caller aloca.
 */
void smr_log_init(smr_log_t *log);

/**
 * @brief Libera os hashes de cada entrada e deixa o log vazio.
 * @param log Log a esvaziar. Nulo é ignorado.
 */
void smr_log_shutdown(smr_log_t *log);

/**
 * @brief Grava o SHA-256 do comando em @c entry->checksum.
 *
 * O campo @c checksum entra zerado nessa conta. @c timestamp já é o
 * @c log_index. O relógio de parede não entra.
 * @param entry Entrada já preenchida, com @c log_index e @c version selados.
 * @return 1 em sucesso, 0 se @p entry for nulo ou o SHA-256 falhar.
 */
int smr_entry_seal_checksum(smr_entry_t *entry);

/**
 * @brief Confere @c entry->checksum contra o comando canônico.
 * @param entry Entrada recebida.
 * @return 1 se o checksum fecha, 0 se @p entry for nulo ou o digest divergir.
 */
int smr_entry_checksum_ok(const smr_entry_t *entry);

/**
 * @brief Instala uma entrada na hash table, em ordem de @c log_index.
 *
 * Índice já aplicado é no-op e devolve sucesso. Índice maior que
 * @c last_applied + 1 não escreve na tabela. Checksum divergente também não.
 * @c SMR_OP_DELETE passa por @c metadata_table_delete. @c SMR_OP_STORE e
 * @c SMR_OP_UPDATE passam por @c metadata_table_install.
 * @param log Log que registra @c last_applied.
 * @param table Hash table do mesmo Super Peer. Quem chama serializa o acesso.
 * @param entry Entrada a aplicar. O log copia o que precisar.
 * @return 1 se aplicou ou se o índice já estava aplicado, 0 se argumento
 *         nulo, checksum inválido, buraco ou tabela cheia.
 */
int smr_log_apply(smr_log_t *log, metadata_table_t *table, const smr_entry_t *entry);

/**
 * @brief Sela um @c STORE no log local ou encaminha ao coordenador.
 *
 * Sem coordenador conhecido, aplica na tabela local. Com coordenador remoto,
 * só aplica a entrada que voltar no @c LOG_APPEND. Sucessor local não abre
 * socket para si.
 * @param sp Super Peer que recebeu o @c STORE.
 * @param meta Registro publicado pelo peer.
 * @return 1 se a tabela local ficou com o registro, 0 se @p sp ou @p meta
 *         forem nulos ou a réplica falhar.
 */
int smr_propose_store(struct superpeer *sp, const file_metadata_t *meta);

/**
 * @brief Sela um @c PUT nome/valor no log local ou encaminha ao coordenador.
 *
 * O ObjectID é SHA-256(nome || valor). O valor não vira campo da entrada:
 * entra só na conta do ObjectID e do checksum.
 * @param sp Super Peer que recebeu o @c PUT. No harness, a porta 5101.
 * @param name Nome lógico, NUL-terminated.
 * @param value Bytes do valor. Pode ser vazio só se @p value_len for 0.
 * @param value_len Tamanho de @p value.
 * @return 1 se a tabela local ficou com o nome, 0 se argumento inválido ou
 *         a réplica falhar.
 */
int smr_propose_put(struct superpeer *sp, const char *name, const uint8_t *value, size_t value_len);

/**
 * @brief Avisa o log de quem passou a ser o coordenador.
 *
 * O novo líder marca checkpoint no último índice aplicado. O líder antigo
 * que volta como seguidor não reaplica o próprio log.
 * @param sp Super Peer que recebeu ou venceu o @c COORDINATOR.
 * @param coordinator_id NodeID anunciado. Nulo limpa o coordenador conhecido.
 * @param i_am_leader 1 se @p coordinator_id é este processo.
 */
void smr_on_coordinator(struct superpeer *sp, const node_id_t *coordinator_id, int i_am_leader);

/**
 * @brief Trata um @c LOG_APPEND já lido do socket.
 *
 * Índice 0 é proposta: o coordenador sela, replica e responde outro
 * @c LOG_APPEND no mesmo socket. Índice a partir de 1 é réplica: aplica e
 * responde @c ACK. Quem chama não segura o lock do log.
 * @param sp Super Peer que recebeu a mensagem.
 * @param conn Socket da conexão. A resposta sai por aqui.
 * @param hdr Header já validado.
 * @param payload Bytes do corpo, sem o header.
 * @param len Tamanho de @p payload. Tem de bater com @c hdr->pl_size.
 * @return 1 se a entrada foi aplicada ou a proposta foi selada, 0 se o
 *         payload foi recusado.
 */
int smr_ingest_append(struct superpeer *sp, int conn, const pl_header *hdr,
                      const uint8_t *payload, size_t len);

#endif
