#include "superpeer/lfu.h"
#include "utils/test_utils.h"

#include <stdio.h>
#include <string.h>

/**
 * @file test_lfu.c
 * @brief Testes de uma região do cache LFU, sem socket.
 */

static void fill_id(uint8_t id[METADATA_OBJECT_ID_SIZE], uint8_t seed) {
  memset(id, seed, METADATA_OBJECT_ID_SIZE);
}

/* Insere `key` com o valor igual à própria chave. */
static int put_key(lfu_cache_t *c, const char *key, uint8_t seed, int *evicted) {
  uint8_t id[METADATA_OBJECT_ID_SIZE];

  fill_id(id, seed);
  return lfu_put(c, key, id, (const uint8_t *)key, strlen(key) + 1, evicted);
}

static int get_key(lfu_cache_t *c, const char *key, uint64_t *freq) {
  uint8_t out[LFU_KEY_MAX];
  size_t len = 0;

  return lfu_get(c, key, out, sizeof out, &len, freq);
}

/**
 * @brief Miss, inserção e hit da mesma chave; o hit devolve o valor inserido.
 */
static void test_miss_then_hit(void) {
  lfu_cache_t c;
  uint8_t out[LFU_KEY_MAX];
  size_t len = 0;
  uint64_t freq = 0;
  uint64_t hits = 0;
  uint64_t misses = 0;
  int evicted = 1;

  expect(lfu_init(&c, 4) == 1, "região de 4 sobe");
  expect(lfu_get(&c, "a.pdf", out, sizeof out, &len, &freq) == 0, "primeiro acesso é miss");
  expect(put_key(&c, "a.pdf", 1, &evicted) == 1 && evicted == 0, "insere sem despejar");
  expect(lfu_get(&c, "a.pdf", out, sizeof out, &len, &freq) == 1, "segundo acesso é hit");
  expect(freq == 2, "hit deixa a frequência em 2");
  expect(len == strlen("a.pdf") + 1 && strcmp((const char *)out, "a.pdf") == 0,
         "hit devolve o valor inserido");
  lfu_stats(&c, &hits, &misses, NULL);
  expect(hits == 1 && misses == 1, "contadores: 1 hit, 1 miss");
  lfu_destroy(&c);
}

/**
 * @brief Cheia, sai a entrada de menor frequência.
 */
static void test_evicts_lowest_frequency(void) {
  lfu_cache_t c;
  uint64_t freq = 0;
  uint64_t evictions = 0;
  int evicted = 0;

  expect(lfu_init(&c, 2) == 1, "região de 2 sobe");
  put_key(&c, "quente", 1, NULL);
  put_key(&c, "frio", 2, NULL);
  get_key(&c, "quente", &freq);
  get_key(&c, "quente", &freq);
  expect(put_key(&c, "novo", 3, &evicted) == 1 && evicted == 1, "terceira chave despeja");
  expect(get_key(&c, "frio", NULL) == 0, "sai a de menor frequência");
  expect(get_key(&c, "quente", NULL) == 1, "a mais acessada fica");
  expect(get_key(&c, "novo", NULL) == 1, "a nova entrou");
  lfu_stats(&c, NULL, NULL, &evictions);
  expect(evictions == 1, "um despejo contado");
  lfu_destroy(&c);
}

/**
 * @brief Empate de frequência: sai a de acesso mais antigo.
 */
static void test_tie_evicts_oldest(void) {
  lfu_cache_t c;
  int evicted = 0;

  expect(lfu_init(&c, 2) == 1, "região de 2 sobe");
  put_key(&c, "antiga", 1, NULL);
  put_key(&c, "recente", 2, NULL);
  expect(put_key(&c, "nova", 3, &evicted) == 1 && evicted == 1, "despeja no empate");
  expect(get_key(&c, "antiga", NULL) == 0, "no empate sai a de acesso mais antigo");
  expect(get_key(&c, "recente", NULL) == 1, "a mais recente fica");
  lfu_destroy(&c);
}

/**
 * @brief Invalidar um ObjectID remove a chave; o próximo acesso é miss.
 */
static void test_invalidate_object(void) {
  lfu_cache_t c;
  uint8_t id[METADATA_OBJECT_ID_SIZE];
  uint8_t other[METADATA_OBJECT_ID_SIZE];

  expect(lfu_init(&c, 4) == 1, "região de 4 sobe");
  put_key(&c, "a.pdf", 1, NULL);
  put_key(&c, "b.pdf", 2, NULL);
  fill_id(id, 1);
  fill_id(other, 9);
  expect(lfu_invalidate_object(&c, other) == 0, "ObjectID ausente não remove nada");
  expect(lfu_invalidate_object(&c, id) == 1, "remove a entrada do ObjectID");
  expect(get_key(&c, "a.pdf", NULL) == 0, "depois de invalidar é miss");
  expect(get_key(&c, "b.pdf", NULL) == 1, "outro ObjectID continua");
  lfu_destroy(&c);
}

/**
 * @brief Reinserir a mesma chave troca o valor e mantém a frequência.
 */
static void test_reput_keeps_frequency(void) {
  lfu_cache_t c;
  uint8_t id[METADATA_OBJECT_ID_SIZE];
  uint8_t out[16];
  size_t len = 0;
  uint64_t freq = 0;
  int evicted = 1;

  expect(lfu_init(&c, 1) == 1, "região de 1 sobe");
  fill_id(id, 1);
  lfu_put(&c, "k", id, (const uint8_t *)"v1", 3, NULL);
  lfu_get(&c, "k", out, sizeof out, &len, &freq);
  expect(lfu_put(&c, "k", id, (const uint8_t *)"v2", 3, &evicted) == 1 && evicted == 0,
         "mesma chave não despeja");
  expect(lfu_get(&c, "k", out, sizeof out, &len, &freq) == 1 && freq == 3,
         "a frequência continua contando");
  expect(strcmp((const char *)out, "v2") == 0, "o valor foi trocado");
  lfu_destroy(&c);
}

/**
 * @brief Argumentos inválidos e destino curto.
 */
static void test_rejects(void) {
  lfu_cache_t c;
  uint8_t id[METADATA_OBJECT_ID_SIZE];
  uint8_t tiny[2];
  size_t len = 0;
  uint64_t hits = 0;
  uint64_t misses = 0;

  expect(lfu_init(&c, 0) == 0, "capacidade 0 falha");
  expect(lfu_init(NULL, 4) == 0, "região nula falha");
  expect(lfu_init(&c, 2) == 1, "região de 2 sobe");
  fill_id(id, 1);
  expect(lfu_put(&c, "k", id, (const uint8_t *)"x", 0, NULL) == 0, "valor vazio falha");
  expect(lfu_put(&c, "k", id, (const uint8_t *)"valor", 6, NULL) == 1, "insere");
  expect(lfu_get(&c, "k", tiny, sizeof tiny, &len, NULL) == -1, "destino curto falha");
  lfu_stats(&c, &hits, &misses, NULL);
  expect(hits == 0 && misses == 0, "destino curto não entra nos contadores");
  lfu_destroy(&c);
  lfu_destroy(NULL);
}

int main(void) {
  test_miss_then_hit();
  test_evicts_lowest_frequency();
  test_tie_evicts_oldest();
  test_invalidate_object();
  test_reput_keeps_frequency();
  test_rejects();
  return test_report();
}
