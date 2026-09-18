#include "common/node.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

int sha256(const uint8_t *in, const size_t len, uint8_t out[NODE_ID_SIZE]) {
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();

  if (!ctx) {
    return 0;
  }

  if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1 || EVP_DigestUpdate(ctx, in, len) != 1 ||
      EVP_DigestFinal(ctx, out, NULL) != 1) {
    EVP_MD_CTX_free(ctx);
    return 0;
  }

  EVP_MD_CTX_free(ctx);
  return 1;
}

int node_uuid_random(node_uuid_t *uuid) {
  int fd;
  uint8_t *p;
  size_t left;

  if (!uuid) {
    return 0;
  }

  fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) {
    return 0;
  }

  p = uuid->bytes;
  left = NODE_UUID_SIZE;
  while (left > 0) {
    ssize_t n = read(fd, p, left);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      close(fd);
      return 0;
    }
    if (n == 0) {
      close(fd);
      return 0;
    }
    p += n;
    left -= n;
  }

  close(fd);
  return 1;
}

int node_id_generate(const uint32_t ipv4, const uint16_t port, const node_uuid_t *uuid, node_id_t *out) {
  uint8_t buffer[4 + 2 + NODE_UUID_SIZE];
  uint16_t port_be;

  if (!uuid || !out) {
    return 0;
  }

  port_be = htons(port);

  memcpy(buffer, &ipv4, 4);
  memcpy(buffer + 4, &port_be, 2);
  memcpy(buffer + 6, uuid, NODE_UUID_SIZE);

  return sha256(buffer, sizeof(buffer), out->bytes);
}

int node_id_cmp(const node_id_t *a, const node_id_t *b) {
  if (!a || !b) {
    if (a == b) {
      return 0;
    }
    return a ? 1 : -1;
  }
  return memcmp(a->bytes, b->bytes, NODE_ID_SIZE);
}

int node_id_to_hex(const node_id_t *id, char *out, size_t out_len) {
  static const char hex[] = "0123456789abcdef";
  size_t i;

  if (!id || !out || out_len < NODE_ID_HEX_SIZE) {
    return 0;
  }

  for (i = 0; i < NODE_ID_SIZE; i++) {
    out[i * 2] = hex[id->bytes[i] >> 4];
    out[i * 2 + 1] = hex[id->bytes[i] & 0x0f];
  }
  out[NODE_ID_SIZE * 2] = '\0';
  return 1;
}
