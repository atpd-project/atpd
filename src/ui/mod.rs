//! Terminal UI helpers and layout formatting for ATPD.
//!
//! Mirrors `src/ui.c` and `include/ui.h` as pure string/writer builders
//! without direct stdout access.

pub mod dashboard;

pub const COLOR_RESET: &str = "\x1b[0m";
pub const COLOR_RED: &str = "\x1b[1;31m";
pub const COLOR_GREEN: &str = "\x1b[1;32m";
pub const COLOR_YELLOW: &str = "\x1b[1;33m";
pub const COLOR_CYAN: &str = "\x1b[1;36m";

pub fn clamp_width(width: usize) -> usize {
    width.clamp(30, 200)
}

pub fn label_width(width: usize) -> usize {
    if width >= 110 {
        16
    } else if width >= 80 {
        14
    } else if width >= 60 {
        12
    } else if width >= 40 {
        10
    } else {
        8
    }
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
        writeln!(
            self.out,
            "\n{}=== {} ==={}",
            self.style(COLOR_CYAN),
            title,
            self.reset()
        )
    }

    pub fn blank(&mut self) -> std::fmt::Result {
        writeln!(self.out)
    }

    pub fn table_header(&mut self, emoji: &str, title: &str) -> std::fmt::Result {
        let icon = if self.emoji_enabled && !emoji.is_empty() {
            emoji
        } else {
            ""
        };
        let space = if !icon.is_empty() { " " } else { "" };
        if self.width < 60 {
            writeln!(self.out, "\n=== {}{}{} ===", icon, space, title)?;
            return Ok(());
        }

        // Titles are ASCII; each supplied icon occupies two terminal columns.
        let title_len = title.len() + if !icon.is_empty() { 3 } else { 0 };
        let total_pad = if self.width as isize - 4 - title_len as isize > 0 {
            (self.width as isize - 4 - title_len as isize) as usize
        } else {
            0
        };
        let left = total_pad / 2;
        let right = total_pad - left;

        writeln!(self.out)?;
        for _ in 0..left {
            write!(self.out, "=")?;
        }
        write!(self.out, " {}{}{} ", icon, space, title)?;
        for _ in 0..right {
            write!(self.out, "=")?;
        }
        writeln!(self.out)?;
        writeln!(self.out)?;
        Ok(())
    }

    pub fn table_row_color(&mut self, label: &str, value: &str, color: &str) -> std::fmt::Result {
        if self.width < 60 {
            let max_value = (self.width as isize - 4 - label.len() as isize).max(1) as usize;
            let value_buf = truncate_utf8(value, max_value);
            writeln!(
                self.out,
                "  {}{}{}: {}",
                self.style(color),
                label,
                self.reset(),
                value_buf
            )
        } else {
            let label_len = label_width(self.width);
            let max_value = (self.width as isize - label_len as isize - 6).max(10) as usize;
            let value_buf = truncate_utf8(value, max_value);
            writeln!(
                self.out,
                "  {}{:<width$}{}  {}",
                self.style(color),
                label,
                self.reset(),
                value_buf,
                width = label_len
            )
        }
    }

    pub fn table_subrow(&mut self, _prefix: &str, label: &str, value: &str) -> std::fmt::Result {
        if self.width < 60 {
            let max_value = (self.width as isize - 6 - label.len() as isize).max(1) as usize;
            let value_buf = truncate_utf8(value, max_value);
            writeln!(self.out, "    {}: {}", label, value_buf)
        } else {
            let label_len = label_width(self.width);
            let max_value = (self.width as isize - label_len as isize - 8).max(10) as usize;
            let value_buf = truncate_utf8(value, max_value);
            writeln!(
                self.out,
                "    {:<width$}  {}",
                label,
                value_buf,
                width = label_len
            )
        }
    }

    pub fn table_subrow_color(
        &mut self,
        _prefix: &str,
        label: &str,
        value: &str,
        color: &str,
    ) -> std::fmt::Result {
        if self.width < 60 {
            let max_value = (self.width as isize - 6 - label.len() as isize).max(1) as usize;
            let value_buf = truncate_utf8(value, max_value);
            writeln!(
                self.out,
                "    {}{}{}: {}",
                self.style(color),
                label,
                self.reset(),
                value_buf
            )
        } else {
            let label_len = label_width(self.width);
            let max_value = (self.width as isize - label_len as isize - 8).max(10) as usize;
            let value_buf = truncate_utf8(value, max_value);
            writeln!(
                self.out,
                "    {}{:<width$}{}  {}",
                self.style(color),
                label,
                self.reset(),
                value_buf,
                width = label_len
            )
        }
    }

    pub fn table_subrow_int(&mut self, prefix: &str, label: &str, value: i32) -> std::fmt::Result {
        self.table_subrow(prefix, label, &value.to_string())
    }

    pub fn table_end(&mut self) -> std::fmt::Result {
        writeln!(self.out)
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
