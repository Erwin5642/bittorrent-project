#include "../../include/superpeer/chord.h"
#include "../../include/common/protocol.h"

#include <string.h>

_Static_assert(CHORD_R == CHORD_SUCCESSORS_MAX, "CHORD_R e o teto do fio divergem");

static void chord_node_clear(chord_node_t *node) {
  memset(node, 0, sizeof *node);
}

static void chord_node_set(chord_node_t *node, const node_id_t *id, uint32_t ipv4, uint16_t port) {
  memset(node, 0, sizeof *node);
  node->id = *id;
  node->ipv4 = ipv4;
  node->port = port;
  node->valid = 1;
}

static int chord_same_id(const chord_node_t *node, const node_id_t *id) {
  return node->valid && node_id_cmp(&node->id, id) == 0;
}

int chord_init(chord_t *chord) {
  if (!chord) {
    return 0;
  }
  memset(chord, 0, sizeof *chord);
  if (pthread_mutex_init(&chord->lock, NULL) != 0) {
    return 0;
  }
  return 1;
}

void chord_shutdown(chord_t *chord) {
  if (!chord) {
    return;
  }
  pthread_mutex_destroy(&chord->lock);
}

int chord_create(chord_t *chord, const node_id_t *self, uint32_t ipv4, uint16_t port) {
  unsigned i;

  if (!chord || !self) {
    return 0;
  }

  pthread_mutex_lock(&chord->lock);
  chord_node_set(&chord->self, self, ipv4, port);
  chord_node_clear(&chord->predecessor);
  chord_node_set(&chord->successors[0], self, ipv4, port);
  for (i = 1; i < CHORD_R; i++) {
    chord_node_clear(&chord->successors[i]);
  }
  for (i = 0; i < CHORD_M; i++) {
    chord_node_set(&chord->fingers[i], self, ipv4, port);
  }
  pthread_mutex_unlock(&chord->lock);
  return 1;
}

int chord_id_add_pow2(const node_id_t *id, const unsigned exp, node_id_t *out) {
  if (!id || !out || exp >= CHORD_M) {
    return 0;
  }

  *out = *id;
  unsigned carry = 1u << (exp % 8);
  int byte;
  for (byte = (int)(NODE_ID_SIZE - 1 - exp / 8); byte >= 0; byte--) {
    const unsigned sum = (unsigned)out->bytes[byte] + carry;

    out->bytes[byte] = (uint8_t)sum;
    carry = sum >> 8;
    if (carry == 0) {
      break;
    }
  }
  return 1;
}

int chord_in_open(const node_id_t *start, const node_id_t *end, const node_id_t *id) {
  if (!start || !end || !id) {
    return 0;
  }
  const int start_vs_end = node_id_cmp(start, end);
  if (start_vs_end == 0) {
    return 1;
  }
  const int start_vs_id = node_id_cmp(start, id);
  const int id_vs_end = node_id_cmp(id, end);
  if (start_vs_end < 0) {
    return start_vs_id < 0 && id_vs_end < 0;
  }
  return start_vs_id < 0 || id_vs_end < 0;
}

int chord_in_half_open(const node_id_t *start, const node_id_t *end, const node_id_t *id) {
  if (!start || !end || !id) {
    return 0;
  }
  const int start_vs_end = node_id_cmp(start, end);
  if (start_vs_end == 0) {
    return 1;
  }
  const int start_vs_id = node_id_cmp(start, id);
  const int id_vs_end = node_id_cmp(id, end);
  if (start_vs_end < 0) {
    return start_vs_id < 0 && id_vs_end <= 0;
  }
  return start_vs_id < 0 || id_vs_end <= 0;
}

int chord_closest_preceding(chord_t *chord, const node_id_t *key, chord_node_t *out) {
  int i;

  if (!chord || !key || !out) {
    return 0;
  }

  pthread_mutex_lock(&chord->lock);
  *out = chord->self;
  for (i = CHORD_M - 1; i >= 0; i--) {
    if (chord->fingers[i].valid &&
        chord_in_open(&chord->self.id, key, &chord->fingers[i].id)) {
      *out = chord->fingers[i];
      break;
    }
  }
  pthread_mutex_unlock(&chord->lock);
  return 1;
}

int chord_apply_notify(chord_t *chord, const chord_node_t *candidate) {
  if (!chord || !candidate || !candidate->valid) {
    return 0;
  }

  pthread_mutex_lock(&chord->lock);
  if (node_id_cmp(&candidate->id, &chord->self.id) != 0 &&
      (!chord->predecessor.valid ||
       chord_in_open(&chord->predecessor.id, &chord->self.id, &candidate->id))) {
    chord->predecessor = *candidate;
  }
  pthread_mutex_unlock(&chord->lock);
  return 1;
}

int chord_drop_node(chord_t *chord, const node_id_t *id) {
  chord_node_t kept[CHORD_R];
  unsigned i;
  unsigned count;

  if (!chord || !id) {
    return 0;
  }

  pthread_mutex_lock(&chord->lock);
  if (node_id_cmp(id, &chord->self.id) == 0) {
    pthread_mutex_unlock(&chord->lock);
    return 1;
  }

  if (chord_same_id(&chord->predecessor, id)) {
    chord_node_clear(&chord->predecessor);
  }

  count = 0;
  for (i = 0; i < CHORD_R; i++) {
    if (chord->successors[i].valid && !chord_same_id(&chord->successors[i], id)) {
      kept[count++] = chord->successors[i];
    }
  }
  for (i = 0; i < CHORD_R; i++) {
    chord_node_clear(&chord->successors[i]);
  }
  if (count == 0) {
    chord->successors[0] = chord->self;
  } else {
    for (i = 0; i < count; i++) {
      chord->successors[i] = kept[i];
    }
  }

  for (i = 0; i < CHORD_M; i++) {
    if (chord_same_id(&chord->fingers[i], id)) {
      chord->fingers[i] = chord->successors[0];
    }
  }
  pthread_mutex_unlock(&chord->lock);
  return 1;
}

int chord_lookup_step(chord_t *chord, const node_id_t *key, int *done, chord_node_t *out) {
  int i;

  if (!chord || !key || !done || !out) {
    return 0;
  }

  pthread_mutex_lock(&chord->lock);
  if (!chord->self.valid || !chord->successors[0].valid) {
    pthread_mutex_unlock(&chord->lock);
    return 0;
  }

  if (chord_in_half_open(&chord->self.id, &chord->successors[0].id, key)) {
    *done = 1;
    *out = chord->successors[0];
    pthread_mutex_unlock(&chord->lock);
    return 1;
  }

  *done = 0;
  *out = chord->self;
  for (i = CHORD_M - 1; i >= 0; i--) {
    if (chord->fingers[i].valid &&
        chord_in_open(&chord->self.id, key, &chord->fingers[i].id)) {
      *out = chord->fingers[i];
      break;
    }
  }
  pthread_mutex_unlock(&chord->lock);
  return 1;
}

int chord_copy_predecessor(chord_t *chord, chord_node_t *out) {
  if (!chord || !out) {
    return 0;
  }
  pthread_mutex_lock(&chord->lock);
  *out = chord->predecessor;
  pthread_mutex_unlock(&chord->lock);
  return 1;
}

int chord_copy_successors(chord_t *chord, chord_node_t *out, unsigned cap, unsigned *count) {
  unsigned i;
  unsigned n;

  if (!chord || !out || !count || cap == 0) {
    return 0;
  }

  pthread_mutex_lock(&chord->lock);
  n = 0;
  for (i = 0; i < CHORD_R && n < cap; i++) {
    if (chord->successors[i].valid) {
      out[n++] = chord->successors[i];
    }
  }
  *count = n;
  pthread_mutex_unlock(&chord->lock);
  return 1;
}

int chord_install_successors(chord_t *chord, const chord_node_t *list, unsigned count, int *changed) {
  unsigned i;

  if (!chord || !list || count == 0 || count > CHORD_R || !list[0].valid) {
    return 0;
  }
  for (i = 0; i < count; i++) {
    if (!list[i].valid) {
      return 0;
    }
  }

  pthread_mutex_lock(&chord->lock);
  if (changed) {
    *changed = !chord->successors[0].valid || node_id_cmp(&chord->successors[0].id, &list[0].id) != 0;
  }
  for (i = 0; i < CHORD_R; i++) {
    chord_node_clear(&chord->successors[i]);
  }
  for (i = 0; i < count; i++) {
    chord->successors[i] = list[i];
  }
  chord->fingers[0] = list[0];
  pthread_mutex_unlock(&chord->lock);
  return 1;
}
