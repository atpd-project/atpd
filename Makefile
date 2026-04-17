# Makefile for ATP (Advanced Transparent Proxy)

PROJECT_NAME = atp
VERSION = 1.0.0
TARGET = atpd

# Default compiler (will be overridden by environment)
CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -pthread -DATP_VERSION=\"$(VERSION)\"
LDFLAGS ?= -pthread

# Directories
SRC_DIR = src
INC_DIR = include
OBJ_DIR = build/obj
BIN_DIR = build/bin
DIST_DIR = dist

# Source files
SRCS = $(wildcard $(SRC_DIR)/*.c)
# Add new source files to OBJS
OBJS = $(OBJ_DIR)/api.o $(OBJ_DIR)/app_filter.o $(OBJ_DIR)/cli.o \
       $(OBJ_DIR)/config.o $(OBJ_DIR)/geoip.o $(OBJ_DIR)/iface_monitor.o \
       $(OBJ_DIR)/ipset.o $(OBJ_DIR)/ipv6_manager.o $(OBJ_DIR)/logger.o \
       $(OBJ_DIR)/mac_filter.o $(OBJ_DIR)/main.o $(OBJ_DIR)/netlink.o \
       $(OBJ_DIR)/netlink_link.o $(OBJ_DIR)/netlink_route.o \
       $(OBJ_DIR)/netlink_rule.o $(OBJ_DIR)/netlink_wait.o \
       $(OBJ_DIR)/perf_mode.o $(OBJ_DIR)/routing.o $(OBJ_DIR)/service.o \
       $(OBJ_DIR)/status.o $(OBJ_DIR)/tproxy.o $(OBJ_DIR)/utils.o


# Headers
HEADERS = $(wildcard $(INC_DIR)/*.h)

.PHONY: all clean distclean help

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

help:
	@echo "ATP Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build the main target (default)"
	@echo "  clean            - Remove build artifacts"
	@echo "  distclean        - Remove build artifacts and distribution"
	@echo "  help             - Show this help"
