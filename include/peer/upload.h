#ifndef UPLOAD_H
#define UPLOAD_H

#include "../common/node.h"
#include "../common/protocol.h"

/**
 * @file upload.h
 * @brief Pipeline de upload do CP2: fragmenta, comprime, armazena e monta o metadado.
 *
 * Junta as etapas do Aluno 1 (fragmentação, LZ4, storage) e produz o
 * @c file_metadata_t que o `peer` envia ao Super Peer via `STORE`. Não faz a
 * parte de rede — isso fica no `peer` (CLI), para manter o pipeline testável.
 */

/**
 * @brief Executa o pipeline local de upload de um arquivo.
 *
 * Lê o arquivo, calcula o `ObjectID` e os hashes de chunk (sobre os bytes
 * originais), comprime cada chunk com LZ4 e o grava no storage local, e
 * preenche @p out_meta (ObjectID, nome, tamanho, chunk_count, hashes, owner).
 * @param path Caminho do arquivo a publicar.
 * @param storage_root Diretório raiz do storage local.
 * @param owner NodeID de quem publica (vai para @c out_meta->owner).
 * @param out_meta Recebe o metadado; o caller libera com @c metadata_release.
 * @return 1 em sucesso, 0 em erro de I/O, compressão, storage ou alocação.
 */
int upload_prepare(const char *path, const char *storage_root,
                   const node_id_t *owner, file_metadata_t *out_meta);

/**
 * @brief Imprime o relatório de verificação do CP2 (File/Size/ObjectID/Chunks/...).
 * @param meta Metadado preenchido por @c upload_prepare.
 */
void upload_print_report(const file_metadata_t *meta);

/**
 * @brief Envia os chunks já comprimidos ao Super Peer, um DOWNLOAD_REP por chunk.
 *
 * Lê os blocos gravados por @c upload_prepare e os entrega em paralelo. Cada
 * worker abre a própria conexão. O STORE do metadado fica a cargo do chamador
 * e precisa ter sido aceito antes: o Super Peer recusa chunk sem registro.
 * @param host IPv4 do Super Peer.
 * @param port Porta TCP do Super Peer.
 * @param self NodeID de quem envia (vira @c src_node).
 * @param meta Metadado já aceito no STORE.
 * @param storage_root Diretório de onde ler os chunks comprimidos.
 * @return 1 se todos os chunks foram confirmados com ACK, 0 em falha de rede ou I/O.
 */
int upload_send_chunks(const char *host, uint16_t port, const node_id_t *self,
                       const file_metadata_t *meta, const char *storage_root);

#endif
