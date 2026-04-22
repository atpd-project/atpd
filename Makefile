# ATP - Advanced Transparent Proxy
# Makefile for Android NDK build

VERSION = 1.0.0

PREFIX = /data/adb/atp
BINDIR = $(PREFIX)/bin
RUNDIR = $(PREFIX)/run
RULESDIR = $(PREFIX)/rules
SINGBOXDIR = $(PREFIX)/sing-box

CC ?= clang
CFLAGS = -Wall -Wextra -O2 -fPIC -D_GNU_SOURCE -std=c11
CFLAGS += -DATP_DEFAULT_DIR=\"$(PREFIX)\"
CFLAGS += -DATP_CONF_FILE=\"atp.conf\"
CFLAGS += -DATP_PID_FILE=\"run/atpd.pid\"
CFLAGS += -DATP_LOG_FILE=\"run/atp.log\"
CFLAGS += -DATP_COMMAND_SOCKET=\"run/atpd.sock\"
CFLAGS += -Iinclude

ifdef DEBUG
CFLAGS += -g -DATP_DEBUG -O0
endif

LIBS = -lpthread

LDFLAGS ?=
LDFLAGS += -static

SRC = \
    src/main.c \
    src/config.c \
    src/config_validator.c \
    src/logger.c \
    src/utils.c \
    src/service.c \
    src/api.c \
    src/netlink.c \
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
    src/reactor.c \
    src/iface_monitor.c \
    src/iface_monitor_reactor.c \
    src/yyjson/yyjson.c

OBJ = $(SRC:.c=.o)
TARGET = build/bin/atpd

.PHONY: all clean distclean

all: $(TARGET)

$(TARGET): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)
	@echo "  LD      $@"

%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build/
	find src/ -name "*.o" -delete

distclean: clean
	rm -f $(TARGET)
