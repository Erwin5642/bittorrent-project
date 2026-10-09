#include <arpa/inet.h>
#include <netinet/in.h>

#include "common/network.h"
#include "common/protocol.h"
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

  expect(chord_apply_notify(&chord, &nearer) == 1, "repoe um predecessor");
  expect(chord_clear_predecessor(NULL) == 0, "limpar predecessor nulo falha");
  expect(chord_clear_predecessor(&chord) == 1, "esvazia o predecessor");
  expect(chord.predecessor.valid == 0, "predecessor ficou vazio");
  expect(chord.successors[0].port == 5103, "limpar o predecessor nao mexe no sucessor");

  chord_shutdown(&chord);
}

static void test_install_successors(void) {
  chord_t chord;
  chord_node_t self;
  chord_node_t next;
  chord_node_t third;
  chord_node_t list[2];
  int changed = 0;

  self = sample(10, 5101);
  next = sample(20, 5102);
  third = sample(30, 5103);
  expect(chord_init(&chord) == 1, "init da lista");
  expect(chord_create(&chord, &self.id, 0x0100007f, 5101) == 1, "anel da lista");
  expect(chord_install_successors(NULL, &next, 1, &changed) == 0, "lista sem anel falha");
  expect(chord_install_successors(&chord, &next, 0, &changed) == 0, "lista vazia falha");
  list[0] = next;
  list[1] = third;
  expect(chord_install_successors(&chord, list, 2, &changed) == 1, "instala dois sucessores");
  expect(changed == 1, "sucessor mudou");
  expect(chord.successors[0].port == 5102, "sucessor imediato");
  expect(chord.successors[1].port == 5103, "segundo sucessor");
  expect(chord.successors[2].valid == 0, "o terceiro slot foi limpo");
  expect(chord.fingers[0].port == 5102, "finger 0 acompanha o sucessor");
  expect(chord.fingers[1].port == 5101, "os outros fingers ficam");
  changed = 1;
  expect(chord_install_successors(&chord, list, 2, &changed) == 1, "reinstala a mesma lista");
  expect(changed == 0, "o mesmo sucessor nao e uma mudanca");
  chord_shutdown(&chord);
}

static void test_set_finger(void) {
  chord_t chord;
  chord_node_t self;
  chord_node_t hop;
  node_id_t owner;
  node_id_t succ;
  node_id_t start;
  int changed = 0;

  self = sample(10, 5101);
  hop = sample(40, 5104);
  expect(chord_init(&chord) == 1, "init do finger");
  expect(chord_create(&chord, &self.id, 0x0100007f, 5101) == 1, "anel do finger");
  expect(chord_set_finger(NULL, 1, &hop, &changed) == 0, "finger sem anel falha");
  expect(chord_set_finger(&chord, CHORD_M, &hop, &changed) == 0, "indice fora da faixa falha");
  expect(chord_set_finger(&chord, 2, &hop, &changed) == 1, "grava o finger 2");
  expect(changed == 1, "finger 2 mudou");
  expect(chord.fingers[2].port == 5104, "finger 2 aponta para 5104");
  expect(chord.fingers[0].port == 5101, "finger 0 nao acompanha o 2");
  expect(chord.successors[0].port == 5101, "sucessor nao muda com o finger");
  changed = 1;
  expect(chord_set_finger(&chord, 2, &hop, &changed) == 1, "regrava o mesmo finger");
  expect(changed == 0, "o mesmo finger nao e uma mudanca");

  owner = id_byte(NODE_ID_SIZE - 1, 10);
  succ = id_byte(NODE_ID_SIZE - 1, 20);
  expect(chord_id_add_pow2(&owner, 0, &start) == 1, "inicio do finger 0");
  expect(chord_in_half_open(&owner, &succ, &start) == 1, "self+1 ainda cabe no sucessor");
  expect(chord_id_add_pow2(&owner, 4, &start) == 1, "inicio do finger 4");
  expect(chord_in_half_open(&owner, &succ, &start) == 0, "self+16 ja passou do sucessor");
  expect(chord_in_half_open(&owner, &owner, &start) == 1, "no sozinho cobre qualquer inicio");
  chord_shutdown(&chord);
}

static void test_lookup_step(void) {
  chord_t chord;
  chord_node_t self;
  chord_node_t pred;
  chord_node_t copied[CHORD_SUCCESSORS_MAX];
  chord_node_t step;
  node_id_t key;
  unsigned count = 99;
  int done = 0;

  self = sample(10, 5101);
  expect(chord_init(&chord) == 1, "init do passo");
  expect(chord_create(&chord, &self.id, 0x0100007f, 5101) == 1, "anel do passo");

  key = id_byte(NODE_ID_SIZE - 1, 3);
  expect(chord_lookup_step(&chord, &key, &done, &step) == 1, "passo no anel de um no");
  expect(done == 1, "a chave cabe em (self, self]");
  expect(step.port == 5101, "o sucessor devolvido e o proprio no");

  expect(chord_copy_predecessor(&chord, &pred) == 1, "copia predecessor vazio");
  expect(pred.valid == 0, "ainda nao ha predecessor");
  expect(chord_copy_successors(&chord, copied, CHORD_SUCCESSORS_MAX, &count) == 1,
         "copia sucessores");
  expect(count == 1, "so o proprio no esta na lista");
  expect(copied[0].port == 5101, "sucessor copiado");

  expect(chord_lookup_step(NULL, &key, &done, &step) == 0, "passo sem anel falha");
  expect(chord_copy_successors(&chord, copied, 0, &count) == 0, "capacidade zero falha");

  chord_shutdown(&chord);
}

static void test_chord_wire(void) {
  chord_peer_t in;
  chord_peer_t out;
  chord_peer_t list[2];
  chord_peer_t got[CHORD_SUCCESSORS_MAX];
  uint8_t raw[CHORD_NODE_WIRE_SIZE];
  uint8_t reply[CHORD_CLOSEST_REP_SIZE];
  uint8_t succ[1u + 2u * CHORD_NODE_WIRE_SIZE];
  uint8_t frame[HEADER_SIZE + CHORD_NODE_WIRE_SIZE];
  uint8_t bad[4];
  pl_header hdr;
  pl_header out_hdr;
  uint8_t done = 9;
  unsigned count = 0;
  ssize_t n;
  uint16_t port_be;

  memset(&in, 0, sizeof in);
  in.id[0] = 0xab;
  in.id[NODE_ID_SIZE - 1] = 0xcd;
  in.ipv4 = 0x0100007f;
  in.port = 5103;

  expect(chord_peer_pack(NULL, raw) == -1, "pack sem origem falha");
  expect(chord_peer_pack(&in, raw) == 0, "pack do no");
  expect(raw[0] == 0xab, "NodeID no inicio");
  memcpy(&port_be, raw + NODE_ID_SIZE + 4, 2);
  expect(ntohs(port_be) == 5103, "porta em big-endian");
  expect(chord_peer_unpack(&out, raw) == 0, "unpack do no");
  expect(out.port == 5103, "porta volta para host order");
  expect(out.ipv4 == in.ipv4, "IPv4 permanece em network order");
  expect(memcmp(out.id, in.id, NODE_ID_SIZE) == 0, "NodeID intacto");

  expect(chord_closest_reply_pack(2, &in, reply, sizeof reply) == -1, "status fora da faixa falha");
  n = chord_closest_reply_pack(CHORD_STEP_DONE, &in, reply, sizeof reply);
  expect(n == (ssize_t)CHORD_CLOSEST_REP_SIZE, "resposta de closest tem 39 bytes");
  expect(chord_closest_reply_unpack(&done, &out, reply, (size_t)n) == 1, "unpack de closest");
  expect(done == CHORD_STEP_DONE, "status de busca concluida");
  expect(out.port == 5103, "no da resposta");
  expect(chord_closest_reply_unpack(&done, &out, reply, CHORD_CLOSEST_REP_SIZE - 1) == 0,
         "closest truncado falha");

  list[0] = in;
  list[1] = in;
  list[1].port = 5104;
  expect(chord_successors_pack(list, 4, succ, sizeof succ) == -1, "contagem acima de 3 falha");
  n = chord_successors_pack(list, 2, succ, sizeof succ);
  expect(n == (ssize_t)(1u + 2u * CHORD_NODE_WIRE_SIZE), "lista de dois sucessores");
  expect(chord_successors_unpack(got, CHORD_SUCCESSORS_MAX, &count, succ, (size_t)n) == 1,
         "unpack da lista");
  expect(count == 2 && got[1].port == 5104, "segundo sucessor");
  bad[0] = 2;
  expect(chord_successors_unpack(got, CHORD_SUCCESSORS_MAX, &count, bad, sizeof bad) == 0,
         "lista truncada falha");
  n = chord_successors_pack(NULL, 0, succ, sizeof succ);
  expect(n == 1 && succ[0] == 0, "lista vazia e so a contagem");

  memset(&hdr, 0, sizeof hdr);
  hdr.protocol_ver = PROTOCOL_VER;
  hdr.msg_type = NOTIFY;
  hdr.pl_size = CHORD_NODE_WIRE_SIZE;
  n = serialize_message(frame, sizeof frame, &in, &hdr);
  expect(n == (ssize_t)(HEADER_SIZE + CHORD_NODE_WIRE_SIZE), "NOTIFY serializado");
  memset(&out, 0, sizeof out);
  expect(deserialize_message(frame, (size_t)n, &out_hdr, &out) == NET_OK, "NOTIFY desserializado");
  expect(out_hdr.msg_type == NOTIFY, "tipo NOTIFY");
  expect(out.port == 5103, "porta do NOTIFY");
  expect(payload_size_for(NOTIFY) == (int32_t)CHORD_NODE_WIRE_SIZE, "NOTIFY tem tamanho fixo");
  expect(payload_size_for(CLOSEST_PRECEDING) < 0, "closest aceita pedido e resposta");
  expect(payload_size_for(GET_PREDECESSOR) < 0, "predecessor aceita corpo vazio");
  expect(message_type_name(GET_SUCCESSORS)[0] == 'G', "nome de GET_SUCCESSORS");
  expect(payload_size_for(FIND_SUCCESSOR) < 0, "lookup aceita caminho variavel");

  {
    chord_peer_t hops[2];
    chord_peer_t back[2];
    uint8_t raw[2u + 2u * CHORD_NODE_WIRE_SIZE];
    unsigned got = 0;
    ssize_t bytes;

    memset(hops, 0, sizeof hops);
    hops[0].port = 5101;
    hops[1].port = 5105;
    hops[1].id[0] = 0xab;
    bytes = chord_lookup_path_pack(hops, 2, raw, sizeof raw);
    expect(bytes == (ssize_t)(2u + 2u * CHORD_NODE_WIRE_SIZE), "caminho de dois saltos");
    expect(chord_lookup_path_unpack(back, 2, &got, raw, (size_t)bytes) == 1, "caminho lido");
    expect(got == 2 && back[0].port == 5101 && back[1].port == 5105, "portas do caminho");
    expect(back[1].id[0] == 0xab, "id do dono");
    expect(chord_lookup_path_pack(hops, CHORD_LOOKUP_PATH_MAX + 1, raw, sizeof raw) < 0,
           "caminho longo demais falha");
  }
}

int main(void) {
  test_add_pow2();
  test_intervals();
  test_solo_ring();
  test_install_successors();
  test_set_finger();
  test_lookup_step();
  test_chord_wire();
  return test_report();
}
