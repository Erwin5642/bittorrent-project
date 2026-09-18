#include "common/protocol.h"
#include "utils/test_utils.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>

int pack_header(const pl_header *in_st, char *out_st);
int unpack_header(pl_header *out_st, const char *in_msg);
int pack_join(const join_t *in_st, char *out_msg);
int pack_ack(const ack_t *in_st, char *out_msg);
int pack_error(const error_t *in_st, char *out_msg);

/**
 * @file test_protocol.c
 * @brief Testes de framing CP1: header, versão, tipos e tamanhos de payload.
 */

static void test_header_size(void) {
  expect(HEADER_SIZE == 99, "HEADER_SIZE é 99");
}

static void test_protocol_ver(void) {
  expect(PROTOCOL_VER == 1, "PROTOCOL_VER é 1");
}

static void test_message_types(void) {
  expect(JOIN == 0, "JOIN = 0");
  expect(PING == 1, "PING = 1");
  expect(PONG == 2, "PONG = 2");
  expect(LEAVE == 3, "LEAVE = 3");
}

static void test_pack_unpack_header(void) {
  pl_header in;
  pl_header out;
  char wire[HEADER_SIZE];
  uint16_t type_be;

  memset(&in, 0, sizeof in);
  in.protocol_ver = PROTOCOL_VER;
  in.msg_type = PING;
  in.src_node[0] = 0xAA;
  in.dst_node[1] = 0xBB;
  in.trsc_id[2] = 0xCC;
  in.time = 0x0102030405060708ULL;
  in.pl_size = 0;
  in.checksum = 0;

  expect(pack_header(&in, wire) == 0, "pack_header");
  expect((uint8_t)wire[0] == PROTOCOL_VER, "byte 0 é protocol_ver");
  memcpy(&type_be, wire + 1, sizeof type_be);
  expect(ntohs(type_be) == PING, "bytes 1-2 são msg_type em network order");

  memset(&out, 0, sizeof out);
  expect(unpack_header(&out, wire) == 0, "unpack_header");
  expect(out.protocol_ver == PROTOCOL_VER, "unpack protocol_ver");
  expect(out.msg_type == PING, "unpack msg_type");
  expect(out.src_node[0] == 0xAA, "unpack src_node");
  expect(out.dst_node[1] == 0xBB, "unpack dst_node");
  expect(out.trsc_id[2] == 0xCC, "unpack trsc_id");
  expect(out.time == 0x0102030405060708ULL, "unpack time");
  expect(out.pl_size == 0, "unpack pl_size");
}

static void test_payload_wire_sizes(void) {
  join_t join;
  ack_t ack;
  error_t err;
  char join_buf[39];
  char ack_buf[32];
  char err_buf[68];

  memset(&join, 0, sizeof join);
  memset(&ack, 0, sizeof ack);
  memset(&err, 0, sizeof err);
  expect(pack_join(&join, join_buf) == 0, "pack_join 39 bytes");
  expect(pack_ack(&ack, ack_buf) == 0, "pack_ack 32 bytes");
  expect(pack_error(&err, err_buf) == 0, "pack_error 68 bytes");
}

int main(void) {
  test_header_size();
  test_protocol_ver();
  test_message_types();
  test_pack_unpack_header();
  test_payload_wire_sizes();
  return test_report();
}
