//! Terminal UI helpers and layout formatting for ATPD.
//!
//! Mirrors `src/ui.c` and `include/ui.h` as pure string/writer builders
//! without direct stdout access.

pub mod dashboard;

pub const COLOR_RESET: &str = "\x1b[0m";
pub const COLOR_BOLD: &str = "\x1b[1m";
pub const COLOR_DIM: &str = "\x1b[2m";
pub const COLOR_RED: &str = "\x1b[1;31m";
pub const COLOR_GREEN: &str = "\x1b[1;32m";
pub const COLOR_YELLOW: &str = "\x1b[1;33m";
pub const COLOR_CYAN: &str = "\x1b[1;36m";

pub fn clamp_width(width: usize) -> usize {
    width.clamp(30, 200)
}

pub fn label_width(width: usize) -> usize {
    if width >= 80 {
        16
    } else if width >= 60 {
        15
    } else if width >= 40 {
        12
    } else {
        10
    }
}

pub fn str_display_width(s: &str) -> usize {
    // Lightweight display-width helper: ASCII and common single-width box/symbol
    // characters count as 1 column, VS16/ZWJ as 0, and remaining non-ASCII (e.g.
    // emoji/CJK) as 2 columns. Switch to `unicode-width` if full Unicode tables
    // are ever required.
    s.chars()
        .map(|ch| match ch {
            '\u{FE0F}' | '\u{200D}' => 0,
            c if c.is_ascii() => 1,
            '\u{2500}'..='\u{257F}' | '\u{25A0}'..='\u{25FF}' | '°' => 1,
            _ => 2,
        })
        .sum()
}

pub fn truncate_utf8(src: &str, max_bytes: usize) -> String {
    if max_bytes == 0 {
        return String::new();
    }
    if src.len() <= max_bytes {
        return src.to_string();
    }
    if max_bytes <= 3 {
        return String::new();
    }
    let keep = max_bytes - 3;
    let mut used = 0;
    for (idx, ch) in src.char_indices() {
        let ch_len = ch.len_utf8();
        if idx + ch_len > keep {
            break;
        }
        used = idx + ch_len;
    }
    let mut res = String::with_capacity(used + 3);
    res.push_str(&src[..used]);
    res.push_str("...");
    res
}

pub struct UiRenderCtx<W> {
    pub out: W,
    pub width: usize,
    pub color_enabled: bool,
    pub emoji_enabled: bool,
}

impl<W: std::fmt::Write> UiRenderCtx<W> {
    pub fn new(out: W, width: usize, color_enabled: bool, emoji_enabled: bool) -> Self {
        let effective_width = if width == 0 { 80 } else { clamp_width(width) };
        Self {
            out,
            width: effective_width,
            color_enabled,
            emoji_enabled,
        }
    }

    fn style<'a>(&self, color: &'a str) -> &'a str {
        if self.color_enabled {
            color
        } else {
            ""
        }
    }

    fn reset(&self) -> &'static str {
        if self.color_enabled {
            COLOR_RESET
        } else {
            ""
        }
    }

    pub fn title(&mut self, title: &str) -> std::fmt::Result {
        let rule_width = if self.width < 60 {
            self.width
        } else {
            self.width.min(90)
        };
        writeln!(
            self.out,
            "\n{}{}{}",
            self.style(COLOR_CYAN),
            title,
            self.reset()
        )?;
        write!(self.out, "{}", self.style(COLOR_DIM))?;
        for _ in 0..rule_width {
            write!(self.out, "━")?;
        }
        writeln!(self.out, "{}", self.reset())
    }

    pub fn table_header(&mut self, emoji: &str, title: &str) -> std::fmt::Result {
        let icon = if self.emoji_enabled && !emoji.is_empty() {
            emoji
        } else {
            ""
        };
        let header_text = if !icon.is_empty() {
            format!("{} {}", icon, title)
        } else {
            title.to_string()
        };

        if self.width < 60 {
            return writeln!(
                self.out,
                "\n{}{}{}",
                self.style(COLOR_BOLD),
                header_text,
                self.reset()
            );
        }

        let text_width = str_display_width(&header_text);
        let divider_width = self.width.min(90);
        let trail_len = divider_width.saturating_sub(text_width + 1);

        writeln!(self.out)?;
        write!(
            self.out,
            "{}{}{}",
            self.style(COLOR_BOLD),
            header_text,
            self.reset()
        )?;
        if trail_len > 0 {
            write!(self.out, " {}", self.style(COLOR_DIM))?;
            for _ in 0..trail_len {
                write!(self.out, "─")?;
            }
            write!(self.out, "{}", self.reset())?;
        }
        writeln!(self.out)
    }

    pub fn table_row_color(&mut self, label: &str, value: &str, color: &str) -> std::fmt::Result {
        let dot = if self.emoji_enabled { "●" } else { "*" };
        if self.width < 60 {
            let label_w = str_display_width(label);
            let max_value = self.width.saturating_sub(6 + label_w).max(1);
            let value_buf = truncate_utf8(value, max_value);
            writeln!(
                self.out,
                "  {}{}{} {}{}:{} {}{}{}",
                self.style(color),
                dot,
                self.reset(),
                self.style(COLOR_DIM),
                label,
                self.reset(),
                self.style(color),
                value_buf,
                self.reset()
            )
        } else {
            let label_len = label_width(self.width);
            let actual_label_w = label_len.max(str_display_width(label));
            let max_value = self.width.saturating_sub(5 + actual_label_w).max(10);
            let value_buf = truncate_utf8(value, max_value);
            writeln!(
                self.out,
                "  {}{}{} {}{:<width$}{} {}{}{}",
                self.style(color),
                dot,
                self.reset(),
                self.style(COLOR_DIM),
                label,
                self.reset(),
                self.style(color),
                value_buf,
                self.reset(),
                width = label_len
            )
        }
    }

    pub fn table_subrow(&mut self, prefix: &str, label: &str, value: &str) -> std::fmt::Result {
        let branch = if prefix.is_empty() { "├─" } else { prefix };
        let branch_w = str_display_width(branch);
        if self.width < 60 {
            let label_w = str_display_width(label);
            let max_value = self.width.saturating_sub(5 + branch_w + label_w).max(1);
            let value_buf = truncate_utf8(value, max_value);
            writeln!(
                self.out,
                "  {}{} {}:{} {}",
                self.style(COLOR_DIM),
                branch,
                label,
                self.reset(),
                value_buf
            )
        } else {
            let label_len = label_width(self.width);
            let subrow_label_len = label_len.saturating_sub(branch_w.saturating_sub(1));
            let actual_label_w = subrow_label_len.max(str_display_width(label));
            let max_value = self
                .width
                .saturating_sub(4 + branch_w + actual_label_w)
                .max(10);
            let value_buf = truncate_utf8(value, max_value);
            writeln!(
                self.out,
                "  {}{} {:<width$}{} {}",
                self.style(COLOR_DIM),
                branch,
                label,
                self.reset(),
                value_buf,
                width = subrow_label_len
            )
        }
    }

    pub fn table_subrow_color(
        &mut self,
        prefix: &str,
        label: &str,
        value: &str,
        color: &str,
    ) -> std::fmt::Result {
        let branch = if prefix.is_empty() { "├─" } else { prefix };
        let branch_w = str_display_width(branch);
        if self.width < 60 {
            let label_w = str_display_width(label);
            let max_value = self.width.saturating_sub(5 + branch_w + label_w).max(1);
            let value_buf = truncate_utf8(value, max_value);
            writeln!(
                self.out,
                "  {}{} {}:{} {}{}{}",
                self.style(COLOR_DIM),
                branch,
                label,
                self.reset(),
                self.style(color),
                value_buf,
                self.reset()
            )
        } else {
            let label_len = label_width(self.width);
            let subrow_label_len = label_len.saturating_sub(branch_w.saturating_sub(1));
            let actual_label_w = subrow_label_len.max(str_display_width(label));
            let max_value = self
                .width
                .saturating_sub(4 + branch_w + actual_label_w)
                .max(10);
            let value_buf = truncate_utf8(value, max_value);
            writeln!(
                self.out,
                "  {}{} {:<width$}{} {}{}{}",
                self.style(COLOR_DIM),
                branch,
                label,
                self.reset(),
                self.style(color),
                value_buf,
                self.reset(),
                width = subrow_label_len
            )
        }
    }

    pub fn table_subrow_int(&mut self, prefix: &str, label: &str, value: i32) -> std::fmt::Result {
        self.table_subrow(prefix, label, &value.to_string())
    }

    pub fn table_end(&mut self) -> std::fmt::Result {
        Ok(())
    }

    pub fn emoji_vpn(&self, connected: bool) -> &'static str {
        if self.emoji_enabled {
            if connected {
                "🔒"
            } else {
                "🔓"
            }
        } else {
            "[VPN]"
        }
    }
}
