# ATP - Advanced Transparent Proxy
# Makefile for Android NDK / Linux build - Pure eBPF Edition (True Native Size-Optimized)

PREFIX ?= /data/adb/atp
BINDIR ?= $(PREFIX)/bin
RUNDIR ?= $(PREFIX)/run
SINGBOXDIR ?= $(PREFIX)/sing-box

CC ?= $(shell which clang 2>/dev/null || echo gcc)

ifeq ($(findstring clang,$(CC)),clang)
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -DNDEBUG -fPIC -Qunused-arguments
CFLAGS += -Oz -flto -ffunction-sections -fdata-sections
CFLAGS += -fno-unwind-tables -fno-asynchronous-unwind-tables
CFLAGS += -fmerge-all-constants -fno-ident
else
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -DNDEBUG -fPIC
CFLAGS += -O2 -flto -ffunction-sections -fdata-sections
CFLAGS += -fno-unwind-tables -fno-asynchronous-unwind-tables
CFLAGS += -fmerge-all-constants -fno-ident
endif
CFLAGS += -fstack-protector-strong
CFLAGS += -DYYJSON_DISABLE_WRITER=1 -DYYJSON_DISABLE_FAST_FP_CONV=1 -DYYJSON_DISABLE_NON_STANDARD=1
CFLAGS += -DATP_DEFAULT_DIR=\"$(PREFIX)\"
CFLAGS += -DATP_CONF_FILE=\"atp.conf\"
CFLAGS += -DATP_PID_FILE=\"run/atpd.pid\"
CFLAGS += -DATP_LOG_FILE=\"run/atp.log\"
CFLAGS += -DATP_COMMAND_SOCKET=\"run/atpd.sock\"
CFLAGS += -Iinclude

ifdef DEBUG
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -fPIC -g -DATP_DEBUG -O0 -fsanitize=address -Iinclude
endif
CFLAGS += -D_FORTIFY_SOURCE=3
CFLAGS += -Ibuild/generated

LIBS = -lpthread

LDFLAGS = -flto
LDFLAGS += -Wl,--gc-sections -Wl,--strip-all -Wl,--build-id=none -Wl,-z,relro,-z,now

SRC = $(wildcard src/*.c)

OBJDIR = build/obj
OBJ = $(SRC:%.c=$(OBJDIR)/%.o)
TARGET = build/bin/atpd
VPN_MODE_TEST = build/tests/test_api_vpn_mode
LOGGER_SAFETY_TEST = build/tests/test_logger_file_safety
RESULT_TEST = build/tests/test_atp_result
VERSION_HEADER = build/generated/version_build.h
VERSION_TEST = build/tests/test_version
CONFIG_VALUE_TEST = build/tests/test_config_value

.PHONY: all test clean distclean install uninstall

all: $(TARGET)

test: $(TARGET) $(VPN_MODE_TEST) $(LOGGER_SAFETY_TEST) $(RESULT_TEST) $(VERSION_TEST) $(CONFIG_VALUE_TEST)
	$(VPN_MODE_TEST)
	$(LOGGER_SAFETY_TEST)
	$(RESULT_TEST)
	$(VERSION_TEST)
	$(CONFIG_VALUE_TEST)
	sh tests/test_config_validation.sh $(TARGET)
	sh tests/test_android_service.sh

$(VPN_MODE_TEST): tests/test_api_vpn_mode.c src/api.c
	@mkdir -p $(dir $@)
	$(CC) -Wall -Wextra -std=c11 -D_GNU_SOURCE -Iinclude -o $@ $^ -lpthread

$(LOGGER_SAFETY_TEST): tests/test_logger_file_safety.c src/logger.c
	@mkdir -p $(dir $@)
	$(CC) -Wall -Wextra -std=c11 -D_GNU_SOURCE -Iinclude -o $@ $^ -lpthread

$(RESULT_TEST): tests/test_atp_result.c
	@mkdir -p $(dir $@)
	$(CC) -Wall -Wextra -std=c11 -D_GNU_SOURCE -Iinclude -o $@ $^

$(VERSION_TEST): tests/test_version.c src/version.c $(VERSION_HEADER)
	@mkdir -p $(dir $@)
	$(CC) -Wall -Wextra -std=c11 -D_GNU_SOURCE -Iinclude -Ibuild/generated -o $@ tests/test_version.c src/version.c

$(CONFIG_VALUE_TEST): tests/test_config_value.c
	@mkdir -p $(dir $@)
	$(CC) -Wall -Wextra -std=c11 -D_GNU_SOURCE -Iinclude -o $@ $^

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(RUNDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/atpd

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/atpd

$(TARGET): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)
	@echo "  LD (Native Lean) $@"

$(VERSION_HEADER): FORCE VERSION scripts/gen_version.sh
	@bash scripts/gen_version.sh $@

FORCE:

$(OBJDIR)/src/version.o: $(VERSION_HEADER)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC       $<"
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build/
	find src/ -name "*.o" -delete

distclean: clean
	rm -f $(TARGET)
