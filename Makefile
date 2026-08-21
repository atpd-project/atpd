# ATP - Advanced Transparent Proxy
# Makefile for Android NDK build - Clang 19 Optimized

VERSION = 1.0.0

PREFIX = /data/adb/atp
BINDIR = $(PREFIX)/bin
RUNDIR = $(PREFIX)/run
RULESDIR = $(PREFIX)/rules
SINGBOXDIR = $(PREFIX)/sing-box

CC = cc
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -fPIC
CFLAGS += -Os -flto
CFLAGS += -fstack-protector-strong -D_FORTIFY_SOURCE=3
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -DATP_DEFAULT_DIR=\"$(PREFIX)\"
CFLAGS += -DATP_CONF_FILE=\"atp.conf\"
CFLAGS += -DATP_PID_FILE=\"run/atpd.pid\"
CFLAGS += -DATP_LOG_FILE=\"run/atp.log\"
CFLAGS += -DATP_COMMAND_SOCKET=\"run/atpd.sock\"
CFLAGS += -Iinclude

ifdef DEBUG
CFLAGS += -g -DATP_DEBUG -O0 -fsanitize=address
endif

LIBS =

LDFLAGS = -flto
LDFLAGS += -Wl,--gc-sections -Wl,--strip-all

SRC = \
	src/api.c \
	src/async_validate.c \
	src/atpd_context.c \
	src/atpd_global.c \
	src/atpd_init.c \
	src/cli.c \
	src/config.c \
	src/logger.c \
	src/main.c \
	src/netlink.c \
	src/reactor.c \
	src/routing.c \
	src/service.c \
	src/uds.c \
	src/utils.c \
	src/wifi.c \
	src/yyjson.c

OBJDIR = build/obj
OBJ = $(SRC:%.c=$(OBJDIR)/%.o)
TARGET = build/bin/atpd

.PHONY: all test clean distclean

all: $(TARGET)

test: $(TARGET)
	$(CC) $(CFLAGS) -o build/test_wifi tests/test_wifi.c src/wifi.c src/atpd_context.c src/utils.c src/logger.c
	build/test_wifi
	bash tests/test_status.sh $(TARGET)
	bash tests/test_core_status.sh $(TARGET)

$(TARGET): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)
	@echo "  LD      $@"

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build/
	find src/ -name "*.o" -delete

distclean: clean
	rm -f $(TARGET)
