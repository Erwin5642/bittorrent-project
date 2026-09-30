#include "../../include/superpeer/metadata.h"

#include <stdlib.h>
#include <string.h>

/* FNV-1a sobre os 32 bytes. Os baldes sao so o resto da divisao. */
static unsigned metadata_bucket(const uint8_t id[METADATA_OBJECT_ID_SIZE]) {
  uint32_t hash = 2166136261u;
  const uint32_t fnv_prime = 16777619u;
  size_t i;

  for (i = 0; i < METADATA_OBJECT_ID_SIZE; i++) {
    hash ^= id[i];
    hash *= fnv_prime;
  }
  return hash % METADATA_TABLE_MAX;
}

static int metadata_same_id(const uint8_t *a, const uint8_t *b) {
  return memcmp(a, b, METADATA_OBJECT_ID_SIZE) == 0;
}

/* Copia o registro e o bloco de hashes. O destino fica dono do bloco novo. */
static int metadata_copy(file_metadata_t *dst, const file_metadata_t *src) {
  uint8_t *hashes = NULL;

  if (src->chunk_count > 0) {
    const size_t size = (size_t)src->chunk_count * METADATA_CHUNK_HASH_SIZE;
    hashes = malloc(size);
    if (!hashes) {
      return 0;
    }
    memcpy(hashes, src->chunk_hashes, size);
  }

  *dst = *src;
  dst->chunk_hashes = hashes;
  return 1;
}

static int metadata_find_slot(const metadata_table_t *table, const uint8_t *object_id) {
  unsigned bucket = metadata_bucket(object_id);
  int idx = table->buckets[bucket];

  while (idx != -1) {
    if (table->slots[idx].in_use && metadata_same_id(table->slots[idx].meta.object_id, object_id)) {
      return idx;
    }
    idx = table->slots[idx].next;
  }
  return -1;
}

static int metadata_free_slot(const metadata_table_t *table) {
  size_t i;

  for (i = 0; i < METADATA_TABLE_MAX; i++) {
    if (!table->slots[i].in_use) {
      return (int)i;
    }
  }
  return -1;
}

void metadata_table_init(metadata_table_t *table) {
  size_t i;

  if (!table) {
    return;
  }
  memset(table, 0, sizeof *table);
  for (i = 0; i < METADATA_TABLE_MAX; i++) {
    table->buckets[i] = -1;
    table->slots[i].next = -1;
  }
}

void metadata_table_clear(metadata_table_t *table) {
  size_t i;

  if (!table) {
    return;
  }
  for (i = 0; i < METADATA_TABLE_MAX; i++) {
    if (table->slots[i].in_use) {
      metadata_release(&table->slots[i].meta);
    }
  }
  metadata_table_init(table);
}

int metadata_table_put(metadata_table_t *table, const file_metadata_t *meta) {
  file_metadata_t copy;

  if (!table || !meta) {
    return 0;
  }
  if (metadata_id_is_zero(meta->object_id) || !metadata_name_ok(meta->filename)) {
    return 0;
  }
  if (meta->chunk_count > 0 && !meta->chunk_hashes) {
    return 0;
  }
  if (metadata_wire_size(meta->chunk_count) == 0) {
    return 0;
  }

  const int existing = metadata_find_slot(table, meta->object_id);
  if (existing < 0 && table->count >= METADATA_TABLE_MAX) {
    return 0;
  }
  if (!metadata_copy(&copy, meta)) {
    return 0;
  }

  if (existing >= 0) {
    copy.version = table->slots[existing].meta.version + 1;
    metadata_release(&table->slots[existing].meta);
    table->slots[existing].meta = copy;
    return 1;
  }

  const int slot = metadata_free_slot(table);
  if (slot < 0) {
    metadata_release(&copy);
    return 0;
  }

  const unsigned bucket = metadata_bucket(meta->object_id);
  table->slots[slot].meta = copy;
  table->slots[slot].in_use = 1;
  table->slots[slot].next = table->buckets[bucket];
  table->buckets[bucket] = slot;
  table->count++;
  return 1;
}

const file_metadata_t *metadata_table_find_id(const metadata_table_t *table,
                                              const uint8_t object_id[METADATA_OBJECT_ID_SIZE]) {
  int idx;

  if (!table || !object_id) {
    return NULL;
  }
  idx = metadata_find_slot(table, object_id);
  if (idx < 0) {
    return NULL;
  }
  return &table->slots[idx].meta;
}

const file_metadata_t *metadata_table_find_name(const metadata_table_t *table, const char *filename) {
  size_t i;

  if (!table || !metadata_name_ok(filename)) {
    return NULL;
  }
  for (i = 0; i < METADATA_TABLE_MAX; i++) {
    if (table->slots[i].in_use && strcmp(table->slots[i].meta.filename, filename) == 0) {
      return &table->slots[i].meta;
    }
  }
  return NULL;
}
