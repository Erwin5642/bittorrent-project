#include "common/node.h"
#include "utils/test_utils.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

/**
 * @file test_node.c
 * @brief Testes unitários de NodeID, UUID e SHA-256.
 */

/**
 * @brief Converte 64 caracteres hex em um NodeID de 32 bytes.
 * @param hex Digest em hex (minúsculo ou maiúsculo).
 * @param out Destino; o caller aloca.
 * @return 1 se o parse for válido, 0 caso contrário.
 */
static int parse_hex32(const char *hex, node_id_t *out) {
  size_t i;

  if (strlen(hex) != NODE_ID_SIZE * 2) {
    return 0;
  }
  for (i = 0; i < NODE_ID_SIZE; i++) {
    unsigned int byte;
    if (sscanf(hex + (i * 2), "%2x", &byte) != 1) {
      return 0;
    }
    out->bytes[i] = (uint8_t)byte;
  }
  return 1;
}

/**
 * @brief Converte um IPv4 dotted-quad para network byte order.
 * @param s Endereço no formato "a.b.c.d".
 * @return Endereço em network byte order, ou 0 se inet_pton falhar.
 */
static uint32_t ipv4_from_str(const char *s) {
  uint32_t addr;

  if (inet_pton(AF_INET, s, &addr) != 1) {
    fprintf(stderr, "inet_pton failed for %s\n", s);
    return 0;
  }
  return addr;
}

/**
 * @brief Preenche o UUID com bytes sequenciais a partir de @p start.
 * @param uuid Destino.
 * @param start Primeiro byte (os seguintes são start+1, start+2, …).
 */
static void fill_uuid(node_uuid_t *uuid, int start) {
  int i;

  for (i = 0; i < NODE_UUID_SIZE; i++) {
    uuid->bytes[i] = (uint8_t)(start + i);
  }
}

/**
 * @brief Confere o vetor 127.0.0.1:8080 com UUID zero contra SHA-256 conhecido.
 */
static void test_known_vector_localhost(void) {
  node_uuid_t uuid;
  node_id_t got;
  node_id_t want;

  memset(&uuid, 0, sizeof uuid);
  expect(parse_hex32("a92ddf0e5cc69064169ec0c8d00a9a53824ff98275840966ec6ea42fc775c56e", &want),
         "parse vetor localhost");
  expect(node_id_generate(ipv4_from_str("127.0.0.1"), 8080, &uuid, &got) == 1, "generate 127.0.0.1:8080");
  expect(memcmp(got.bytes, want.bytes, NODE_ID_SIZE) == 0,
         "NodeID 127.0.0.1:8080 UUID zero bate com SHA-256 conhecido");
}

/**
 * @brief Confere o vetor 192.168.0.1:443 com UUID 00..0f contra SHA-256 conhecido.
 */
static void test_known_vector_lan(void) {
  node_uuid_t uuid;
  node_id_t got;
  node_id_t want;

  fill_uuid(&uuid, 0);
  expect(parse_hex32("560cbcaf67ac4bcd37bfa02d35289305badf4d7557afd8fe9525f6158d6cdbaa", &want),
         "parse vetor LAN");
  expect(node_id_generate(ipv4_from_str("192.168.0.1"), 443, &uuid, &got) == 1, "generate 192.168.0.1:443");
  expect(memcmp(got.bytes, want.bytes, NODE_ID_SIZE) == 0,
         "NodeID 192.168.0.1:443 UUID 00..0f bate com SHA-256 conhecido");
}

/**
 * @brief Gera o NodeID duas vezes com os mesmos inputs e exige igualdade.
 */
static void test_deterministic(void) {
  node_uuid_t uuid;
  node_id_t a;
  node_id_t b;

  fill_uuid(&uuid, 7);
  expect(node_id_generate(ipv4_from_str("10.0.0.1"), 9000, &uuid, &a) == 1, "generate a");
  expect(node_id_generate(ipv4_from_str("10.0.0.1"), 9000, &uuid, &b) == 1, "generate b");
  expect(memcmp(a.bytes, b.bytes, NODE_ID_SIZE) == 0, "mesmo IP+porta+UUID gera o mesmo NodeID");
}

/**
 * @brief Mantém IP e porta e troca o UUID; os NodeIDs devem diferir.
 */
static void test_uuid_changes_id(void) {
  node_uuid_t uuid_a;
  node_uuid_t uuid_b;
  node_id_t a;
  node_id_t b;

  fill_uuid(&uuid_a, 1);
  fill_uuid(&uuid_b, 2);
  expect(node_id_generate(ipv4_from_str("10.0.0.1"), 9000, &uuid_a, &a) == 1, "generate uuid a");
  expect(node_id_generate(ipv4_from_str("10.0.0.1"), 9000, &uuid_b, &b) == 1, "generate uuid b");
  expect(memcmp(a.bytes, b.bytes, NODE_ID_SIZE) != 0, "UUID diferente gera NodeID diferente");
}

/**
 * @brief Mantém IP e UUID e troca a porta; os NodeIDs devem diferir.
 */
static void test_port_changes_id(void) {
  node_uuid_t uuid;
  node_id_t a;
  node_id_t b;

  fill_uuid(&uuid, 3);
  expect(node_id_generate(ipv4_from_str("10.0.0.1"), 8000, &uuid, &a) == 1, "generate porta 8000");
  expect(node_id_generate(ipv4_from_str("10.0.0.1"), 8001, &uuid, &b) == 1, "generate porta 8001");
  expect(memcmp(a.bytes, b.bytes, NODE_ID_SIZE) != 0, "porta diferente gera NodeID diferente");
}

/**
 * @brief Mantém porta e UUID e troca o IP; os NodeIDs devem diferir.
 */
static void test_ip_changes_id(void) {
  node_uuid_t uuid;
  node_id_t a;
  node_id_t b;

  fill_uuid(&uuid, 4);
  expect(node_id_generate(ipv4_from_str("10.0.0.1"), 8000, &uuid, &a) == 1, "generate .1");
  expect(node_id_generate(ipv4_from_str("10.0.0.2"), 8000, &uuid, &b) == 1, "generate .2");
  expect(memcmp(a.bytes, b.bytes, NODE_ID_SIZE) != 0, "IP diferente gera NodeID diferente");
}

/**
 * @brief Exige que uuid ou out nulos façam node_id_generate falhar.
 */
static void test_null_args(void) {
  node_uuid_t uuid;
  node_id_t id;

  fill_uuid(&uuid, 0);
  expect(node_id_generate(ipv4_from_str("127.0.0.1"), 80, NULL, &id) == 0, "uuid NULL falha");
  expect(node_id_generate(ipv4_from_str("127.0.0.1"), 80, &uuid, NULL) == 0, "out NULL falha");
}

/**
 * @brief Lê dois UUIDs de /dev/urandom e exige que sejam diferentes.
 */
static void test_uuid_random(void) {
  node_uuid_t a;
  node_uuid_t b;

  memset(&a, 0, sizeof a);
  memset(&b, 0, sizeof b);
  expect(node_uuid_random(&a) == 1, "urandom a");
  expect(node_uuid_random(&b) == 1, "urandom b");
  expect(memcmp(a.bytes, b.bytes, NODE_UUID_SIZE) != 0, "dois UUID aleatórios diferem");
}

/**
 * @brief Gera dois NodeIDs no mesmo IP/porta com UUIDs aleatórios distintos.
 */
static void test_random_ids_differ(void) {
  node_uuid_t uuid_a;
  node_uuid_t uuid_b;
  node_id_t a;
  node_id_t b;
  uint32_t ip = ipv4_from_str("127.0.0.1");

  expect(node_uuid_random(&uuid_a) == 1, "uuid a");
  expect(node_uuid_random(&uuid_b) == 1, "uuid b");
  expect(node_id_generate(ip, 7000, &uuid_a, &a) == 1, "id a");
  expect(node_id_generate(ip, 7000, &uuid_b, &b) == 1, "id b");
  expect(memcmp(a.bytes, b.bytes, NODE_ID_SIZE) != 0,
         "mesmo IP/porta com UUID aleatório distinto gera NodeID distinto");
}

/**
 * @brief Exige igualdade, ordem lexicográfica e tratamento de ponteiro nulo.
 */
static void test_id_cmp(void) {
  node_id_t a;
  node_id_t b;

  memset(&a, 0, sizeof a);
  memset(&b, 0, sizeof b);
  expect(node_id_cmp(&a, &b) == 0, "IDs iguais comparam 0");

  b.bytes[0] = 1;
  expect(node_id_cmp(&a, &b) < 0, "00.. menor que 01..");
  expect(node_id_cmp(&b, &a) > 0, "01.. maior que 00..");

  a.bytes[0] = 1;
  a.bytes[NODE_ID_SIZE - 1] = 1;
  b.bytes[NODE_ID_SIZE - 1] = 2;
  expect(node_id_cmp(&a, &b) < 0, "último byte desempata lexicograficamente");

  expect(node_id_cmp(NULL, NULL) == 0, "dois nulos comparam 0");
  expect(node_id_cmp(NULL, &a) < 0, "nulo é menor que ID válido");
  expect(node_id_cmp(&a, NULL) > 0, "ID válido é maior que nulo");
}

/**
 * @brief Confere hex minúsculo de 64 chars, buffer curto e argumentos nulos.
 */
static void test_id_to_hex(void) {
  node_id_t id;
  char hex[NODE_ID_HEX_SIZE];
  char too_small[NODE_ID_HEX_SIZE - 1];

  expect(parse_hex32("a92ddf0e5cc69064169ec0c8d00a9a53824ff98275840966ec6ea42fc775c56e", &id),
         "parse vetor localhost para hex");
  expect(node_id_to_hex(&id, hex, sizeof hex) == 1, "to_hex sucede");
  expect(strlen(hex) == (size_t)(NODE_ID_SIZE * 2), "hex tem 64 caracteres");
  expect(strcmp(hex, "a92ddf0e5cc69064169ec0c8d00a9a53824ff98275840966ec6ea42fc775c56e") == 0,
         "hex minúsculo bate com o vetor conhecido");

  expect(node_id_to_hex(&id, too_small, sizeof too_small) == 0, "buffer curto falha");
  expect(node_id_to_hex(NULL, hex, sizeof hex) == 0, "id NULL falha");
  expect(node_id_to_hex(&id, NULL, sizeof hex) == 0, "out NULL falha");
}

/**
 * @brief Executa a suíte de NodeID e devolve o código de test_report.
 */
int main(void) {
  test_known_vector_localhost();
  test_known_vector_lan();
  test_deterministic();
  test_uuid_changes_id();
  test_port_changes_id();
  test_ip_changes_id();
  test_null_args();
  test_uuid_random();
  test_random_ids_differ();
  test_id_cmp();
  test_id_to_hex();

  return test_report();
}
