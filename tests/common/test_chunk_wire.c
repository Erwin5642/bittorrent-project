#include <netinet/in.h>

#include "common/network.h"
#include "common/protocol.h"
#include "utils/test_utils.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

typedef struct {
  int fd;
  uint8_t *buf;
  size_t cap;
  msg_t msg;
} recv_job_t;

/* O socketpair enche antes de 4 MiB; o recv tem de correr junto com o send. */
static void *recv_job(void *arg) {
  recv_job_t *job = arg;
  job->msg = recv_message(job->fd, job->buf, job->cap, NULL, 0);
  return NULL;
}

/**
 * @file test_chunk_wire.c
 * @brief Layout de LOOKUP/DOWNLOAD_* e framing de payload acima do teto de controle.
 */

static void test_lookup_round_trip(void) {
  uint8_t buf[METADATA_FILENAME_MAX];
  char name[METADATA_FILENAME_MAX];
  ssize_t n;

  n = lookup_pack("small.txt", buf, sizeof buf);
  expect(n == (ssize_t)METADATA_FILENAME_MAX, "LOOKUP ocupa o campo fixo");
  expect(lookup_unpack(name, sizeof name, buf, (size_t)n) == 1, "LOOKUP unpack");
  expect(strcmp(name, "small.txt") == 0, "nome preservado");
  expect(lookup_pack("", buf, sizeof buf) < 0, "nome vazio rejeitado");
  expect(lookup_unpack(name, sizeof name, buf, 10) == 0, "tamanho errado rejeitado");
}

static void test_download_req_round_trip(void) {
  uint8_t id[METADATA_OBJECT_ID_SIZE];
  uint8_t buf[DOWNLOAD_REQ_WIRE_SIZE];
  uint8_t back[METADATA_OBJECT_ID_SIZE];
  uint32_t index = 0;
  ssize_t n;

  memset(id, 0x5a, sizeof id);
  n = download_req_pack(id, 7, buf, sizeof buf);
  expect(n == (ssize_t)DOWNLOAD_REQ_WIRE_SIZE, "DOWNLOAD_REQ tem 36 bytes");
  expect(download_req_unpack(back, &index, buf, (size_t)n) == 1, "DOWNLOAD_REQ unpack");
  expect(index == 7, "indice preservado");
  expect(memcmp(id, back, sizeof id) == 0, "ObjectID preservado");
  memset(id, 0, sizeof id);
  expect(download_req_pack(id, 0, buf, sizeof buf) < 0, "ObjectID zero rejeitado");
}

/* Payload de 4 MiB no mesmo socket, e a mensagem seguinte ainda e lida inteira. */
static void test_large_frame_then_ping(void) {
  int sv[2] = {-1, -1};
  uint8_t id[METADATA_OBJECT_ID_SIZE];
  uint8_t *comp = NULL;
  uint8_t *wire = NULL;
  uint8_t *frame = NULL;
  uint8_t *got = NULL;
  uint8_t ping_buf[HEADER_SIZE];
  pl_header hdr;
  pl_header ping;
  msg_t r;
  size_t wire_len;
  uint8_t back_id[METADATA_OBJECT_ID_SIZE];
  uint32_t index = 0;
  uint32_t plain = 0;
  uint32_t comp_len = 0;
  const uint8_t *comp_back = NULL;

  expect(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
  memset(id, 0x11, sizeof id);
  comp = malloc(CHUNK_PLAIN_MAX);
  expect(comp != NULL, "aloca chunk de 4 MiB");
  if (!comp)
    goto done;
  memset(comp, 0xa5, CHUNK_PLAIN_MAX);

  wire_len = download_rep_wire_size(CHUNK_PLAIN_MAX);
  expect(wire_len > MAX_CONTROL_PAYLOAD_SZ, "passa do teto de controle");
  expect(wire_len <= MAX_DATA_PAYLOAD_SZ, "cabe no teto de dados");
  wire = malloc(wire_len);
  frame = malloc((size_t)HEADER_SIZE + wire_len);
  got = malloc(MAX_DATA_PAYLOAD_SZ);
  expect(wire && frame && got, "buffers do framing");
  if (!wire || !frame || !got)
    goto done;

  expect(download_rep_pack(id, 2, CHUNK_PLAIN_MAX, comp, CHUNK_PLAIN_MAX, wire, wire_len) ==
             (ssize_t)wire_len,
         "DOWNLOAD_REP pack no limite");

  memset(&hdr, 0, sizeof hdr);
  hdr.protocol_ver = PROTOCOL_VER;
  hdr.msg_type = DOWNLOAD_REP;
  hdr.pl_size = (uint32_t)wire_len;
  memcpy(hdr.src_node, id, sizeof id);

  {
    recv_job_t job = {sv[1], got, MAX_DATA_PAYLOAD_SZ, {{0}, NULL, 0}};
    pthread_t th;

    expect(pthread_create(&th, NULL, recv_job, &job) == 0, "thread de recv");
    expect(simple_send(sv[0], frame, wire, (uint32_t)wire_len, &hdr) == NET_OK, "envia frame grande");
    pthread_join(th, NULL);
    r = job.msg;
  }
  expect(r.status == NET_OK, "recv do frame grande");
  expect(r.header.msg_type == DOWNLOAD_REP, "tipo DOWNLOAD_REP");
  expect(r.header.pl_size == wire_len, "pl_size do frame grande");
  expect(memcmp(got, wire, wire_len) == 0, "bytes do frame grande");
  expect(download_rep_unpack(back_id, &index, &plain, &comp_back, &comp_len, got, wire_len) == 1,
         "unpack do frame grande");
  expect(index == 2 && plain == CHUNK_PLAIN_MAX && comp_len == CHUNK_PLAIN_MAX, "campos do prefixo");
  expect(comp_back && memcmp(comp_back, comp, 64) == 0, "inicio do bloco comprimido");

  memset(&ping, 0, sizeof ping);
  ping.protocol_ver = PROTOCOL_VER;
  ping.msg_type = PING;
  ping.pl_size = 0;
  expect(send_message(sv[0], ping_buf, sizeof ping_buf, NULL, &ping) == NET_OK, "PING depois do frame");
  r = recv_message(sv[1], got, MAX_DATA_PAYLOAD_SZ, NULL, 0);
  expect(r.status == NET_OK && r.header.msg_type == PING, "segunda mensagem nao dessincroniza");

  hdr.pl_size = MAX_DATA_PAYLOAD_SZ + 1;
  expect(simple_send(sv[0], frame, wire, hdr.pl_size, &hdr) == NET_ERROR, "acima do teto de dados");

done:
  free(got);
  free(frame);
  free(wire);
  free(comp);
  if (sv[0] >= 0)
    net_close(sv[0]);
  if (sv[1] >= 0)
    net_close(sv[1]);
}

int main(void) {
  test_lookup_round_trip();
  test_download_req_round_trip();
  test_large_frame_then_ping();
  return test_report();
}
