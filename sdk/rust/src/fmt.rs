//! A fixed-size, stack-allocated string builder.
//!
//! There is no allocator in a basic Gearbox mod and `alloc::format!` therefore
//! does not exist, but every panel wants to draw `"Turn 12"`. [`Buf`] is the
//! smallest thing that gets you there: an array plus a length, with `push_*`
//! methods that append and silently stop at capacity.
//!
//! **Overflow truncates, it never panics.** That is deliberate and it matches
//! the host, which truncates log lines at 2048 bytes, panel text at 512 and
//! button labels at 64 without complaint. A mod that traps because a label got
//! long is a worse mod than one that draws a short label. Call
//! [`Buf::overflowed`] if you need to know it happened.

use core::fmt::Write;

/// A `N`-byte UTF-8 buffer. `N` is bytes, not characters.
///
/// ```ignore
/// let mut line = Buf::<64>::new();
/// line.push_str("Turn ").push_u64(turn as u64);
/// panel.text(8, 8, Color::WHITE, line.as_str());
/// ```
pub struct Buf<const N: usize> {
    buf: [u8; N],
    len: usize,
    overflow: bool,
}

impl<const N: usize> Default for Buf<N> {
    fn default() -> Self {
        Self::new()
    }
}

impl<const N: usize> Buf<N> {
    /// Empty buffer. `const`, so it can initialise a `static`.
    pub const fn new() -> Self {
        Buf {
            buf: [0u8; N],
            len: 0,
            overflow: false,
        }
    }

    /// Reset to empty, including the overflow flag. Reuse one buffer per hook
    /// rather than declaring several: `N` bytes of stack are `N` bytes you are
    /// not spending on anything else, and the default WASM stack is 1 MiB of a
    /// budget you declared in `MANIFEST.json`.
    pub fn clear(&mut self) -> &mut Self {
        self.len = 0;
        self.overflow = false;
        self
    }

    /// Bytes written so far.
    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Always `N`.
    pub fn capacity(&self) -> usize {
        N
    }

    /// True if any push since the last [`clear`](Self::clear) did not fit.
    pub fn overflowed(&self) -> bool {
        self.overflow
    }

    /// The bytes written so far.
    pub fn as_bytes(&self) -> &[u8] {
        // `get(..).unwrap_or` rather than `&self.buf[..self.len]`: the indexing
        // form carries a panic branch, and a panic branch in a `no_std` mod
        // drags the panic machinery into the binary for a bound that cannot be
        // violated anyway. `self.len <= N` is an invariant of every method here.
        self.buf.get(..self.len).unwrap_or(&[])
    }

    /// The bytes written so far, as `&str`.
    ///
    /// Never fails: `push_str` only ever copies whole characters and the
    /// numeric pushes are ASCII, so the contents are always valid UTF-8. The
    /// validation here is cheap insurance, not a real branch — and it is
    /// `unwrap_or("")` rather than `unwrap()` so that no panic path exists.
    pub fn as_str(&self) -> &str {
        core::str::from_utf8(self.as_bytes()).unwrap_or("")
    }

    /// Append a single byte. Caller is responsible for UTF-8 validity; the
    /// numeric helpers use this with ASCII only.
    fn push_byte(&mut self, b: u8) -> &mut Self {
        if self.len < N {
            self.buf[self.len] = b;
            self.len += 1;
        } else {
            self.overflow = true;
        }
        self
    }

    /// Append a string, or as much of it as fits.
    ///
    /// When it does not all fit, the copy backs off to the nearest character
    /// boundary rather than slicing a multi-byte codepoint in half. Half a
    /// codepoint would make [`as_str`](Self::as_str) return the wrong thing and
    /// would draw as a replacement glyph.
    pub fn push_str(&mut self, s: &str) -> &mut Self {
        let room = N - self.len;
        let mut take = if s.len() <= room { s.len() } else { room };
        if take < s.len() {
            self.overflow = true;
            while take > 0 && !s.is_char_boundary(take) {
                take -= 1;
            }
        }
        for &b in s.as_bytes().iter().take(take) {
            self.push_byte(b);
        }
        self
    }

    /// Append base-10 digits.
    pub fn push_u64(&mut self, mut v: u64) -> &mut Self {
        if v == 0 {
            return self.push_byte(b'0');
        }
        // u64::MAX is 20 digits.
        let mut tmp = [0u8; 20];
        let mut n = 0;
        while v > 0 {
            tmp[n] = b'0' + (v % 10) as u8;
            v /= 10;
            n += 1;
        }
        while n > 0 {
            n -= 1;
            self.push_byte(tmp[n]);
        }
        self
    }

    /// Append base-10 digits with a leading `-` when negative.
    pub fn push_i64(&mut self, v: i64) -> &mut Self {
        if v < 0 {
            self.push_byte(b'-');
            // `unsigned_abs` rather than `-v`: negating i64::MIN overflows, and
            // in debug builds that is a panic.
            self.push_u64(v.unsigned_abs())
        } else {
            self.push_u64(v as u64)
        }
    }

    /// Append the integer part of a float, sign included. Fractional digits are
    /// discarded, which is what the C example does with a treasury.
    ///
    /// `core` has no `f64::abs` (that lives in `std`, on top of libm) so the
    /// sign is handled by comparison and negation, both of which are core.
    pub fn push_f64_trunc(&mut self, v: f64) -> &mut Self {
        if v.is_nan() {
            return self.push_str("nan");
        }
        let neg = v < 0.0;
        let a = if neg { -v } else { v };
        if neg {
            self.push_byte(b'-');
        }
        if a.is_infinite() || a >= 18_000_000_000_000_000_000.0 {
            return self.push_str("inf");
        }
        // Float-to-int casts saturate in Rust (since 1.45), so the guard above
        // is for legibility, not soundness.
        self.push_u64(a as u64)
    }

    /// Append a float with a fixed number of decimals (capped at 9), truncating
    /// rather than rounding. `push_f64_fixed(1234.5, 2)` gives `1234.50`.
    pub fn push_f64_fixed(&mut self, v: f64, decimals: u32) -> &mut Self {
        let d = if decimals > 9 { 9 } else { decimals };
        if d == 0 || v.is_nan() {
            return self.push_f64_trunc(v);
        }
        let mut scale = 1u64;
        let mut i = 0;
        while i < d {
            scale *= 10;
            i += 1;
        }

        let neg = v < 0.0;
        let a = if neg { -v } else { v };
        let scaled = a * scale as f64;
        if a.is_infinite() || scaled >= 18_000_000_000_000_000_000.0 {
            // Out of u64 range once scaled; fall back to the integer part.
            return self.push_f64_trunc(v);
        }
        if neg {
            self.push_byte(b'-');
        }
        let n = scaled as u64;
        self.push_u64(n / scale);
        self.push_byte(b'.');
        // Zero-pad the fraction: 1234.5 scaled by 100 is 123450, and 50 must
        // print as "50" while 1234.05 must print as "05".
        let frac = n % scale;
        let mut pad = scale / 10;
        while pad > 1 && frac < pad {
            self.push_byte(b'0');
            pad /= 10;
        }
        self.push_u64(frac)
    }
}

/// `write!` support, for when a format string is genuinely easier to read.
///
/// Be aware of what it costs: `core::fmt` brings its own formatting machinery
/// into the module — several kilobytes of it, enough to dominate the size of a
/// small mod. The `push_*` methods above pull in nothing. Use `write!` where
/// clarity wins and the size does not.
///
/// `write_str` returns `Ok` even when the buffer overflows, so `write!(...)`
/// never fails and there is nothing to `unwrap`. Check
/// [`Buf::overflowed`] if truncation matters to you.
impl<const N: usize> Write for Buf<N> {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        self.push_str(s);
        Ok(())
    }
}
