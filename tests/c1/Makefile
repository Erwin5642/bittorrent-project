CC = gcc
CFLAGS = -Wall -Wextra -pedantic -I../../include -I../../tests
LDFLAGS = -lz -lcrypto
ROOT = ../..

COMMON = $(ROOT)/src/common
TEST_UTILS = $(ROOT)/tests/utils/test_utils.c

.PHONY: all clean

all: test_protocol

test_protocol: test_protocol.c $(COMMON)/protocol.c $(COMMON)/network.c $(COMMON)/node.c $(TEST_UTILS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f test_protocol
