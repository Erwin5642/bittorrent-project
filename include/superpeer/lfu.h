#ifndef LFU_H
#define LFU_H

#include "common/protocol.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file lfu.h
 * @brief Uma região do cache LFU do Super Peer (CP5).
 *
 * Cada acesso incrementa a frequência da entrada. Cheia, a região despeja a
 * de menor frequência e, no empate, a de acesso mais antigo. O último acesso é
 * um relógio lógico da região, não o relógio de parede. Cada região tem o
 * próprio lock, que é folha: nenhuma função do módulo toma outro lock.
 */

/** Tamanho da chave, incluindo o NUL. A chave é o nome do registro. */
#define LFU_KEY_MAX METADATA_FILENAME_MAX

/**
 * @brief Entrada de uma região.
 */
typedef struct {
  char key[LFU_KEY_MAX];                       /**< Chave, NUL-terminated. */
  uint8_t object_id[METADATA_OBJECT_ID_SIZE];  /**< ObjectID do registro, para invalidar. */
  uint8_t *value;                              /**< Bytes guardados; a região é dona. */
  size_t value_len;                            /**< Tamanho de @c value. */
  uint64_t frequency;                          /**< Acessos desde a inserção. */
  uint64_t last_access;                        /**< Relógio lógico do último acesso. */
  int used;                                    /**< 1 se o slot está ocupado. */
} lfu_entry_t;

/**
 * @brief Região do cache, com capacidade e contadores próprios.
 */
typedef struct {
  lfu_entry_t *entries;  /**< @c capacity slots. */
  size_t capacity;       /**< Máximo de entradas. */
  uint64_t clock;        /**< Relógio lógico; cresce a cada acesso ou inserção. */
  uint64_t hits;         /**< @c lfu_get que acharam a chave. */
  uint64_t misses;       /**< @c lfu_get que não acharam. */
  uint64_t evictions;    /**< Entradas despejadas por falta de espaço. */
  pthread_mutex_t lock;  /**< Serializa a região. */
} lfu_cache_t;

/**
 * @brief Cria uma região vazia.
 * @param cache Destino; o caller aloca.
 * @param capacity Máximo de entradas; 0 falha.
 * @return 1 em sucesso, 0 se argumento inválido, memória ou mutex falharem.
 */
int lfu_init(lfu_cache_t *cache, size_t capacity);

/**
 * @brief Libera os valores, os slots e o lock.
 * @param cache Região criada por @c lfu_init. Nulo é ignorado.
 */
void lfu_destroy(lfu_cache_t *cache);

/**
 * @brief Procura a chave. No hit, conta o acesso e copia o valor.
 * @param cache Região.
 * @param key Chave, NUL-terminated.
 * @param out Destino do valor; o caller aloca.
 * @param out_cap Capacidade de @p out.
 * @param out_len Recebe o tamanho do valor no hit.
 * @param frequency Recebe a frequência depois deste acesso no hit. Pode ser nulo.
 * @return 1 no hit, 0 no miss, -1 se argumento inválido ou @p out_cap for
 *         menor que o valor. Só o hit e o miss entram nos contadores.
 */
int lfu_get(lfu_cache_t *cache, const char *key, uint8_t *out, size_t out_cap,
            size_t *out_len, uint64_t *frequency);

/**
 * @brief Insere a chave com frequência 1, despejando se a região estiver cheia.
 *
 * Se a chave já existe (dois misses concorrentes da mesma chave), troca o
 * valor e mantém a frequência.
 * @param cache Região.
 * @param key Chave, NUL-terminated, menor que @c LFU_KEY_MAX.
 * @param object_id ObjectID do registro.
 * @param value Bytes a guardar; a região copia.
 * @param value_len Tamanho de @p value; 0 falha.
 * @param evicted Recebe 1 se uma entrada foi despejada. Pode ser nulo.
 * @return 1 em sucesso, 0 se argumento inválido ou memória faltar.
 */
int lfu_put(lfu_cache_t *cache, const char *key, const uint8_t object_id[METADATA_OBJECT_ID_SIZE],
            const uint8_t *value, size_t value_len, int *evicted);

/**
 * @brief Remove as entradas de um ObjectID.
 * @param cache Região.
 * @param object_id ObjectID cujo registro mudou.
 * @return Quantidade de entradas removidas.
 */
size_t lfu_invalidate_object(lfu_cache_t *cache, const uint8_t object_id[METADATA_OBJECT_ID_SIZE]);

/**
 * @brief Lê os contadores da região.
 * @param cache Região.
 * @param hits Recebe os hits. Pode ser nulo.
 * @param misses Recebe os misses. Pode ser nulo.
 * @param evictions Recebe os despejos. Pode ser nulo.
 */
void lfu_stats(lfu_cache_t *cache, uint64_t *hits, uint64_t *misses, uint64_t *evictions);

#endif
