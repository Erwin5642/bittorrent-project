#ifndef DOWNLOAD_H
#define DOWNLOAD_H

#include "../common/node.h"

#include <stdint.h>

/**
 * @file download.h
 * @brief Pipeline de download do CP2: remonta e verifica um arquivo do storage.
 *
 * Resolve `nome → metadado` pelo índice local (o mesmo papel do `LOOKUP`, já
 * que upload e download rodam na mesma máquina), lê os chunks do storage,
 * descomprime (LZ4), valida o SHA-256 de cada chunk e do arquivo remontado
 * contra o `ObjectID`, e grava a saída idêntica ao original.
 */

/**
 * @brief Baixa (remonta) um arquivo publicado, verificando integridade.
 * @param name Nome lógico do arquivo (como no upload).
 * @param output Caminho do arquivo de saída a gravar.
 * @param storage_root Diretório raiz do storage local.
 * @return 1 em sucesso, 0 se o metadado/chunk faltar, a verificação SHA-256
 *         falhar ou houver erro de I/O.
 */
int download_file(const char *name, const char *output, const char *storage_root);

/**
 * @brief Baixa um arquivo do Super Peer: LOOKUP e DOWNLOAD_REQ em paralelo.
 *
 * O nome vira um LOOKUP. Cada chunk pede um DOWNLOAD_REP, descomprime, confere
 * o SHA-256 contra o metadado e o SHA-256 do arquivo remontado contra o ObjectID.
 * @param host IPv4 do Super Peer.
 * @param port Porta TCP do Super Peer.
 * @param self NodeID de quem pede (vira @c src_node).
 * @param name Nome lógico do arquivo.
 * @param output Caminho do arquivo de saída.
 * @return 1 em sucesso, 0 se o LOOKUP falhar, um chunk faltar ou o SHA-256 não bater.
 */
int download_from_superpeer(const char *host, uint16_t port, const node_id_t *self,
                            const char *name, const char *output);

#endif
