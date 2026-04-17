# Makefile for ATP (Advanced Transparent Proxy)
# Android NDK cross-compilation ready

PROJECT_NAME = atp
VERSION = 1.0.0
TARGET = atpd

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread -D_GNU_SOURCE -DATP_VERSION=\"$(VERSION)\"
LDFLAGS = -pthread -lcurl

# Android NDK cross-compilation (uncomment for Android build)
# NDK = /path/to/android-ndk
# TOOLCHAIN = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64
# CC = $(TOOLCHAIN)/bin/aarch64-linux-android21-clang
# CFLAGS = -Wall -Wextra -O2 -pthread -D__ANDROID__ -DATP_VERSION=\"$(VERSION)\"
# LDFLAGS = -pthread -lcurl -static

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
CONFDIR ?= /etc/atp
LOGDIR ?= /var/log/atp
RUNDIR ?= /var/run/atp

# Android install paths
ANDROID_BINDIR ?= /data/adb/atp/bin
ANDROID_CONFDIR ?= /data/adb/atp

.PHONY: all clean distclean install uninstall install-android test docs help

all: $(BIN_DIR)/$(TARGET)
	@echo "Build complete: $(BIN_DIR)/$(TARGET)"
	@echo "Run 'make install' to install or 'make install-android' for Android"

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(DIST_DIR):
	mkdir -p $(DIST_DIR)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -I$(INC_DIR) -c $< -o $@

$(BIN_DIR)/$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

# Test targets
TEST_SRCS = $(wildcard tests/test_*.c)
TEST_TARGETS = $(patsubst tests/test_%.c, $(BIN_DIR)/test_%, $(TEST_SRCS))

$(BIN_DIR)/test_%: tests/test_%.c $(filter-out $(OBJ_DIR)/main.o, $(OBJS)) | $(BIN_DIR)
	$(CC) $(CFLAGS) -I$(INC_DIR) $^ -o $@ $(LDFLAGS)

test: $(TEST_TARGETS)
	@echo "Running tests..."
	@for test in $(TEST_TARGETS); do \
		echo "=== Running $$test ==="; \
		$$test; \
		echo ""; \
	done
	@if [ -f tests/run_tests.sh ]; then \
		bash tests/run_tests.sh; \
	fi

# Linux installation
install: $(BIN_DIR)/$(TARGET)
	mkdir -p $(DESTDIR)$(BINDIR)
	mkdir -p $(DESTDIR)$(CONFDIR)
	mkdir -p $(DESTDIR)$(LOGDIR)
	mkdir -p $(DESTDIR)$(RUNDIR)
	
	install -m 755 $(BIN_DIR)/$(TARGET) $(DESTDIR)$(BINDIR)/
	
	if [ ! -f $(DESTDIR)$(CONFDIR)/atp.conf ]; then \
		install -m 644 examples/atp.conf.example $(DESTDIR)$(CONFDIR)/atp.conf; \
	fi
	
	if [ -d /etc/init.d ]; then \
		install -m 755 scripts/atp.init $(DESTDIR)/etc/init.d/atp; \
	fi
	
	if [ -d /etc/systemd/system ]; then \
		install -m 644 scripts/atp.service $(DESTDIR)/etc/systemd/system/; \
	fi
	
	@echo "Installation complete"

# Android installation
install-android: $(BIN_DIR)/$(TARGET)
	adb shell "su -c 'mkdir -p $(ANDROID_BINDIR) $(ANDROID_CONFDIR)/run $(ANDROID_CONFDIR)/rules'"
	adb push $(BIN_DIR)/$(TARGET) /data/local/tmp/atpd
	adb shell "su -c 'cp /data/local/tmp/atpd $(ANDROID_BINDIR)/ && chmod 755 $(ANDROID_BINDIR)/atpd'"
	adb shell "rm /data/local/tmp/atpd"
	
	if [ ! -f examples/atp.conf.example ]; then \
		adb push examples/atp.conf.example /data/local/tmp/atp.conf; \
		adb shell "su -c 'cp /data/local/tmp/atp.conf $(ANDROID_CONFDIR)/atp.conf'"; \
		adb shell "rm /data/local/tmp/atp.conf"; \
	fi
	
	if [ -d /data/adb/service.d ]; then \
		adb shell "su -c 'cat > /data/adb/service.d/atp.sh << EOF\n#!/system/bin/sh\n$(ANDROID_BINDIR)/atpd start\nEOF'"; \
		adb shell "su -c 'chmod 755 /data/adb/service.d/atp.sh'"; \
	fi
	
	@echo "Android installation complete"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)/etc/init.d/atp
	rm -f $(DESTDIR)/etc/systemd/system/atp.service
	@echo "Uninstallation complete"

uninstall-android:
	adb shell "su -c 'rm -f $(ANDROID_BINDIR)/atpd'"
	adb shell "su -c 'rm -f /data/adb/service.d/atp.sh'"
	@echo "Android uninstallation complete"

# Distribution
dist: clean $(BIN_DIR)/$(TARGET)
	mkdir -p $(DIST_DIR)/atp-$(VERSION)
	
	cp $(BIN_DIR)/$(TARGET) $(DIST_DIR)/atp-$(VERSION)/
	cp -r examples $(DIST_DIR)/atp-$(VERSION)/
	cp -r scripts $(DIST_DIR)/atp-$(VERSION)/
	cp -r docs $(DIST_DIR)/atp-$(VERSION)/ 2>/dev/null || true
	cp README.md LICENSE $(DIST_DIR)/atp-$(VERSION)/ 2>/dev/null || true
	
	cd $(DIST_DIR) && tar czf atp-$(VERSION).tar.gz atp-$(VERSION)
	@echo "Distribution created: $(DIST_DIR)/atp-$(VERSION).tar.gz"

# Documentation
docs:
	@echo "Generating documentation..."
	@if command -v doxygen >/dev/null 2>&1; then \
		doxygen docs/Doxyfile 2>/dev/null || echo "Doxygen config not found"; \
	else \
		echo "Doxygen not installed, skipping docs generation"; \
	fi

# Clean targets
clean:
	rm -rf $(OBJ_DIR)

distclean: clean
	rm -rf $(BIN_DIR)
	rm -rf $(DIST_DIR)

# Help
help:
	@echo "ATP (Advanced Transparent Proxy) Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build the main target (default)"
	@echo "  test             - Build and run tests"
	@echo "  install          - Install to Linux system"
	@echo "  install-android  - Install to Android device via adb"
	@echo "  uninstall        - Remove from Linux system"
	@echo "  uninstall-android- Remove from Android device"
	@echo "  dist             - Create distribution tarball"
	@echo "  docs             - Generate documentation"
	@echo "  clean            - Remove build artifacts"
	@echo "  distclean        - Remove build artifacts and distribution"
	@echo "  help             - Show this help"
	@echo ""
	@echo "Variables:"
	@echo "  CC               - C compiler (default: gcc)"
	@echo "  PREFIX           - Installation prefix (default: /usr/local)"
	@echo "  DESTDIR          - Staging directory for packaging"
	@echo "  NDK              - Android NDK path (for cross-compilation)"
	@echo ""
	@echo "Android cross-compilation:"
	@echo "  export NDK=/path/to/android-ndk"
	@echo "  make CC=\$$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang"