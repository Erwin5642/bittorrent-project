#include "peer/session.h"
#include "utils/test_utils.h"

#include <string.h>

/**
 * @file test_session.c
 * @brief Testes de session_split: tokens, aspas, comentários e erros.
 */

static int split(const char *text, char **argv, size_t cap) {
  static char buf[SESSION_LINE_MAX];

  strncpy(buf, text, sizeof buf - 1);
  buf[sizeof buf - 1] = '\0';
  return session_split(buf, argv, cap);
}

static void test_simples(void) {
  char *argv[SESSION_ARGV_MAX];
  int n = split("upload documento_a.pdf\n", argv, SESSION_ARGV_MAX);

  expect(n == 2, "dois tokens");
  expect(strcmp(argv[0], "upload") == 0, "primeiro token");
  expect(strcmp(argv[1], "documento_a.pdf") == 0, "segundo token sem o \\n");
}

static void test_espacos_extras(void) {
  char *argv[SESSION_ARGV_MAX];
  int n = split("  \t state   127.0.0.1\t5101 \r\n", argv, SESSION_ARGV_MAX);

  expect(n == 3, "tokens ignorando espaços e tabs nas pontas");
  expect(strcmp(argv[0], "state") == 0, "state");
  expect(strcmp(argv[1], "127.0.0.1") == 0, "host");
  expect(strcmp(argv[2], "5101") == 0, "porta sem o \\r");
}

static void test_vazio_e_comentario(void) {
  char *argv[SESSION_ARGV_MAX];

  expect(split("", argv, SESSION_ARGV_MAX) == 0, "linha vazia");
  expect(split("   \n", argv, SESSION_ARGV_MAX) == 0, "só espaços");
  expect(split("# comentário\n", argv, SESSION_ARGV_MAX) == 0, "comentário");
  expect(split("   # comentário\n", argv, SESSION_ARGV_MAX) == 0, "comentário indentado");
  expect(split("put a #b\n", argv, SESSION_ARGV_MAX) == 3, "# no meio da linha é texto");
}

static void test_aspas(void) {
  char *argv[SESSION_ARGV_MAX];
  int n = split("upload \"meu arquivo.pdf\" x\n", argv, SESSION_ARGV_MAX);

  expect(n == 3, "aspas agrupam um token");
  expect(strcmp(argv[1], "meu arquivo.pdf") == 0, "aspas saem do token");
  expect(strcmp(argv[2], "x") == 0, "token depois das aspas");

  n = split("put nome \"\"\n", argv, SESSION_ARGV_MAX);
  expect(n == 3 && argv[2][0] == '\0', "token vazio entre aspas");
}

static void test_erros(void) {
  char *argv[3];

  expect(split("upload \"sem fim\n", argv, 3) == -1, "aspa sem fechar");
  expect(split("upload \"a\"b\n", argv, 3) == -1, "texto colado na aspa de fechamento");
  expect(split("a b c d\n", argv, 3) == -1, "mais tokens que a capacidade");
  expect(split("a b c\n", argv, 3) == 3, "exatamente a capacidade");
  expect(session_split(NULL, argv, 3) == -1, "linha NULL");
  expect(session_split((char[]){"a"}, NULL, 3) == -1, "argv NULL");
}

int main(void) {
  test_simples();
  test_espacos_extras();
  test_vazio_e_comentario();
  test_aspas();
  test_erros();
  return test_report();
}
