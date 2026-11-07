#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "common/network.h"
#include "utils/test_utils.h"

static long elapsed_ms(const struct timespec *a, const struct timespec *b) {
  return (long)(b->tv_sec - a->tv_sec) * 1000L + (b->tv_nsec - a->tv_nsec) / 1000000L;
}

/* Escuta em porta efêmera de 127.0.0.1. Devolve o fd e a porta. */
static int listen_ephemeral(uint16_t *port) {
  struct sockaddr_in addr;
  socklen_t len = sizeof addr;
  int fd = socket(AF_INET, SOCK_STREAM, 0);

  addr.sin_family = AF_INET;
  addr.sin_port = 0;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, 8) < 0 ||
      getsockname(fd, (struct sockaddr *)&addr, &len) < 0) {
    return -1;
  }
  *port = ntohs(addr.sin_port);
  return fd;
}

/**
 * @brief connect com prazo conecta num destino vivo e devolve socket bloqueante.
 */
static void test_connect_timeout_ok(void) {
  uint16_t port = 0;
  int lfd = listen_ephemeral(&port);
  int fd;
  int peer;
  uint8_t byte = 7;
  uint8_t got = 0;

  expect(lfd >= 0, "escuta");
  fd = net_connect_timeout("127.0.0.1", port, 1000);
  expect(fd >= 0, "conecta com prazo");
  peer = accept(lfd, NULL, NULL);
  expect(peer >= 0, "aceita");
  expect(send_all(fd, &byte, 1) == NET_OK, "envia em socket que voltou ao modo bloqueante");
  expect(recv_all(peer, &got, 1) == NET_OK && got == 7, "o peer recebe o byte");
  close(fd);
  close(peer);
  close(lfd);
}

/**
 * @brief Porta sem ninguém ouvindo falha de imediato, sem esperar o prazo.
 */
static void test_connect_refused(void) {
  uint16_t port = 0;
  int lfd = listen_ephemeral(&port);
  struct timespec t0;
  struct timespec t1;
  int fd;

  expect(lfd >= 0, "escuta");
  close(lfd);
  clock_gettime(CLOCK_MONOTONIC, &t0);
  fd = net_connect_timeout("127.0.0.1", port, 3000);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  expect(fd < 0, "porta fechada recusa");
  expect(elapsed_ms(&t0, &t1) < 1000, "recusa nao espera o prazo");
}

/**
 * @brief Peer que aceita e nunca responde: recv_all devolve NET_ERROR perto do prazo.
 */
static void test_recv_timeout(void) {
  uint16_t port = 0;
  int lfd = listen_ephemeral(&port);
  struct timespec t0;
  struct timespec t1;
  uint8_t buf[4];
  int fd;
  int peer;
  int rc;
  long ms;

  expect(lfd >= 0, "escuta");
  fd = net_connect_timeout("127.0.0.1", port, 1000);
  peer = accept(lfd, NULL, NULL);
  expect(fd >= 0 && peer >= 0, "conecta e aceita");
  expect(net_set_timeout(fd, 300) == 0, "arma o prazo");
  clock_gettime(CLOCK_MONOTONIC, &t0);
  rc = recv_all(fd, buf, sizeof buf);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  ms = elapsed_ms(&t0, &t1);
  expect(rc == NET_ERROR, "silencio do peer vira NET_ERROR");
  expect(ms >= 250 && ms < 1500, "volta perto do prazo de 300 ms");
  expect(net_set_timeout(fd, -1) == -1, "prazo negativo e recusado");
  close(fd);
  close(peer);
  close(lfd);
}

/**
 * @brief Prazo não atrapalha quem responde a tempo, e EOF continua sendo NET_CLOSED.
 */
static void test_timeout_keeps_normal_io(void) {
  uint16_t port = 0;
  int lfd = listen_ephemeral(&port);
  uint8_t byte = 9;
  uint8_t got = 0;
  int fd;
  int peer;

  expect(lfd >= 0, "escuta");
  fd = net_connect_timeout("127.0.0.1", port, 1000);
  peer = accept(lfd, NULL, NULL);
  expect(net_set_timeout(fd, 500) == 0, "arma o prazo");
  expect(send_all(peer, &byte, 1) == NET_OK, "peer responde");
  expect(recv_all(fd, &got, 1) == NET_OK && got == 9, "resposta dentro do prazo chega");
  close(peer);
  expect(recv_all(fd, &got, 1) == NET_CLOSED, "peer que fecha continua sendo NET_CLOSED");
  close(fd);
  close(lfd);
}

int main(void) {
  test_connect_timeout_ok();
  test_connect_refused();
  test_recv_timeout();
  test_timeout_keeps_normal_io();
  return test_report();
}
