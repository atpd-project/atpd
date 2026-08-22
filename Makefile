# ATP - Advanced Transparent Proxy
# Makefile for Android NDK / Linux build - Pure eBPF Edition (True Native Size-Optimized)

VERSION = 2.0.0

PREFIX = /data/adb/atp
BINDIR = $(PREFIX)/bin
RUNDIR = $(PREFIX)/run
SINGBOXDIR = $(PREFIX)/sing-box

CC = clang
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -DNDEBUG -fPIC -Qunused-arguments
CFLAGS += -Oz -flto -ffunction-sections -fdata-sections
CFLAGS += -fno-unwind-tables -fno-asynchronous-unwind-tables
CFLAGS += -fmerge-all-constants -fno-ident -fomit-frame-pointer -fno-math-errno -fvisibility=hidden
CFLAGS += -DLOG_LOCATION_ENABLED=0
CFLAGS += -DYYJSON_DISABLE_WRITER=1 -DYYJSON_DISABLE_FAST_FP_CONV=1 -DYYJSON_DISABLE_NON_STANDARD=1
CFLAGS += -DYYJSON_DISABLE_UTF8_VALIDATION=1 -DYYJSON_DISABLE_COMMENT=1 -DYYJSON_DISABLE_INF_AND_NAN=1 -DYYJSON_DISABLE_NUM_FORMAT_CHECK=1
CFLAGS += -DATP_DEFAULT_DIR=\"$(PREFIX)\"
CFLAGS += -DATP_CONF_FILE=\"atp.conf\"
CFLAGS += -DATP_PID_FILE=\"run/atpd.pid\"
CFLAGS += -DATP_LOG_FILE=\"run/atp.log\"
CFLAGS += -DATP_COMMAND_SOCKET=\"run/atpd.sock\"
CFLAGS += -Iinclude

ifdef DEBUG
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -fPIC -g -DATP_DEBUG -O0 -fsanitize=address -Iinclude
endif

LIBS = -lpthread

LDFLAGS = -flto
LDFLAGS += -Wl,--gc-sections -Wl,--strip-all -Wl,--build-id=none -Wl,--icf=all -Wl,--as-needed

SRC = $(wildcard src/*.c)

OBJDIR = build/obj
OBJ = $(SRC:%.c=$(OBJDIR)/%.o)
TARGET = build/bin/atpd

.PHONY: all clean distclean

all: $(TARGET)

$(TARGET): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)
	@strip -s --strip-all --remove-section=.comment --remove-section=.note* --remove-section=.ARM.exidx* --remove-section=.eh_frame* $@ 2>/dev/null || true
	@echo "  LD (Native Lean) $@"

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC       $<"
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build/
	find src/ -name "*.o" -delete

distclean: clean
	rm -f $(TARGET)
