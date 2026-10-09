CC = gcc
CFLAGS = -Wall -Wextra -pedantic -Iinclude -Itests -pthread
LDFLAGS = -lcrypto -lz -llz4 -pthread

BIN_DIR = bin
OBJ_DIR = obj
SRC_DIR = src

SUPERPEER_SRC = $(SRC_DIR)/superpeer/superpeer.c

# Módulos compartilhados (objetos em obj/, fontes em src/common/)
COMMON_OBJS = \
	$(OBJ_DIR)/common/node.o \
	$(OBJ_DIR)/common/config.o \
	$(OBJ_DIR)/common/network.o \
	$(OBJ_DIR)/common/protocol.o \
	$(OBJ_DIR)/common/compression.o
SUPERPEER_OBJ = $(OBJ_DIR)/superpeer/superpeer.o
METADATA_OBJ = $(OBJ_DIR)/superpeer/metadata.o
CHORD_OBJ = $(OBJ_DIR)/superpeer/chord.o
SUPERPEER_MAIN_OBJ = $(OBJ_DIR)/superpeer/main.o
PEER_MAIN_OBJ = $(OBJ_DIR)/peer/peer.o

# Módulos do pipeline de arquivos do peer (CP2)
PEER_OBJS = \
	$(OBJ_DIR)/peer/upload.o \
	$(OBJ_DIR)/peer/download.o \
	$(OBJ_DIR)/peer/file_pipeline.o \
	$(OBJ_DIR)/peer/storage.o

# Executáveis a gerar
TARGETS = $(BIN_DIR)/superpeer $(BIN_DIR)/node $(BIN_DIR)/client
TEST_NODE = $(BIN_DIR)/test_node
TEST_CONFIG = $(BIN_DIR)/test_config
TEST_SUPERPEER = $(BIN_DIR)/test_superpeer
TEST_METADATA = $(BIN_DIR)/test_metadata
TEST_COMPRESSION = $(BIN_DIR)/test_compression
TEST_FILE_PIPELINE = $(BIN_DIR)/test_file_pipeline
TEST_STORAGE = $(BIN_DIR)/test_storage
TEST_UPLOAD = $(BIN_DIR)/test_upload
TEST_DOWNLOAD = $(BIN_DIR)/test_download
TEST_CHUNK_WIRE = $(BIN_DIR)/test_chunk_wire
TEST_CHUNK_SERVICE = $(BIN_DIR)/test_chunk_service
TEST_CHORD = $(BIN_DIR)/test_chord
TEST_UTILS_OBJ = $(OBJ_DIR)/tests/utils/test_utils.o

# Alvo padrão: cria os diretórios e gera tudo
all: $(BIN_DIR) $(OBJ_DIR) $(TARGETS)

test: $(TEST_NODE) $(TEST_CONFIG) $(TEST_SUPERPEER) $(TEST_METADATA) $(TEST_COMPRESSION) $(TEST_FILE_PIPELINE) $(TEST_STORAGE) $(TEST_UPLOAD) $(TEST_DOWNLOAD) $(TEST_CHUNK_WIRE) $(TEST_CHUNK_SERVICE) $(TEST_CHORD)
	$(TEST_NODE)
	$(TEST_CONFIG)
	$(TEST_SUPERPEER)
	$(TEST_METADATA)
	$(TEST_COMPRESSION)
	$(TEST_FILE_PIPELINE)
	$(TEST_STORAGE)
	$(TEST_UPLOAD)
	$(TEST_DOWNLOAD)
	$(TEST_CHUNK_WIRE)
	$(TEST_CHUNK_SERVICE)
	$(TEST_CHORD)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# Ligação
$(BIN_DIR)/superpeer: $(SUPERPEER_OBJ) $(METADATA_OBJ) $(CHORD_OBJ) $(SUPERPEER_MAIN_OBJ) $(OBJ_DIR)/peer/storage.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/node: $(BIN_DIR)/superpeer
	cp -f $< $@

$(BIN_DIR)/client: $(PEER_MAIN_OBJ) $(PEER_OBJS) $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/test_node: tests/common/test_node.c $(OBJ_DIR)/common/node.o $(TEST_UTILS_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) 

$(BIN_DIR)/test_config: tests/common/test_config.c $(OBJ_DIR)/common/config.o $(TEST_UTILS_OBJ) | $(BIN_DIR) $(OBJ_DIR)
	$(CC) $(CFLAGS) $^ -o $@

$(BIN_DIR)/test_superpeer: tests/superpeer/test_superpeer.c $(SUPERPEER_OBJ) $(METADATA_OBJ) $(CHORD_OBJ) $(OBJ_DIR)/peer/storage.o $(COMMON_OBJS) $(TEST_UTILS_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/test_metadata: tests/superpeer/test_metadata.c $(METADATA_OBJ) $(COMMON_OBJS) $(TEST_UTILS_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/test_compression: tests/common/test_compression.c $(OBJ_DIR)/common/compression.o $(TEST_UTILS_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/test_file_pipeline: tests/peer/test_file_pipeline.c $(OBJ_DIR)/peer/file_pipeline.o $(OBJ_DIR)/common/node.o $(TEST_UTILS_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/test_storage: tests/peer/test_storage.c $(OBJ_DIR)/peer/storage.o $(TEST_UTILS_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@

$(BIN_DIR)/test_upload: tests/peer/test_upload.c $(OBJ_DIR)/peer/upload.o $(OBJ_DIR)/peer/file_pipeline.o $(OBJ_DIR)/peer/storage.o $(OBJ_DIR)/common/compression.o $(OBJ_DIR)/common/node.o $(OBJ_DIR)/common/protocol.o $(OBJ_DIR)/common/network.o $(TEST_UTILS_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/test_download: tests/peer/test_download.c $(OBJ_DIR)/peer/download.o $(OBJ_DIR)/peer/upload.o $(OBJ_DIR)/peer/file_pipeline.o $(OBJ_DIR)/peer/storage.o $(OBJ_DIR)/common/compression.o $(OBJ_DIR)/common/node.o $(OBJ_DIR)/common/protocol.o $(OBJ_DIR)/common/network.o $(TEST_UTILS_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/test_chunk_wire: tests/common/test_chunk_wire.c $(OBJ_DIR)/common/protocol.o $(OBJ_DIR)/common/network.o $(OBJ_DIR)/common/node.o $(TEST_UTILS_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/test_chunk_service: tests/superpeer/test_chunk_service.c $(SUPERPEER_OBJ) $(METADATA_OBJ) $(CHORD_OBJ) $(OBJ_DIR)/peer/storage.o $(COMMON_OBJS) $(TEST_UTILS_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/test_chord: tests/superpeer/test_chord.c $(CHORD_OBJ) $(OBJ_DIR)/common/node.o $(TEST_UTILS_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

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
