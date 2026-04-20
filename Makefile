# ATP - Advanced Transparent Proxy
# Makefile for Android NDK build

# Project version
VERSION = 1.0.0

# Installation paths
PREFIX = /data/adb/atp
BINDIR = $(PREFIX)/bin
RUNDIR = $(PREFIX)/run
RULESDIR = $(PREFIX)/rules
SINGBOXDIR = $(PREFIX)/sing-box

# Compiler and flags
CC ?= gcc
CFLAGS = -Wall -Wextra -O2 -fPIC -D_GNU_SOURCE
CFLAGS += -Iinclude -Iinclude/cjson -DVERSION=\"$(VERSION)\"
CFLAGS += -DATP_DEFAULT_DIR=\"$(PREFIX)\"
CFLAGS += -DATP_CONF_FILE=\"atp.conf\"
CFLAGS += -DATP_PID_FILE=\"run/atpd.pid\"
CFLAGS += -DATP_LOG_FILE=\"run/atp.log\"
CFLAGS += -DATP_COMMAND_SOCKET=\"run/atpd.sock\"

# Allow external flags to be appended (do not override internal flags)
CFLAGS += $(EXTRA_CFLAGS)

# Debug build
ifdef DEBUG
CFLAGS += -g -DATP_DEBUG -O0
endif

# Libraries
LIBS = -lpthread -lcurl

# Linker flags
LDFLAGS ?=
LDFLAGS += $(EXTRA_LDFLAGS)

# Source files
SRC = \
    src/main.c \
    src/config.c \
    src/config_validator.c \
    src/logger.c \
    src/utils.c \
    src/service.c \
    src/api.c \
    src/netlink.c \
    src/netlink_monitor.c \
    src/app_filter.c \
    src/fcm_monitor.c \
    src/perf_mode.c \
    src/status.c \
    src/ui.c \
    src/cli.c \
    src/tproxy.c \
    src/routing.c \
    src/geoip.c \
    src/ipset.c \
    src/mac_filter.c \
    src/ipv6_manager.c \
    src/inet_diag.c \
    src/version.c \
    src/cjson/cJSON.c

# Object files
OBJ = $(SRC:.c=.o)

# Output binary
TARGET = build/bin/atpd

# Build targets
.PHONY: all clean install install-android distclean

# Generate version header
include/version.h: scripts/gen_version.sh
	@echo "Generating version.h..."
	@chmod +x scripts/gen_version.sh
	@./scripts/gen_version.sh

# Ensure version.o depends on version.h
src/version.o: include/version.h

# Ensure version.h is generated before build
all: include/version.h $(TARGET)

$(TARGET): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS) $(LDFLAGS)
	@echo "  LD      $@"

%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build/
	rm -f src/*.o
	rm -f src/cjson/*.o

distclean: clean
	rm -f $(TARGET)

install: $(TARGET)
	@echo "Installing ATP to $(PREFIX)"
	@mkdir -p $(BINDIR)
	@mkdir -p $(RUNDIR)
	@mkdir -p $(RULESDIR)
	@mkdir -p $(SINGBOXDIR)
	@cp $(TARGET) $(BINDIR)/
	@chmod 755 $(BINDIR)/atpd
	@echo "ATP installed successfully"

install-android: $(TARGET)
	adb push $(TARGET) $(BINDIR)/
	adb shell chmod 755 $(BINDIR)/atpd
	adb shell mkdir -p $(RUNDIR)
	adb shell mkdir -p $(RULESDIR)
	adb shell mkdir -p $(SINGBOXDIR)
	@echo "ATP pushed to device"

uninstall:
	rm -f $(BINDIR)/atpd
	@echo "ATP uninstalled"

# Create release tarball
release: clean
	@mkdir -p release/atp-$(VERSION)
	@cp -r src include scripts Makefile README.md release/atp-$(VERSION)/
	@cd release && tar -czf atp-$(VERSION).tar.gz atp-$(VERSION)
	@echo "Release package: release/atp-$(VERSION).tar.gz"

# Help
help:
	@echo "ATP Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all             Build atpd binary"
	@echo "  clean           Remove build artifacts"
	@echo "  distclean       Remove everything"
	@echo "  install         Install to $(PREFIX)"
	@echo "  install-android Push to device via adb"
	@echo "  uninstall       Remove from $(PREFIX)"
	@echo "  release         Create release tarball"
	@echo ""
	@echo "Variables:"
	@echo "  CC              C compiler (default: gcc)"
	@echo "  DEBUG=1         Build with debug symbols"
	@echo "  EXTRA_CFLAGS    Additional compiler flags"
	@echo "  EXTRA_LDFLAGS   Additional linker flags"
