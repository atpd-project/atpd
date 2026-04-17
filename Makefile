# Makefile for ATP (Advanced Transparent Proxy)

PROJECT_NAME = atp
VERSION = 1.0.0
TARGET = atpd

# Detect NDK environment
ifneq ($(ANDROID_NDK_ROOT),)
    CC = $(ANDROID_NDK_ROOT)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang
    CFLAGS = -Wall -Wextra -O2 -pthread -D__ANDROID__ -DATP_VERSION=\"$(VERSION)\"
    LDFLAGS = -pthread -lcurl -static -llog
else
    CC = gcc
    CFLAGS = -Wall -Wextra -O2 -pthread -DATP_VERSION=\"$(VERSION)\"
    LDFLAGS = -pthread -lcurl
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

# Install paths
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

.PHONY: all clean distclean install install-android help

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
	rm -rf $(DIST_DIR)

install: $(BIN_DIR)/$(TARGET)
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN_DIR)/$(TARGET) $(DESTDIR)$(BINDIR)/

install-android: $(BIN_DIR)/$(TARGET)
	adb shell "su -c 'mkdir -p /data/adb/atp/bin'"
	adb push $(BIN_DIR)/$(TARGET) /data/local/tmp/atpd
	adb shell "su -c 'cp /data/local/tmp/atpd /data/adb/atp/bin/ && chmod 755 /data/adb/atp/bin/atpd'"
	adb shell "rm /data/local/tmp/atpd"
	@echo "Android installation complete"

help:
	@echo "ATP Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build the main target (default)"
	@echo "  clean            - Remove build artifacts"
	@echo "  distclean        - Remove build artifacts and distribution"
	@echo "  install          - Install to Linux system"
	@echo "  install-android  - Install to Android device via adb"
	@echo "  help             - Show this help"
