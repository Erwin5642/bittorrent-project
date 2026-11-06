#include "superpeer/lfu.h"

#include <stdlib.h>
#include <string.h>

static void entry_clear(lfu_entry_t *e) {
  free(e->value);
  memset(e, 0, sizeof *e);
}

int lfu_init(lfu_cache_t *cache, size_t capacity) {
  if (!cache || capacity == 0) {
    return 0;
  }
  memset(cache, 0, sizeof *cache);
  cache->entries = calloc(capacity, sizeof *cache->entries);
  if (!cache->entries) {
    return 0;
  }
  if (pthread_mutex_init(&cache->lock, NULL) != 0) {
    free(cache->entries);
    cache->entries = NULL;
    return 0;
  }
  cache->capacity = capacity;
  return 1;
}

void lfu_destroy(lfu_cache_t *cache) {
  size_t i;

  if (!cache || !cache->entries) {
    return;
  }
  for (i = 0; i < cache->capacity; i++) {
    entry_clear(&cache->entries[i]);
  }
  free(cache->entries);
  cache->entries = NULL;
  cache->capacity = 0;
  pthread_mutex_destroy(&cache->lock);
}

/* Slot da chave, ou -1. Quem chama segura o lock. */
static long find_key(const lfu_cache_t *cache, const char *key) {
  size_t i;

  for (i = 0; i < cache->capacity; i++) {
    if (cache->entries[i].used && strcmp(cache->entries[i].key, key) == 0) {
      return (long)i;
    }
  }
  return -1;
}

int lfu_get(lfu_cache_t *cache, const char *key, uint8_t *out, size_t out_cap,
            size_t *out_len, uint64_t *frequency) {
  lfu_entry_t *e;
  long slot;

  if (!cache || !cache->entries || !key || !out || !out_len) {
    return -1;
  }
  pthread_mutex_lock(&cache->lock);
  slot = find_key(cache, key);
  if (slot < 0) {
    cache->misses++;
    pthread_mutex_unlock(&cache->lock);
    return 0;
  }
  e = &cache->entries[slot];
  if (e->value_len > out_cap) {
    pthread_mutex_unlock(&cache->lock);
    return -1;
  }
  e->frequency++;
  e->last_access = ++cache->clock;
  memcpy(out, e->value, e->value_len);
  *out_len = e->value_len;
  if (frequency) {
    *frequency = e->frequency;
  }
  cache->hits++;
  pthread_mutex_unlock(&cache->lock);
  return 1;
}

/* Slot livre, ou o de menor frequência e acesso mais antigo. Quem chama segura o lock. */
static size_t pick_slot(const lfu_cache_t *cache, int *evicted) {
  size_t i;
  size_t victim = 0;

  for (i = 0; i < cache->capacity; i++) {
    if (!cache->entries[i].used) {
      *evicted = 0;
      return i;
    }
  }
  for (i = 1; i < cache->capacity; i++) {
    const lfu_entry_t *e = &cache->entries[i];
    const lfu_entry_t *v = &cache->entries[victim];

    if (e->frequency < v->frequency ||
        (e->frequency == v->frequency && e->last_access < v->last_access)) {
      victim = i;
    }
  }
  *evicted = 1;
  return victim;
}

int lfu_put(lfu_cache_t *cache, const char *key, const uint8_t object_id[METADATA_OBJECT_ID_SIZE],
            const uint8_t *value, size_t value_len, int *evicted) {
  uint8_t *copy;
  lfu_entry_t *e;
  long slot;
  int out_evicted = 0;

  if (evicted) {
    *evicted = 0;
  }
  if (!cache || !cache->entries || !key || !object_id || !value || value_len == 0 ||
      strlen(key) >= LFU_KEY_MAX) {
    return 0;
  }
  copy = malloc(value_len);
  if (!copy) {
    return 0;
  }
  memcpy(copy, value, value_len);

  pthread_mutex_lock(&cache->lock);
  slot = find_key(cache, key);
  if (slot >= 0) {
    /* Outro miss da mesma chave chegou antes: troca o valor, mantém a frequência. */
    e = &cache->entries[slot];
    free(e->value);
  } else {
    e = &cache->entries[pick_slot(cache, &out_evicted)];
    if (out_evicted) {
      cache->evictions++;
    }
    entry_clear(e);
    memcpy(e->key, key, strlen(key) + 1);
    e->frequency = 1;
    e->used = 1;
  }
  memcpy(e->object_id, object_id, METADATA_OBJECT_ID_SIZE);
  e->value = copy;
  e->value_len = value_len;
  e->last_access = ++cache->clock;
  pthread_mutex_unlock(&cache->lock);

  if (evicted) {
    *evicted = out_evicted;
  }
  return 1;
}

size_t lfu_invalidate_object(lfu_cache_t *cache, const uint8_t object_id[METADATA_OBJECT_ID_SIZE]) {
  size_t i;
  size_t removed = 0;

  if (!cache || !cache->entries || !object_id) {
    return 0;
  }
  pthread_mutex_lock(&cache->lock);
  for (i = 0; i < cache->capacity; i++) {
    lfu_entry_t *e = &cache->entries[i];

    if (e->used && memcmp(e->object_id, object_id, METADATA_OBJECT_ID_SIZE) == 0) {
      entry_clear(e);
      removed++;
    }
  }
  pthread_mutex_unlock(&cache->lock);
  return removed;
}

void lfu_stats(lfu_cache_t *cache, uint64_t *hits, uint64_t *misses, uint64_t *evictions) {
  if (!cache) {
    return;
  }
  pthread_mutex_lock(&cache->lock);
  if (hits) {
    *hits = cache->hits;
  }
  if (misses) {
    *misses = cache->misses;
  }
  if (evictions) {
    *evictions = cache->evictions;
  }
  pthread_mutex_unlock(&cache->lock);
}
