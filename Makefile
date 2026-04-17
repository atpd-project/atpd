# Makefile for ATP (Advanced Transparent Proxy)

PROJECT_NAME = atp
VERSION = 1.0.0
TARGET = atpd

# Detect NDK environment
ifneq ($(ANDROID_NDK_ROOT),)
    CC = $(ANDROID_NDK_ROOT)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang
    CFLAGS = -Wall -Wextra -O2 -pthread -D__ANDROID__ -DATP_VERSION=\"$(VERSION)\" \
             -fPIC -fpie -ftls-model=local-exec
    # Static linking with TLS alignment fix for Bionic
    LDFLAGS = -L/tmp/curl-android/lib -lcurl -pthread -static \
              -Wl,-z,max-page-size=4096 \
              -Wl,-z,common-page-size=4096 \
              -Wl,-Bstatic \
              -Wl,--no-undefined \
              -Wl,--no-warn-shared-textrel
else
    CC = gcc
    CFLAGS = -Wall -Wextra -O2 -pthread -DATP_VERSION=\"$(VERSION)\"
    LDFLAGS = -lcurl -pthread
endif

# Directories
SRC_DIR = src
INC_DIR = include
OBJ_DIR = build/obj
BIN_DIR = build/bin
DIST_DIR = dist

# Source files
SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

# Headers
HEADERS = $(wildcard $(INC_DIR)/*.h)

.PHONY: all clean distclean

all: $(BIN_DIR)/$(TARGET)
	@echo "Build complete: $(BIN_DIR)/$(TARGET)"

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -I$(INC_DIR) -c $< -o $@

$(BIN_DIR)/$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

clean:
	rm -rf $(OBJ_DIR)

distclean: clean
	rm -rf $(BIN_DIR)
