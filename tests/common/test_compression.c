#include "common/compression.h"
#include "utils/test_utils.h"

#include <stdint.h>
#include <string.h>

/**
 * @file test_compression.c
 * @brief Testes do wrapper LZ4: round-trip, limites e erros.
 */

/* Comprime e descomprime devolve exatamente o buffer original. */
static void test_round_trip(void) {
  const char *msg = "bittorrent-project checkpoint 2: pipeline de arquivos "
                    "arquivo -> SHA-256 -> fragmentacao -> LZ4 -> checksum -> "
                    "transferencia; texto repetido texto repetido texto.";
  size_t in_len = strlen(msg);
  uint8_t comp[512];
  uint8_t back[512];
  size_t comp_len = 0;
  size_t back_len = 0;

  expect(lz4_compress_bound(in_len) > 0, "compress_bound > 0");
  expect(lz4_compress_bound(in_len) <= sizeof comp, "buffer comporta o bound");

  expect(lz4_compress((const uint8_t *)msg, in_len, comp, sizeof comp,
                      &comp_len) == COMP_OK,
         "lz4_compress OK");
  expect(comp_len > 0, "comp_len > 0");

  expect(lz4_decompress(comp, comp_len, back, sizeof back, &back_len) == COMP_OK,
         "lz4_decompress OK");
  expect(back_len == in_len, "back_len == in_len");
  expect(memcmp(msg, back, in_len) == 0, "conteudo identico ao original");
}

/* Dado altamente redundante deve comprimir para menos que o original. */
static void test_compresses_redundant(void) {
  uint8_t in[4096];
  uint8_t comp[8192];
  size_t comp_len = 0;

  memset(in, 'A', sizeof in);
  expect(lz4_compress(in, sizeof in, comp, sizeof comp, &comp_len) == COMP_OK,
         "compress de bloco redundante OK");
  expect(comp_len < sizeof in, "comprime dado redundante");
}

/* Argumentos inválidos e buffers curtos retornam COMP_ERROR sem crash. */
static void test_error_paths(void) {
  uint8_t in[64];
  uint8_t comp[128];
  uint8_t tiny[2];
  size_t out_len = 0;

  memset(in, 0x5A, sizeof in);

  expect(lz4_compress(NULL, sizeof in, comp, sizeof comp, &out_len) == COMP_ERROR,
         "compress rejeita in NULL");
  expect(lz4_compress(in, 0, comp, sizeof comp, &out_len) == COMP_ERROR,
         "compress rejeita in_len 0");
  expect(lz4_compress(in, sizeof in, tiny, sizeof tiny, &out_len) == COMP_ERROR,
         "compress rejeita out_cap insuficiente");
  expect(lz4_compress_bound(0) == 0, "compress_bound(0) == 0");
}

/*
 * Truncar a entrada comprimida é detectado pela descompressão "safe".
 * Observação: o formato de bloco do LZ4 não tem checksum, então virar um byte
 * isolado nem sempre é detectável — a integridade de conteúdo do pipeline vem
 * do SHA-256 por chunk e do CRC32 do framing, não do LZ4.
 */
static void test_truncated_input(void) {
  const char *msg = "conteudo qualquer para comprimir e depois truncar no fio";
  size_t in_len = strlen(msg);
  uint8_t comp[256];
  uint8_t back[256];
  size_t comp_len = 0;
  size_t back_len = 0;

  expect(lz4_compress((const uint8_t *)msg, in_len, comp, sizeof comp,
                      &comp_len) == COMP_OK,
         "compress para teste de truncamento");
  expect(comp_len > 1, "comprimido tem mais de 1 byte");
  expect(lz4_decompress(comp, comp_len - 1, back, sizeof back, &back_len) ==
             COMP_ERROR,
         "decompress rejeita entrada truncada");
}

int main(void) {
  test_round_trip();
  test_compresses_redundant();
  test_error_paths();
  test_truncated_input();
  return test_report();
}
