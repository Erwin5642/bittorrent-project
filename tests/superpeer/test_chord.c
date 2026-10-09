#include "superpeer/chord.h"
#include "utils/test_utils.h"

#include <string.h>

/**
 * @file test_chord.c
 * @brief Aritmética do anel e o anel de um nó, sem socket.
 */

static node_id_t id_byte(unsigned index, uint8_t value) {
  node_id_t id;

  memset(&id, 0, sizeof id);
  id.bytes[index] = value;
  return id;
}

static void test_add_pow2(void) {
  node_id_t zero;
  node_id_t out;
  node_id_t full;

  memset(&zero, 0, sizeof zero);
  memset(&full, 0xff, sizeof full);

  expect(chord_id_add_pow2(NULL, 0, &out) == 0, "add sem origem falha");
  expect(chord_id_add_pow2(&zero, CHORD_M, &out) == 0, "expoente fora da faixa falha");

  expect(chord_id_add_pow2(&zero, 0, &out) == 1, "2^0");
  expect(out.bytes[NODE_ID_SIZE - 1] == 1, "2^0 cai no ultimo byte");

  expect(chord_id_add_pow2(&zero, 255, &out) == 1, "2^255");
  expect(out.bytes[0] == 0x80, "2^255 cai no bit alto");

  out = id_byte(NODE_ID_SIZE - 1, 0xff);
  expect(chord_id_add_pow2(&out, 0, &out) == 1, "carry entre bytes");
  expect(out.bytes[NODE_ID_SIZE - 1] == 0, "byte baixo zerou");
  expect(out.bytes[NODE_ID_SIZE - 2] == 1, "carry foi para o byte anterior");

  expect(chord_id_add_pow2(&full, 0, &out) == 1, "volta pelo zero");
  expect(node_id_cmp(&out, &zero) == 0, "0xff..ff + 1 == 0");
}

static void test_intervals(void) {
  node_id_t a = id_byte(NODE_ID_SIZE - 1, 1);
  node_id_t b = id_byte(NODE_ID_SIZE - 1, 5);
  node_id_t mid = id_byte(NODE_ID_SIZE - 1, 3);
  node_id_t high = id_byte(0, 1);
  node_id_t low = id_byte(NODE_ID_SIZE - 1, 1);

  expect(chord_in_open(NULL, &b, &mid) == 0, "intervalo sem extremo falha");
  expect(chord_in_open(&a, &a, &b) == 1, "extremos iguais cobrem o anel");
  expect(chord_in_half_open(&a, &a, &a) == 1, "meio-aberto igual cobre o proprio id");

  expect(chord_in_open(&a, &b, &mid) == 1, "(1, 5) contem 3");
  expect(chord_in_open(&a, &b, &a) == 0, "(1, 5) exclui 1");
  expect(chord_in_open(&a, &b, &b) == 0, "(1, 5) exclui 5");
  expect(chord_in_half_open(&a, &b, &b) == 1, "(1, 5] contem 5");
  expect(chord_in_half_open(&a, &b, &a) == 0, "(1, 5] exclui 1");

  expect(chord_in_open(&high, &low, &a) == 0, "volta pelo zero nao contem o meio baixo de (alto, baixo)");
  expect(chord_in_half_open(&b, &a, &high) == 1, "(5, 1] contem quem passou do zero");
}

static chord_node_t sample(uint8_t mark, uint16_t port) {
  chord_node_t node;

  memset(&node, 0, sizeof node);
  node.id = id_byte(NODE_ID_SIZE - 1, mark);
  node.ipv4 = 0x0100007f;
  node.port = port;
  node.valid = 1;
  return node;
}

static void test_solo_ring(void) {
  chord_t chord;
  chord_node_t self;
  chord_node_t nearer;
  chord_node_t farther;
  chord_node_t found;
  node_id_t key;

  self = sample(10, 5101);
  expect(chord_init(NULL) == 0, "init nulo falha");
  expect(chord_init(&chord) == 1, "init");
  expect(chord_create(&chord, &self.id, 0x0100007f, 5101) == 1, "cria anel de um no");
  expect(chord.successors[0].valid == 1, "sucessor preenchido");
  expect(node_id_cmp(&chord.successors[0].id, &chord.self.id) == 0, "sucessor e o proprio no");
  expect(chord.predecessor.valid == 0, "sem predecessor");
  expect(chord.fingers[0].port == 5101, "finger 0 aponta para a propria porta");
  expect(chord.fingers[CHORD_M - 1].valid == 1, "ultimo finger preenchido");

  nearer = sample(4, 5102);
  farther = sample(1, 5103);
  expect(chord_apply_notify(&chord, &nearer) == 1, "adota o primeiro predecessor");
  expect(chord.predecessor.port == 5102, "predecessor e 5102");
  expect(chord_apply_notify(&chord, &farther) == 1, "aviso mais distante");
  expect(chord.predecessor.port == 5102, "predecessor continua 5102");
  expect(chord_apply_notify(&chord, &chord.self) == 1, "aviso de si mesmo");
  expect(chord.predecessor.port == 5102, "si mesmo nao vira predecessor");

  key = id_byte(NODE_ID_SIZE - 1, 9);
  chord.fingers[3] = nearer;
  expect(chord_closest_preceding(&chord, &key, &found) == 1, "proximo passo");
  expect(found.port == 5102, "escolhe o finger antes da chave");

  chord.successors[0] = nearer;
  chord.successors[1] = farther;
  expect(chord_drop_node(&chord, &nearer.id) == 1, "remove o sucessor");
  expect(chord.successors[0].port == 5103, "o proximo da lista assume");
  expect(chord.predecessor.valid == 0, "predecessor removido junto");
  expect(chord.fingers[3].port == 5103, "finger do no removido aponta para o novo sucessor");

  chord_shutdown(&chord);
}

int main(void) {
  test_add_pow2();
  test_intervals();
  test_solo_ring();
  return test_report();
}
