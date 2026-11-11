#ifndef SESSION_H
#define SESSION_H

#include <stddef.h>

/**
 * @file session.h
 * @brief Quebra de linhas de comando do cliente em modo sessão (`--interactive` e `--batch`).
 */

/** Teto de tokens aceitos numa linha de comando da sessão. */
#define SESSION_ARGV_MAX 32

/** Tamanho máximo de uma linha de comando da sessão, incluindo o NUL. */
#define SESSION_LINE_MAX 2048

/**
 * @brief Divide @p line em tokens separados por espaço ou tab, sem alocar.
 *
 * Aspas duplas agrupam um token com espaços (`upload "meu arquivo.pdf"`); as
 * aspas não entram no token. Não há escape com barra invertida. O fim da linha
 * (`\n` ou `\r\n`) é descartado. Um token vazio entre aspas (`""`) é válido.
 *
 * @param line Linha editada no lugar: os separadores viram NUL e @p argv aponta para dentro dela.
 * @param argv Destino dos ponteiros; o caller aloca @p cap posições.
 * @param cap Capacidade de @p argv.
 * @return Quantidade de tokens, 0 para linha vazia ou só de comentário (`#`),
 *         ou -1 se houver aspa sem fechar ou mais de @p cap tokens.
 */
int session_split(char *line, char **argv, size_t cap);

#endif
