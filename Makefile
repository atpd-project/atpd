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

# Build all
all: $(VERSION_H) $(OBJ_DIR) $(BIN_DIR) $(BIN_DIR)/$(TARGET)
	@echo "Build complete: $(BIN_DIR)/$(TARGET)"

# Generate version header
$(VERSION_H):
	@echo "Generating version header..."
	@rm -f $(VERSION_H)
	@chmod +x scripts/gen_version.sh
	@./scripts/gen_version.sh
	@echo "Generated $(VERSION_H)"

# Create directories
$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)
	@mkdir -p $(OBJ_DIR)/cjson

$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

# Show version information
version: $(VERSION_H)
	@echo "ATP Version Information:"
	@echo "  Version:   $$(grep ATP_VERSION_STRING $(VERSION_H) | cut -d'"' -f2)"
	@echo "  Major:     $$(grep ATP_VERSION_MAJOR $(VERSION_H) | cut -d' ' -f3)"
	@echo "  Minor:     $$(grep ATP_VERSION_MINOR $(VERSION_H) | cut -d' ' -f3)"
	@echo "  Patch:     $$(grep ATP_VERSION_PATCH $(VERSION_H) | cut -d' ' -f3)"
	@echo "  Suffix:    $$(grep ATP_VERSION_SUFFIX $(VERSION_H) | cut -d'"' -f2)"
	@echo "  Build:     $$(grep ATP_VERSION_BUILD $(VERSION_H) | cut -d' ' -f3)"
	@echo "  Commit:    $$(grep ATP_VERSION_COMMIT $(VERSION_H) | cut -d'"' -f2)"
	@echo "  Branch:    $$(grep ATP_VERSION_BRANCH $(VERSION_H) | cut -d'"' -f2)"

# Compile cJSON
$(OBJ_DIR)/cjson/cJSON.o: $(SRC_DIR)/cjson/cJSON.c $(HEADERS) | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(CJSON_CFLAGS) -I$(INC_DIR) -c $< -o $@

# Compile all other source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS) $(VERSION_H) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -I$(INC_DIR) -c $< -o $@

# Link object files
$(BIN_DIR)/$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

# Clean
clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

distclean: clean
	rm -f $(VERSION_H)
	rm -rf $(DIST_DIR)

help:
	@echo "ATP Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build the main target (default)"
	@echo "  version          - Generate and show version information"
	@echo "  clean            - Remove build artifacts"
	@echo "  distclean        - Remove build artifacts, version.h, and distribution"
	@echo "  help             - Show this help"
	@echo ""
	@echo "Variables:"
	@echo "  CC               - C compiler (default: gcc or NDK clang)"
	@echo "  CFLAGS           - Compiler flags"
	@echo "  LDFLAGS          - Linker flags"
	@echo ""
	@echo "Examples:"
	@echo "  make all                     - Build with default settings"
	@echo "  make clean && make           - Clean rebuild"
	@echo "  make version                 - Show version info"
	@echo "  make CC=gcc CFLAGS=-O0       - Build with debug flags"
