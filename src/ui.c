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
#include <wchar.h>
#include <locale.h>

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
    /* Fallback: try environment variable */
    char *columns = getenv("COLUMNS");
    if (columns) {
        int cols = atoi(columns);
        if (cols > 0) return cols;
    }
    return 80;
}

/* Count display width of a string (handles CJK and emoji) */
static int str_display_width(const char *str) {
    int width = 0;
    const char *p = str;
    
    /* Set locale for wide character support */
    static int locale_set = 0;
    if (!locale_set) {
        setlocale(LC_ALL, "C.UTF-8");
        locale_set = 1;
    }
    
    while (*p) {
        unsigned char c = (unsigned char)*p;
        
        /* UTF-8 multi-byte characters */
        if (c >= 0xF0) {
            /* 4-byte UTF-8 (includes many emojis) */
            width += 2;  /* Most emojis take 2 columns */
            p += 4;
        } else if (c >= 0xE0) {
            /* 3-byte UTF-8 (CJK characters) */
            wchar_t wc;
            if (mbtowc(&wc, p, 3) > 0) {
                /* East Asian wide characters */
                if (wc >= 0x1100 && wc <= 0x115F) width += 2;      /* Hangul */
                else if (wc >= 0x2E80 && wc <= 0xA4CF) width += 2; /* CJK */
                else if (wc >= 0xAC00 && wc <= 0xD7A3) width += 2; /* Hangul */
                else if (wc >= 0xF900 && wc <= 0xFAFF) width += 2; /* CJK */
                else if (wc >= 0xFF00 && wc <= 0xFF60) width += 2; /* Fullwidth */
                else width += 1;
            } else {
                width += 1;
            }
            p += 3;
        } else if (c >= 0xC0) {
            /* 2-byte UTF-8 */
            width += 1;
            p += 2;
        } else if (c == 0x1B) {
            /* ANSI escape sequence - skip entirely */
            const char *start = p;
            p++;
            if (*p == '[') {
                p++;
                while (*p && (*p < 'A' || *p > 'Z') && (*p < 'a' || *p > 'z')) {
                    p++;
                }
                if (*p) p++;
            }
            /* No width contribution from escape sequences */
            (void)start;
        } else {
            /* ASCII */
            width += 1;
            p++;
        }
    }
    return width;
}

/* Truncate string to fit display width */
static void truncate_string_display(const char *src, char *dst, int max_width) {
    int src_width = str_display_width(src);
    
    if (src_width <= max_width) {
        strcpy(dst, src);
        return;
    }
    
    /* Need to truncate, leave room for "..." */
    int target_width = max_width - 3;
    if (target_width <= 0) {
        strcpy(dst, "...");
        return;
    }
    
    const char *p = src;
    char *d = dst;
    int current_width = 0;
    int last_copy_pos = 0;
    
    setlocale(LC_ALL, "C.UTF-8");
    
    while (*p && current_width < target_width) {
        unsigned char c = (unsigned char)*p;
        int char_width;
        int bytes;
        
        if (c >= 0xF0) {
            char_width = 2;
            bytes = 4;
        } else if (c >= 0xE0) {
            wchar_t wc;
            if (mbtowc(&wc, p, 3) > 0) {
                if (wc >= 0x1100 && wc <= 0x115F) char_width = 2;
                else if (wc >= 0x2E80 && wc <= 0xA4CF) char_width = 2;
                else if (wc >= 0xAC00 && wc <= 0xD7A3) char_width = 2;
                else if (wc >= 0xF900 && wc <= 0xFAFF) char_width = 2;
                else if (wc >= 0xFF00 && wc <= 0xFF60) char_width = 2;
                else char_width = 1;
            } else {
                char_width = 1;
            }
            bytes = 3;
        } else if (c >= 0xC0) {
            char_width = 1;
            bytes = 2;
        } else {
            char_width = 1;
            bytes = 1;
        }
        
        if (current_width + char_width > target_width) {
            break;
        }
        
        for (int i = 0; i < bytes; i++) {
            *d++ = *p++;
        }
        current_width += char_width;
        last_copy_pos = d - dst;
    }
    
    /* Add ellipsis */
    strcpy(d, "...");
}

/* Initialize UI module */
void ui_init(void) {
    if (g_initialized) return;
    
    setlocale(LC_ALL, "C.UTF-8");
    g_term_width = get_terminal_width();
    /* Min 40, Max 160 */
    if (g_term_width < 40) g_term_width = 40;
    if (g_term_width > 160) g_term_width = 160;
    g_content_width = g_term_width - 4;
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
    if (g_term_width < 40) g_term_width = 40;
    if (g_term_width > 160) g_term_width = 160;
    g_content_width = g_term_width - 4;
    g_initialized = 1;
}

/* Ensure UI is initialized */
static void ensure_init(void) {
    if (!g_initialized) ui_init();
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
    int title_width = str_display_width(title);
    int available_width = g_term_width - 4;
    int padding = (available_width - title_width) / 2;
    if (padding < 0) padding = 0;
    
    printf(COLOR_CYAN "│");
    for (int i = 0; i < padding; i++) printf(" ");
    printf("%s", title);
    int right_padding = available_width - title_width - padding;
    for (int i = 0; i < right_padding; i++) printf(" ");
    printf("│\n" COLOR_RESET);
    
    ui_table_sep();
}

void ui_table_row(const char *label, const char *value) {
    ensure_init();
    int label_width = 15;
    int value_width = g_content_width - label_width - 3;
    char truncated_value[512];
    
    truncate_string_display(value, truncated_value, value_width);
    
    printf("│ %-*s │ %-*s │\n", label_width, label, value_width, truncated_value);
}

void ui_table_row_color(const char *label, const char *value, const char *color) {
    ensure_init();
    int label_width = 15;
    int value_width = g_content_width - label_width - 3;
    char truncated_value[512];
    
    truncate_string_display(value, truncated_value, value_width);
    
    printf("│ %-*s │ %s%-*s" COLOR_RESET " │\n", 
           label_width, label, color, value_width, truncated_value);
}

void ui_table_subrow(const char *prefix, const char *label, const char *value) {
    ensure_init();
    int prefix_width = str_display_width(prefix);
    int label_width = 13;
    int value_width = g_content_width - 15 - 3;
    char truncated_value[512];
    
    truncate_string_display(value, truncated_value, value_width);
    
    printf("│  %s%-*s │ %-*s │\n", 
           prefix, label_width - prefix_width, label, value_width, truncated_value);
}

void ui_table_subrow_color(const char *prefix, const char *label, const char *value, const char *color) {
    ensure_init();
    int prefix_width = str_display_width(prefix);
    int label_width = 13;
    int value_width = g_content_width - 15 - 3;
    char truncated_value[512];
    
    truncate_string_display(value, truncated_value, value_width);
    
    printf("│  %s%-*s │ %s%-*s" COLOR_RESET " │\n", 
           prefix, label_width - prefix_width, label, color, value_width, truncated_value);
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
    char truncated[512];
    int max_len = g_content_width - 4;
    truncate_string_display(message, truncated, max_len);
    printf("│ %s⚠ %-*s" COLOR_RESET " │\n", COLOR_YELLOW, max_len, truncated);
}

void ui_table_error(const char *message) {
    ensure_init();
    char truncated[512];
    int max_len = g_content_width - 4;
    truncate_string_display(message, truncated, max_len);
    printf("│ %s✗ %-*s" COLOR_RESET " │\n", COLOR_RED, max_len, truncated);
}

void ui_table_info(const char *message) {
    ensure_init();
    char truncated[512];
    int max_len = g_content_width - 4;
    truncate_string_display(message, truncated, max_len);
    printf("│ %sℹ %-*s" COLOR_RESET " │\n", COLOR_CYAN, max_len, truncated);
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

const char* ui_emoji_ok(void) { return "✓"; }
const char* ui_emoji_fail(void) { return "✗"; }
const char* ui_emoji_warning(void) { return "⚠"; }
const char* ui_emoji_info(void) { return "ℹ"; }
const char* ui_emoji_success(void) { return "✅"; }

const char* ui_emoji_vpn(int connected) {
    return connected ? "🔒" : "🔓";
}

const char* ui_emoji_service(int running) {
    return running ? "🚀" : "⏹️";
}

const char* ui_emoji_mobile(void) { return "📱"; }
const char* ui_emoji_wifi(int connected) {
    return connected ? "📶" : "⚠";
}
const char* ui_emoji_hotspot(void) { return "🔥"; }
const char* ui_emoji_usb(void) { return "🔌"; }

const char* ui_emoji_app_filter(void) { return "📱"; }
const char* ui_emoji_mac_filter(void) { return "🔢"; }
const char* ui_emoji_geo_bypass(void) { return "🌏"; }

const char* ui_emoji_download(void) { return "📥"; }
const char* ui_emoji_upload(void) { return "📤"; }
const char* ui_emoji_speed_up(void) { return "📈"; }
const char* ui_emoji_speed_down(void) { return "📉"; }

const char* ui_emoji_temperature(void) { return "🌡️"; }
const char* ui_emoji_uptime(void) { return "⏱️"; }
const char* ui_emoji_cpu(void) { return "⚙️"; }
const char* ui_emoji_memory(void) { return "💾"; }
