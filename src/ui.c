/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * UI module - Terminal output formatting with adaptive width
 */

#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdarg.h>

/* Terminal width */
static int g_term_width = 80;
static int g_content_width = 0;
static int g_initialized = 0;
static int g_in_table = 0;

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
    /* Min 60, Max 120 */
    if (g_term_width < 60) g_term_width = 60;
    if (g_term_width > 120) g_term_width = 120;
    g_content_width = g_term_width - 4;  /* Account for borders */
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
    if (g_term_width < 60) g_term_width = 60;
    if (g_term_width > 120) g_term_width = 120;
    g_content_width = g_term_width - 4;
    g_initialized = 1;
}

/* Ensure UI is initialized */
static void ensure_init(void) {
    if (!g_initialized) ui_init();
}

/* Helper: truncate string to fit width */
static void truncate_string(const char *src, char *dst, int max_len) {
    int len = strlen(src);
    if (len <= max_len) {
        strcpy(dst, src);
    } else {
        strncpy(dst, src, max_len - 3);
        dst[max_len - 3] = '\0';
        strcat(dst, "...");
    }
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
    for (int i = 0; i < g_term_width; i++) printf("─");
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
    g_in_table = 1;
    
    /* Draw top border */
    printf(COLOR_CYAN "┌");
    for (int i = 0; i < g_term_width - 2; i++) printf("─");
    printf("┐\n" COLOR_RESET);
}

void ui_table_sep(void) {
    ensure_init();
    printf(COLOR_CYAN "├");
    for (int i = 0; i < g_term_width - 2; i++) printf("─");
    printf("┤\n" COLOR_RESET);
}

void ui_table_header(const char *title) {
    ensure_init();
    int title_len = strlen(title);
    int padding = (g_term_width - title_len - 4) / 2;
    
    printf(COLOR_CYAN "│");
    for (int i = 0; i < padding; i++) printf(" ");
    printf("%s", title);
    for (int i = 0; i < g_term_width - title_len - padding - 4; i++) printf(" ");
    printf("│\n" COLOR_RESET);
    ui_table_sep();
}

void ui_table_row(const char *label, const char *value) {
    ensure_init();
    int label_width = 15;
    int value_width = g_content_width - label_width - 2;
    char truncated_value[256];
    
    truncate_string(value, truncated_value, value_width);
    
    printf("│ %-*s │ %-*s │\n", label_width, label, value_width, truncated_value);
}

void ui_table_row_color(const char *label, const char *value, const char *color) {
    ensure_init();
    int label_width = 15;
    int value_width = g_content_width - label_width - 2;
    char truncated_value[256];
    
    truncate_string(value, truncated_value, value_width);
    
    printf("│ %-*s │ %s%-*s" COLOR_RESET " │\n", 
           label_width, label, color, value_width, truncated_value);
}

void ui_table_subrow(const char *prefix, const char *label, const char *value) {
    ensure_init();
    int prefix_len = strlen(prefix);
    int label_width = 13 - prefix_len;
    int value_width = g_content_width - 15 - 2;
    char truncated_value[256];
    
    truncate_string(value, truncated_value, value_width);
    
    printf("│  %s%-*s │ %-*s │\n", 
           prefix, label_width, label, value_width, truncated_value);
}

void ui_table_subrow_color(const char *prefix, const char *label, const char *value, const char *color) {
    ensure_init();
    int prefix_len = strlen(prefix);
    int label_width = 13 - prefix_len;
    int value_width = g_content_width - 15 - 2;
    char truncated_value[256];
    
    truncate_string(value, truncated_value, value_width);
    
    printf("│  %s%-*s │ %s%-*s" COLOR_RESET " │\n", 
           prefix, label_width, label, color, value_width, truncated_value);
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
    char truncated[256];
    int max_len = g_content_width - 2;
    truncate_string(message, truncated, max_len);
    printf("│ %s⚠ %-*s" COLOR_RESET " │\n", COLOR_YELLOW, max_len - 4, truncated);
}

void ui_table_error(const char *message) {
    ensure_init();
    char truncated[256];
    int max_len = g_content_width - 2;
    truncate_string(message, truncated, max_len);
    printf("│ %s✗ %-*s" COLOR_RESET " │\n", COLOR_RED, max_len - 4, truncated);
}

void ui_table_info(const char *message) {
    ensure_init();
    char truncated[256];
    int max_len = g_content_width - 2;
    truncate_string(message, truncated, max_len);
    printf("│ %sℹ %-*s" COLOR_RESET " │\n", COLOR_CYAN, max_len - 4, truncated);
}

void ui_table_end(void) {
    ensure_init();
    /* Draw bottom border */
    printf(COLOR_CYAN "└");
    for (int i = 0; i < g_term_width - 2; i++) printf("─");
    printf("┘\n" COLOR_RESET);
    g_in_table = 0;
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
/* Emoji helpers                                */
/* ============================================ */

/* Status emojis */
const char* ui_emoji_ok(void) { return "✓"; }
const char* ui_emoji_fail(void) { return "✗"; }
const char* ui_emoji_warning(void) { return "⚠"; }
const char* ui_emoji_info(void) { return "ℹ"; }
const char* ui_emoji_success(void) { return "✅"; }

/* VPN emojis */
const char* ui_emoji_vpn(int connected) {
    return connected ? "🔒" : "🔓";
}

const char* ui_emoji_service(int running) {
    return running ? "🚀" : "⏹️";
}

/* Network interface emojis */
const char* ui_emoji_mobile(void) { return "📱"; }
const char* ui_emoji_wifi(int connected) {
    return connected ? "📶" : "⚠";
}
const char* ui_emoji_hotspot(void) { return "🔥"; }
const char* ui_emoji_usb(void) { return "🔌"; }

/* Filter emojis */
const char* ui_emoji_app_filter(void) { return "📱"; }
const char* ui_emoji_mac_filter(void) { return "🔢"; }
const char* ui_emoji_geo_bypass(void) { return "🌏"; }

/* Traffic emojis */
const char* ui_emoji_download(void) { return "📥"; }
const char* ui_emoji_upload(void) { return "📤"; }
const char* ui_emoji_speed_up(void) { return "📈"; }
const char* ui_emoji_speed_down(void) { return "📉"; }

/* System emojis */
const char* ui_emoji_temperature(void) { return "🌡️"; }
const char* ui_emoji_uptime(void) { return "⏱️"; }
const char* ui_emoji_cpu(void) { return "⚙️"; }
const char* ui_emoji_memory(void) { return "💾"; }
