#include "superpeer/superpeer.h"
#include "utils/test_utils.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>

/**
 * @file test_superpeer.c
 * @brief Testes unitários da tabela de membros e do handler de JOIN (sem socket).
 */

/**
 * @brief Preenche um NodeID com bytes sequenciais a partir de @p seed.
 */
static void fill_id(node_id_t *id, uint8_t seed) {
  size_t i;

  for (i = 0; i < NODE_ID_SIZE; i++) {
    id->bytes[i] = (uint8_t)(seed + i);
  }
}

/**
 * @brief Converte dotted-quad para network byte order.
 */
static uint32_t ipv4_from_str(const char *s) {
  uint32_t addr;

  if (inet_pton(AF_INET, s, &addr) != 1) {
    return 0;
  }
  return addr;
}

/**
 * @brief Monta um JOIN válido: node_id igual a src_node.
 */
static void make_join(pl_header *hdr, join_t *join, uint8_t seed, uint32_t ipv4, uint16_t port, uint8_t type) {
  node_id_t id;

  memset(hdr, 0, sizeof *hdr);
  memset(join, 0, sizeof *join);
  fill_id(&id, seed);
  memcpy(hdr->src_node, id.bytes, NODE_ID_SIZE);
  memcpy(join->node_id, id.bytes, NODE_ID_SIZE);
  hdr->msg_type = JOIN;
  hdr->pl_size = 39;
  join->ipv4 = ipv4;
  join->port = port;
  join->node_type = type;
}

/**
 * @brief Super Peer zerado só com tabela vazia (sem listen).
 */
static void sp_reset(superpeer_t *sp) {
  memset(sp, 0, sizeof *sp);
  sp->listend_fd = -1;
  member_table_init(&sp->members);
}

/**
 * @brief Inserção, busca por id/endereço e JOIN repetido no mesmo IP+porta.
 */
static void test_member_table_upsert_and_find(void) {
  member_table_t table;
  member_t a;
  member_t b;
  const member_t *found;

  member_table_init(&table);
  memset(&a, 0, sizeof a);
  fill_id(&a.id, 1);
  a.ipv4 = ipv4_from_str("10.0.0.1");
  a.port = 9001;
  a.node_type = PEER;
  a.state = MEMBER_ALIVE;

  expect(member_table_update(&table, &a) == 1, "insert a");
  expect(table.count == 1, "count 1 após insert");
  found = member_table_find_id(&table, &a.id);
  expect(found != NULL && found->port == 9001, "find_id encontra a");
  found = member_table_find_addr(&table, a.ipv4, a.port);
  expect(found != NULL && node_id_cmp(&found->id, &a.id) == 0, "find_addr encontra a");

  memset(&b, 0, sizeof b);
  fill_id(&b.id, 9);
  b.ipv4 = a.ipv4;
  b.port = a.port;
  b.node_type = SUPERPEER;
  b.state = MEMBER_ALIVE;
  b.version = 3;
  expect(member_table_update(&table, &b) == 1, "upsert mesmo addr");
  expect(table.count == 1, "upsert não duplica");
  found = member_table_find_addr(&table, a.ipv4, a.port);
  expect(found != NULL && node_id_cmp(&found->id, &b.id) == 0, "upsert troca o NodeID");
  expect(found->version == 3, "upsert guarda version nova");
  expect(member_table_find_id(&table, &a.id) == NULL, "id antigo some no upsert");
}

/**
 * @brief Tabela cheia recusa inserção nova, mas ainda atualiza entrada existente.
 */
static void test_member_table_full(void) {
  member_table_t table;
  member_t m;
  size_t i;

  member_table_init(&table);
  memset(&m, 0, sizeof m);
  m.node_type = PEER;
  m.state = MEMBER_ALIVE;
  m.ipv4 = ipv4_from_str("10.0.0.2");
  for (i = 0; i < MEMBER_TABLE_MAX_SIZE; i++) {
    fill_id(&m.id, (uint8_t)i);
    m.port = (uint16_t)(1000 + i);
    if (!member_table_update(&table, &m)) {
      expect(0, "enche a tabela");
      return;
    }
  }
  expect(table.count == MEMBER_TABLE_MAX_SIZE, "tabela no limite");

  fill_id(&m.id, 1);
  m.port = 1000;
  m.version = 7;
  expect(member_table_update(&table, &m) == 1, "upsert em tabela cheia sucede");
  expect(table.count == MEMBER_TABLE_MAX_SIZE, "count não cresce no upsert");

  fill_id(&m.id, 99);
  m.port = 65000;
  expect(member_table_update(&table, &m) == 0, "insert novo em tabela cheia falha");
}

/**
 * @brief Ponteiros nulos nas operações da tabela.
 */
static void test_member_table_null(void) {
  member_table_t table;
  member_t m;
  node_id_t id;

  member_table_init(NULL);
  memset(&m, 0, sizeof m);
  fill_id(&id, 1);
  expect(member_table_update(NULL, &m) == 0, "update table NULL");
  expect(member_table_update(&table, NULL) == 0, "update member NULL");
  expect(member_table_find_id(NULL, &id) == NULL, "find_id table NULL");
  expect(member_table_find_id(&table, NULL) == NULL, "find_id id NULL");
  expect(member_table_find_addr(NULL, 1, 1) == NULL, "find_addr table NULL");
}

/**
 * @brief JOIN válido entra na tabela e o ACK ecoa o NodeID do joiner.
 */
static void test_handle_join_ok(void) {
  superpeer_t sp;
  pl_header hdr;
  join_t join;
  ack_t ack;
  error_t err;
  const member_t *found;
  node_id_t want;

  sp_reset(&sp);
  make_join(&hdr, &join, 3, ipv4_from_str("192.168.0.10"), 9001, PEER);
  expect(superpeer_handle_join(&sp, &hdr, &join, &ack, &err) == 1, "JOIN peer sucede");
  expect(sp.members.count == 1, "um membro após JOIN");
  memcpy(want.bytes, join.node_id, NODE_ID_SIZE);
  expect(memcmp(ack.node_id, join.node_id, NODE_ID_SIZE) == 0, "ACK ecoa NodeID");
  found = member_table_find_id(&sp.members, &want);
  expect(found != NULL && found->state == MEMBER_ALIVE, "membro ALIVE");
  expect(found->node_type == PEER && found->port == 9001, "tipo e porta do JOIN");
  expect(found->version == 0, "primeiro JOIN version 0");
}

/**
 * @brief Segundo JOIN no mesmo IP+porta atualiza e incrementa version.
 */
static void test_handle_join_repeat(void) {
  superpeer_t sp;
  pl_header hdr;
  join_t join;
  ack_t ack;
  error_t err;
  const member_t *found;

  sp_reset(&sp);
  make_join(&hdr, &join, 4, ipv4_from_str("10.1.0.1"), 8000, PEER);
  expect(superpeer_handle_join(&sp, &hdr, &join, &ack, &err) == 1, "primeiro JOIN");
  make_join(&hdr, &join, 5, ipv4_from_str("10.1.0.1"), 8000, SUPERPEER);
  expect(superpeer_handle_join(&sp, &hdr, &join, &ack, &err) == 1, "JOIN repetido sucede");
  expect(sp.members.count == 1, "ainda um membro");
  found = member_table_find_addr(&sp.members, join.ipv4, join.port);
  expect(found != NULL && found->node_type == SUPERPEER, "tipo atualizado");
  expect(found != NULL && found->version == 1, "version incrementada");
}

/**
 * @brief node_id diferente de src_node, ID zero, IP/porta zero → ERROR 1.
 */
static void test_handle_join_malformed(void) {
  superpeer_t sp;
  pl_header hdr;
  join_t join;
  ack_t ack;
  error_t err;

  sp_reset(&sp);
  make_join(&hdr, &join, 6, ipv4_from_str("10.0.0.1"), 9000, PEER);
  hdr.src_node[0] ^= 0xff;
  expect(superpeer_handle_join(&sp, &hdr, &join, &ack, &err) == 0, "src != node_id falha");
  expect(err.code == 1, "código 1 src divergente");
  expect(sp.members.count == 0, "não registra JOIN inválido");

  make_join(&hdr, &join, 6, ipv4_from_str("10.0.0.1"), 9000, PEER);
  memset(join.node_id, 0, NODE_ID_SIZE);
  memset(hdr.src_node, 0, NODE_ID_SIZE);
  expect(superpeer_handle_join(&sp, &hdr, &join, &ack, &err) == 0, "NodeID zero falha");
  expect(err.code == 1, "código 1 id zero");

  make_join(&hdr, &join, 6, ipv4_from_str("10.0.0.1"), 0, PEER);
  expect(superpeer_handle_join(&sp, &hdr, &join, &ack, &err) == 0, "porta 0 falha");
  expect(err.code == 1, "código 1 porta 0");

  make_join(&hdr, &join, 6, 0, 9000, PEER);
  expect(superpeer_handle_join(&sp, &hdr, &join, &ack, &err) == 0, "ipv4 0 falha");
  expect(err.code == 1, "código 1 ipv4 0");
}

/**
 * @brief node_type fora de PEER/SUPERPEER → ERROR 4; msg_type ≠ JOIN → ERROR 3.
 */
static void test_handle_join_type_errors(void) {
  superpeer_t sp;
  pl_header hdr;
  join_t join;
  ack_t ack;
  error_t err;

  sp_reset(&sp);
  make_join(&hdr, &join, 7, ipv4_from_str("10.0.0.1"), 9000, 2);
  expect(superpeer_handle_join(&sp, &hdr, &join, &ack, &err) == 0, "tipo 2 falha");
  expect(err.code == 4, "código 4 node_type");

  make_join(&hdr, &join, 7, ipv4_from_str("10.0.0.1"), 9000, PEER);
  hdr.msg_type = LEAVE;
  expect(superpeer_handle_join(&sp, &hdr, &join, &ack, &err) == 0, "LEAVE no handler falha");
  expect(err.code == 3, "código 3 msg_type");
}

/**
 * @brief Tabela cheia recusa JOIN novo com ERROR 2.
 */
static void test_handle_join_table_full(void) {
  superpeer_t sp;
  pl_header hdr;
  join_t join;
  ack_t ack;
  error_t err;
  size_t i;

  sp_reset(&sp);
  for (i = 0; i < MEMBER_TABLE_MAX_SIZE; i++) {
    make_join(&hdr, &join, (uint8_t)i, ipv4_from_str("10.0.0.8"), (uint16_t)(2000 + i), PEER);
    if (!superpeer_handle_join(&sp, &hdr, &join, &ack, &err)) {
      expect(0, "enche via JOIN");
      return;
    }
  }
  expect(sp.members.count == MEMBER_TABLE_MAX_SIZE, "tabela cheia via JOIN");
  make_join(&hdr, &join, 1, ipv4_from_str("10.0.0.8"), 65001, PEER);
  expect(superpeer_handle_join(&sp, &hdr, &join, &ack, &err) == 0, "JOIN extra falha");
  expect(err.code == 2, "código 2 tabela cheia");
}

/**
 * @brief Argumentos nulos fazem o handler falhar.
 */
static void test_handle_join_null(void) {
  superpeer_t sp;
  pl_header hdr;
  join_t join;
  ack_t ack;
  error_t err;

  sp_reset(&sp);
  make_join(&hdr, &join, 1, ipv4_from_str("127.0.0.1"), 8080, PEER);
  expect(superpeer_handle_join(NULL, &hdr, &join, &ack, &err) == 0, "sp NULL");
  expect(err.code == 1, "código 1 sp NULL");
  expect(superpeer_handle_join(&sp, NULL, &join, &ack, &err) == 0, "hdr NULL");
  expect(superpeer_handle_join(&sp, &hdr, NULL, &ack, &err) == 0, "join NULL");
  expect(superpeer_handle_join(&sp, &hdr, &join, NULL, &err) == 0, "ack NULL");
  expect(superpeer_handle_join(&sp, &hdr, &join, &ack, NULL) == 0, "err NULL");
}

/**
 * @brief Executa a suíte do Super Peer e devolve o código de test_report.
 */
int main(void) {
  test_member_table_upsert_and_find();
  test_member_table_full();
  test_member_table_null();
  test_handle_join_ok();
  test_handle_join_repeat();
  test_handle_join_malformed();
  test_handle_join_type_errors();
  test_handle_join_table_full();
  test_handle_join_null();
  return test_report();
}
