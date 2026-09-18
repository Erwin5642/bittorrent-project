#include "common/config.h"
#include "utils/test_utils.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/**
 * @file test_config.c
 * @brief Testes unitários do parser de arquivos .conf.
 */

/** Caminho fixo no workspace; cada teste sobrescreve e dá unlink. */
#define TEST_CONF_PATH "obj/test-config.conf"

/**
 * @brief Escreve @p contents em TEST_CONF_PATH.
 * @return 1 em sucesso, 0 se fopen/fwrite falhar.
 */
static int write_temp_conf(const char *contents) {
  FILE *fp;
  size_t n;

  fp = fopen(TEST_CONF_PATH, "w");
  if (!fp) {
    return 0;
  }
  n = strlen(contents);
  if (fwrite(contents, 1, n, fp) != n) {
    fclose(fp);
    unlink(TEST_CONF_PATH);
    return 0;
  }
  fclose(fp);
  return 1;
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
 * @brief Carrega um Super Peer com um bootstrap e confere ip/porta/tipo.
 */
static void test_load_superpeer_with_bootstrap(void) {
  node_config_t cfg;

  expect(write_temp_conf("ip=127.0.0.1\n"
                         "port=8080\n"
                         "type=superpeer\n"
                         "bootstrap=192.168.0.1:9000\n"),
         "escreve conf superpeer");
  expect(node_config_load(TEST_CONF_PATH, &cfg) == 1, "load superpeer sucede");
  expect(cfg.ipv4 == ipv4_from_str("127.0.0.1"), "ip anunciado");
  expect(cfg.port == 8080, "porta 8080");
  expect(cfg.node_type == superpeer, "type superpeer");
  expect(cfg.bootstrap_count == 1, "um bootstrap");
  expect(cfg.bootstrap[0].ipv4 == ipv4_from_str("192.168.0.1"), "bootstrap ip");
  expect(cfg.bootstrap[0].port == 9000, "bootstrap porta");
  unlink(TEST_CONF_PATH);
}

/**
 * @brief Peer sem chave bootstrap deve ficar com lista vazia.
 */
static void test_load_peer_without_bootstrap(void) {
  node_config_t cfg;

  expect(write_temp_conf("ip=10.0.0.2\n"
                         "port=7000\n"
                         "type=peer\n"),
         "escreve conf peer");
  expect(node_config_load(TEST_CONF_PATH, &cfg) == 1, "load peer sucede");
  expect(cfg.node_type == peer, "type peer");
  expect(cfg.port == 7000, "porta 7000");
  expect(cfg.bootstrap_count == 0, "sem bootstrap");
  unlink(TEST_CONF_PATH);
}

/**
 * @brief `bootstrap=` vazio e comentários/espaços não quebram o parse.
 */
static void test_comments_whitespace_empty_bootstrap(void) {
  node_config_t cfg;

  expect(write_temp_conf("# overlay local\n"
                         "\n"
                         "  ip = 127.0.0.1  \n"
                         "port = 1\n"
                         "type=superpeer\n"
                         "bootstrap=\n"),
         "escreve conf com comentários");
  expect(node_config_load(TEST_CONF_PATH, &cfg) == 1, "load com espaços sucede");
  expect(cfg.port == 1, "porta mínima");
  expect(cfg.bootstrap_count == 0, "bootstrap vazio");
  unlink(TEST_CONF_PATH);
}

/**
 * @brief Vários Super Peers de bootstrap separados por vírgula.
 */
static void test_bootstrap_list(void) {
  node_config_t cfg;

  expect(write_temp_conf("ip=127.0.0.1\n"
                         "port=8080\n"
                         "type=superpeer\n"
                         "bootstrap=10.0.0.1:8000, 10.0.0.2:8001\n"),
         "escreve conf com lista");
  expect(node_config_load(TEST_CONF_PATH, &cfg) == 1, "load lista sucede");
  expect(cfg.bootstrap_count == 2, "dois bootstraps");
  expect(cfg.bootstrap[0].port == 8000, "primeiro porto");
  expect(cfg.bootstrap[1].ipv4 == ipv4_from_str("10.0.0.2"), "segundo ip");
  unlink(TEST_CONF_PATH);
}

/**
 * @brief ip, port e type são obrigatórios.
 */
static void test_missing_required(void) {
  node_config_t cfg;

  expect(write_temp_conf("port=8080\ntype=peer\n"), "sem ip");
  expect(node_config_load(TEST_CONF_PATH, &cfg) == 0, "falha sem ip");
  unlink(TEST_CONF_PATH);

  expect(write_temp_conf("ip=127.0.0.1\ntype=peer\n"), "sem port");
  expect(node_config_load(TEST_CONF_PATH, &cfg) == 0, "falha sem port");
  unlink(TEST_CONF_PATH);

  expect(write_temp_conf("ip=127.0.0.1\nport=8080\n"), "sem type");
  expect(node_config_load(TEST_CONF_PATH, &cfg) == 0, "falha sem type");
  unlink(TEST_CONF_PATH);
}

/**
 * @brief Valores inválidos e chave desconhecida fazem o load falhar.
 */
static void test_invalid_fields(void) {
  node_config_t cfg;

  expect(write_temp_conf("ip=999.0.0.1\nport=8080\ntype=peer\n"), "ip ruim");
  expect(node_config_load(TEST_CONF_PATH, &cfg) == 0, "falha ip inválido");
  unlink(TEST_CONF_PATH);

  expect(write_temp_conf("ip=127.0.0.1\nport=0\ntype=peer\n"), "porta 0");
  expect(node_config_load(TEST_CONF_PATH, &cfg) == 0, "falha porta 0");
  unlink(TEST_CONF_PATH);

  expect(write_temp_conf("ip=127.0.0.1\nport=65536\ntype=peer\n"), "porta alta");
  expect(node_config_load(TEST_CONF_PATH, &cfg) == 0, "falha porta 65536");
  unlink(TEST_CONF_PATH);

  expect(write_temp_conf("ip=127.0.0.1\nport=8080\ntype=tracker\n"), "tipo ruim");
  expect(node_config_load(TEST_CONF_PATH, &cfg) == 0, "falha type inválido");
  unlink(TEST_CONF_PATH);

  expect(write_temp_conf("ip=127.0.0.1\nport=8080\ntype=peer\nfoo=bar\n"), "chave extra");
  expect(node_config_load(TEST_CONF_PATH, &cfg) == 0, "falha chave desconhecida");
  unlink(TEST_CONF_PATH);

  expect(write_temp_conf("ip=127.0.0.1\nip=10.0.0.1\nport=8080\ntype=peer\n"), "ip duplicado");
  expect(node_config_load(TEST_CONF_PATH, &cfg) == 0, "falha chave duplicada");
  unlink(TEST_CONF_PATH);

  expect(write_temp_conf("ip=127.0.0.1\nport=8080\ntype=superpeer\nbootstrap=127.0.0.1\n"),
         "bootstrap sem porta");
  expect(node_config_load(TEST_CONF_PATH, &cfg) == 0, "falha bootstrap malformado");
  unlink(TEST_CONF_PATH);
}

/**
 * @brief Caminho inexistente e ponteiros nulos falham.
 */
static void test_null_and_missing_file(void) {
  node_config_t cfg;

  expect(node_config_load(NULL, &cfg) == 0, "path NULL falha");
  expect(node_config_load("/tmp/does-not-exist-bt-conf", NULL) == 0, "out NULL falha");
  expect(node_config_load("/tmp/does-not-exist-bt-conf", &cfg) == 0, "arquivo ausente falha");
}

/**
 * @brief Executa a suíte de config e devolve o código de test_report.
 */
int main(void) {
  test_load_superpeer_with_bootstrap();
  test_load_peer_without_bootstrap();
  test_comments_whitespace_empty_bootstrap();
  test_bootstrap_list();
  test_missing_required();
  test_invalid_fields();
  test_null_and_missing_file();
  return test_report();
}
