#ifndef METADATA_H
#define METADATA_H

#include "common/protocol.h"

#include <stddef.h>
#include <stdint.h>

/**
 * @file metadata.h
 * @brief Hash table de metadata do Super Peer, indexada por ObjectID.
 *
 * O registro e o layout no fio vivem em @c protocol.h. Este módulo só
 * guarda os registros.
 */

/** Número máximo de registros na tabela. */
#define METADATA_TABLE_MAX 256

/**
 * @brief Slot da hash table. A lista de cada balde é encadeada por @c next.
 */
typedef struct {
  file_metadata_t meta; /**< Cópia do registro. A tabela é dona de @c chunk_hashes. */
  int in_use;           /**< 1 se o slot guarda um registro. */
  int next;             /**< Próximo slot da lista, ou -1. */
} metadata_slot_t;

/**
 * @brief Hash table em memória, encadeada pela chave ObjectID.
 *
 * Capacidade fixa. A busca por nome percorre os slots em uso. Quem chama
 * serializa o acesso; a tabela não tem lock próprio.
 */
typedef struct {
  metadata_slot_t slots[METADATA_TABLE_MAX]; /**< Pool de registros. */
  int buckets[METADATA_TABLE_MAX];           /**< Cabeça de cada lista, ou -1. */
  size_t count;                              /**< Registros em uso. */
} metadata_table_t;

/**
 * @brief Zera a tabela. Não libera hashes; a tabela precisa estar vazia.
 * @param table Destino; o caller aloca.
 */
void metadata_table_init(metadata_table_t *table);

/**
 * @brief Libera os hashes de cada slot e deixa a tabela vazia.
 * @param table Tabela a esvaziar.
 */
void metadata_table_clear(metadata_table_t *table);

/**
 * @brief Insere ou atualiza um registro pela chave ObjectID.
 *
 * Copia o nome, os identificadores e o bloco de hashes. Na primeira
 * inserção, @c version permanece a do registro. Se o ObjectID já existe,
 * os campos são substituídos e @c version vira o valor armazenado mais um.
 * @param table Tabela de destino.
 * @param meta Registro a copiar. @c filename é string não vazia.
 *             @c chunk_hashes é obrigatório quando @c chunk_count é maior que zero.
 * @return 1 em sucesso, 0 se argumento inválido, ObjectID zerado, nome vazio
 *         ou tabela cheia numa inserção nova.
 * @note Tabela cheia não escreve fora do pool e não altera o registro existente.
 */
int metadata_table_put(metadata_table_t *table, const file_metadata_t *meta);

/**
 * @brief Busca um registro pelo ObjectID.
 * @param table Tabela a consultar.
 * @param object_id Chave de 32 bytes.
 * @return Ponteiro para o registro interno, ou NULL se não houver.
 * @note O ponteiro vale até o próximo @c metadata_table_put ou @c metadata_table_clear.
 */
const file_metadata_t *metadata_table_find_id(const metadata_table_t *table,
                                              const uint8_t object_id[METADATA_OBJECT_ID_SIZE]);

/**
 * @brief Busca um registro pelo nome lógico.
 * @param table Tabela a consultar.
 * @param filename Nome NUL-terminated. A comparação é exata.
 * @return Ponteiro para o primeiro registro com esse nome, ou NULL.
 * @note O ponteiro vale até o próximo @c metadata_table_put ou @c metadata_table_clear.
 */
const file_metadata_t *metadata_table_find_name(const metadata_table_t *table, const char *filename);

#endif
