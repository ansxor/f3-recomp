//! Line-RAM decode and per-scanline scene rows, mechanically ported from
//! `runtime/renderer/game/lines.{hpp,cpp}`.

use crate::config::{Geometry, VideoConfig};
use crate::game::ram_shadow::{RamShadow, fresh_content_version};
use crate::game::scene::{SceneClip, SceneRow, SceneSource, VideoRam};
use crate::game::tiles::{FullResolutionAltMaps, GameTiles};
use crate::memory_map::{
    LINERAM_BANK_OFFSET, LINERAM_LATCH_STRIDE, LINERAM_SECTION_STRIDE, LINERAM_SECTIONS_OFFSET,
    LINERAM_SUBSECTION_STRIDE, OFFS_LINERAM,
};
use crate::state_io::{
    CanonicalLineParams, CanonicalRecord, CanonicalSceneRow, StateReader, StateWriter,
};

const LATCH_BYTES: usize = 8 * LINERAM_LATCH_STRIDE;
const SECTION_BYTES: usize = 8 * LINERAM_SECTION_STRIDE;
const CONTROL_BYTES: usize = 0x20;
const SCANLINES: usize = 256;

/// `GameLines::state_size()`: decoded line params plus the per-scanline scene rows.
pub const LINES_STATE_SIZE: usize =
    CanonicalLineParams::SIZE * SCANLINES + CanonicalSceneRow::SIZE * SCANLINES;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct LineClip {
    left: i16,
    right: i16,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct LinePivot {
    mix_value: u16,
    prio: u8,
    blend_mode: u8,
    clip_enable: u8,
    clip_inv: u8,
    clip_inv_mode: bool,
    blend_select_v: bool,
    x_sample_enable: bool,
    pivot_control: u8,
    pivot_enable: u16,
}

impl LinePivot {
    fn set_mix(&mut self, value: u16) {
        self.mix_value = value;
        self.prio = (value & 0x0f) as u8;
        self.blend_mode = ((value >> 14) & 3) as u8;
        self.clip_inv = ((value >> 4) & 0x0f) as u8;
        self.clip_enable = ((value >> 8) & 0x0f) as u8;
        self.clip_inv_mode = value & 0x1000 != 0;
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct LineSprite {
    mix_value: u16,
    prio: u8,
    blend_mode: u8,
    clip_enable: u8,
    clip_inv: u8,
    clip_inv_mode: bool,
    blend_select_v: bool,
    x_sample_enable: bool,
}

impl LineSprite {
    fn set_mix(&mut self, value: u16) {
        self.mix_value = value;
        self.prio = (value & 0x0f) as u8;
        self.blend_mode = ((value >> 14) & 3) as u8;
        self.clip_inv = ((value >> 4) & 0x0f) as u8;
        self.clip_enable = ((value >> 8) & 0x0f) as u8;
        self.clip_inv_mode = value & 0x1000 != 0;
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct LinePlayfield {
    mix_value: u16,
    prio: u8,
    blend_mode: u8,
    clip_enable: u8,
    clip_inv: u8,
    clip_inv_mode: bool,
    x_sample_enable: bool,
    /// bits 0-8 column scroll, bit 9 alternate map.
    colscroll: u16,
    x_scale: i32,
    y_scale: i32,
    pal_add: u16,
    rowscroll: i32,
}

impl Default for LinePlayfield {
    fn default() -> Self {
        Self {
            mix_value: 0,
            prio: 0,
            blend_mode: 0,
            clip_enable: 0,
            clip_inv: 0,
            clip_inv_mode: false,
            x_sample_enable: false,
            colscroll: 0,
            x_scale: 256,
            y_scale: 0,
            pal_add: 0,
            rowscroll: 0,
        }
    }
}

impl LinePlayfield {
    fn set_mix(&mut self, value: u16) {
        self.mix_value = value;
        self.prio = (value & 0x0f) as u8;
        self.blend_mode = ((value >> 14) & 3) as u8;
        self.clip_inv = ((value >> 4) & 0x0f) as u8;
        self.clip_enable = ((value >> 8) & 0x0f) as u8;
        self.clip_inv_mode = value & 0x1000 != 0;
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct LineParams {
    clip: [LineClip; 4],
    blend: [u8; 4],
    x_sample: u8,
    bg_palette: u16,
    /// Defaults match the FDP until a line RAM latch sets them; not serialized (lines are
    /// re-decoded from video RAM at every VBSTART).
    palette_15bit: bool,
    blur: bool,
    pivot: LinePivot,
    sp: [LineSprite; 4],
    pf: [LinePlayfield; 4],
}

impl Default for LineParams {
    fn default() -> Self {
        Self {
            clip: [LineClip::default(); 4],
            blend: [0; 4],
            x_sample: 16,
            bg_palette: 0,
            palette_15bit: true,
            blur: true,
            pivot: LinePivot::default(),
            sp: [LineSprite::default(); 4],
            pf: [LinePlayfield::default(); 4],
        }
    }
}

/// Presented alt-map rows (PF2/PF3 `alt_map` rows that draw) over the visible scanlines.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AltRowCounts {
    pub remapped: u64,
    pub fallback: u64,
}

pub struct GameLines {
    video: VideoConfig,
    geometry: Geometry,
    lines: [LineParams; SCANLINES],
    rows: [SceneRow; SCANLINES],
    control_0: [u16; 8],
    control_1: [u16; 8],
    shadow_latches: RamShadow<LATCH_BYTES>,
    shadow_sections: RamShadow<SECTION_BYTES>,
    shadow_control: RamShadow<CONTROL_BYTES>,
    rows_current: bool,
    rows_flipped: bool,
    decode_flipped: bool,
    rows_version: u64,
}

impl GameLines {
    pub fn new(video: &VideoConfig) -> Self {
        let mut lines = Self {
            video: *video,
            geometry: Geometry::from_video(video),
            lines: [LineParams::default(); SCANLINES],
            rows: [SceneRow::default(); SCANLINES],
            control_0: [0; 8],
            control_1: [0; 8],
            shadow_latches: RamShadow::new(),
            shadow_sections: RamShadow::new(),
            shadow_control: RamShadow::new(),
            rows_current: false,
            rows_flipped: false,
            decode_flipped: false,
            rows_version: fresh_content_version(),
        };
        lines.reset();
        lines
    }

    pub fn reset(&mut self) {
        self.control_0.fill(0);
        self.control_1.fill(0);
        for line in &mut self.lines {
            *line = LineParams::default();
        }
        for row in &mut self.rows {
            *row = SceneRow::default();
        }
        self.forget_decode();
    }

    /// Decode line RAM and FDP controls, mirroring `Video::read_line_ram`. Fields carry forward
    /// when a subsection latch is absent, so all 256 scanlines are walked in order with a
    /// persistent scratch line.
    pub fn decode(&mut self, vram: &VideoRam<'_>, flipped: bool) {
        let lineram = &vram.graphics[OFFS_LINERAM..];
        let latch_span: &[u8; LATCH_BYTES] = lineram[..LATCH_BYTES]
            .try_into()
            .expect("line RAM latch tables");
        let latches_changed = self.shadow_latches.changed(latch_span);
        let section_span: &[u8; SECTION_BYTES] = lineram
            [LINERAM_SECTIONS_OFFSET..LINERAM_SECTIONS_OFFSET + SECTION_BYTES]
            .try_into()
            .expect("line RAM section tables");
        let sections_changed = self.shadow_sections.changed(section_span);
        let control_span: &[u8; CONTROL_BYTES] = vram.control[..CONTROL_BYTES]
            .try_into()
            .expect("control register file");
        let control_changed = self.shadow_control.changed(control_span);
        if !latches_changed
            && !sections_changed
            && !control_changed
            && self.decode_flipped == flipped
        {
            return;
        }
        self.decode_flipped = flipped;
        self.rows_current = false;
        for i in 0..8 {
            self.control_0[i] = vram.control_u16(i);
            self.control_1[i] = vram.control_u16(8 + i);
        }

        let read_be16 = |at: usize| u16::from_be_bytes([lineram[at], lineram[at + 1]]);
        let latched_addr = |latches: &[u16; 8], section: usize, subsection: usize, y: usize| {
            let latch = latches[section];
            let base = LINERAM_SECTIONS_OFFSET
                + LINERAM_SECTION_STRIDE * section
                + LINERAM_SUBSECTION_STRIDE * subsection;
            if latch & (1 << (subsection + 4)) != 0 {
                Some(base + LINERAM_BANK_OFFSET + y * 2)
            } else if latch & (1 << subsection) != 0 {
                Some(base + y * 2)
            } else {
                None
            }
        };

        let mut line = LineParams::default();
        for screen_y in 0..SCANLINES {
            let y = if flipped {
                SCANLINES - 1 - screen_y
            } else {
                screen_y
            };
            let mut latches = [0u16; 8];
            for (s, latch) in latches.iter_mut().enumerate() {
                *latch = read_be16(s * LINERAM_LATCH_STRIDE + y * 2);
            }

            // 4000 (column scroll & clip-plane high bits for PF 2 and 3)
            for i in [2usize, 3] {
                if let Some(at) = latched_addr(&latches, 0, i, y) {
                    let colscroll = read_be16(at);
                    // Bit 9 carries the alternate-map select; scroll arithmetic masks to 9 bits,
                    // so the stored word keeps both.
                    line.pf[i].colscroll = (colscroll & 0x1ff)
                        | if self.video.alt_map_selectable() {
                            colscroll & 0x200
                        } else {
                            0
                        };
                    set_clip_upper(
                        &mut line.clip[2 * (i - 2)],
                        ((colscroll >> 12) & 1) as i8,
                        ((colscroll >> 13) & 1) as i8,
                    );
                    set_clip_upper(
                        &mut line.clip[2 * (i - 2) + 1],
                        ((colscroll >> 14) & 1) as i8,
                        ((colscroll >> 15) & 1) as i8,
                    );
                }
            }

            // 5000 (clip planes low 8 bits)
            for i in 0..4 {
                if let Some(at) = latched_addr(&latches, 1, i, y) {
                    let clip_lows = read_be16(at);
                    set_clip_lower(
                        &mut line.clip[i],
                        (clip_lows & 0xff) as u8,
                        ((clip_lows >> 8) & 0xff) as u8,
                    );
                }
            }

            // 6000 (sprite blend modes, pivot control, alpha values, mosaic, bg palette)
            if let Some(at) = latched_addr(&latches, 2, 0, y) {
                let value = read_be16(at);
                line.pivot.blend_select_v = value & 0x0200 != 0;
                line.pivot.pivot_control = ((value >> 8) & 0xff) as u8;
                for group in 0..4 {
                    let blend = ((value >> (group * 2)) & 3) as u8;
                    line.sp[group].mix_value =
                        (line.sp[group].mix_value & 0x3fff) | (u16::from(blend) << 14);
                    line.sp[group].blend_mode = blend;
                }
            }
            if let Some(at) = latched_addr(&latches, 2, 1, y) {
                let blend_vals = read_be16(at);
                for idx in 0..4 {
                    let alpha = ((blend_vals >> (4 * idx)) & 0x0f) as u8;
                    line.blend[idx] = 8.min(0x0f - alpha);
                }
            }
            if let Some(at) = latched_addr(&latches, 2, 2, y) {
                let x_mosaic = read_be16(at);
                line.x_sample = (16 - ((x_mosaic >> 4) & 0x0f)) as u8;
                for (pf_num, pf) in line.pf.iter_mut().enumerate() {
                    pf.x_sample_enable = x_mosaic & (1 << pf_num) != 0;
                }
                line.palette_15bit = x_mosaic & 0x4000 == 0;
                line.blur = x_mosaic & 0x2000 == 0;
                for sprite in &mut line.sp {
                    sprite.x_sample_enable = x_mosaic & 0x100 != 0;
                }
                line.pivot.x_sample_enable = x_mosaic & 0x200 != 0;
            }
            if let Some(at) = latched_addr(&latches, 2, 3, y) {
                line.bg_palette = read_be16(at);
            }

            // 7000 (pivot & sprite mixing / priority)
            if let Some(at) = latched_addr(&latches, 3, 0, y) {
                line.pivot.pivot_enable = read_be16(at);
            }
            if let Some(at) = latched_addr(&latches, 3, 1, y) {
                line.pivot.set_mix(read_be16(at));
            }
            if let Some(at) = latched_addr(&latches, 3, 2, y) {
                let sprite_mix = read_be16(at);
                for (group, sprite) in line.sp.iter_mut().enumerate() {
                    let value = (sprite.mix_value & 0xc00f) | ((sprite_mix & 0x3ff) << 4);
                    sprite.set_mix(value);
                    sprite.blend_select_v = (sprite_mix >> (12 + group)) & 1 != 0;
                }
            }
            if let Some(at) = latched_addr(&latches, 3, 3, y) {
                let sprite_prio = read_be16(at);
                for (group, sprite) in line.sp.iter_mut().enumerate() {
                    let prio = ((sprite_prio >> (group * 4)) & 0x0f) as u8;
                    sprite.mix_value = (sprite.mix_value & 0xfff0) | u16::from(prio);
                    sprite.prio = prio;
                }
            }

            // 8000 (playfield zoom)
            const FIX_Y: [usize; 4] = [0, 3, 2, 1];
            for (i, fix_y) in FIX_Y.iter().enumerate() {
                if let Some(at) = latched_addr(&latches, 4, i, y) {
                    let pf_scale = read_be16(at);
                    line.pf[i].x_scale = 256 - i32::from((pf_scale >> 8) & 0xff);
                    line.pf[*fix_y].y_scale = i32::from(pf_scale & 0xff) << 1;
                }
            }

            // 9000 (playfield palette add)
            for i in 0..4 {
                if let Some(at) = latched_addr(&latches, 5, i, y) {
                    line.pf[i].pal_add = read_be16(at).wrapping_mul(16);
                }
            }

            // A000 (playfield rowscroll)
            for i in 0..4 {
                if let Some(at) = latched_addr(&latches, 6, i, y) {
                    let rowscroll = i32::from(read_be16(at)) << (8 - 6);
                    line.pf[i].rowscroll = ((rowscroll as u32 & 0xffff_ff00)
                        .wrapping_sub(rowscroll as u32 & 0xff))
                        as i32;
                }
            }

            // B000 (playfield mixing info)
            for i in 0..4 {
                if let Some(at) = latched_addr(&latches, 7, i, y) {
                    line.pf[i].set_mix(read_be16(at));
                }
            }

            self.lines[screen_y] = line;
        }
    }

    fn get_pf_scroll(&self, pf_num: usize, flipped: bool) -> (i32, i32) {
        let mut sx_raw = self.control_0[pf_num] as i16;
        let mut sy_raw = self.control_0[pf_num + 4] as i16;

        sy_raw = sy_raw.wrapping_add(1 << 7); // 9.7 fixed point

        if flipped {
            sx_raw = sx_raw.wrapping_add(320 << 6);
            sx_raw = sx_raw.wrapping_add((512 + 192) << 6);
            sy_raw = sy_raw.wrapping_neg();
        }

        sx_raw = sx_raw.wrapping_add(((40 - 4 * pf_num) << 6) as i16);

        let mut sx = i32::from(sx_raw) << (8 - 6);
        let mut sy = i32::from(sy_raw) << (8 - 7);
        sx ^= 0b1111_1100;
        sx -= 46 << 8;
        if flipped {
            sy = -sy;
        }

        (sx, sy)
    }

    /// Fill the 256 scene rows from the decoded line params. No-op while the rows are current.
    pub fn prepare(&mut self, flipped: bool) -> &mut Self {
        if self.rows_current && self.rows_flipped == flipped {
            return self;
        }
        self.rows_current = true;
        self.rows_flipped = flipped;
        self.rows_version = fresh_content_version();
        let mut reg_sx = [0i32; 4];
        let mut reg_sy = [0i32; 4];
        let mut reg_fx_y = [0i32; 4];
        for pf in 0..4 {
            let (sx, sy) = self.get_pf_scroll(pf, flipped);
            reg_sx[pf] = sx;
            reg_sy[pf] = sy;
            reg_fx_y[pf] = sy;
        }

        let text_scroll_x = self.control_1[4] as i16;
        let text_scroll_y = self.control_1[5] as i16;
        let text_reg_sx = if flipped {
            text_scroll_x.wrapping_sub(12)
        } else {
            (-text_scroll_x).wrapping_sub(5)
        };
        let text_reg_sy = if flipped {
            text_scroll_y
        } else {
            text_scroll_y.wrapping_neg()
        };

        for screen_y in 0..SCANLINES {
            let line = self.lines[screen_y];
            let row = &mut self.rows[screen_y];

            // Clips (calibrated oracle l-1/r-2)
            for i in 0..4 {
                row.clips[i].left = line.clip[i].left.wrapping_sub(1);
                row.clips[i].right = line.clip[i].right.wrapping_sub(2);
            }

            // Blend
            row.blend = line.blend;

            // Background
            row.background = line.bg_palette;

            // Mosaic period
            row.mosaic_period = line.x_sample;
            row.palette_15bit = line.palette_15bit;
            row.blur = line.blur;

            // Bitmap mode
            row.bitmap = line.pivot.pivot_control & 0xa0 != 0;

            // Text layer
            row.text.priority = line.pivot.prio;
            row.text.blend_mode = line.pivot.blend_mode;
            row.text.clip_enabled = line.pivot.clip_enable;
            row.text.clip_inverted = line.pivot.clip_inv;
            row.text.clip_inverse = line.pivot.clip_inv_mode;
            row.text.enabled = line.pivot.mix_value & 0x2000 != 0 && line.pivot.blend_mode != 0b11;
            row.text.blend_select = line.pivot.blend_select_v;
            row.text.mosaic = line.pivot.x_sample_enable;

            row.text_x = ((46 + i32::from(text_reg_sx)) & 0x1ff) as i16;
            let line_y_text = (i32::from(text_reg_sy) + screen_y as i32)
                & (if row.bitmap { 0xff } else { 0x1ff });
            // source_y / text_y contract is BEFORE global flip
            row.text_y = line_y_text as i16;

            // Sprite layers
            for i in 0..4 {
                row.sprites[i].priority = line.sp[i].prio;
                row.sprites[i].blend_mode = line.sp[i].blend_mode;
                row.sprites[i].clip_enabled = line.sp[i].clip_enable;
                row.sprites[i].clip_inverted = line.sp[i].clip_inv;
                row.sprites[i].clip_inverse = line.sp[i].clip_inv_mode;
                row.sprites[i].enabled =
                    line.sp[i].mix_value & 0x2000 != 0 && line.sp[i].blend_mode != 0b00;
                row.sprites[i].blend_select = line.sp[i].blend_select_v;
                row.sprites[i].mosaic = line.sp[i].x_sample_enable;
            }

            // Playfields
            for i in 0..4 {
                row.playfields[i].layer.priority = line.pf[i].prio;
                row.playfields[i].layer.blend_mode = line.pf[i].blend_mode;
                row.playfields[i].layer.clip_enabled = line.pf[i].clip_enable;
                row.playfields[i].layer.clip_inverted = line.pf[i].clip_inv;
                row.playfields[i].layer.clip_inverse = line.pf[i].clip_inv_mode;
                row.playfields[i].layer.enabled =
                    line.pf[i].mix_value & 0x2000 != 0 && line.pf[i].blend_mode != 0b11;
                row.playfields[i].layer.blend_select = false;
                row.playfields[i].layer.mosaic = line.pf[i].x_sample_enable;

                let mut reg_fx_x = reg_sx[i] + line.pf[i].rowscroll;
                reg_fx_x += 10 * (line.pf[i].x_scale - 256);
                row.playfields[i].source_x = reg_fx_x + (46 << 8);
                row.playfields[i].x_step = line.pf[i].x_scale;
                row.playfields[i].y_step = line.pf[i].y_scale;
                row.playfields[i].y_fraction = reg_fx_y[i] as u8;
                row.playfields[i].palette_add = line.pf[i].pal_add;
                row.playfields[i].alt_map = line.pf[i].colscroll & 0x200 != 0;
                row.playfields[i].full_res_offset = -1;

                let line_y_pf = ((reg_fx_y[i] >> 8) + i32::from(line.pf[i].colscroll)) & 0x1ff;
                // source_y / text_y contract is BEFORE global flip
                row.playfields[i].source_y = line_y_pf;
            }

            if screen_y != 0 {
                for (fraction, playfield) in reg_fx_y.iter_mut().zip(line.pf.iter()) {
                    *fraction += playfield.y_scale;
                }
            }
        }
        self
    }

    /// `prepare -> cull_empty_rows -> resolve_alt_maps` for the presented scene.
    pub fn resolve_scene(
        &mut self,
        flipped: bool,
        tiles: &GameTiles,
        maps: &mut FullResolutionAltMaps,
        tile_pixels: &[u8],
    ) -> AltRowCounts {
        self.prepare(flipped);
        self.cull_empty_rows(tiles);
        self.resolve_alt_maps(maps, tiles, tile_pixels)
    }

    pub fn row(&self, scanout_y: usize) -> &SceneRow {
        &self.rows[scanout_y & 255]
    }

    pub fn rows(&self) -> &[SceneRow; SCANLINES] {
        &self.rows
    }

    pub fn rows_version(&self) -> u64 {
        self.rows_version
    }

    #[cfg(test)]
    fn lines(&self) -> &[LineParams; SCANLINES] {
        &self.lines
    }

    fn cull_empty_rows(&mut self, tiles: &GameTiles) {
        let mut changed = false;
        let flipped = self.rows_flipped;
        for row in &mut self.rows {
            for i in 0..4 {
                let pf = &mut row.playfields[i];
                let source_y = (pf.source_y as u32 & 511) ^ if flipped { 511 } else { 0 };
                let empty = !tiles.row_used(GameTiles::physical_map(i, pf.alt_map), source_y);
                changed |= pf.empty_row != empty;
                pf.empty_row = empty;
            }
        }
        if changed {
            self.rows_version = fresh_content_version();
        }
    }

    fn resolve_alt_maps(
        &mut self,
        maps: &mut FullResolutionAltMaps,
        tiles: &GameTiles,
        tile_pixels: &[u8],
    ) -> AltRowCounts {
        let mut counts = AltRowCounts::default();
        if !self.video.full_resolution_alt_maps {
            return counts;
        }
        maps.update(tiles, tile_pixels);
        self.rows_version = fresh_content_version();
        let flipped = self.rows_flipped;
        let first_line = self.geometry.first_line() as usize;
        let end_line = self.geometry.end_line() as usize;
        for (y, row) in self.rows.iter_mut().enumerate() {
            for i in 2..4 {
                let pf = &mut row.playfields[i];
                pf.full_res_offset = -1;
                if !pf.alt_map || pf.empty_row || flipped {
                    continue;
                }
                // Every map row the scanline can sample: the highest sub-row phase stays below
                // y_fraction + y_step (expanded sub-rows, compositor vertical phase).
                let rows_spanned = ((i32::from(pf.y_fraction) + pf.y_step - 1) >> 8).clamp(0, 31);
                let first = pf.source_y as u32 & 511;
                let last = (first + rows_spanned as u32) & 511;
                pf.full_res_offset =
                    maps.offset(i, (first >> 4) as usize, (last >> 4) as usize) as i16;
                if y < first_line || y >= end_line || !pf.layer.enabled {
                    continue;
                }
                if pf.full_res_offset >= 0 {
                    counts.remapped += 1;
                } else {
                    counts.fallback += 1;
                }
            }
        }
        counts
    }

    fn forget_decode(&mut self) {
        self.shadow_latches.invalidate();
        self.shadow_sections.invalidate();
        self.shadow_control.invalidate();
        self.rows_current = false;
        self.rows_version = fresh_content_version();
    }

    pub fn state_size(&self) -> usize {
        LINES_STATE_SIZE
    }

    pub fn save_state(&self, writer: &mut StateWriter<'_>) -> Result<(), String> {
        for line in &self.lines {
            writer.write_canonical(&pack_line_params(line))?;
        }
        for row in &self.rows {
            writer.write_canonical(&pack_row(row))?;
        }
        Ok(())
    }

    pub fn load_state(&mut self, reader: &mut StateReader<'_>) -> Result<(), String> {
        self.forget_decode();
        for line in &mut self.lines {
            let params = reader.read_canonical::<CanonicalLineParams>()?;
            *line = unpack_line_params(&params);
        }
        for row in &mut self.rows {
            let canonical = reader.read_canonical::<CanonicalSceneRow>()?;
            *row = unpack_row(&canonical);
        }
        Ok(())
    }
}

impl SceneSource for GameLines {
    fn reset(&mut self) {
        Self::reset(self);
    }

    fn decode(&mut self, vram: &VideoRam<'_>) {
        Self::decode(self, vram, false);
    }
}

fn set_clip_upper(clip: &mut LineClip, left: i8, right: i8) {
    clip.left = (clip.left & 0xff) | (i16::from(left) << 8);
    clip.right = (clip.right & 0xff) | (i16::from(right) << 8);
}

fn set_clip_lower(clip: &mut LineClip, left: u8, right: u8) {
    clip.left = (clip.left & 0x100) | i16::from(left);
    clip.right = (clip.right & 0x100) | i16::from(right);
}

fn pack_layer(layer: &crate::game::scene::SceneLayer) -> crate::state_io::CanonicalSceneLayer {
    crate::state_io::CanonicalSceneLayer {
        priority: layer.priority,
        blend_mode: layer.blend_mode,
        clip_enabled: layer.clip_enabled,
        clip_inverted: layer.clip_inverted,
        clip_inverse: u8::from(layer.clip_inverse),
        enabled: u8::from(layer.enabled),
        blend_select: u8::from(layer.blend_select),
        mosaic: u8::from(layer.mosaic),
    }
}

fn unpack_layer(layer: &crate::state_io::CanonicalSceneLayer) -> crate::game::scene::SceneLayer {
    crate::game::scene::SceneLayer {
        priority: layer.priority,
        blend_mode: layer.blend_mode,
        clip_enabled: layer.clip_enabled,
        clip_inverted: layer.clip_inverted,
        clip_inverse: layer.clip_inverse != 0,
        enabled: layer.enabled != 0,
        blend_select: layer.blend_select != 0,
        mosaic: layer.mosaic != 0,
    }
}

fn pack_line_params(line: &LineParams) -> CanonicalLineParams {
    let mut packed = CanonicalLineParams::default();
    for (c, clip) in line.clip.iter().enumerate() {
        packed.clip[c].left = clip.left;
        packed.clip[c].right = clip.right;
    }
    packed.blend = line.blend;
    packed.x_sample = line.x_sample;
    packed.bg_palette = line.bg_palette;
    packed.pivot.mix_value = line.pivot.mix_value;
    packed.pivot.prio = line.pivot.prio;
    packed.pivot.blend_mode = line.pivot.blend_mode;
    packed.pivot.clip_enable = line.pivot.clip_enable;
    packed.pivot.clip_inv = line.pivot.clip_inv;
    packed.pivot.clip_inv_mode = u8::from(line.pivot.clip_inv_mode);
    packed.pivot.blend_select_v = u8::from(line.pivot.blend_select_v);
    packed.pivot.x_sample_enable = u8::from(line.pivot.x_sample_enable);
    packed.pivot.pivot_control = line.pivot.pivot_control;
    packed.pivot.pivot_enable = line.pivot.pivot_enable;
    for p in 0..4 {
        packed.sp[p] = crate::state_io::CanonicalLineSprite {
            mix_value: line.sp[p].mix_value,
            prio: line.sp[p].prio,
            blend_mode: line.sp[p].blend_mode,
            clip_enable: line.sp[p].clip_enable,
            clip_inv: line.sp[p].clip_inv,
            clip_inv_mode: u8::from(line.sp[p].clip_inv_mode),
            blend_select_v: u8::from(line.sp[p].blend_select_v),
            x_sample_enable: u8::from(line.sp[p].x_sample_enable),
        };
        packed.pf[p] = crate::state_io::CanonicalLinePlayfield {
            mix_value: line.pf[p].mix_value,
            prio: line.pf[p].prio,
            blend_mode: line.pf[p].blend_mode,
            clip_enable: line.pf[p].clip_enable,
            clip_inv: line.pf[p].clip_inv,
            clip_inv_mode: u8::from(line.pf[p].clip_inv_mode),
            x_sample_enable: u8::from(line.pf[p].x_sample_enable),
            colscroll: line.pf[p].colscroll,
            x_scale: line.pf[p].x_scale,
            y_scale: line.pf[p].y_scale,
            pal_add: line.pf[p].pal_add,
            rowscroll: line.pf[p].rowscroll,
        };
    }
    packed
}

fn unpack_line_params(packed: &CanonicalLineParams) -> LineParams {
    let mut line = LineParams::default();
    let clips = packed.clip;
    for (c, clip) in clips.iter().enumerate() {
        line.clip[c] = LineClip {
            left: clip.left,
            right: clip.right,
        };
    }
    line.blend = packed.blend;
    line.x_sample = packed.x_sample;
    line.bg_palette = packed.bg_palette;
    let pivot = packed.pivot;
    line.pivot = LinePivot {
        mix_value: pivot.mix_value,
        prio: pivot.prio,
        blend_mode: pivot.blend_mode,
        clip_enable: pivot.clip_enable,
        clip_inv: pivot.clip_inv,
        clip_inv_mode: pivot.clip_inv_mode != 0,
        blend_select_v: pivot.blend_select_v != 0,
        x_sample_enable: pivot.x_sample_enable != 0,
        pivot_control: pivot.pivot_control,
        pivot_enable: pivot.pivot_enable,
    };
    let sprites = packed.sp;
    for (p, packed) in sprites.iter().enumerate() {
        line.sp[p] = LineSprite {
            mix_value: packed.mix_value,
            prio: packed.prio,
            blend_mode: packed.blend_mode,
            clip_enable: packed.clip_enable,
            clip_inv: packed.clip_inv,
            clip_inv_mode: packed.clip_inv_mode != 0,
            blend_select_v: packed.blend_select_v != 0,
            x_sample_enable: packed.x_sample_enable != 0,
        };
    }
    let playfields = packed.pf;
    for (p, packed) in playfields.iter().enumerate() {
        line.pf[p] = LinePlayfield {
            mix_value: packed.mix_value,
            prio: packed.prio,
            blend_mode: packed.blend_mode,
            clip_enable: packed.clip_enable,
            clip_inv: packed.clip_inv,
            clip_inv_mode: packed.clip_inv_mode != 0,
            x_sample_enable: packed.x_sample_enable != 0,
            colscroll: packed.colscroll,
            x_scale: packed.x_scale,
            y_scale: packed.y_scale,
            pal_add: packed.pal_add,
            rowscroll: packed.rowscroll,
        };
    }
    line
}

fn pack_row(row: &SceneRow) -> CanonicalSceneRow {
    let mut packed = CanonicalSceneRow::default();
    for (p, playfield) in row.playfields.iter().enumerate() {
        packed.playfields[p] = crate::state_io::CanonicalScenePlayfield {
            layer: pack_layer(&playfield.layer),
            source_x: playfield.source_x,
            source_y: playfield.source_y,
            x_step: playfield.x_step,
            y_step: playfield.y_step,
            y_fraction: playfield.y_fraction,
            palette_add: playfield.palette_add,
        };
    }
    for (p, sprite) in row.sprites.iter().enumerate() {
        packed.sprites[p] = pack_layer(sprite);
    }
    packed.text = pack_layer(&row.text);
    for (c, clip) in row.clips.iter().enumerate() {
        packed.clips[c] = crate::state_io::CanonicalSceneClip {
            left: clip.left,
            right: clip.right,
        };
    }
    packed.blend = row.blend;
    packed.background = row.background;
    packed.text_x = row.text_x;
    packed.text_y = row.text_y;
    packed.mosaic_period = row.mosaic_period;
    packed.bitmap = u8::from(row.bitmap);
    packed
}

fn unpack_row(packed: &CanonicalSceneRow) -> SceneRow {
    let mut row = SceneRow::default();
    let playfields = packed.playfields;
    for (p, canonical) in playfields.iter().enumerate() {
        row.playfields[p] = crate::game::scene::ScenePlayfield {
            layer: unpack_layer(&canonical.layer),
            source_x: canonical.source_x,
            source_y: canonical.source_y,
            x_step: canonical.x_step,
            y_step: canonical.y_step,
            y_fraction: canonical.y_fraction,
            palette_add: canonical.palette_add,
            alt_map: false,
            empty_row: false,
            full_res_offset: -1,
        };
    }
    let sprites = packed.sprites;
    for (p, canonical) in sprites.iter().enumerate() {
        row.sprites[p] = unpack_layer(canonical);
    }
    row.text = unpack_layer(&packed.text);
    let clips = packed.clips;
    for (c, canonical) in clips.iter().enumerate() {
        row.clips[c] = SceneClip {
            left: canonical.left,
            right: canonical.right,
        };
    }
    row.blend = packed.blend;
    row.background = packed.background;
    row.text_x = packed.text_x;
    row.text_y = packed.text_y;
    row.mosaic_period = packed.mosaic_period;
    row.bitmap = packed.bitmap != 0;
    row
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::memory_map::{GRAPHICS_RAM_SIZE, OFFS_PLAYFIELDS};

    /// Line RAM latch table of section 2 (0x6000 words).
    const LINE_LATCH_TABLE: usize = OFFS_LINERAM + 2 * LINERAM_LATCH_STRIDE;
    /// 0x6000 subsection 0: sprite blend modes / pivot control.
    const LINE_MODE_TABLE: usize =
        OFFS_LINERAM + LINERAM_SECTIONS_OFFSET + 2 * LINERAM_SECTION_STRIDE;
    /// 0x6000 subsection 1: blend alpha values.
    const LINE_SELECT_TABLE: usize = LINE_MODE_TABLE + LINERAM_SUBSECTION_STRIDE;
    /// 0x6000 subsection 2: mosaic / 15-bit / blur.
    const LINE_MOSAIC_TABLE: usize = LINE_SELECT_TABLE + LINERAM_SUBSECTION_STRIDE;
    /// 0x6000 subsection 3: background palette.
    const LINE_BACKGROUND_TABLE: usize = LINE_MOSAIC_TABLE + LINERAM_SUBSECTION_STRIDE;
    /// 0x7000 subsection 1: pivot mix.
    const LINE_PIVOT_MIX: usize = OFFS_LINERAM
        + LINERAM_SECTIONS_OFFSET
        + 3 * LINERAM_SECTION_STRIDE
        + LINERAM_SUBSECTION_STRIDE;
    /// 0x4000 subsection 2: PF2 column scroll / alt-map select.
    const PF2_COLSCROLL: usize =
        OFFS_LINERAM + LINERAM_SECTIONS_OFFSET + 2 * LINERAM_SUBSECTION_STRIDE;
    /// 0xB000 subsection 2: PF2 mixing info.
    const PF2_MIX: usize = OFFS_LINERAM
        + LINERAM_SECTIONS_OFFSET
        + 7 * LINERAM_SECTION_STRIDE
        + 2 * LINERAM_SUBSECTION_STRIDE;

    struct Sources {
        graphics: Vec<u8>,
        control: [u8; 0x20],
    }

    impl Sources {
        fn new() -> Self {
            Self {
                graphics: vec![0; GRAPHICS_RAM_SIZE],
                control: [0; 0x20],
            }
        }

        fn vram(&self) -> VideoRam<'_> {
            VideoRam::new(&self.graphics, &self.control, 0)
        }

        fn word(&mut self, at: usize, value: u16) {
            self.graphics[at] = (value >> 8) as u8;
            self.graphics[at + 1] = value as u8;
        }

        fn latch(&mut self, section_base: usize, y: usize, bits: u16) {
            self.word(section_base + y * 2, bits);
        }
    }

    #[test]
    fn unlatched_subsections_carry_forward() {
        let mut sources = Sources::new();
        sources.latch(LINE_LATCH_TABLE, 0, 1 << 3);
        sources.word(LINE_BACKGROUND_TABLE, 0x1234);
        sources.latch(LINE_LATCH_TABLE, 2, 1 << 3);
        sources.word(LINE_BACKGROUND_TABLE + 4, 0x00ff);
        let mut lines = GameLines::new(&VideoConfig::default());
        lines.decode(&sources.vram(), false);
        lines.prepare(false);
        assert_eq!(lines.row(0).background, 0x1234);
        assert_eq!(
            lines.row(1).background,
            0x1234,
            "line 1 carries line 0's value"
        );
        assert_eq!(
            lines.row(2).background,
            0x00ff,
            "line 2 adopts its own latch"
        );
        assert_eq!(lines.row(255).background, 0x00ff);
    }

    #[test]
    fn decode_reads_every_section_word() {
        let mut sources = Sources::new();
        sources.latch(
            LINE_LATCH_TABLE,
            0,
            (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3),
        );
        sources.word(LINE_MODE_TABLE, 0x0200 | (0x28 << 8));
        sources.word(LINE_SELECT_TABLE, 0x210f);
        sources.word(LINE_MOSAIC_TABLE, 0x4000 | 0x2000 | 0x0030);
        sources.word(LINE_BACKGROUND_TABLE, 0x1234);
        sources.latch(
            OFFS_LINERAM + 3 * LINERAM_LATCH_STRIDE,
            0,
            (1 << 0) | (1 << 1),
        );
        sources.word(
            OFFS_LINERAM + LINERAM_SECTIONS_OFFSET + 3 * LINERAM_SECTION_STRIDE,
            0x4567,
        );
        sources.word(LINE_PIVOT_MIX, 0x3001);
        let mut lines = GameLines::new(&VideoConfig::default());
        lines.decode(&sources.vram(), false);

        let line = lines.lines()[0];
        assert!(line.pivot.blend_select_v);
        assert_eq!(line.pivot.pivot_control, 0x2a);
        assert_eq!(line.pivot.pivot_enable, 0x4567);
        assert_eq!(line.pivot.mix_value, 0x3001);
        assert_eq!(line.pivot.prio, 1);
        assert_eq!(line.pivot.blend_mode, 0);
        // Alpha nibbles 15,0,1,2 -> saturated weights 0,8,8,8.
        assert_eq!(line.blend, [0, 8, 8, 8]);
        // Mosaic word: x_sample 16 - 3, 15-bit and blur latches clear the FDP defaults.
        assert_eq!(line.x_sample, 13);
        assert!(!line.palette_15bit);
        assert!(!line.blur);
        assert_eq!(line.bg_palette, 0x1234);
    }

    #[test]
    fn decode_is_idempotent_and_flipscreen_reverses_scanlines() {
        let mut sources = Sources::new();
        sources.latch(LINE_LATCH_TABLE, 255, 1 << 3);
        sources.word(LINE_BACKGROUND_TABLE + 255 * 2, 0xabcd);
        sources.control[0] = 0x00;
        sources.control[1] = 0x40;
        let mut lines = GameLines::new(&VideoConfig::default());
        lines.decode(&sources.vram(), true);
        lines.prepare(true);
        assert_eq!(
            lines.row(0).background,
            0xabcd,
            "flipped scanout starts at line 255"
        );
        assert_eq!(lines.row(1).background, 0xabcd);
        let version = lines.rows_version();
        lines.prepare(true);
        assert_eq!(
            lines.rows_version(),
            version,
            "prepare is a no-op while current"
        );
        lines.prepare(false);
        assert_ne!(
            lines.rows_version(),
            version,
            "flip change re-derives the rows"
        );

        let version = lines.rows_version();
        lines.decode(&sources.vram(), false);
        lines.prepare(false);
        assert_ne!(
            lines.rows_version(),
            version,
            "flip change re-decodes and re-derives"
        );
        assert_eq!(lines.row(0).background, 0, "unflipped line 0 never latches");
        assert_eq!(lines.row(255).background, 0xabcd);
    }

    #[test]
    fn clips_apply_upper_and_lower_halves_and_calibration() {
        let mut sources = Sources::new();
        // Section 0 subsection 2/3: colscroll carries the clip high bits for PF2/PF3.
        sources.latch(OFFS_LINERAM, 0, (1 << 2) | (1 << 3));
        sources.word(PF2_COLSCROLL, 0x1000); // clip 0 left high bit
        sources.word(PF2_COLSCROLL + LINERAM_SUBSECTION_STRIDE, 0xc000); // clip 3 left+right high
        // Section 1 subsection 0: clip plane 0 low bytes.
        sources.latch(OFFS_LINERAM + LINERAM_LATCH_STRIDE, 0, 1 << 0);
        sources.word(
            OFFS_LINERAM + LINERAM_SECTIONS_OFFSET + LINERAM_SECTION_STRIDE,
            0x1234,
        );
        let mut lines = GameLines::new(&VideoConfig::default());
        lines.decode(&sources.vram(), false);
        lines.prepare(false);
        let row = lines.row(0);
        assert_eq!(row.clips[0].left, (0x100 | 0x34) - 1);
        assert_eq!(row.clips[0].right, 0x12 - 2);
        assert_eq!(
            row.clips[1].left, -1,
            "PF2 bits 14/15 clear clip 1 high bits"
        );
        assert_eq!(row.clips[1].right, -2);
        assert_eq!(row.clips[3].left, 0x100 - 1);
        assert_eq!(row.clips[3].right, 0x100 - 2);
    }

    #[test]
    fn prepare_derives_playfield_geometry_and_layer_flags() {
        let mut sources = Sources::new();
        // control_0: PF1 x scroll 0x1234, y scroll 0x0100.
        sources.control[2] = 0x12;
        sources.control[3] = 0x34;
        sources.control[2 + 8] = 0x01;
        sources.control[3 + 8] = 0x00;
        // PF1 zoom: x_scale 256 - 0x20 = 224, y_scale for FIX_Y[1]=3.
        sources.latch(OFFS_LINERAM + 4 * LINERAM_LATCH_STRIDE, 0, 1 << 1);
        sources.word(
            OFFS_LINERAM
                + LINERAM_SECTIONS_OFFSET
                + 4 * LINERAM_SECTION_STRIDE
                + LINERAM_SUBSECTION_STRIDE,
            (0x20 << 8) | 0x10,
        );
        // PF1 palette add: 0x0010 * 16.
        sources.latch(OFFS_LINERAM + 5 * LINERAM_LATCH_STRIDE, 0, 1 << 1);
        sources.word(
            OFFS_LINERAM
                + LINERAM_SECTIONS_OFFSET
                + 5 * LINERAM_SECTION_STRIDE
                + LINERAM_SUBSECTION_STRIDE,
            0x0010,
        );
        // PF1 enable + priority 7: B000 subsection 1.
        sources.latch(OFFS_LINERAM + 7 * LINERAM_LATCH_STRIDE, 0, 1 << 1);
        sources.word(
            OFFS_LINERAM
                + LINERAM_SECTIONS_OFFSET
                + 7 * LINERAM_SECTION_STRIDE
                + LINERAM_SUBSECTION_STRIDE,
            0x2007,
        );
        let mut lines = GameLines::new(&VideoConfig::default());
        lines.decode(&sources.vram(), false);
        lines.prepare(false);

        let pf = lines.row(0).playfields[1];
        assert_eq!(pf.layer.priority, 7);
        assert!(pf.layer.enabled);
        assert_eq!(pf.x_step, 224);
        assert_eq!(pf.palette_add, 0x0010 * 16);
        // PF1's zoom word writes y_scale to pf[FIX_Y[1]] = pf[3].
        assert_eq!(lines.row(0).playfields[3].y_step, 0x10 << 1);
        // source_x = reg_sx + 10 * (224 - 256) + 46*256.
        let (reg_sx, _) = lines.get_pf_scroll(1, false);
        assert_eq!(pf.source_x, reg_sx + 10 * (224 - 256) + (46 << 8));
    }

    #[test]
    fn resolve_scene_culls_empty_rows_and_counts_alt_rows() {
        let mut sources = Sources::new();
        let video = VideoConfig {
            extended_alt_maps: true,
            ..VideoConfig::default()
        };
        let mut lines = GameLines::new(&video);
        let mut tiles = GameTiles::new(&video);
        let mut maps = FullResolutionAltMaps::new(&video);

        lines.resolve_scene(false, &tiles, &mut maps, &[]);
        assert!(
            lines.row(24).playfields[0].empty_row,
            "maps with no tile codes are culled"
        );
        assert_eq!(lines.row(24).playfields[0].full_res_offset, -1);

        // Mark PF2's alternate map rows used so nothing is culled.
        for i in 0..tiles.cells_per_map() {
            sources.word(OFFS_PLAYFIELDS + 4 * 0x2000 + i * 4 + 2, 1);
        }
        tiles.decode(&sources.vram());
        lines.decode(&sources.vram(), false);
        lines.reset();
        let counts = lines.resolve_scene(false, &tiles, &mut maps, &[]);
        assert_eq!(counts, AltRowCounts::default(), "no alt-map rows requested");

        // Now ask every line to sample the PF2 alternate map, enabled with priority.
        for y in 0..SCANLINES {
            sources.latch(OFFS_LINERAM, y, 1 << 2);
            sources.word(PF2_COLSCROLL + y * 2, 0x200);
            sources.latch(OFFS_LINERAM + 7 * LINERAM_LATCH_STRIDE, y, 1 << 2);
            sources.word(PF2_MIX + y * 2, 0x2001);
        }
        tiles.decode(&sources.vram());
        lines.reset();
        lines.decode(&sources.vram(), false);
        lines.resolve_scene(false, &tiles, &mut maps, &[]);
        assert!(!lines.row(24).playfields[2].empty_row);
        assert!(lines.row(24).playfields[2].alt_map);
        assert!(lines.row(24).playfields[2].layer.enabled);
    }

    #[test]
    fn resolve_scene_reports_remapped_and_fallback_rows() {
        let video = VideoConfig {
            extended_alt_maps: true,
            full_resolution_alt_maps: true,
            ..VideoConfig::default()
        };
        let geometry = Geometry::from_video(&video);
        let mut sources = Sources::new();
        for y in 0..SCANLINES {
            sources.latch(OFFS_LINERAM, y, 1 << 2);
            sources.word(PF2_COLSCROLL + y * 2, 0x200);
            sources.latch(OFFS_LINERAM + 7 * LINERAM_LATCH_STRIDE, y, 1 << 2);
            sources.word(PF2_MIX + y * 2, 0x2001);
        }
        // Alternate PF2 map: every row has a tile code.
        for i in 0..64usize * 32 {
            sources.word(OFFS_PLAYFIELDS + 4 * 0x2000 + i * 4 + 2, 1);
        }
        let mut tiles = GameTiles::new(&video);
        tiles.decode(&sources.vram());
        let mut maps = FullResolutionAltMaps::new(&video);

        // A constant tile ROM makes every sample equal, so offset 0 is exact everywhere.
        let flat = vec![0x05u8; 8 * 256];
        let mut lines = GameLines::new(&video);
        lines.decode(&sources.vram(), false);
        let counts = lines.resolve_scene(false, &tiles, &mut maps, &flat);
        assert_eq!(
            counts.remapped,
            u64::from(geometry.height()),
            "visible alt rows resolve to the full-resolution twin"
        );
        assert_eq!(counts.fallback, 0);
        assert_eq!(
            lines.row(geometry.first_line() as usize).playfields[2].full_res_offset,
            0
        );

        // Distinct tile content breaks the alias: every visible row falls back.
        let mut varied = vec![0u8; 8 * 256];
        for (i, byte) in varied.iter_mut().enumerate() {
            *byte = (i * 37 + i / 16) as u8;
        }
        lines.reset();
        lines.decode(&sources.vram(), false);
        let mut maps = FullResolutionAltMaps::new(&video);
        let counts = lines.resolve_scene(false, &tiles, &mut maps, &varied);
        assert_eq!(counts.remapped, 0);
        assert_eq!(counts.fallback, u64::from(geometry.height()));
        assert_eq!(
            lines.row(geometry.first_line() as usize).playfields[2].full_res_offset,
            -1
        );
    }

    #[test]
    fn state_round_trips_with_exact_size() {
        let mut sources = Sources::new();
        sources.latch(
            LINE_LATCH_TABLE,
            0,
            (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3),
        );
        sources.word(LINE_MODE_TABLE, 0x0a00);
        sources.word(LINE_BACKGROUND_TABLE, 0x3344);
        sources.latch(
            OFFS_LINERAM + 3 * LINERAM_LATCH_STRIDE,
            0,
            (1 << 0) | (1 << 1),
        );
        sources.word(
            OFFS_LINERAM + LINERAM_SECTIONS_OFFSET + 3 * LINERAM_SECTION_STRIDE,
            0xbeef,
        );
        let video = VideoConfig::default();
        let mut lines = GameLines::new(&video);
        lines.decode(&sources.vram(), false);
        lines.prepare(false);
        assert_eq!(lines.state_size(), 87808);
        assert_eq!(lines.state_size(), LINES_STATE_SIZE);

        let mut bytes = vec![0u8; lines.state_size()];
        let mut writer = StateWriter::new(&mut bytes);
        lines.save_state(&mut writer).unwrap();
        assert_eq!(writer.remaining(), 0);

        let mut restored = GameLines::new(&video);
        let mut reader = StateReader::new(&bytes);
        restored.load_state(&mut reader).unwrap();
        assert_eq!(reader.remaining(), 0);

        let mut after = vec![0u8; restored.state_size()];
        let mut writer = StateWriter::new(&mut after);
        restored.save_state(&mut writer).unwrap();
        assert_eq!(after, bytes);
        assert_eq!(restored.row(0).background, 0x3344);
        assert_eq!(restored.lines()[0].pivot.pivot_enable, 0xbeef);

        // Loading forgets the decode: the next decode re-reads video RAM.
        restored.decode(&sources.vram(), false);
        restored.prepare(false);
        assert_eq!(restored.row(0).background, 0x3344);
    }
}
