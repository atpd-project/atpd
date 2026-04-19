/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * UI module - Terminal output formatting with adaptive width
 * Optimized for phones and tablets in both portrait and landscape
 */

#include "ui.h"
#include "atp.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdarg.h>

/* External global configuration */
extern atp_config_t g_config;

/* Terminal width */
static int g_term_width = 80;
static int g_initialized = 0;

/* Get terminal width dynamically */
static int get_terminal_width(void) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
        return w.ws_col;
    }
    return 80;  /* Default fallback */
}

/* Initialize UI module */
void ui_init(void) {
    if (g_initialized) return;
    
    g_term_width = get_terminal_width();
    /* Min 30, Max 200 (covers phones to large tablets) */
    if (g_term_width < 30) g_term_width = 30;
    if (g_term_width > 200) g_term_width = 200;
    g_initialized = 1;
}

/* Get current terminal width */
int ui_get_width(void) {
    if (!g_initialized) ui_init();
    return g_term_width;
}

/* Force set terminal width (for testing) */
void ui_set_width(int width) {
    g_term_width = width;
    if (g_term_width < 30) g_term_width = 30;
    if (g_term_width > 200) g_term_width = 200;
    g_initialized = 1;
}

/* Ensure UI is initialized */
static void ensure_init(void) {
    if (!g_initialized) ui_init();
}

/* Get adaptive label width based on terminal size */
static int get_label_width(void) {
    if (g_term_width >= 110) return 16;   /* Large tablet landscape / desktop */
    if (g_term_width >= 80)  return 14;   /* Small tablet landscape / large tablet portrait */
    if (g_term_width >= 60)  return 12;   /* Phone landscape / small tablet portrait */
    if (g_term_width >= 40)  return 10;   /* Phone portrait */
    return 8;                              /* Very small screen */
}

/* Helper: truncate string to fit width */
static void truncate_string(const char *src, char *dst, int max_len) {
    int len = strlen(src);
    if (len <= max_len) {
        strcpy(dst, src);
    } else if (max_len > 3) {
        strncpy(dst, src, max_len - 3);
        dst[max_len - 3] = '\0';
        strcat(dst, "...");
    } else {
        dst[0] = '\0';
    }
}

/* Print section header with adaptive formatting */
static void print_section_header(const char *title) {
    int title_len = strlen(title);
    
    /* For narrow screens, use simple format */
    if (g_term_width < 60) {
        printf("\n=== %s ===\n", title);
        return;
    }
    
    /* For wider screens, use padded format */
    int available = g_term_width - 4;
    int total_pad = available - title_len;
    int left_pad = total_pad / 2;
    int right_pad = total_pad - left_pad;
    
    printf("\n");
    for (int i = 0; i < left_pad; i++) printf("=");
    printf(" %s ", title);
    for (int i = 0; i < right_pad; i++) printf("=");
    printf("\n");
}

/* Print aligned row with label and value */
static void print_row(const char *label, const char *value, int indent) {
    int label_width = get_label_width();
    int max_value_width = g_term_width - indent - label_width - 4;
    if (max_value_width < 10) max_value_width = 10;
    
    char truncated_value[512];
    truncate_string(value, truncated_value, max_value_width);
    
    /* Print indent */
    for (int i = 0; i < indent; i++) printf(" ");
    
    /* Print label and value */
    printf("%-*s  %s\n", label_width, label, truncated_value);
}

/* ============================================ */
/* Basic output functions                       */
/* ============================================ */

void ui_title(const char *title) {
    ensure_init();
    printf("\n" COLOR_CYAN "=== %s ===\n" COLOR_RESET, title);
}

void ui_subtitle(const char *subtitle) {
    ensure_init();
    printf(COLOR_CYAN "--- %s ---\n" COLOR_RESET, subtitle);
}

void ui_separator(void) {
    ensure_init();
    printf(COLOR_CYAN);
    for (int i = 0; i < g_term_width; i++) printf("-");
    printf(COLOR_RESET "\n");
}

void ui_blank(void) {
    printf("\n");
}

/* ============================================ */
/* Table drawing functions                      */
/* ============================================ */

void ui_table_begin(void) {
    ensure_init();
    /* Empty - header will be printed by ui_table_header */
}

void ui_table_sep(void) {
    ensure_init();
    /* Empty - no separator needed in plain text mode */
}

void ui_table_header(const char *title) {
    ensure_init();
    print_section_header(title);
}

void ui_table_row(const char *label, const char *value) {
    ensure_init();
    print_row(label, value, 2);
}

void ui_table_row_color(const char *label, const char *value, const char *color) {
    ensure_init();
    int label_width = get_label_width();
    int max_value_width = g_term_width - 2 - label_width - 4;
    if (max_value_width < 10) max_value_width = 10;
    
    char truncated_value[512];
    truncate_string(value, truncated_value, max_value_width);
    
    printf("  %s%-*s" COLOR_RESET "  %s\n", color, label_width, label, truncated_value);
}

void ui_table_subrow(const char *prefix, const char *label, const char *value) {
    ensure_init();
    char combined_label[128];
    snprintf(combined_label, sizeof(combined_label), "%s%s", prefix, label);
    print_row(combined_label, value, 4);
}

void ui_table_subrow_color(const char *prefix, const char *label, const char *value, const char *color) {
    ensure_init();
    int label_width = get_label_width();
    int max_value_width = g_term_width - 4 - label_width - 4;
    if (max_value_width < 10) max_value_width = 10;
    
    char truncated_value[512];
    truncate_string(value, truncated_value, max_value_width);
    
    char combined_label[128];
    snprintf(combined_label, sizeof(combined_label), "%s%s", prefix, label);
    
    printf("    %s%-*s" COLOR_RESET "  %s\n", color, label_width, combined_label, truncated_value);
}

void ui_table_subrow_int(const char *prefix, const char *label, int value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    ui_table_subrow(prefix, label, buf);
}

void ui_table_subrow_emoji(const char *emoji, const char *label, const char *value) {
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%s ", emoji);
    ui_table_subrow(prefix, label, value);
}

void ui_table_subrow_emoji_color(const char *emoji, const char *label, const char *value, const char *color) {
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%s ", emoji);
    ui_table_subrow_color(prefix, label, value, color);
}

void ui_table_warning(const char *message) {
    ensure_init();
    int max_len = g_term_width - 4;
    char truncated[512];
    truncate_string(message, truncated, max_len);
    printf("  " COLOR_YELLOW "WARN: %s" COLOR_RESET "\n", truncated);
}

void ui_table_error(const char *message) {
    ensure_init();
    int max_len = g_term_width - 4;
    char truncated[512];
    truncate_string(message, truncated, max_len);
    printf("  " COLOR_RED "ERROR: %s" COLOR_RESET "\n", truncated);
}

void ui_table_info(const char *message) {
    ensure_init();
    int max_len = g_term_width - 4;
    char truncated[512];
    truncate_string(message, truncated, max_len);
    printf("  " COLOR_CYAN "INFO: %s" COLOR_RESET "\n", truncated);
}

void ui_table_end(void) {
    ensure_init();
    printf("\n");
}

/* ============================================ */
/* Status indicator functions                   */
/* ============================================ */

void ui_status_ok(const char *label) {
    ui_table_subrow_emoji_color(ui_emoji_ok(), label, "OK", COLOR_GREEN);
}

void ui_status_fail(const char *label) {
    ui_table_subrow_emoji_color(ui_emoji_fail(), label, "FAIL", COLOR_RED);
}

void ui_status_warn(const char *label) {
    ui_table_subrow_emoji_color(ui_emoji_warning(), label, "WARN", COLOR_YELLOW);
}

void ui_status_off(const char *label) {
    ui_table_subrow_emoji_color("○", label, "OFF", COLOR_WHITE);
}

void ui_status_async(const char *label) {
    ui_table_subrow_emoji_color("⏳", label, "ASYNC", COLOR_YELLOW);
}

/* ============================================ */
/* Simple message output functions              */
/* ============================================ */

void ui_info(const char *fmt, ...) {
    va_list args;
    printf(COLOR_CYAN);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf(COLOR_RESET "\n");
}

void ui_success(const char *fmt, ...) {
    va_list args;
    printf(COLOR_GREEN);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf(COLOR_RESET "\n");
}

void ui_warn(const char *fmt, ...) {
    va_list args;
    printf(COLOR_YELLOW);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf(COLOR_RESET "\n");
}

void ui_error(const char *fmt, ...) {
    va_list args;
    printf(COLOR_RED);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf(COLOR_RESET "\n");
}

void ui_key_value(const char *key, const char *value) {
    printf("  %-20s: %s\n", key, value);
}

void ui_key_value_color(const char *key, const char *value, const char *color) {
    printf("  %-20s: %s%s%s\n", key, color, value, COLOR_RESET);
}

/* ============================================ */
/* Banner output                                */
/* ============================================ */

void ui_banner(void) {
    printf("\033[1;36m"
    "    ___  __________  ____ \n"
    "   /   |/_  __/ __ \\/ __ \\\n"
    "  / /| | / / / /_/ / / / /\n"
    " / ___ |/ / / ____/ /_/ / \n"
    "/_/  |_/_/ /_/    /_____/  \033[0m\n");
    ui_separator();
}

void ui_banner_with_version(const char *version) {
    printf("\033[1;36m"
    "    ___  __________  ____ \n"
    "   /   |/_  __/ __ \\/ __ \\\n"
    "  / /| | / / / /_/ / / / /\n"
    " / ___ |/ / / ____/ /_/ / \n"
    "/_/  |_/_/ /_/    /_____/  v%s\033[0m\n", version);
    ui_separator();
}

/* ============================================ */
/* Emoji helpers (adaptive based on config)     */
/* ============================================ */

const char* ui_emoji_ok(void) { 
    return g_config.ui_emoji_enabled ? "✓" : "[OK]"; 
}

const char* ui_emoji_fail(void) { 
    return g_config.ui_emoji_enabled ? "✗" : "[FAIL]"; 
}

const char* ui_emoji_warning(void) { 
    return g_config.ui_emoji_enabled ? "⚠" : "[WARN]"; 
}

const char* ui_emoji_info(void) { 
    return g_config.ui_emoji_enabled ? "ℹ" : "[INFO]"; 
}

const char* ui_emoji_success(void) { 
    return g_config.ui_emoji_enabled ? "✅" : "[OK]"; 
}

const char* ui_emoji_vpn(int connected) {
    if (g_config.ui_emoji_enabled) {
        return connected ? "🔒" : "🔓";
    }
    return "[VPN]";
}

const char* ui_emoji_service(int running) {
    if (g_config.ui_emoji_enabled) {
        return running ? "🚀" : "⏹️";
    }
    return running ? "[RUNNING]" : "[STOPPED]";
}

const char* ui_emoji_mobile(void) { 
    return g_config.ui_emoji_enabled ? "📱" : "[MOBILE]"; 
}

const char* ui_emoji_wifi(int connected) {
    if (g_config.ui_emoji_enabled) {
        return connected ? "📶" : "⚠";
    }
    return "[WIFI]";
}

const char* ui_emoji_hotspot(void) { 
    return g_config.ui_emoji_enabled ? "🔥" : "[HOTSPOT]"; 
}

const char* ui_emoji_usb(void) { 
    return g_config.ui_emoji_enabled ? "🔌" : "[USB]"; 
}

const char* ui_emoji_app_filter(void) { 
    return g_config.ui_emoji_enabled ? "📱" : "[APP]"; 
}

const char* ui_emoji_mac_filter(void) { 
    return g_config.ui_emoji_enabled ? "🔢" : "[MAC]"; 
}

const char* ui_emoji_geo_bypass(void) { 
    return g_config.ui_emoji_enabled ? "🌏" : "[CN]"; 
}

const char* ui_emoji_download(void) { 
    return g_config.ui_emoji_enabled ? "📥" : "[RX]"; 
}

const char* ui_emoji_upload(void) { 
    return g_config.ui_emoji_enabled ? "📤" : "[TX]"; 
}

const char* ui_emoji_speed_up(void) { 
    return g_config.ui_emoji_enabled ? "📈" : "[RX SPD]"; 
}

const char* ui_emoji_speed_down(void) { 
    return g_config.ui_emoji_enabled ? "📉" : "[TX SPD]"; 
}

const char* ui_emoji_temperature(void) { 
    return g_config.ui_emoji_enabled ? "🌡️" : "[TEMP]"; 
}

const char* ui_emoji_uptime(void) { 
    return g_config.ui_emoji_enabled ? "⏱️" : "[UPTIME]"; 
}

const char* ui_emoji_cpu(void) { 
    return g_config.ui_emoji_enabled ? "⚙️" : "[CPU]"; 
}

const char* ui_emoji_memory(void) { 
    return g_config.ui_emoji_enabled ? "💾" : "[MEM]"; 
}
