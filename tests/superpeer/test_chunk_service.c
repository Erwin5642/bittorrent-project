#include <netinet/in.h>

#include "common/network.h"
#include "common/protocol.h"
#include "peer/storage.h"
#include "superpeer/superpeer.h"
#include "utils/test_utils.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/**
 * @file test_chunk_service.c
 * @brief Super Peer servindo LOOKUP e chunks: ausente, gravacao e leitura.
 */

#define TEST_PORT 56221
#define TEST_CONF "/tmp/bt_chunk_service.conf"

static node_id_t g_self;

static int start_superpeer(superpeer_t *sp) {
  FILE *fp = fopen(TEST_CONF, "w");

  if (!fp)
    return 0;
  fprintf(fp, "ip=127.0.0.1\nport=%d\ntype=superpeer\nbootstrap=\n", TEST_PORT);
  fclose(fp);
  return superpeer_init(sp, TEST_CONF, TEST_PORT, "chunk-test");
}

static void *run_superpeer(void *arg) {
  superpeer_run(arg);
  return NULL;
}

static int exchange(uint16_t type, const uint8_t *payload, uint32_t len,
                    uint8_t *reply, uint32_t reply_cap, msg_t *out) {
  uint8_t *frame = malloc((size_t)HEADER_SIZE + len);
  pl_header hdr;
  int fd;
  int ok = 0;

  if (!frame)
    return 0;
  fd = net_connect("127.0.0.1", TEST_PORT);
  if (fd < 0) {
    free(frame);
    return 0;
  }
  memset(&hdr, 0, sizeof hdr);
  hdr.protocol_ver = PROTOCOL_VER;
  hdr.msg_type = type;
  hdr.time = (uint64_t)time(NULL);
  hdr.pl_size = len;
  memcpy(hdr.src_node, g_self.bytes, NODE_ID_SIZE);
  if (simple_send(fd, frame, payload, len, &hdr) == NET_OK) {
    *out = simple_recv(fd, reply, reply_cap);
    ok = 1;
  }
  net_close(fd);
  free(frame);
  return ok;
}

static int store_meta(const file_metadata_t *meta) {
  uint8_t payload[METADATA_PAYLOAD_MAX];
  uint8_t reply[MAX_CONTROL_PAYLOAD_SZ];
  msg_t r;
  ssize_t n = metadata_pack(meta, payload, sizeof payload);

  if (n < 0)
    return 0;
  if (!exchange(STORE, payload, (uint32_t)n, reply, sizeof reply, &r))
    return 0;
  return r.status == NET_OK && r.header.msg_type == ACK;
}

static void cleanup_object(const uint8_t object_id[NODE_ID_SIZE]) {
  char path[STORAGE_PATH_MAX];
  char dir[STORAGE_PATH_MAX];

  if (storage_chunk_path(path, sizeof path, "data/storage", object_id, 0))
    unlink(path);
  if (storage_object_dir(dir, sizeof dir, "data/storage", object_id))
    rmdir(dir);
}

static void test_service(void) {
  superpeer_t sp;
  pthread_t th;
  file_metadata_t meta;
  uint8_t lookup[METADATA_FILENAME_MAX];
  uint8_t req[DOWNLOAD_REQ_WIRE_SIZE];
  uint8_t chunk[8] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
  uint8_t rep[CHUNK_WIRE_PREFIX + sizeof chunk];
  uint8_t *reply = NULL;
  msg_t r;
  ssize_t n;
  uint8_t object_id[METADATA_OBJECT_ID_SIZE];
  uint32_t index = 0;
  uint32_t plain = 0;
  uint32_t comp_len = 0;
  const uint8_t *comp = NULL;

  memset(g_self.bytes, 0x42, sizeof g_self.bytes);
  reply = malloc(MAX_DATA_PAYLOAD_SZ);
  expect(reply != NULL, "buffer de resposta");
  if (!reply)
    return;
  expect(start_superpeer(&sp) == 1, "superpeer sobe");
  expect(pthread_create(&th, NULL, run_superpeer, &sp) == 0, "thread de accept");

  n = lookup_pack("nao-existe.txt", lookup, sizeof lookup);
  expect(n > 0 && exchange(LOOKUP, lookup, (uint32_t)n, reply, MAX_DATA_PAYLOAD_SZ, &r),
         "LOOKUP enviado");
  expect(r.status == NET_OK && r.header.msg_type == ERROR, "LOOKUP de nome ausente -> ERROR");

  metadata_init(&meta);
  memset(meta.object_id, 0x11, sizeof meta.object_id);
  snprintf(meta.filename, sizeof meta.filename, "fixture.txt");
  meta.size = sizeof chunk;
  meta.chunk_count = 1;
  meta.version = 1;
  memcpy(meta.owner.bytes, g_self.bytes, NODE_ID_SIZE);
  meta.chunk_hashes = malloc(METADATA_CHUNK_HASH_SIZE);
  expect(meta.chunk_hashes != NULL, "hash do chunk");
  if (meta.chunk_hashes)
    memset(meta.chunk_hashes, 0x22, METADATA_CHUNK_HASH_SIZE);
  expect(store_meta(&meta) == 1, "STORE aceito");

  expect(download_req_pack(meta.object_id, 0, req, sizeof req) > 0, "DOWNLOAD_REQ pack");
  expect(exchange(DOWNLOAD_REQ, req, DOWNLOAD_REQ_WIRE_SIZE, reply, MAX_DATA_PAYLOAD_SZ, &r),
         "DOWNLOAD_REQ de chunk ausente enviado");
  expect(r.status == NET_OK && r.header.msg_type == ERROR, "chunk ausente -> ERROR");

  n = download_rep_pack(meta.object_id, 0, (uint32_t)sizeof chunk, chunk, (uint32_t)sizeof chunk,
                        rep, sizeof rep);
  expect(n > 0 && exchange(DOWNLOAD_REP, rep, (uint32_t)n, reply, MAX_DATA_PAYLOAD_SZ, &r),
         "DOWNLOAD_REP enviado");
  expect(r.status == NET_OK && r.header.msg_type == ACK, "chunk gravado -> ACK");

  if (exchange(DOWNLOAD_REQ, req, DOWNLOAD_REQ_WIRE_SIZE, reply, MAX_DATA_PAYLOAD_SZ, &r)) {
    expect(r.status == NET_OK && r.header.msg_type == DOWNLOAD_REP, "DOWNLOAD_REQ devolve o chunk");
    expect(download_rep_unpack(object_id, &index, &plain, &comp, &comp_len, reply,
                               r.header.pl_size) == 1,
           "resposta desempacotada");
    expect(index == 0 && plain == sizeof chunk && comp_len == sizeof chunk, "tamanhos da resposta");
    expect(comp && memcmp(comp, chunk, sizeof chunk) == 0, "bytes do chunk");
  } else {
    expect(0, "DOWNLOAD_REQ apos a gravacao");
  }

  expect(download_req_pack(meta.object_id, 9, req, sizeof req) > 0, "indice fora da faixa");
  expect(exchange(DOWNLOAD_REQ, req, DOWNLOAD_REQ_WIRE_SIZE, reply, MAX_DATA_PAYLOAD_SZ, &r),
         "DOWNLOAD_REQ indice 9 enviado");
  expect(r.status == NET_OK && r.header.msg_type == ERROR, "indice inexistente -> ERROR");

  n = lookup_pack("fixture.txt", lookup, sizeof lookup);
  expect(n > 0 && exchange(LOOKUP, lookup, (uint32_t)n, reply, MAX_DATA_PAYLOAD_SZ, &r),
         "LOOKUP do nome publicado");
  expect(r.status == NET_OK && r.header.msg_type == STORE, "LOOKUP responde STORE");

  cleanup_object(meta.object_id);
  metadata_release(&meta);
  free(reply);
  unlink(TEST_CONF);
  (void)th;
}

int main(void) {
  test_service();
  return test_report();
}
