/*
 * rendezvous.c — servidor de rendezvous dos Super Peers.
 *
 * Guarda o endereço público (IP visto na conexão + porta de escuta informada) de cada
 * Super Peer que se registra e devolve os endereços de outros Super Peers para o novo
 * nó usar como bootstrap. Protocolo de texto sobre TCP: uma conexão = um comando.
 *
 *   REGISTER <porta> [ip]   -> OK <ip>:<porta> | PEER <ip>:<porta> (até MAX_REPLY) | END
 *   UNREGISTER <porta> [ip] -> OK
 *   LIST                    -> PEER <ip>:<porta> (todos) | END
 *   PING                    -> PONG
 *   erro                    -> ERR <motivo>
 *
 * REGISTER também renova o registro (TTL). O [ip] opcional substitui o IP visto na conexão
 * (útil atrás de NAT/proxy, quando o servidor enxerga um endereço que não serve para os pares).
 *
 * Uso: rendezvous [-p porta] [-t ttl_segundos] [-v]
 * A porta padrão é a variável de ambiente PORT (como nas plataformas de deploy) ou 7000.
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_ENTRIES 256
#define MAX_REPLY 3
#define LINE_MAX_LEN 128
#define IO_TIMEOUT_SEC 2

typedef struct {
    uint32_t ip;    /* network byte order */
    uint16_t port;  /* host byte order */
    time_t first;   /* primeiro registro (os mais antigos são os mais estáveis) */
    time_t seen;    /* última renovação */
    int used;
} entry_t;

static entry_t table[MAX_ENTRIES];
static int ttl = 60;
static int verbose;
static volatile sig_atomic_t stop;

static void on_signal(int sig) {
    (void)sig;
    stop = 1;
}

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    va_list ap;
    char ts[16];
    time_t now = time(NULL);

    strftime(ts, sizeof ts, "%H:%M:%S", localtime(&now));
    fprintf(stderr, "%s [rendezvous] ", ts);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void expire(time_t now) {
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (table[i].used && now - table[i].seen > ttl) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &table[i].ip, ip, sizeof ip);
            logf_("expirou %s:%u", ip, table[i].port);
            table[i].used = 0;
        }
    }
}

static entry_t *find(uint32_t ip, uint16_t port) {
    for (int i = 0; i < MAX_ENTRIES; i++)
        if (table[i].used && table[i].ip == ip && table[i].port == port)
            return &table[i];
    return NULL;
}

static int upsert(uint32_t ip, uint16_t port, time_t now) {
    entry_t *e = find(ip, port);

    if (!e) {
        for (int i = 0; i < MAX_ENTRIES && !e; i++)
            if (!table[i].used)
                e = &table[i];
        if (!e)
            return 0;
        e->used = 1;
        e->ip = ip;
        e->port = port;
        e->first = now;
    }
    e->seen = now;
    return 1;
}

static void put(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(int fd, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0)
        (void)!send(fd, buf, (size_t)n, MSG_NOSIGNAL);
}

static void put_peer(int fd, const entry_t *e) {
    char ip[INET_ADDRSTRLEN];

    inet_ntop(AF_INET, &e->ip, ip, sizeof ip);
    put(fd, "PEER %s:%u\n", ip, e->port);
}

static int parse_port(const char *s, uint16_t *out) {
    char *end;
    long v;

    if (!s || !*s)
        return 0;
    v = strtol(s, &end, 10);
    if (*end || v < 1 || v > 65535)
        return 0;
    *out = (uint16_t)v;
    return 1;
}

/* Lê uma linha (até \n) com o timeout do socket. Devolve 0 se falhar. */
static int read_line(int fd, char *buf, size_t cap) {
    size_t n = 0;

    while (n + 1 < cap) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);

        if (r <= 0)
            return 0;
        if (c == '\n')
            break;
        if (c != '\r')
            buf[n++] = c;
    }
    buf[n] = '\0';
    return n > 0;
}

static void handle(int fd, uint32_t peer_ip) {
    char line[LINE_MAX_LEN], *cmd, *a1, *a2, *save = NULL;
    time_t now = time(NULL);
    uint16_t port;
    uint32_t ip = peer_ip;
    char ipstr[INET_ADDRSTRLEN];

    if (!read_line(fd, line, sizeof line)) {
        put(fd, "ERR linha vazia ou muito longa\n");
        return;
    }
    expire(now);
    cmd = strtok_r(line, " \t", &save);
    a1 = strtok_r(NULL, " \t", &save);
    a2 = strtok_r(NULL, " \t", &save);

    if (strcmp(cmd, "PING") == 0) {
        put(fd, "PONG\n");
    } else if (strcmp(cmd, "LIST") == 0) {
        for (int i = 0; i < MAX_ENTRIES; i++)
            if (table[i].used)
                put_peer(fd, &table[i]);
        put(fd, "END\n");
    } else if (strcmp(cmd, "REGISTER") == 0 || strcmp(cmd, "UNREGISTER") == 0) {
        int is_reg = cmd[0] == 'R';

        if (!parse_port(a1, &port)) {
            put(fd, "ERR porta invalida\n");
            return;
        }
        if (a2 && inet_pton(AF_INET, a2, &ip) != 1) {
            put(fd, "ERR ip invalido\n");
            return;
        }
        inet_ntop(AF_INET, &ip, ipstr, sizeof ipstr);
        if (!is_reg) {
            entry_t *e = find(ip, port);

            if (e)
                e->used = 0;
            logf_("saiu %s:%u", ipstr, port);
            put(fd, "OK\n");
            return;
        }
        if (!upsert(ip, port, now)) {
            put(fd, "ERR tabela cheia\n");
            return;
        }
        if (verbose)
            logf_("registro %s:%u", ipstr, port);
        put(fd, "OK %s:%u\n", ipstr, port);
        /* Outros Super Peers, os mais antigos primeiro (mais estáveis). */
        entry_t *sent[MAX_REPLY] = {0};
        for (int k = 0; k < MAX_REPLY; k++) {
            entry_t *best = NULL;

            for (int i = 0; i < MAX_ENTRIES; i++) {
                entry_t *e = &table[i];
                int dup = 0;

                if (!e->used || (e->ip == ip && e->port == port))
                    continue;
                for (int j = 0; j < k; j++)
                    dup |= sent[j] == e;
                if (!dup && (!best || e->first < best->first))
                    best = e;
            }
            if (!best)
                break;
            sent[k] = best;
            put_peer(fd, best);
        }
        put(fd, "END\n");
    } else {
        put(fd, "ERR comando desconhecido\n");
    }
}

int main(int argc, char **argv) {
    int port = getenv("PORT") ? atoi(getenv("PORT")) : 7000, opt, srv, one = 1;
    struct sockaddr_in addr = {0};
    struct sigaction sa = {0};

    while ((opt = getopt(argc, argv, "p:t:vh")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 't': ttl = atoi(optarg); break;
        case 'v': verbose = 1; break;
        default:
            fprintf(stderr, "uso: %s [-p porta] [-t ttl_segundos] [-v]\n", argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }
    if (port < 1 || port > 65535 || ttl < 1) {
        fprintf(stderr, "porta ou ttl invalido\n");
        return 2;
    }
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    srv = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(srv, 64) < 0) {
        perror("bind/listen");
        return 1;
    }
    logf_("escutando em 0.0.0.0:%d (ttl %ds)", port, ttl);

    while (!stop) {
        struct sockaddr_in from;
        socklen_t len = sizeof from;
        struct timeval tv = {IO_TIMEOUT_SEC, 0};
        int fd = accept(srv, (struct sockaddr *)&from, &len);

        if (fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        handle(fd, from.sin_addr.s_addr);
        close(fd);
    }
    close(srv);
    logf_("encerrado");
    return 0;
}
