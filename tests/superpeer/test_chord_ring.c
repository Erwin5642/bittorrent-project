#include <arpa/inet.h>
#include <netinet/in.h>

#include "common/network.h"
#include "common/protocol.h"
#include "superpeer/superpeer.h"
#include "utils/test_utils.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * @file test_chord_ring.c
 * @brief Dois Super Peers fecham o anel: cada um vira sucessor e predecessor do outro.
 */

#define PORT_A 56121
#define PORT_B 56122
#define CONF_A "/tmp/bt-chord-a.conf"
#define CONF_B "/tmp/bt-chord-b.conf"

static int write_conf(const char *path, uint16_t port, uint16_t bootstrap) {
  FILE *fp = fopen(path, "w");

  if (!fp) {
    return 0;
  }
  fprintf(fp, "ip=127.0.0.1\nport=%u\ntype=superpeer\n", (unsigned)port);
  if (bootstrap == 0) {
    fprintf(fp, "bootstrap=\n");
  } else {
    fprintf(fp, "bootstrap=127.0.0.1:%u\n", (unsigned)bootstrap);
  }
  fclose(fp);
  return 1;
}

static void *run_superpeer(void *arg) {
  superpeer_run(arg);
  return NULL;
}

static int circle_closed(superpeer_t *a, superpeer_t *b) {
  chord_node_t succ;
  chord_node_t pred;
  unsigned n = 0;

  if (!chord_copy_successors(&a->chord, &succ, 1, &n) || n != 1 || succ.port != PORT_B) {
    return 0;
  }
  if (!chord_copy_predecessor(&a->chord, &pred) || !pred.valid || pred.port != PORT_B) {
    return 0;
  }
  n = 0;
  if (!chord_copy_successors(&b->chord, &succ, 1, &n) || n != 1 || succ.port != PORT_A) {
    return 0;
  }
  if (!chord_copy_predecessor(&b->chord, &pred) || !pred.valid || pred.port != PORT_A) {
    return 0;
  }
  return 1;
}

static int knows(superpeer_t *sp, uint32_t ipv4, uint16_t port) {
  const member_t *found;
  int ok;

  pthread_mutex_lock(&sp->members_lock);
  found = member_table_find_addr(&sp->members, ipv4, port);
  ok = found != NULL && found->node_type == SUPERPEER && found->state == MEMBER_ALIVE;
  pthread_mutex_unlock(&sp->members_lock);
  return ok;
}

static void test_two_nodes_close_the_ring(void) {
  superpeer_t a;
  superpeer_t b;
  pthread_t thread_a;
  pthread_t thread_b;
  struct timespec start;
  struct timespec now;
  int closed = 0;

  expect(write_conf(CONF_A, PORT_A, 0) == 1, "config de A");
  expect(write_conf(CONF_B, PORT_B, PORT_A) == 1, "config de B");
  expect(superpeer_init(&a, CONF_A, PORT_A, "ring-a") == 1, "A escuta");
  expect(superpeer_init(&b, CONF_B, PORT_B, "ring-b") == 1, "B escuta");
  expect(pthread_create(&thread_a, NULL, run_superpeer, &a) == 0, "accept de A");
  expect(pthread_create(&thread_b, NULL, run_superpeer, &b) == 0, "accept de B");

  clock_gettime(CLOCK_MONOTONIC, &start);
  do {
    int64_t elapsed;

    if (circle_closed(&a, &b)) {
      closed = 1;
      break;
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    elapsed = (int64_t)(now.tv_sec - start.tv_sec) * 1000 +
              (int64_t)(now.tv_nsec - start.tv_nsec) / 1000000;
    if (elapsed >= 8000) {
      break;
    }
    nanosleep(&(struct timespec){.tv_nsec = 50L * 1000000L}, NULL);
  } while (1);

  expect(closed == 1, "A e B fecham o circulo");
  expect(knows(&a, b.cfg.ipv4, PORT_B) == 1, "A guarda B na membership");
  expect(knows(&b, a.cfg.ipv4, PORT_A) == 1, "B guarda A na membership");
  pthread_mutex_lock(&a.chord.lock);
  expect(a.chord.fingers[0].port == PORT_B, "finger 0 de A aponta para B");
  pthread_mutex_unlock(&a.chord.lock);

  {
    uint8_t frame[HEADER_SIZE + NODE_ID_SIZE];
    uint8_t reply[2u + CHORD_LOOKUP_PATH_MAX * CHORD_NODE_WIRE_SIZE];
    chord_peer_t path[CHORD_LOOKUP_PATH_MAX];
    pl_header hdr;
    msg_t msg;
    unsigned count = 0;
    int fd;

    fd = net_connect("127.0.0.1", PORT_A);
    expect(fd >= 0, "lookup conecta em A");
    if (fd >= 0) {
      memset(&hdr, 0, sizeof hdr);
      hdr.protocol_ver = PROTOCOL_VER;
      hdr.msg_type = FIND_SUCCESSOR;
      hdr.time = (uint64_t)time(NULL);
      hdr.pl_size = NODE_ID_SIZE;
      memcpy(hdr.src_node, a.self_id.bytes, NODE_ID_SIZE);
      expect(simple_send(fd, frame, b.self_id.bytes, NODE_ID_SIZE, &hdr) == NET_OK,
             "lookup enviado");
      msg = simple_recv(fd, reply, sizeof reply);
      net_close(fd);
      expect(msg.status == NET_OK && msg.header.msg_type == FIND_SUCCESSOR, "lookup respondido");
      expect(chord_lookup_path_unpack(path, CHORD_LOOKUP_PATH_MAX, &count, reply,
                                      msg.header.pl_size) == 1,
             "caminho do lookup");
      expect(count >= 2 && path[0].port == PORT_A && path[count - 1].port == PORT_B,
             "o dono do id de B e B");
    }
  }
}

int main(void) {
  int rc;

  test_two_nodes_close_the_ring();
  rc = test_report();
  fflush(stdout);
  fflush(stderr);
  /* As threads de accept e de manutenção continuam vivas. exit() as encontra no meio de um printf. */
  _Exit(rc);
}
