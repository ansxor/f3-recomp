use std::ops::Range;

use f3rt_sprites::commandw::{
    EmittedEntry, FullDetailRecord, emit_full_detail_record, scroll_bias,
};
use f3rt_sprites::presentation::group_key_class;

use crate::decode::{DecodedSpriteEntry, decode_emitted_entries};
use crate::game::sprites::scene_sprite;
use crate::game::{MAX_PRESENTED_SPRITES, SceneSprite};

const BODY_GROUP_CLASS: u32 = 1;

pub trait FullDetailLookup {
    fn full_detail_record(&self, key: u64, group_x: i32, group_y: i32) -> Option<FullDetailRecord>;
}

#[derive(Clone, Debug, Default)]
pub struct FullDetailStats {
    pub frames: u64,
    pub expanded_objects: u64,
    pub expanded_sprites: u64,
}

pub struct CommandwFullDetailStage {
    visible: Range<i32>,
    emitted: Vec<EmittedEntry>,
    decoded: Vec<DecodedSpriteEntry>,
    output: Vec<SceneSprite>,
    stats: FullDetailStats,
}

impl CommandwFullDetailStage {
    pub fn new(visible_y: i32, visible_height: i32) -> Self {
        Self {
            visible: visible_y..visible_y + visible_height,
            emitted: Vec::with_capacity(256),
            decoded: vec![DecodedSpriteEntry::default(); 512],
            output: Vec::with_capacity(MAX_PRESENTED_SPRITES),
            stats: FullDetailStats::default(),
        }
    }

    pub fn stats(&self) -> &FullDetailStats {
        &self.stats
    }

    pub fn reset(&mut self) {
        self.emitted.clear();
        self.output.clear();
    }

    pub fn report(&self) -> String {
        format!(
            "full detail: frames={} expanded_objects={} expanded_sprites={}\n",
            self.stats.frames, self.stats.expanded_objects, self.stats.expanded_sprites
        )
    }

    pub fn present(
        &mut self,
        sprites: &mut Vec<SceneSprite>,
        lookup: &dyn FullDetailLookup,
        rom: &[u8],
        flipped: bool,
    ) {
        self.stats.frames += 1;
        self.output.clear();
        let mut remaining = sprites.as_slice();
        while let Some((run, rest)) = SpriteRun::take(remaining) {
            let room = MAX_PRESENTED_SPRITES - self.output.len();
            if let Some((group, record)) = run.full_detail(lookup)
                && let Some(decoded) = self.decode_record(record, group, rom, flipped)
            {
                let count = decoded.min(room);
                self.output
                    .extend(self.decoded[..count].iter().map(scene_sprite));
                self.stats.expanded_objects += 1;
                self.stats.expanded_sprites += count as u64;
            } else {
                self.output
                    .extend_from_slice(&run.0[..run.0.len().min(room)]);
            }
            remaining = rest;
        }
        std::mem::swap(sprites, &mut self.output);
    }

    fn decode_record(
        &mut self,
        record: FullDetailRecord,
        group: u64,
        rom: &[u8],
        flipped: bool,
    ) -> Option<usize> {
        self.emitted.clear();
        emit_full_detail_record(rom, record, group, &mut self.emitted)?;
        if self.decoded.len() < self.emitted.len() {
            self.decoded
                .resize(self.emitted.len(), DecodedSpriteEntry::default());
        }
        let (bias_x, bias_y) = scroll_bias(flipped);
        Some(decode_emitted_entries(
            &self.emitted,
            self.visible.clone(),
            bias_x,
            bias_y,
            flipped,
            true,
            &mut self.decoded,
        ))
    }
}

struct SpriteRun<'a>(&'a [SceneSprite]);

impl<'a> SpriteRun<'a> {
    fn take(sprites: &'a [SceneSprite]) -> Option<(Self, &'a [SceneSprite])> {
        let (head, tail) = sprites.split_first()?;
        let count = if group_key_class(head.group) == BODY_GROUP_CLASS {
            1 + tail
                .iter()
                .take_while(|member| {
                    member.group == head.group
                        && member.group_x == head.group_x
                        && member.group_y == head.group_y
                })
                .count()
        } else {
            1
        };
        let (run, rest) = sprites.split_at(count);
        Some((Self(run), rest))
    }

    fn full_detail(&self, cache: &dyn FullDetailLookup) -> Option<(u64, FullDetailRecord)> {
        let head = &self.0[0];
        if group_key_class(head.group) != BODY_GROUP_CLASS {
            return None;
        }
        let record = cache.full_detail_record(head.group, head.group_x, head.group_y)?;
        Some((head.group, record))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use f3rt_sprites::commandw::{DESC_TABLE_ADDR, ProjectedBody, SIZE_OFFSET_TABLE_ADDR};
    use f3rt_sprites::presentation::pack_group_key;

    struct StaticLookup(Option<(u64, i32, i32, FullDetailRecord)>);

    impl FullDetailLookup for StaticLookup {
        fn full_detail_record(
            &self,
            key: u64,
            group_x: i32,
            group_y: i32,
        ) -> Option<FullDetailRecord> {
            let (k, gx, gy, rec) = self.0?;
            (k == key && gx == group_x && gy == group_y).then_some(rec)
        }
    }

    fn rom_with_2x2_desc() -> Vec<u8> {
        let mut rom = vec![0u8; 0x0d0000];
        let desc_ptr = 0x010000usize;
        rom[DESC_TABLE_ADDR + 4..DESC_TABLE_ADDR + 8]
            .copy_from_slice(&(desc_ptr as u32).to_be_bytes());
        rom[desc_ptr..desc_ptr + 2].copy_from_slice(&1u16.to_be_bytes());
        rom[desc_ptr + 2..desc_ptr + 4].copy_from_slice(&1u16.to_be_bytes());
        let hdr = desc_ptr + 4;
        for (index, delta) in [10u16, 10, 14].into_iter().enumerate() {
            rom[hdr + index * 2..hdr + index * 2 + 2].copy_from_slice(&delta.to_be_bytes());
        }
        for (i, tile) in [10u16, 11, 12, 13].iter().enumerate() {
            rom[hdr + 6 + i * 2..hdr + 8 + i * 2].copy_from_slice(&tile.to_be_bytes());
        }
        let zero_tbl = 0x020000usize;
        for dim in 0..8usize {
            rom[SIZE_OFFSET_TABLE_ADDR + dim * 4..SIZE_OFFSET_TABLE_ADDR + dim * 4 + 4]
                .copy_from_slice(&(zero_tbl as u32).to_be_bytes());
        }
        rom
    }

    fn sprite_fields(sprite: &SceneSprite) -> impl PartialEq + std::fmt::Debug + use<> {
        (
            (
                sprite.x,
                sprite.y,
                sprite.scale_x,
                sprite.scale_y,
                sprite.tile,
                sprite.palette,
            ),
            (
                sprite.flip_x,
                sprite.flip_y,
                sprite.group,
                sprite.group_x,
                sprite.group_y,
                sprite.group_pc,
                sprite.blend_weight,
            ),
        )
    }

    fn body(group_x: i32, group_y: i32, flip: u8) -> FullDetailRecord {
        FullDetailRecord {
            flags0: 0x80 | flip,
            color: 5,
            desc: 1,
            projected: ProjectedBody {
                screen_x: 248 + group_x as i16,
                screen_y: 120 + group_y as i16,
                zoom_x: 0,
                zoom_y: 0,
            },
        }
    }

    fn expanded_grid(group: u64, record: FullDetailRecord, flipped: bool) -> [SceneSprite; 4] {
        let source_x = record.flags0 & 1 != 0;
        let source_y = record.flags0 & 2 != 0;
        std::array::from_fn(|i| {
            let column = i / 2;
            let row = i % 2;
            let tile = 10 + 2 * (column ^ usize::from(source_x)) + (row ^ usize::from(source_y));
            let mut x = i32::from(record.projected.screen_x) + 16 * column as i32;
            let mut y = i32::from(record.projected.screen_y) + 16 * row as i32;
            if flipped {
                x = 512 - 16 - (x + 50);
                y = 256 - 16 - (y - 16);
            } else {
                x -= 50;
                y += 17;
            }
            SceneSprite {
                x: x * 256,
                y: y * 256,
                scale_x: 256,
                scale_y: 256,
                tile: tile as u32,
                palette: record.color,
                flip_x: source_x ^ flipped,
                flip_y: source_y ^ flipped,
                group,
                group_x: i32::from(record.projected.screen_x) * 256,
                group_y: i32::from(record.projected.screen_y) * 256,
                group_pc: f3rt_sprites::commandw::BODY_SINGLE_PC,
                blend_weight: 1.0,
            }
        })
    }

    type RecordKeys = std::collections::BTreeMap<(u64, i32, i32), FullDetailRecord>;

    impl FullDetailLookup for RecordKeys {
        fn full_detail_record(
            &self,
            key: u64,
            group_x: i32,
            group_y: i32,
        ) -> Option<FullDetailRecord> {
            self.get(&(key, group_x, group_y)).copied()
        }
    }

    fn identity(sprite: &SceneSprite) -> (u64, i32, i32) {
        (sprite.group, sprite.group_x, sprite.group_y)
    }

    proptest::proptest! {
        #![proptest_config(proptest::test_runner::Config::with_cases(384))]

        #[test]
        fn presentation_matches_ordered_run_replacement_and_truncation(
            batches in proptest::collection::vec((0u32..4, 0i32..3, 0i32..3, 1usize..5), 0..50),
            availability in proptest::num::u32::ANY,
            flipped in proptest::bool::ANY,
            overflow_gap in proptest::option::of(0usize..8),
        ) {
            let rom = rom_with_2x2_desc();
            let mut stage = CommandwFullDetailStage::new(16, 224);
            let mut sprites = Vec::new();
            let mut lookup = RecordKeys::new();
            let mut expected_expansions = 0;
            let mut expected_tiles = 0;
            for frame in [flipped, !flipped] {
                sprites.clear();
                lookup.clear();
                let prefix = overflow_gap.map_or(0, |gap| MAX_PRESENTED_SPRITES - gap);
                let hud = SceneSprite { tile: 90, x: 1, y: 2, blend_weight: 0.25, ..Default::default() };
                sprites.resize(prefix, hud);
                for (class, x, y, count) in batches.iter().copied().chain([(1, 0, 0, 2), (0, 0, 0, 1), (1, 0, 0, 1)]) {
                    let key = match class {
                        0 => 0,
                        3 => pack_group_key(0x410100, 1, 0, 0),
                        _ => pack_group_key(0x410000, class, 0, 0),
                    };
                    let seed = (class * 9 + (x * 3 + y) as u32) % 32;
                    if class % 2 == 1 && ((availability >> seed) & 1 != 0 || class == 1 && x == 0 && y == 0) {
                        lookup.insert((key, x, y), body(x, y, seed as u8 & 3));
                    }
                    for _ in 0..count {
                        sprites.push(SceneSprite { tile: 1000 + sprites.len() as u32, group: key,
                            group_x: x, group_y: y, ..hud });
                    }
                }
                let mut expected = Vec::new();
                for run in sprites.chunk_by(|a, b| identity(a) == identity(b)) {
                    if let Some(record) = lookup.get(&identity(&run[0])) {
                        expected_expansions += 1;
                        expected_tiles += MAX_PRESENTED_SPRITES.saturating_sub(expected.len()).min(4) as u64;
                        expected.extend(expanded_grid(run[0].group, *record, frame));
                    } else {
                        expected.extend_from_slice(run);
                    }
                }
                expected.truncate(MAX_PRESENTED_SPRITES);
                stage.present(&mut sprites, &lookup, &rom, frame);
                proptest::prop_assert_eq!(sprites.len(), expected.len());
                for (actual, expected) in sprites.iter().zip(&expected) {
                    proptest::prop_assert_eq!(sprite_fields(actual), sprite_fields(expected));
                }
                proptest::prop_assert_eq!(stage.stats().expanded_objects, expected_expansions);
                proptest::prop_assert_eq!(stage.stats().expanded_sprites, expected_tiles);
            }
            proptest::prop_assert_eq!(stage.stats().frames, 2);
        }

        #[test]
        fn emitted_entries_round_trip_scroll_flip_and_block_tags(
            flip in 0u8..4, composite in proptest::bool::ANY, anchor in proptest::bool::ANY,
            x in 225i16..260, y in 80i16..140,
        ) {
            use f3rt_sprites::commandw::{ANCHOR_OFFSET_TABLE_ADDR, BODY_MULTI_PC};
            let mut rom = rom_with_2x2_desc();
            let key = 0x4100000100010002;
            if composite {
                let ptr = 0x1000u32;
                rom[DESC_TABLE_ADDR + 8..DESC_TABLE_ADDR + 12].copy_from_slice(&ptr.to_be_bytes());
                rom[0x1000..0x100a].copy_from_slice(&[2, 0, 0, 1, 3, 1, 0, 1, 7, 2]);
            }
            rom[ANCHOR_OFFSET_TABLE_ADDR + 4..ANCHOR_OFFSET_TABLE_ADDR + 8].copy_from_slice(&[0, 5, 0xff, 0xfc]);
            let record = FullDetailRecord { desc: if composite { 2 } else { 1 }, flags0: 0x80 | flip | if anchor { 8 } else { 0 },
                projected: ProjectedBody { screen_x: x, screen_y: y, zoom_x: 0, zoom_y: 0 }, color: 5 };
            let mut emitted = Vec::new();
            emit_full_detail_record(&rom, record, key, &mut emitted).unwrap();
            for flipped in [false, true] {
                let mut decoded = [DecodedSpriteEntry::default(); 8];
                let (bias_x, bias_y) = scroll_bias(flipped);
                let count = decode_emitted_entries(&emitted, 16..240, bias_x, bias_y, flipped, true, &mut decoded);
                let parts: &[(u8, u8)] = if composite { &[(3, 1), (7, 2)] } else { &[(0, 0)] };
                proptest::prop_assert_eq!(count, parts.len() * 4);
                for ((color, xor), grid) in parts.iter().copied().zip(decoded[..count].as_chunks::<4>().0) {
                    let part_flip = flip ^ xor;
                    let mut part = body(0, 0, part_flip);
                    part.projected.screen_x = x;
                    part.projected.screen_y = y;
                    part.color += color;
                    let dx = if part_flip & 1 == 0 { -5 } else { 5 };
                    let dy = if part_flip & 2 == 0 { 4 } else { -4 };
                    if anchor { part.projected.screen_x += dx; part.projected.screen_y += dy; }
                    for (actual, mut expected) in grid.iter().map(scene_sprite).zip(expanded_grid(key, part, flipped)) {
                        expected.group_x = i32::from(x) * 256;
                        expected.group_y = i32::from(y) * 256;
                        if composite { expected.group_pc = BODY_MULTI_PC; }
                        proptest::prop_assert_eq!(sprite_fields(&actual), sprite_fields(&expected));
                    }
                }
            }
        }
    }

    #[test]
    fn collapses_body_run_into_single_expansion_and_preserves_other_classes() {
        let rom = rom_with_2x2_desc();
        let body_key = pack_group_key(0x410080, 1, 0, 0);
        let shadow_key = pack_group_key(0x410080, 2, 0, 0);
        let lookup = StaticLookup(Some((
            body_key,
            160 << 8,
            120 << 8,
            FullDetailRecord {
                flags0: 0x80,
                color: 5,
                desc: 1,
                projected: ProjectedBody {
                    screen_x: 160,
                    screen_y: 120,
                    zoom_x: 0,
                    zoom_y: 0,
                },
            },
        )));

        let hud = SceneSprite {
            tile: 99,
            group: 0,
            ..SceneSprite::default()
        };
        let shadow = SceneSprite {
            tile: 77,
            group: shadow_key,
            ..SceneSprite::default()
        };
        let coarse_a = SceneSprite {
            tile: 1,
            group: body_key,
            group_x: 160 << 8,
            group_y: 120 << 8,
            ..SceneSprite::default()
        };
        let coarse_b = SceneSprite {
            tile: 2,
            ..coarse_a
        };

        let mut sprites = vec![hud, shadow, coarse_a, coarse_b];
        let mut stage = CommandwFullDetailStage::new(32, 224);
        stage.present(&mut sprites, &lookup, &rom, false);

        assert_eq!(sprites.len(), 6);
        assert_eq!(sprites[0].tile, 99);
        assert_eq!(sprites[1].tile, 77);
        assert_eq!(
            sprites[2..].iter().map(|s| s.tile).collect::<Vec<_>>(),
            vec![10, 11, 12, 13]
        );
        assert!(sprites[2..].iter().all(|s| s.group == body_key));
        assert_eq!(stage.stats().expanded_objects, 1);
        assert_eq!(stage.stats().expanded_sprites, 4);
    }
}
