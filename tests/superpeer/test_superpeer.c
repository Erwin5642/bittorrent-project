#include "superpeer/superpeer.h"
#include "utils/test_utils.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * @file test_superpeer.c
 * @brief Testes unitários de membros, JOIN e STORE (sem socket).
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
 * @brief Super Peer zerado, com as tabelas vazias (sem listen).
 */
static void sp_reset(superpeer_t *sp) {
  memset(sp, 0, sizeof *sp);
  sp->listend_fd = -1;
  member_table_init(&sp->members);
  metadata_table_init(&sp->metadata);
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
 * @brief Empacota um STORE cujo owner vira src_node. Libera os hashes locais.
 */
static int pack_store(uint8_t *buf, size_t cap, pl_header *hdr, uint32_t index, const char *name,
                      uint32_t version, uint32_t chunks) {
  file_metadata_t meta;
  ssize_t n;
  size_t i;

  metadata_init(&meta);
  meta.object_id[0] = 0xA0;
  meta.object_id[1] = (uint8_t)(index >> 8);
  meta.object_id[2] = (uint8_t)index;
  meta.object_id[3] = 0x5A;
  strncpy(meta.filename, name, METADATA_FILENAME_MAX - 1);
  meta.size = 1000u + index;
  meta.chunk_count = chunks;
  meta.version = version;
  memset(meta.owner.bytes, 0x11, NODE_ID_SIZE);
  meta.owner.bytes[0] = (uint8_t)(index + 1);
  if (chunks > 0) {
    meta.chunk_hashes = malloc((size_t)chunks * METADATA_CHUNK_HASH_SIZE);
    if (!meta.chunk_hashes) {
      return 0;
    }
    for (i = 0; i < (size_t)chunks * METADATA_CHUNK_HASH_SIZE; i++) {
      meta.chunk_hashes[i] = (uint8_t)(0x40 + i);
    }
  }
  n = metadata_pack(&meta, buf, cap);
  memset(hdr, 0, sizeof *hdr);
  hdr->msg_type = STORE;
  hdr->pl_size = n < 0 ? 0 : (uint32_t)n;
  memcpy(hdr->src_node, meta.owner.bytes, NODE_ID_SIZE);
  metadata_release(&meta);
  return n > 0;
}

/**
 * @brief STORE válido entra na tabela, preserva a versão e ecoa o owner no ACK.
 */
static void test_handle_store_ok(void) {
  superpeer_t sp;
  pl_header hdr;
  ack_t ack;
  error_t err;
  uint8_t buf[METADATA_WIRE_PREFIX + METADATA_CHUNK_HASH_SIZE];
  const file_metadata_t *found;

  sp_reset(&sp);
  expect(pack_store(buf, sizeof buf, &hdr, 1, "arquivo.pdf", 4, 1), "empacota STORE");
  expect(superpeer_handle_store(&sp, &hdr, buf, hdr.pl_size, &ack, &err) == 1, "STORE sucede");
  expect(sp.metadata.count == 1, "um registro");
  expect(memcmp(ack.node_id, hdr.src_node, NODE_ID_SIZE) == 0, "ACK ecoa owner");
  found = metadata_table_find_name(&sp.metadata, "arquivo.pdf");
  expect(found != NULL && found->version == 4, "primeira versao permanece");
  expect(found != NULL && found->size == 1001, "tamanho original");
  expect(found != NULL && found->chunk_count == 1 && found->chunk_hashes[0] == 0x40, "hash do chunk");
  metadata_table_clear(&sp.metadata);
}

/**
 * @brief ObjectID já conhecido atualiza os campos e incrementa version.
 */
static void test_handle_store_updates(void) {
  superpeer_t sp;
  pl_header hdr;
  ack_t ack;
  error_t err;
  uint8_t buf[METADATA_WIRE_PREFIX];
  const file_metadata_t *found;

  sp_reset(&sp);
  expect(pack_store(buf, sizeof buf, &hdr, 2, "a.pdf", 1, 0), "primeiro STORE");
  expect(superpeer_handle_store(&sp, &hdr, buf, hdr.pl_size, &ack, &err) == 1, "insere");
  expect(pack_store(buf, sizeof buf, &hdr, 2, "b.pdf", 9, 0), "segundo STORE");
  expect(superpeer_handle_store(&sp, &hdr, buf, hdr.pl_size, &ack, &err) == 1, "atualiza");
  expect(sp.metadata.count == 1, "nao duplica");
  found = metadata_table_find_name(&sp.metadata, "b.pdf");
  expect(found != NULL && found->version == 2, "versao armazenada mais um");
  expect(metadata_table_find_name(&sp.metadata, "a.pdf") == NULL, "nome antigo sai");
  metadata_table_clear(&sp.metadata);
}

/**
 * @brief Payload curto ou owner diferente de src_node → ERROR 1.
 */
static void test_handle_store_malformed(void) {
  superpeer_t sp;
  pl_header hdr;
  ack_t ack;
  error_t err;
  uint8_t buf[METADATA_WIRE_PREFIX];

  sp_reset(&sp);
  expect(pack_store(buf, sizeof buf, &hdr, 3, "c.pdf", 1, 0), "empacota");
  expect(superpeer_handle_store(&sp, &hdr, buf, 10, &ack, &err) == 0, "cauda curta falha");
  expect(err.code == 1, "código 1 payload curto");
  expect(sp.metadata.count == 0, "nao registra payload curto");

  expect(pack_store(buf, sizeof buf, &hdr, 3, "c.pdf", 1, 0), "empacota de novo");
  hdr.src_node[0] ^= 0xff;
  expect(superpeer_handle_store(&sp, &hdr, buf, hdr.pl_size, &ack, &err) == 0, "owner divergente falha");
  expect(err.code == 1, "código 1 owner");
  expect(sp.metadata.count == 0, "nao registra owner divergente");
}

/**
 * @brief msg_type diferente de STORE → ERROR 3.
 */
static void test_handle_store_wrong_type(void) {
  superpeer_t sp;
  pl_header hdr;
  ack_t ack;
  error_t err;
  uint8_t buf[METADATA_WIRE_PREFIX];

  sp_reset(&sp);
  expect(pack_store(buf, sizeof buf, &hdr, 4, "d.pdf", 1, 0), "empacota");
  hdr.msg_type = JOIN;
  expect(superpeer_handle_store(&sp, &hdr, buf, hdr.pl_size, &ack, &err) == 0, "JOIN no handler falha");
  expect(err.code == 3, "código 3 msg_type");
  expect(sp.metadata.count == 0, "nao registra tipo errado");
}

/**
 * @brief Tabela cheia recusa inserção nova com ERROR 6 e ainda atualiza a existente.
 */
static void test_handle_store_table_full(void) {
  superpeer_t sp;
  pl_header hdr;
  ack_t ack;
  error_t err;
  file_metadata_t meta;
  uint8_t buf[METADATA_WIRE_PREFIX];
  uint32_t i;
  const file_metadata_t *found;

  sp_reset(&sp);
  for (i = 0; i < METADATA_TABLE_MAX; i++) {
    metadata_init(&meta);
    meta.object_id[0] = 0xA0;
    meta.object_id[1] = (uint8_t)(i >> 8);
    meta.object_id[2] = (uint8_t)i;
    meta.object_id[3] = 0x5A;
    strncpy(meta.filename, "cheio.pdf", METADATA_FILENAME_MAX - 1);
    meta.size = i;
    meta.version = 1;
    memset(meta.owner.bytes, 0x22, NODE_ID_SIZE);
    if (!metadata_table_put(&sp.metadata, &meta)) {
      expect(0, "enche a tabela");
      metadata_table_clear(&sp.metadata);
      return;
    }
  }
  expect(pack_store(buf, sizeof buf, &hdr, 65000, "novo.pdf", 1, 0), "empacota extra");
  expect(superpeer_handle_store(&sp, &hdr, buf, hdr.pl_size, &ack, &err) == 0, "STORE extra falha");
  expect(err.code == 6, "código 6 tabela cheia");
  expect(sp.metadata.count == METADATA_TABLE_MAX, "count nao cresce");

  expect(pack_store(buf, sizeof buf, &hdr, 0, "atualizado.pdf", 3, 0), "empacota existente");
  expect(superpeer_handle_store(&sp, &hdr, buf, hdr.pl_size, &ack, &err) == 1, "atualiza no limite");
  found = metadata_table_find_name(&sp.metadata, "atualizado.pdf");
  expect(found != NULL && found->version == 2, "versao incrementada no limite");
  metadata_table_clear(&sp.metadata);
}

/**
 * @brief Argumentos nulos fazem o handler de STORE falhar.
 */
static void test_handle_store_null(void) {
  superpeer_t sp;
  pl_header hdr;
  ack_t ack;
  error_t err;
  uint8_t buf[METADATA_WIRE_PREFIX];

  sp_reset(&sp);
  expect(pack_store(buf, sizeof buf, &hdr, 5, "e.pdf", 1, 0), "empacota");
  expect(superpeer_handle_store(NULL, &hdr, buf, hdr.pl_size, &ack, &err) == 0, "sp NULL");
  expect(err.code == 1, "código 1 sp NULL");
  expect(superpeer_handle_store(&sp, NULL, buf, hdr.pl_size, &ack, &err) == 0, "hdr NULL");
  expect(superpeer_handle_store(&sp, &hdr, NULL, hdr.pl_size, &ack, &err) == 0, "payload NULL");
  expect(superpeer_handle_store(&sp, &hdr, buf, hdr.pl_size, NULL, &err) == 0, "ack NULL");
  expect(superpeer_handle_store(&sp, &hdr, buf, hdr.pl_size, &ack, NULL) == 0, "err NULL");
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
  test_handle_store_ok();
  test_handle_store_updates();
  test_handle_store_malformed();
  test_handle_store_wrong_type();
  test_handle_store_table_full();
  test_handle_store_null();
  return test_report();
}
