#ifndef DOWNLOAD_H
#define DOWNLOAD_H

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

#endif
