#ifndef CHORD_H
#define CHORD_H

#include "common/node.h"

#include <pthread.h>
#include <stdint.h>

/**
 * @file chord.h
 * @brief Anel Chord do Super Peer: sucessor, predecessor e finger table.
 *
 * Este módulo guarda o anel e a aritmética do identificador. A thread de
 * manutenção e as RPCs entre Super Peers entram em cima desta base.
 * O lock protege só a memória: quem for abrir socket copia o destino e
 * solta o lock antes.
 */

/** Bits do identificador. Uma entrada da finger table por bit. */
#define CHORD_M 256

/** Sucessores guardados além do imediato, para a queda de um nó. */
#define CHORD_R 3

/**
 * @brief Nó do anel, com o endereço usado para abrir a conexão.
 */
typedef struct {
  node_id_t id;   /**< Identificador no anel. */
  uint32_t ipv4;  /**< IPv4 anunciado, network byte order. */
  uint16_t port;  /**< Porta TCP, host byte order. */
  int valid;      /**< 1 se a entrada está preenchida. */
} chord_node_t;

/**
 * @brief Estado local do anel.
 *
 * No nó sozinho, @c successors[0] é o próprio nó e @c predecessor.valid é 0.
 * @c fingers[i] guarda @c successor(self + 2^i). @c fingers[0] é o sucessor
 * imediato.
 */
typedef struct {
  chord_node_t self;                     /**< Este nó. */
  chord_node_t predecessor;              /**< Nó anterior. @c valid 0 se vazio. */
  chord_node_t successors[CHORD_R];      /**< @c [0] é o sucessor imediato. */
  chord_node_t fingers[CHORD_M];         /**< Atalhos. Índice 0 repete o sucessor. */
  pthread_mutex_t lock;                  /**< Protege este estado. */
} chord_t;

/**
 * @brief Zera o anel e cria o lock.
 * @param chord Estado; o caller aloca.
 * @return 1 em sucesso, 0 se @p chord for nulo ou o lock falhar.
 */
int chord_init(chord_t *chord);

/**
 * @brief Destrói o lock criado por @c chord_init.
 * @param chord Estado já inicializado. Nulo é ignorado.
 */
void chord_shutdown(chord_t *chord);

/**
 * @brief Forma o anel de um nó: o sucessor é o próprio nó, sem predecessor.
 * @param chord Estado já passado por @c chord_init.
 * @param self Identificador deste Super Peer.
 * @param ipv4 IPv4 anunciado, network byte order.
 * @param port Porta TCP, host byte order.
 * @return 1 em sucesso, 0 se algum argumento obrigatório for nulo.
 * @note As 256 entradas da finger table apontam para o próprio nó.
 */
int chord_create(chord_t *chord, const node_id_t *self, uint32_t ipv4, uint16_t port);

/**
 * @brief Calcula @c id + 2^exp módulo 2^256.
 * @param id Identificador de origem.
 * @param exp Expoente de 0 a @c CHORD_M - 1. É o índice da finger table.
 * @param out Recebe o resultado; o caller aloca.
 * @return 1 em sucesso, 0 se algum ponteiro for nulo ou @p exp estiver fora da faixa.
 */
int chord_id_add_pow2(const node_id_t *id, unsigned exp, node_id_t *out);

/**
 * @brief Diz se @p id está no intervalo aberto @c (start, end) do anel.
 * @param start Extremo inicial, excluído.
 * @param end Extremo final, excluído.
 * @param id Identificador consultado.
 * @return 1 se estiver dentro, 0 se estiver fora ou se algum ponteiro for nulo.
 * @note Com @p start igual a @p end o intervalo é o anel inteiro.
 */
int chord_in_open(const node_id_t *start, const node_id_t *end, const node_id_t *id);

/**
 * @brief Diz se @p id está no intervalo @c (start, end] do anel.
 * @param start Extremo inicial, excluído.
 * @param end Extremo final, incluído.
 * @param id Identificador consultado.
 * @return 1 se estiver dentro, 0 se estiver fora ou se algum ponteiro for nulo.
 * @note Com @p start igual a @p end o intervalo é o anel inteiro. É o caso do
 *       nó sozinho, em que o sucessor é ele mesmo.
 */
int chord_in_half_open(const node_id_t *start, const node_id_t *end, const node_id_t *id);

/**
 * @brief Devolve o finger de maior índice que ainda está antes de @p key.
 * @param chord Anel já criado.
 * @param key Identificador procurado.
 * @param out Recebe o nó escolhido. Se nenhum finger servir, recebe @c self.
 * @return 1 em sucesso, 0 se algum ponteiro for nulo.
 */
int chord_closest_preceding(chord_t *chord, const node_id_t *key, chord_node_t *out);

/**
 * @brief Adota @p candidate como predecessor quando ele é o mais próximo.
 * @param chord Anel já criado.
 * @param candidate Nó que avisou. Ignorado se for o próprio nó.
 * @return 1 em sucesso, 0 se @p chord ou @p candidate for nulo ou inválido.
 * @note Adota quando não há predecessor, ou quando o candidato está em
 *       @c (predecessor, self).
 */
int chord_apply_notify(chord_t *chord, const chord_node_t *candidate);

/**
 * @brief Tira @p id do predecessor, da lista de sucessores e da finger table.
 * @param chord Anel já criado.
 * @param id Identificador que deixou de responder. O próprio nó é ignorado.
 * @return 1 em sucesso, 0 se algum ponteiro for nulo.
 * @note Se a lista de sucessores esvazia, o sucessor volta a ser o próprio nó.
 *       Fingers que apontavam para @p id passam a apontar para o novo sucessor.
 */
int chord_drop_node(chord_t *chord, const node_id_t *id);

#endif
