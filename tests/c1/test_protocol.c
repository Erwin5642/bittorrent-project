#include "common/network.h"
#include "common/protocol.h"
#include "utils/test_utils.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <zlib.h>

int pack_header(const pl_header *in_st, uint8_t *out_st);
int unpack_header(pl_header *out_st, const uint8_t *in_msg);
int pack_join(const join_t *in_st, uint8_t *out_msg);
int pack_ack(const ack_t *in_st, uint8_t *out_msg);
int pack_error(const error_t *in_st, uint8_t *out_msg);

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
  uint8_t wire[HEADER_SIZE];
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
  expect(wire[0] == PROTOCOL_VER, "byte 0 é protocol_ver");
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
  uint8_t join_buf[39];
  uint8_t ack_buf[32];
  uint8_t err_buf[68];

  memset(&join, 0, sizeof join);
  memset(&ack, 0, sizeof ack);
  memset(&err, 0, sizeof err);
  expect(pack_join(&join, join_buf) == 0, "pack_join 39 bytes");
  expect(pack_ack(&ack, ack_buf) == 0, "pack_ack 32 bytes");
  expect(pack_error(&err, err_buf) == 0, "pack_error 68 bytes");
}

/* C5: deserialize_message deve aceitar STORE (payload variável), não rejeitar. */
static void test_deserialize_store(void) {
  file_metadata_t meta;
  file_metadata_t got;
  pl_header hdr;
  pl_header out_hdr;
  uint8_t frame[HEADER_SIZE + METADATA_PAYLOAD_MAX];
  ssize_t wire;
  uLong crc;

  metadata_init(&meta);
  meta.object_id[0] = 0x01;              /* ObjectID não pode ser zero */
  strcpy(meta.filename, "arquivo.pdf");
  meta.size = 12458291ULL;
  meta.chunk_count = 0;                  /* sem cauda: wire == METADATA_WIRE_PREFIX */

  wire = metadata_pack(&meta, frame + HEADER_SIZE, sizeof frame - HEADER_SIZE);
  expect(wire == (ssize_t)METADATA_WIRE_PREFIX, "metadata_pack prefixo de 336 bytes");

  memset(&hdr, 0, sizeof hdr);
  hdr.protocol_ver = PROTOCOL_VER;
  hdr.msg_type = STORE;
  hdr.pl_size = (uint32_t)wire;
  crc = crc32(0L, Z_NULL, 0);
  crc = crc32(crc, (const Bytef *)(frame + HEADER_SIZE), (uInt)wire);
  hdr.checksum = (uint32_t)crc;
  expect(pack_header(&hdr, frame) == 0, "pack_header STORE");

  expect(deserialize_message(frame, (size_t)HEADER_SIZE + (size_t)wire, &out_hdr,
                             NULL) == NET_OK,
         "deserialize_message aceita STORE (C5)");
  expect(out_hdr.msg_type == STORE, "header STORE preservado");

  /* O handler recupera o registro a partir do payload cru. */
  expect(metadata_unpack(&got, frame + HEADER_SIZE, (size_t)wire) == 1,
         "metadata_unpack round-trip");
  expect(got.size == meta.size, "size preservado");
  expect(memcmp(got.object_id, meta.object_id, METADATA_OBJECT_ID_SIZE) == 0,
         "object_id preservado");
  metadata_release(&got);
}

int main(void) {
  test_header_size();
  test_protocol_ver();
  test_message_types();
  test_pack_unpack_header();
  test_payload_wire_sizes();
  test_deserialize_store();
  return test_report();
}
