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

/** Prazo do connect das mensagens do 2PC e do IST, em milissegundos. */
#define SMR_CONNECT_TIMEOUT_MS 2000
/** Prazo de cada send e recv do 2PC e do IST: voto, ACK da decisão, SNAPSHOT e STATE_TRANSFER. */
#define SMR_IO_TIMEOUT_MS 5000
/**
 * Prazo de quem encaminhou a proposta esperando o coordenador fechar a rodada.
 * Precisa ficar acima da soma dos prazos de uma rodada e da pausa
 * @c BT_PAUSE_AFTER_PREPARE usada na demonstração.
 */
#define SMR_FORWARD_TIMEOUT_MS 120000
/** Silêncio do coordenador, depois do YES, antes de o participante perguntar o desfecho. */
#define SMR_DECISION_POLL_MS 5000
/** Espera até o IST que corrige um buraco no log. Deixa um COMMIT em voo chegar antes. */
#define SMR_IST_RETRY_DELAY_MS 1000
/** Tentativas, entre as esperas, de fechar um slot PREPARED sem decisão. */
#define SMR_SETTLE_TRIES 12

struct superpeer;

/**
 * @brief Fase local de uma transação do 2PC.
 *
 * O slot ocupa @c SMR_TX_PREPARED. @c SMR_TX_COMMITTED e @c SMR_TX_ABORTED
 * são decisões: a hash table só muda na primeira.
 */
typedef enum {
  SMR_TX_NONE = 0,      /**< Sem transação no slot. */
  SMR_TX_PREPARED = 1,  /**< Entrada guardada, ainda fora da hash table. */
  SMR_TX_COMMITTED = 2, /**< Decisão de aplicar, ou registro de COMMIT. */
  SMR_TX_ABORTED = 3    /**< Decisão de descartar, ou registro de ABORT. */
} smr_tx_phase_t;

/**
 * @brief Oferta do IST: marco de checkpoint e quantas entradas o pedinte não tem.
 *
 * O corpo são as entradas com @c log_index maior que o @c last_applied do
 * pedinte. O formato no fio de @c SNAPSHOT e @c STATE_TRANSFER espera o
 * contrato com o Aluno 1.
 */
typedef struct {
  uint64_t checkpoint_index;    /**< Marco já gravado no log de origem. */
  uint64_t source_last_applied; /**< @c last_applied de quem oferece. */
  size_t missing_count;         /**< Entradas com índice acima do pedinte. */
} smr_ist_offer_t;

/**
 * @brief Log local, em ordem crescente de @c log_index.
 *
 * @c count é o número de entradas seladas. @c last_applied é 0 enquanto
 * nenhuma foi instalada na hash table; a próxima entrada aplicável é
 * @c last_applied + 1. Buraco não avança @c last_applied. O slot
 * @c prepared fica fora da tabela até @c smr_tx_finish em @c SMR_TX_COMMITTED.
 */
typedef struct {
  smr_entry_t entries[SMR_LOG_MAX]; /**< Entradas em [0, count). O log é dono de @c stored.chunk_hashes. */
  size_t count;                     /**< Entradas seladas. */
  uint64_t last_applied;            /**< Maior índice já instalado, ou 0. */
  uint64_t checkpoint_index;        /**< Último índice marcado como checkpoint, ou 0. */
  smr_tx_phase_t prepared_phase;    /**< @c SMR_TX_PREPARED enquanto o slot está ocupado. */
  smr_entry_t prepared;             /**< Entrada do slot. O log é dono de @c stored.chunk_hashes. */
} smr_log_t;

/**
 * @brief Zera o log. Não libera hashes; o log precisa estar vazio.
 * @param log Destino; o caller aloca.
 */
void smr_log_init(smr_log_t *log);

/**
 * @brief Libera os hashes de cada entrada e do slot PREPARED, e deixa o log vazio.
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
 * @brief Aceita ou recusa uma entrada no slot @c PREPARED, sem escrever na tabela.
 *
 * O voto é YES quando o checksum fecha, @c log_index é @c last_applied + 1
 * e o slot está livre. Qualquer outra situação é NO e o slot não muda.
 * @param log Log que guarda o slot.
 * @param entry Entrada já selada. O log copia o que precisar.
 * @return 1 se a entrada ficou no slot, 0 se o voto é NO ou o argumento é nulo.
 */
int smr_prepare_vote(smr_log_t *log, const smr_entry_t *entry);

/**
 * @brief Diz se um voto NO de @c PREPARE veio de um buraco no log.
 *
 * Há buraco quando o @c log_index da entrada passa de @c last_applied + 1: este
 * nó perdeu um ou mais COMMITs e só o IST o põe em dia. Índice já aplicado, slot
 * ocupado e checksum ruim não são buraco.
 * @param log Log local.
 * @param entry Entrada do @c PREPARE.
 * @return 1 se há buraco, 0 caso contrário ou se algum argumento for nulo.
 */
int smr_vote_hole(const smr_log_t *log, const smr_entry_t *entry);

/**
 * @brief Fecha a rodada a partir dos votos.
 *
 * Sem participante (@p n == 0) a decisão é @c SMR_TX_COMMITTED: o coordenador
 * sozinho comita. Um NO basta para @c SMR_TX_ABORTED.
 * @param yes 1 para YES e 0 para NO. Pode ser nulo se @p n for 0.
 * @param n Quantidade de votos remotos.
 * @return @c SMR_TX_COMMITTED ou @c SMR_TX_ABORTED.
 */
smr_tx_phase_t smr_tx_decide_votes(const int *yes, size_t n);

/**
 * @brief Decisão quando o coordenador some durante o @c PREPARE.
 *
 * Algum sobrevivente em @c SMR_TX_COMMITTED fecha em COMMIT. Sem nenhum
 * COMMIT, inclusive lista vazia, a decisão é @c SMR_TX_ABORTED.
 * @param phases Fase vista em cada sobrevivente. Nulo vale lista vazia.
 * @param n Quantidade de fases.
 * @return @c SMR_TX_COMMITTED ou @c SMR_TX_ABORTED.
 */
smr_tx_phase_t smr_tx_decide_recover(const smr_tx_phase_t *phases, size_t n);

/**
 * @brief Aplica o slot no COMMIT ou descarta no ABORT.
 *
 * @c SMR_TX_COMMITTED passa por @c smr_log_apply. Repetir o mesmo índice não
 * muda @c version. Slot vazio no COMMIT é sucesso: não havia o que aplicar.
 * @c SMR_TX_ABORTED libera o slot e não escreve na tabela.
 * @param log Log com o slot.
 * @param table Hash table do mesmo Super Peer.
 * @param decision @c SMR_TX_COMMITTED ou @c SMR_TX_ABORTED.
 * @return 1 em sucesso, 0 se argumento nulo, decisão desconhecida ou o apply falhar.
 */
int smr_tx_finish(smr_log_t *log, metadata_table_t *table, smr_tx_phase_t decision);

/**
 * @brief Lista as entradas do log estritamente posteriores a @p requester_last.
 *
 * Não inclui o slot @c PREPARED. @p missing recebe ponteiros para dentro de
 * @p log, em ordem de @c log_index, até @p missing_cap. @c missing_count conta
 * todas, mesmo quando o array não cabe.
 * @param log Log de quem oferece o estado.
 * @param requester_last Último índice que o pedinte já aplicou. 0 pede o log inteiro.
 * @param offer Destino do marco e da contagem. O caller aloca.
 * @param missing Destino dos ponteiros. Pode ser nulo se só a contagem importa.
 * @param missing_cap Capacidade de @p missing.
 * @return 1 em sucesso, 0 se @p log ou @p offer for nulo.
 */
int smr_ist_offer(const smr_log_t *log, uint64_t requester_last, smr_ist_offer_t *offer,
                  const smr_entry_t **missing, size_t missing_cap);

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
 * O novo líder puxa quem estiver à frente e lê a gaveta @c PREPARED dos vivos.
 * Se algum já aplicou o @c COMMIT, os demais também aplicam. Se ninguém
 * aplicou, a rodada aborta. Um seguidor pede @c SNAPSHOT ao coordenador e
 * aplica só o que vier em @c STATE_TRANSFER.
 * @param sp Super Peer que recebeu ou venceu o @c COORDINATOR.
 * @param coordinator_id NodeID anunciado. Nulo limpa o coordenador conhecido.
 * @param i_am_leader 1 se @p coordinator_id é este processo.
 */
void smr_on_coordinator(struct superpeer *sp, const node_id_t *coordinator_id, int i_am_leader);

/**
 * @brief Trata um @c LOG_APPEND já lido do socket.
 *
 * Índice 0 é proposta: o coordenador sela, roda 2PC e responde outro
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

/**
 * @brief Vota um @c PREPARE e, no mesmo socket, espera @c COMMIT ou @c ABORT.
 *
 * YES no fio é @c ACK. NO é @c ERROR. O corpo é o de @c log_append_pack.
 * A hash table só muda se o @c COMMIT seguinte fechar o slot.
 * @param sp Participante.
 * @param conn Socket que já entregou o @c PREPARE. A decisão é lida aqui.
 * @param hdr Header do @c PREPARE.
 * @param payload Corpo, sem o header.
 * @param len Tamanho de @p payload.
 * @return 1 se o voto foi YES e a decisão foi aplicada, 0 se o voto foi NO
 *         ou a decisão foi recusada.
 */
int smr_ingest_prepare(struct superpeer *sp, int conn, const pl_header *hdr,
                       const uint8_t *payload, size_t len);

/**
 * @brief Fecha o slot com um @c COMMIT ou @c ABORT vindo de outra conexão.
 *
 * Cobre a queda do coordenador na fase @c PREPARE: o líder novo manda a
 * decisão num socket novo. O corpo tem 24 bytes, @c transaction_id e
 * @c log_index. O slot só fecha se os dois baterem com o @c PREPARED local.
 * @param sp Participante.
 * @param conn Socket da decisão. A resposta sai por aqui.
 * @param hdr Header já validado. O tipo é @c COMMIT ou @c ABORT.
 * @param payload Corpo, sem o header.
 * @param len Tamanho de @p payload.
 * @return 1 se o slot fechou, 0 se a decisão não casa com o slot.
 */
int smr_ingest_decision(struct superpeer *sp, int conn, const pl_header *hdr,
                        const uint8_t *payload, size_t len);

/**
 * @brief Responde um pedido @c SNAPSHOT e em seguida manda o delta.
 *
 * O pedido tem 9 bytes: kind 1 e `last_applied` em big-endian. A resposta
 * tem 46 bytes: kind 2, `checkpoint_index`, `last_applied` da origem,
 * `missing_count`, a fase da gaveta (0 ou 1), o `log_index` dela e o
 * `transaction_id`. Cada entrada que falta sai como @c STATE_TRANSFER no
 * mesmo socket. `missing_count` 0 não manda @c STATE_TRANSFER.
 * @param sp Quem oferece o log.
 * @param conn Socket do pedido.
 * @param hdr Header do @c SNAPSHOT.
 * @param payload Corpo do pedido.
 * @param len Tamanho de @p payload.
 * @return 1 se a resposta saiu, 0 se o pedido foi recusado.
 */
int smr_ingest_snapshot(struct superpeer *sp, int conn, const pl_header *hdr,
                        const uint8_t *payload, size_t len);

#endif
