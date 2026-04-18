# Makefile for ATP (Advanced Transparent Proxy)

PROJECT_NAME = atp
VERSION = 1.0.0
TARGET = atpd

# Detect NDK environment
ifneq ($(ANDROID_NDK_ROOT),)
    CC = $(ANDROID_NDK_ROOT)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang
    CFLAGS = -Wall -Wextra -O2 -pthread -D__ANDROID__ -DATP_VERSION=\"$(VERSION)\" -fPIC -fpie
    CFLAGS += -I$(INC_DIR)
    LDFLAGS = -L/tmp/curl-musl/lib -lcurl -pthread -pie
    # Uncomment if HTTPS is needed
    # LDFLAGS += -lssl -lcrypto
else
    CC = gcc
    CFLAGS = -Wall -Wextra -O2 -pthread -DATP_VERSION=\"$(VERSION)\"
    CFLAGS += -I$(INC_DIR)
    LDFLAGS = -lcurl -pthread
    # Uncomment if HTTPS is needed
    # LDFLAGS += -lssl -lcrypto
endif

# Directories
SRC_DIR = src
INC_DIR = include
OBJ_DIR = build/obj
BIN_DIR = build/bin
DIST_DIR = dist

# Source files
SRCS = $(SRC_DIR)/api.c \
       $(SRC_DIR)/app_filter.c \
       $(SRC_DIR)/cjson/cJSON.c \
       $(SRC_DIR)/cli.c \
       $(SRC_DIR)/config.c \
       $(SRC_DIR)/geoip.c \
       $(SRC_DIR)/ipset.c \
       $(SRC_DIR)/ipv6_manager.c \
       $(SRC_DIR)/logger.c \
       $(SRC_DIR)/mac_filter.c \
       $(SRC_DIR)/main.c \
       $(SRC_DIR)/netlink.c \
       $(SRC_DIR)/perf_mode.c \
       $(SRC_DIR)/routing.c \
       $(SRC_DIR)/service.c \
       $(SRC_DIR)/status.c \
       $(SRC_DIR)/tproxy.c \
       $(SRC_DIR)/utils.c

OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

# Headers
HEADERS = $(wildcard $(INC_DIR)/*.h) $(wildcard $(INC_DIR)/cjson/*.h)

.PHONY: all clean distclean help

all: $(BIN_DIR)/$(TARGET)
	@echo "Build complete: $(BIN_DIR)/$(TARGET)"

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS) | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INC_DIR) -c $< -o $@

# Special rule for cjson in subdirectory
$(OBJ_DIR)/cjson/%.o: $(SRC_DIR)/cjson/%.c $(HEADERS) | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INC_DIR) -c $< -o $@

# Ensure cjson object is included
$(BIN_DIR)/$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

clean:
	rm -rf $(OBJ_DIR)

distclean: clean
	rm -rf $(BIN_DIR)
	rm -rf $(DIST_DIR)

help:
	@echo "ATP Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build the main target (default)"
	@echo "  clean            - Remove build artifacts"
	@echo "  distclean        - Remove build artifacts and distribution"
	@echo "  help             - Show this help"
