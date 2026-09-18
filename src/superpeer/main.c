#include "../../include/superpeer/superpeer.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s --config PATH [--port N] [--name NAME]\n"
          "       %s PATH.conf\n",
          argv0, argv0);
}

int main(int argc, char **argv) {
  superpeer_t sp;
  const char *conf_path = NULL;
  const char *name = NULL;
  uint16_t port = 0;
  int opt;
  static const struct option long_opts[] = {
      {"config", required_argument, NULL, 'c'},
      {"port", required_argument, NULL, 'p'},
      {"name", required_argument, NULL, 'n'},
      {NULL, 0, NULL, 0},
  };

  setvbuf(stdout, NULL, _IONBF, 0);

  if (argc == 2 && argv[1][0] != '-') {
    conf_path = argv[1];
  } else {
    while ((opt = getopt_long(argc, argv, "c:p:n:", long_opts, NULL)) != -1) {
      char *end = NULL;
      unsigned long value;

      switch (opt) {
      case 'c':
        conf_path = optarg;
        break;
      case 'p':
        value = strtoul(optarg, &end, 10);
        if (!optarg[0] || !end || *end != '\0' || value == 0 || value > 65535UL) {
          usage(argv[0]);
          return 1;
        }
        port = (uint16_t)value;
        break;
      case 'n':
        name = optarg;
        break;
      default:
        usage(argv[0]);
        return 1;
      }
    }
    if (!conf_path && optind < argc) {
      conf_path = argv[optind];
    }
  }

  if (!conf_path) {
    usage(argv[0]);
    return 1;
  }
  if (!superpeer_init(&sp, conf_path, port, name)) {
    return 1;
  }
  member_table_print(&sp.members);
  return superpeer_run(&sp) ? 0 : 1;
}
