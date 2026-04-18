# Makefile for ATP (Advanced Transparent Proxy)

PROJECT_NAME = atp
TARGET = atpd

# Detect NDK environment
ifneq ($(ANDROID_NDK_ROOT),)
    CC = $(ANDROID_NDK_ROOT)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang
    CFLAGS = -Wall -Wextra -O2 -pthread -D__ANDROID__ -fPIC -fpie
    CFLAGS += -I$(INC_DIR)
    LDFLAGS = -L/tmp/curl-musl/lib -lcurl -pthread -pie
else
    CC = gcc
    CFLAGS = -Wall -Wextra -O2 -pthread
    CFLAGS += -I$(INC_DIR)
    LDFLAGS = -lcurl -pthread
endif

# Directories
SRC_DIR = src
INC_DIR = include
OBJ_DIR = build/obj
BIN_DIR = build/bin
DIST_DIR = dist

# Version header (auto-generated)
VERSION_H = $(INC_DIR)/version.h

# Source files
SRCS = $(SRC_DIR)/api.c \
       $(SRC_DIR)/app_filter.c \
       $(SRC_DIR)/cjson/cJSON.c \
       $(SRC_DIR)/cli.c \
       $(SRC_DIR)/config.c \
       $(SRC_DIR)/geoip.c \
       $(SRC_DIR)/inet_diag.c \
       $(SRC_DIR)/ipset.c \
       $(SRC_DIR)/ipv6_manager.c \
       $(SRC_DIR)/logger.c \
       $(SRC_DIR)/mac_filter.c \
       $(SRC_DIR)/main.c \
       $(SRC_DIR)/netlink.c \
       $(SRC_DIR)/netlink_monitor.c \
       $(SRC_DIR)/perf_mode.c \
       $(SRC_DIR)/routing.c \
       $(SRC_DIR)/service.c \
       $(SRC_DIR)/status.c \
       $(SRC_DIR)/tproxy.c \
       $(SRC_DIR)/utils.c \
       $(SRC_DIR)/version.c

OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

# Headers
HEADERS = $(wildcard $(INC_DIR)/*.h) $(wildcard $(INC_DIR)/cjson/*.h)

# Additional flags for cJSON compilation
CJSON_CFLAGS = -I$(INC_DIR)/cjson

.PHONY: all clean distclean help version

# Generate version header before compilation
$(VERSION_H):
	@echo "Generating version header..."
	@chmod +x scripts/gen_version.sh
	@./scripts/gen_version.sh

# Build all (depends on version header)
all: $(VERSION_H) $(BIN_DIR)/$(TARGET)
	@echo "Build complete: $(BIN_DIR)/$(TARGET)"

# Show version information
version:
	@./scripts/gen_version.sh
	@echo "Version information generated in $(VERSION_H)"
	@echo "ATP_VERSION_STRING: $$(grep ATP_VERSION_STRING $(VERSION_H) | cut -d'"' -f2)"

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# Generic rule for all .c files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS) $(VERSION_H) | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INC_DIR) -c $< -o $@

# Special rule for cJSON.c (needs cjson header path)
$(OBJ_DIR)/cjson/%.o: $(SRC_DIR)/cjson/%.c $(HEADERS) | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CJSON_CFLAGS) -I$(INC_DIR) -c $< -o $@

$(BIN_DIR)/$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

clean:
	rm -rf $(OBJ_DIR)
	rm -f $(VERSION_H)

distclean: clean
	rm -rf $(BIN_DIR)
	rm -rf $(DIST_DIR)

help:
	@echo "ATP Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build the main target (default)"
	@echo "  version          - Generate and show version information"
	@echo "  clean            - Remove build artifacts"
	@echo "  distclean        - Remove build artifacts and distribution"
	@echo "  help             - Show this help"
