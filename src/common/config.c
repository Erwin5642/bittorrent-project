#define _POSIX_C_SOURCE 200809L

#include "common/config.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *s) {
  char *end;

  while (*s && isspace((unsigned char)*s)) {
    s++;
  }
  if (*s == '\0') {
    return s;
  }

  end = s + strlen(s) - 1;
  while (end > s && isspace((unsigned char)*end)) {
    end--;
  }
  end[1] = '\0';
  return s;
}

static int parse_port(const char *s, uint16_t *out) {
  char *end;
  unsigned long v;

  if (!s || !out || *s == '\0') {
    return 0;
  }

  errno = 0;
  v = strtoul(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0' || v < 1 || v > 65535) {
    return 0;
  }
  *out = (uint16_t)v;
  return 1;
}

static int parse_ipv4(const char *s, uint32_t *out) {
  if (!s || !out) {
    return 0;
  }
  return inet_pton(AF_INET, s, out) == 1;
}

static int parse_endpoint(const char *s, node_endpoint_t *out) {
  const char *colon;
  char ip[INET_ADDRSTRLEN];
  size_t ip_len;

  colon = strrchr(s, ':');
  if (!colon || colon == s) {
    return 0;
  }

  ip_len = (size_t)(colon - s);
  if (ip_len == 0 || ip_len >= sizeof ip) {
    return 0;
  }

  memcpy(ip, s, ip_len);
  ip[ip_len] = '\0';
  if (!parse_ipv4(ip, &out->ipv4)) {
    return 0;
  }
  return parse_port(colon + 1, &out->port);
}

static int parse_bootstrap_list(const char *value, node_config_t *out) {
  char buf[CONFIG_LINE_MAX];
  char *save;
  char *tok;

  out->bootstrap_count = 0;
  if (value[0] == '\0') {
    return 1;
  }
  if (strlen(value) >= sizeof buf) {
    return 0;
  }

  memcpy(buf, value, strlen(value) + 1);
  tok = strtok_r(buf, ",", &save);
  while (tok) {
    tok = trim(tok);
    if (*tok != '\0') {
      if (out->bootstrap_count >= CONFIG_BOOTSTRAP_MAX) {
        return 0;
      }
      if (!parse_endpoint(tok, &out->bootstrap[out->bootstrap_count])) {
        return 0;
      }
      out->bootstrap_count++;
    }
    tok = strtok_r(NULL, ",", &save);
  }
  return 1;
}

static int parse_type(const char *s, node_type_t *out) {
  if (strcmp(s, "peer") == 0) {
    *out = PEER;
    return 1;
  }
  if (strcmp(s, "superpeer") == 0) {
    *out = SUPERPEER;
    return 1;
  }
  return 0;
}

int node_config_load(const char *path, node_config_t *out) {
  FILE *fp;
  char line[CONFIG_LINE_MAX];
  int seen_ip = 0;
  int seen_port = 0;
  int seen_type = 0;
  int seen_bootstrap = 0;

  if (!path || !out) {
    return 0;
  }

  memset(out, 0, sizeof *out);

  fp = fopen(path, "r");
  if (!fp) {
    return 0;
  }

  while (fgets(line, sizeof line, fp)) {
    char *eq;
    char *key;
    char *value;
    size_t n = strlen(line);

    if (n == sizeof(line) - 1 && line[n - 1] != '\n') {
      fclose(fp);
      return 0;
    }

    if (n > 0 && line[n - 1] == '\n') {
      line[n - 1] = '\0';
      n--;
    }
    if (n > 0 && line[n - 1] == '\r') {
      line[n - 1] = '\0';
    }

    key = trim(line);
    if (*key == '\0' || *key == '#') {
      continue;
    }

    eq = strchr(key, '=');
    if (!eq) {
      fclose(fp);
      return 0;
    }
    *eq = '\0';
    key = trim(key);
    value = trim(eq + 1);

    if (strcmp(key, "ip") == 0) {
      if (seen_ip || !parse_ipv4(value, &out->ipv4)) {
        fclose(fp);
        return 0;
      }
      seen_ip = 1;
    } else if (strcmp(key, "port") == 0) {
      if (seen_port || !parse_port(value, &out->port)) {
        fclose(fp);
        return 0;
      }
      seen_port = 1;
    } else if (strcmp(key, "type") == 0) {
      if (seen_type || !parse_type(value, &out->node_type)) {
        fclose(fp);
        return 0;
      }
      seen_type = 1;
    } else if (strcmp(key, "bootstrap") == 0) {
      if (seen_bootstrap || !parse_bootstrap_list(value, out)) {
        fclose(fp);
        return 0;
      }
      seen_bootstrap = 1;
    } else {
      fclose(fp);
      return 0;
    }
  }

  fclose(fp);
  return seen_ip && seen_port && seen_type;
}

static int parse_priority(const char *s) {
  char *end;
  unsigned long value;

  if (!s || *s == '\0') {
    return 0;
  }
  errno = 0;
  value = strtoul(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0' || value > 4294967295UL) {
    return 0;
  }
  return 1;
}

static int parse_roster_id(const char *s) {
  size_t i;

  if (!s || *s == '\0') {
    return 0;
  }
  for (i = 0; s[i] != '\0'; i++) {
    if (!isxdigit((unsigned char)s[i])) {
      return 0;
    }
  }
  return 1;
}

/* Separa exatamente 6 campos. Campo a mais ou a menos falha. */
static int split_roster_fields(char *line, char *fields[6]) {
  int n;
  char *cursor;

  cursor = line;
  for (n = 0; n < 5; n++) {
    char *comma = strchr(cursor, ',');

    if (!comma) {
      return 0;
    }
    *comma = '\0';
    fields[n] = trim(cursor);
    cursor = comma + 1;
  }
  if (strchr(cursor, ',') != NULL) {
    return 0;
  }
  fields[5] = trim(cursor);
  return 1;
}

int node_roster_load(const char *path, uint16_t self_port, node_config_t *out, char *name,
                     size_t name_cap) {
  FILE *fp;
  char line[CONFIG_LINE_MAX];
  char chosen_name[CONFIG_LINE_MAX];
  node_endpoint_t first_superpeer;
  int have_first = 0;
  int found_self = 0;

  if (!path || !out || !name || name_cap < 2 || self_port == 0) {
    return 0;
  }

  memset(out, 0, sizeof *out);
  memset(&first_superpeer, 0, sizeof first_superpeer);
  chosen_name[0] = '\0';

  fp = fopen(path, "r");
  if (!fp) {
    return 0;
  }

  while (fgets(line, sizeof line, fp)) {
    char *fields[6];
    char *text;
    node_type_t type;
    uint32_t ipv4;
    uint16_t port;
    size_t n = strlen(line);

    if (n == sizeof(line) - 1 && line[n - 1] != '\n') {
      fclose(fp);
      return 0;
    }
    if (n > 0 && line[n - 1] == '\n') {
      line[n - 1] = '\0';
      n--;
    }
    if (n > 0 && line[n - 1] == '\r') {
      line[n - 1] = '\0';
    }

    text = trim(line);
    if (*text == '\0' || *text == '#') {
      continue;
    }
    if (!split_roster_fields(text, fields)) {
      fclose(fp);
      return 0;
    }
    if (fields[0][0] == '\0' || !parse_type(fields[1], &type) || !parse_ipv4(fields[2], &ipv4) ||
        !parse_port(fields[3], &port) || !parse_roster_id(fields[4]) || !parse_priority(fields[5])) {
      fclose(fp);
      return 0;
    }

    if (type == SUPERPEER && !have_first) {
      first_superpeer.ipv4 = ipv4;
      first_superpeer.port = port;
      have_first = 1;
    }
    if (port != self_port) {
      continue;
    }
    if (found_self) {
      fclose(fp);
      return 0;
    }
    if (strlen(fields[0]) >= name_cap) {
      fclose(fp);
      return 0;
    }
    memcpy(chosen_name, fields[0], strlen(fields[0]) + 1);
    out->ipv4 = ipv4;
    out->port = port;
    out->node_type = type;
    found_self = 1;
  }

  fclose(fp);
  if (!found_self) {
    return 0;
  }
  if (have_first && first_superpeer.port != self_port) {
    out->bootstrap[0] = first_superpeer;
    out->bootstrap_count = 1;
  }
  memcpy(name, chosen_name, strlen(chosen_name) + 1);
  return 1;
}
