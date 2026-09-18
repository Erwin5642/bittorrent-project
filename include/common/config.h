#ifndef CONFIG_H
#define CONFIG_H

#include "node.h"

#include <stddef.h>
#include <stdint.h>

/**
 * @file config.h
 * @brief Parser de arquivos .conf (`chave=valor`) e configuração local do nó.
 */

/** Número máximo de Super Peers de bootstrap. */
#define CONFIG_BOOTSTRAP_MAX 8

/** Tamanho máximo de uma linha do .conf, incluindo o NUL. */
#define CONFIG_LINE_MAX 256

/**
 * @brief Endereço IP+porta de um Super Peer de bootstrap.
 */
typedef struct {
  uint32_t ipv4; /**< IPv4, network byte order. */
  uint16_t port; /**< Porta TCP, host byte order. */
} node_endpoint_t;

/**
 * @brief Configuração local lida do arquivo .conf.
 *
 * Chaves: `ip`, `port`, `type` (obrigatórias) e `bootstrap` (opcional).
 */
typedef struct {
  uint32_t ipv4; /**< IPv4 anunciado, network byte order. */
  uint16_t port; /**< Porta TCP, host byte order. */
  node_type_t node_type; /**< peer ou superpeer. */
  node_endpoint_t bootstrap[CONFIG_BOOTSTRAP_MAX]; /**< Super Peers iniciais. */
  int bootstrap_count; /**< Quantidade válida em @c bootstrap. */
} node_config_t;

/**
 * @brief Carrega um arquivo `chave=valor` para @p out.
 * @param path Caminho do .conf (ex.: `config/sp1.conf`).
 * @param out Destino; o caller aloca.
 * @return 1 em sucesso, 0 se o arquivo ou algum campo for inválido.
 * @note `ip` é o endereço anunciado (NodeID), não o bind de `net_listen`.
 *       `bootstrap` vazio ou ausente = primeiro Super Peer da overlay.
 */
int node_config_load(const char *path, node_config_t *out);

#endif
