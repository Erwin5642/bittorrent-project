/*
 * session.c — quebra de linhas de comando do cliente em modo sessão.
 * Código puro (sem rede), para poder ser testado sozinho em tests/peer/test_session.c.
 */
#include "../../include/peer/session.h"

#include <string.h>

static int is_blank(char c) {
	return c == ' ' || c == '\t';
}

/* Devolve o número de tokens, 0 se a linha for vazia ou comentário, -1 em erro. */
int session_split(char *line, char **argv, size_t cap) {
	char *p;
	size_t n = 0;
	size_t len;

	if (!line || !argv || cap == 0)
		return -1;

	len = strlen(line);
	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
		line[--len] = '\0';

	p = line;
	for (;;) {
		char *out;

		while (is_blank(*p))
			p++;
		if (*p == '\0')
			break;
		if (n == 0 && *p == '#')
			return 0;
		if (n == cap)
			return -1;

		argv[n++] = p;
		out = p;
		if (*p == '"') {
			/* token entre aspas: vai até a aspa de fechamento */
			p++;
			argv[n - 1] = out;
			while (*p != '"') {
				if (*p == '\0')
					return -1;
				*out++ = *p++;
			}
			p++;
			if (*p != '\0' && !is_blank(*p))
				return -1; /* texto colado na aspa de fechamento */
			*out = '\0';
			if (*p != '\0')
				p++;
			continue;
		}
		while (*p != '\0' && !is_blank(*p))
			p++;
		if (*p != '\0')
			*p++ = '\0';
	}
	return (int)n;
}
