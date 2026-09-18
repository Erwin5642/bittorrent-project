#include "utils/test_utils.h"

#include <stdio.h>

static int g_failed;
static int g_ran;

void expect(int cond, const char *msg) {
  g_ran++;
  if (!cond) {
    fprintf(stderr, "FAIL: %s\n", msg);
    g_failed++;
  }
}

int test_report(void) {
  if (g_failed) {
    fprintf(stderr, "%d/%d testes falharam\n", g_failed, g_ran);
    return 1;
  }
  printf("%d testes ok\n", g_ran);
  return 0;
}
