#ifndef ATP_UI_H
#define ATP_UI_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

/* Color definitions (compatible with logger.h) */
#ifndef COLOR_RESET
#define COLOR_RESET     "\033[0m"
#endif
#ifndef COLOR_RED
#define COLOR_RED       "\033[1;31m"
#endif
#ifndef COLOR_GREEN
#define COLOR_GREEN     "\033[1;32m"
#endif
#ifndef COLOR_YELLOW
#define COLOR_YELLOW    "\033[1;33m"
#endif
#ifndef COLOR_CYAN
#define COLOR_CYAN      "\033[1;36m"
#endif
#ifndef COLOR_WHITE
#define COLOR_WHITE     "\033[0;37m"
#endif
#ifndef COLOR_BOLD
#define COLOR_BOLD      "\033[1m"
#endif

/* ============================================ */
/* Initialization                               */
/* ============================================ */

/* Initialize UI module (auto-detects terminal width) */
void ui_init(void);

/* Get current terminal width */
int ui_get_width(void);

/* Force set terminal width (for testing) */
void ui_set_width(int width);

/* Set custom output stream (default: stdout) */
void ui_set_output_file(FILE *fp);
void ui_set_emoji_enabled(int enable);

/* ============================================ */
/* Basic output functions (auto-formatted)     */
/* ============================================ */

/* Print a title/header line */
void ui_title(const char *title);

/* Print a subtitle line */
void ui_subtitle(const char *subtitle);

/* Print a separator line */
void ui_separator(void);

/* Print a blank line */
void ui_blank(void);

/* ============================================ */
/* Table drawing functions                      */
/* ============================================ */

/* Start a new table */
void ui_table_begin(void);

/* Draw table separator line */
void ui_table_sep(void);

/* Draw table header with centered title */
void ui_table_header(const char *title);

/* Draw two-column table row */
void ui_table_row(const char *label, const char *value);
void ui_table_row_color(const char *label, const char *value, const char *color);

/* Draw sub-row with prefix (├─, └─, etc.) */
void ui_table_subrow(const char *prefix, const char *label, const char *value);
void ui_table_subrow_color(const char *prefix, const char *label, const char *value, const char *color);
void ui_table_subrow_int(const char *prefix, const char *label, int value);

/* Draw sub-row with emoji prefix */
void ui_table_subrow_emoji(const char *emoji, const char *label, const char *value);
void ui_table_subrow_emoji_color(const char *emoji, const char *label, const char *value, const char *color);

/* Draw a warning/error/info row that spans both columns */
void ui_table_warning(const char *message);
void ui_table_error(const char *message);
void ui_table_info(const char *message);

/* End current table */
void ui_table_end(void);

/* ============================================ */
/* Status indicator functions                   */
/* ============================================ */

/* Print status indicator with emoji */
void ui_status_ok(const char *label);
void ui_status_fail(const char *label);
void ui_status_warn(const char *label);
void ui_status_off(const char *label);
void ui_status_async(const char *label);

/* ============================================ */
/* Simple message output functions              */
/* ============================================ */

/* Print colored info message */
void ui_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_success(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Print key-value pair */
void ui_key_value(const char *key, const char *value);
void ui_key_value_color(const char *key, const char *value, const char *color);

/* ============================================ */
/* Banner output                                */
/* ============================================ */

/* Print ATP logo banner (version must be printed separately) */
void ui_banner(void);

/* Print banner with version (convenience function) */
void ui_banner_with_version(const char *version);

/* ============================================ */
/* Emoji helpers (returns emoji string)        */
/* ============================================ */

/* Status emojis */
const char* ui_emoji_ok(void);
const char* ui_emoji_fail(void);
const char* ui_emoji_warning(void);
const char* ui_emoji_info(void);
const char* ui_emoji_success(void);

/* VPN emojis */
const char* ui_emoji_vpn(int connected);
const char* ui_emoji_service(int running);

/* Network interface emojis */
const char* ui_emoji_mobile(void);
const char* ui_emoji_wifi(int connected);
const char* ui_emoji_hotspot(void);
const char* ui_emoji_usb(void);

/* Filter emojis */
const char* ui_emoji_app_filter(void);
const char* ui_emoji_mac_filter(void);
const char* ui_emoji_geo_bypass(void);

/* Traffic emojis */
const char* ui_emoji_download(void);
const char* ui_emoji_upload(void);
const char* ui_emoji_speed_up(void);
const char* ui_emoji_speed_down(void);

/* System emojis */
const char* ui_emoji_temperature(void);
const char* ui_emoji_uptime(void);
const char* ui_emoji_cpu(void);
const char* ui_emoji_memory(void);

void ui_set_no_color(int enable);
int ui_get_no_color(void);
#endif /* ATP_UI_H */
