#include "../../include/superpeer/superpeer.h"

#include <stdio.h>

int main(int argc, char **argv) {
  superpeer_t sp;

  if (argc != 2) {
    fprintf(stderr, "usage: %s <config.conf>\n", argv[0]);
    return 1;
  }
  if (!superpeer_init(&sp, argv[1])) {
    return 1;
  }
  member_table_print(&sp.members);
  return superpeer_run(&sp) ? 0 : 1;
}
