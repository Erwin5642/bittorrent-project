CC = gcc
CFLAGS = -Wall -Wextra -pedantic -Iinclude -Itests
LDFLAGS = -lcrypto

BIN_DIR = bin
OBJ_DIR = obj
SRC_DIR = src

SUPERPEER_SRC = $(SRC_DIR)/superpeer/superpeer.c

# Módulos compartilhados (objetos em obj/, fontes em src/common/)
COMMON_OBJS = \
	$(OBJ_DIR)/common/node.o \
	$(OBJ_DIR)/common/config.o \
	$(OBJ_DIR)/common/network.o \
	$(OBJ_DIR)/common/protocol.o
SUPERPEER_OBJ = $(OBJ_DIR)/superpeer/superpeer.o

# Executáveis a gerar
TARGETS = $(BIN_DIR)/superpeer
TEST_NODE = $(BIN_DIR)/test_node
TEST_CONFIG = $(BIN_DIR)/test_config
TEST_UTILS_OBJ = $(OBJ_DIR)/tests/utils/test_utils.o

# Alvo padrão: cria os diretórios e gera tudo
all: $(BIN_DIR) $(OBJ_DIR) $(TARGETS)

test: $(TEST_NODE) $(TEST_CONFIG)
	$(TEST_NODE)
	$(TEST_CONFIG)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# Ligação
$(BIN_DIR)/superpeer: $(SUPERPEER_OBJ) $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/test_node: tests/common/test_node.c $(OBJ_DIR)/common/node.o $(TEST_UTILS_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/test_config: tests/common/test_config.c $(OBJ_DIR)/common/config.o $(TEST_UTILS_OBJ) | $(BIN_DIR) $(OBJ_DIR)
	$(CC) $(CFLAGS) $^ -o $@

# Compila tests/foo.c em obj/tests/foo.o
$(OBJ_DIR)/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Compila src/foo.c em obj/foo.o (preserva common/ e superpeer/)
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Remove executáveis e objetos
clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

.PHONY: all clean test
