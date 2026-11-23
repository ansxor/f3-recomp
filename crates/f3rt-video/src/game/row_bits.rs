//! Packed texels and row coverage masks.

/// Extract `len` low bits starting at column `x` from a packed bit array.
pub fn extract_columns(words: &[u32], x: i32, len: u32) -> u32 {
    let word = (x as u32) >> 5;
    let shift = (x as u32) & 31;
    let mut bits = words[word as usize] >> shift;
    if shift + len > 32 {
        bits |= words[word as usize + 1] << (32 - shift);
    }
    bits & ((1u32 << len) - 1)
}

/// Clear `bits` starting at column `x`.
pub fn clear_columns(words: &mut [u32], x: i32, bits: u32) {
    let word = (x as u32) >> 5;
    let shift = (x as u32) & 31;
    words[word as usize] &= !(bits << shift);
    if shift > 16 {
        words[word as usize + 1] &= !(bits >> (32 - shift));
    }
}

/// Set `bits` starting at column `x`.
pub fn set_columns(words: &mut [u32], x: i32, bits: u32) {
    let word = (x as u32) >> 5;
    let shift = (x as u32) & 31;
    words[word as usize] |= bits << shift;
    if shift > 16 {
        words[word as usize + 1] |= bits >> (32 - shift);
    }
}

/// Visit each run of set bits as `(first, length)`.
pub fn for_each_run(mut bits: u32, mut body: impl FnMut(u32, u32)) {
    while bits != 0 {
        let first = bits.trailing_zeros();
        let shifted = !(bits >> first);
        body(
            first,
            if shifted != 0 {
                shifted.trailing_zeros()
            } else {
                32 - first
            },
        );
        bits &= bits.wrapping_add(1u32 << first);
    }
}

/// One glyph row's four pen groups as a big-endian word (or the mirrored packing).
pub fn glyph_row_pens(row: &[u8], mirrored: bool) -> u32 {
    let word = u32::from_le_bytes([row[0], row[1], row[2], row[3]]);
    if !mirrored {
        return word.swap_bytes();
    }
    (word >> 4 & 0x0f0f_0f0f) | (word & 0x0f0f_0f0f) << 4
}

/// Bit `4k` set when nibble `k` is nonzero.
pub fn nonzero_nibbles(pens: u32) -> u32 {
    let mut flags = pens | pens >> 1;
    flags = (flags | flags >> 2) & 0x1111_1111; // bit 4k: nibble k is nonzero
    flags = (flags | flags >> 3) & 0x0303_0303;
    flags = (flags | flags >> 6) & 0x000f_000f;
    (flags | flags >> 12) & 0xff
}

/// A 16-pixel pen row split into four aligned 32-bit groups.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PenRow {
    words: [u32; 4],
}

impl PenRow {
    pub fn new(row: &[u8], pen_mask: u8) -> Self {
        let mask = u32::from(pen_mask) * 0x0101_0101;
        let mut words = [0u32; 4];
        for (i, word) in words.iter_mut().enumerate() {
            *word =
                u32::from_le_bytes([row[4 * i], row[4 * i + 1], row[4 * i + 2], row[4 * i + 3]])
                    & mask;
        }
        Self { words }
    }

    pub fn group(&self, group: usize, mirrored: bool) -> u32 {
        if mirrored {
            self.words[3 - group].swap_bytes()
        } else {
            self.words[group]
        }
    }

    pub fn blank(&self) -> bool {
        (self.words[0] | self.words[1] | self.words[2] | self.words[3]) == 0
    }

    pub fn all_opaque(&self) -> bool {
        (nonzero(self.words[0])
            & nonzero(self.words[1])
            & nonzero(self.words[2])
            & nonzero(self.words[3]))
            == 0x8080_8080
    }

    pub fn opaque(&self, mirrored: bool) -> u32 {
        let gather = |group: usize, mirrored: bool| {
            let flags = (self.words[group].wrapping_add(0x7f7f_7f7f) >> 7) & 0x0101_0101;
            let multiplier = if mirrored { 0x0804_0201 } else { 0x0102_0408 };
            flags.wrapping_mul(multiplier) >> 24
        };
        if !mirrored {
            return gather(0, false)
                | gather(1, false) << 4
                | gather(2, false) << 8
                | gather(3, false) << 12;
        }
        gather(3, true) | gather(2, true) << 4 | gather(1, true) << 8 | gather(0, true) << 12
    }
}

fn nonzero(word: u32) -> u32 {
    word.wrapping_add(0x7f7f_7f7f) & 0x8080_8080
}

#[cfg(test)]
mod tests {
    use super::*;

    proptest::proptest! {
        #![proptest_config(proptest::test_runner::Config::with_cases(64))]
        #[test]
        fn column_updates_match_a_flat_bitset(
            start in 0i32..=48,
            bits in proptest::prelude::any::<u16>(),
            previous in proptest::prelude::any::<u64>(),
        ) {
            use proptest::prelude::*;
            let from_flat = |v: u64| [v as u32, (v >> 32) as u32];
            let mask = u64::from(bits) << start;
            let mut words = from_flat(previous);
            prop_assert_eq!(extract_columns(&words, start, 16), (previous >> start) as u16 as u32);
            set_columns(&mut words, start, u32::from(bits));
            prop_assert_eq!(words, from_flat(previous | mask));
            clear_columns(&mut words, start, u32::from(bits));
            prop_assert_eq!(words, from_flat(previous & !mask));
        }

        #[test]
        fn pen_row_packs_masked_texels_in_both_directions(
            pens in proptest::array::uniform16(0u8..64),
            mask in 0u8..64,
        ) {
            use proptest::prelude::*;
            let packed = PenRow::new(&pens, mask);
            for flipped in [false, true] {
                let unpacked = [0, 1, 2, 3].map(|i| packed.group(i, flipped).to_le_bytes()).into_iter().flatten();
                let visible = (0..16).fold(0, |bits, i| {
                    let source = if flipped { 15 - i } else { i };
                    bits | (u32::from(pens[source] & mask != 0) << i)
                });
                prop_assert!(unpacked.enumerate().all(|(i, pen)| pen == pens[if flipped { 15 - i } else { i }] & mask), "packed texels differ from the masked input");
                prop_assert_eq!(packed.opaque(flipped), visible);
                prop_assert_eq!(packed.blank(), visible == 0);
                prop_assert_eq!(packed.all_opaque(), visible == 0xffff);
            }
        }
    }

    #[test]
    fn extract_spans_words() {
        let words = [0b1011u32, 0];
        assert_eq!(extract_columns(&words, 0, 4), 0b1011);
        assert_eq!(extract_columns(&words, 1, 2), 0b01);
        let words = [0xffff_ffffu32, 0x0000_000f];
        assert_eq!(extract_columns(&words, 30, 4), 0x0f);
    }

    #[test]
    fn clear_and_set_spans_words() {
        let mut words = [0xffff_ffffu32, 0xffff_ffff];
        clear_columns(&mut words, 30, 0b11);
        assert_eq!(words[0], 0x3fff_ffff);
        assert_eq!(words[1], 0xffff_ffff);

        let mut words = [0u32, 0];
        set_columns(&mut words, 30, 0b10);
        assert_eq!(words[0], 0x8000_0000);
        assert_eq!(words[1], 0);
        set_columns(&mut words, 31, 0b10);
        assert_eq!(words[0], 0x8000_0000);
        assert_eq!(words[1], 0b1);
    }

    #[test]
    fn runs_are_reported_with_lengths() {
        let mut runs = Vec::new();
        for_each_run(0b1011, |first, len| runs.push((first, len)));
        assert_eq!(runs, [(0, 2), (3, 1)]);

        let mut runs = Vec::new();
        for_each_run(0xffff_ffff, |first, len| runs.push((first, len)));
        assert_eq!(runs, [(0, 32)]);

        let mut runs = Vec::new();
        for_each_run(0, |first, len| runs.push((first, len)));
        assert!(runs.is_empty());
    }

    #[test]
    fn glyph_rows_byte_swap_and_mirror() {
        let row = [0x12, 0x34, 0x56, 0x78];
        assert_eq!(glyph_row_pens(&row, false), 0x1234_5678);
        assert_eq!(glyph_row_pens(&row, true), 0x8765_4321);
    }

    #[test]
    fn nonzero_nibbles_flags() {
        assert_eq!(nonzero_nibbles(0), 0);
        assert_eq!(nonzero_nibbles(0x0000_0005), 0b0001);
        assert_eq!(nonzero_nibbles(0x0000_3005), 0b1001);
        assert_eq!(nonzero_nibbles(0xffff_ffff), 0xff);
    }

    #[test]
    fn pen_row_masks_groups_and_opacity() {
        let mut row = [0u8; 16];
        row[0..4].copy_from_slice(&[0x01, 0x02, 0x03, 0x04]);
        let pens = PenRow::new(&row, 0x0f);
        assert_eq!(pens.group(0, false), 0x0403_0201);
        assert_eq!(pens.group(0, true), 0);
        assert!(!pens.blank());
        assert!(!pens.all_opaque());
        assert_eq!(pens.opaque(false), 0x000f);

        let full = [0xffu8; 16];
        let pens = PenRow::new(&full, 0x0f);
        assert!(pens.all_opaque());
        assert_eq!(pens.opaque(false), 0xffff);
        assert_eq!(pens.opaque(true), 0xffff);
        assert_eq!(pens.group(0, false), 0x0f0f_0f0f);
        assert_eq!(pens.group(0, true), 0x0f0f_0f0f);

        let blank = PenRow::new(&[0u8; 16], 0x0f);
        assert!(blank.blank());
        assert!(!blank.all_opaque());
        assert_eq!(blank.opaque(false), 0);
    }
}
