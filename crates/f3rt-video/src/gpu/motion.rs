//! Render-only temporal sprite/playfield geometry, mechanically ported from
//! `runtime/renderer/gpu/motion.{hpp,cpp}`.
//!
//! Coordinates are 24.8; interpolation rounds to the nearest fixed-point unit. The history
//! compares two consecutive captured snapshots and samples the geometry at `alpha`.

use crate::config::GameVideoOptions;
use crate::game::clip::{ClipRanges, clip_ranges_for_row, scene_order};
use crate::game::scene::{
    LAYER_COUNT, LayerId, LayerKind, SCROLLED_LAYERS, SceneLayer, SceneRow, SceneSprite, kind,
};
use crate::game::scene_snapshot::SceneSnapshot;

/// Layer-mask bit marking a frame whose scene rows carry motion geometry.
pub const MOTION_LAYER_MASK: u32 = 0x8000_0000;

/// Per-pair accounting for [`GpuMotionHistory::apply_into`].
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct MotionInterpolationStats {
    pub sprites: u32,
    pub playfield_rows: u32,
    pub text_rows: u32,
    pub moving_sprites: u32,
    pub moving_playfield_rows: u32,
    pub moving_text_rows: u32,
    pub rejected_sprites: u32,
    pub rejected_rows: u32,
    pub sprite_count_rejections: u32,
    pub sprite_group_rejections: u32,
    pub sprite_transform_rejections: u32,
    pub sprite_jump_rejections: u32,
    pub row_control_rejections: u32,
    pub row_transform_rejections: u32,
    pub row_jump_rejections: u32,
    pub paired: bool,
    pub alpha: f32,
}

impl MotionInterpolationStats {
    pub const fn new() -> Self {
        Self {
            sprites: 0,
            playfield_rows: 0,
            text_rows: 0,
            moving_sprites: 0,
            moving_playfield_rows: 0,
            moving_text_rows: 0,
            rejected_sprites: 0,
            rejected_rows: 0,
            sprite_count_rejections: 0,
            sprite_group_rejections: 0,
            sprite_transform_rejections: 0,
            sprite_jump_rejections: 0,
            row_control_rejections: 0,
            row_transform_rejections: 0,
            row_jump_rejections: 0,
            paired: false,
            alpha: 1.0,
        }
    }
}

/// Temporal geometry of one scanout row: playfield source/phase and text scroll in 24.8.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MotionRowState {
    /// playfield source x
    pub source_x: [i32; 4],
    /// playfield source_y * 256 + y_fraction
    pub phase: [i32; 4],
    pub text_x: i32,
    pub text_y: i32,
}

/// Temporal geometry for one present: the scene's motion-affected fields at the sampled alpha,
/// in 24.8 fixed point. Initialized to the canonical frame values, so any field motion did not
/// accept equals the snapshot one. Text scroll is carried here because [`SceneRow`] only holds
/// integer scroll.
#[derive(Clone)]
pub struct MotionState {
    pub sprite_x: [i32; SceneSnapshot::MAX_SPRITES],
    pub sprite_y: [i32; SceneSnapshot::MAX_SPRITES],
    pub rows: [MotionRowState; 256],
}

impl Default for MotionState {
    fn default() -> Self {
        Self {
            sprite_x: [0; SceneSnapshot::MAX_SPRITES],
            sprite_y: [0; SceneSnapshot::MAX_SPRITES],
            rows: [MotionRowState::default(); 256],
        }
    }
}

/// Result of [`GpuMotionHistory::apply`]; `state` is meaningful only when `stats.paired`.
#[derive(Clone, Default)]
pub struct MotionResult {
    pub stats: MotionInterpolationStats,
    pub state: MotionState,
}

#[derive(Clone, Copy, Default)]
struct Sprite {
    value: SceneSprite,
    dx: i32,
    dy: i32,
    eligible: bool,
}

#[derive(Clone, Copy)]
struct Row {
    scene: SceneRow,
    order: [LayerId; LAYER_COUNT],
    /// SCROLLED_LAYERS: enabled, unmosaiced, sane clips
    valid: [bool; 5],
    delta: [i32; 10],
    eligible: [bool; 10],
}

impl Default for Row {
    fn default() -> Self {
        Self {
            scene: SceneRow::default(),
            order: [LayerId::Pf0; LAYER_COUNT],
            valid: [false; 5],
            delta: [0; 10],
            eligible: [false; 10],
        }
    }
}

/// Render-only history. Allocate only when explicitly enabling temporal capture.
pub struct GpuMotionHistory {
    // C++ keeps these inline; Rust test threads have smaller stacks, so they live on the heap
    // once per explicitly enabled history.
    sprites: Box<[Sprite; SceneSnapshot::MAX_SPRITES]>,
    rows: Box<[Row; 256]>,
    frame: u64,
    sprite_count: usize,
    pen_mask: u8,
    flipped: bool,
    sprite_flipped: bool,
    border: i32,
    captured: bool,
    paired: bool,
    pair_stats: MotionInterpolationStats,
}

impl GpuMotionHistory {
    /// Only `options.border` matters: it bounds layer clip ranges exactly as the GPU encoder
    /// does, so history compares the same visible ranges.
    pub fn new(options: GameVideoOptions) -> Self {
        Self {
            sprites: Box::new([Sprite::default(); SceneSnapshot::MAX_SPRITES]),
            rows: Box::new([Row::default(); 256]),
            frame: 0,
            sprite_count: 0,
            pen_mask: 0,
            flipped: false,
            sprite_flipped: false,
            border: options.border as i32,
            captured: false,
            paired: false,
            pair_stats: MotionInterpolationStats::default(),
        }
    }

    pub fn reset(&mut self) {
        self.captured = false;
        self.paired = false;
        self.frame = 0;
    }

    fn ranges(&self, row: &SceneRow, id: LayerId) -> ClipRanges {
        clip_ranges_for_row(
            row,
            row.layer(id),
            (46 - self.border) as i16,
            (366 + self.border) as i16,
        )
    }

    /// Layer control equality: priority/blend/enable/select/mosaic, clip enables and polarity,
    /// and the resulting visible ranges (not the raw clip planes).
    fn same_controls(&self, a: &SceneRow, b: &SceneRow, id: LayerId) -> bool {
        if a.layer(id) != b.layer(id) {
            return false;
        }
        if a.clips == b.clips {
            return true;
        }
        same_ranges(&self.ranges(a, id), &self.ranges(b, id))
    }

    fn pair_sprites(
        &mut self,
        scene: &SceneSnapshot,
        dx: &mut [i32],
        dy: &mut [i32],
        eligible: &mut [bool],
    ) {
        let current = &scene.sprites;
        let previous_sprites = &self.sprites;
        let previous = |i: usize| -> &SceneSprite { &previous_sprites[i].value };
        let now = |i: usize| -> &SceneSprite { &current[i] };

        let mut old_groups = GroupTable::default();
        let mut new_groups = GroupTable::default();
        build_group_table(self.sprite_count, &previous, &mut old_groups);
        build_group_table(scene.sprite_count, &now, &mut new_groups);

        const NO_SPRITE: u32 = u32::MAX;
        let mut stats = self.pair_stats;

        for (i, v) in current[..scene.sprite_count].iter().enumerate() {
            let v = *v;
            if v.group == 0 {
                continue;
            }
            let old_run = find_group(&old_groups, &previous, v.group);
            let new_run = find_group(&new_groups, &now, v.group);
            let paired = old_run != NO_SPRITE
                && new_run != NO_SPRITE
                && old_groups.origin_unique[old_run as usize]
                && new_groups.origin_unique[new_run as usize];
            if !paired {
                if i < self.sprite_count && v == self.sprites[i].value {
                    continue;
                }
                stats.rejected_sprites += 1;
                stats.sprite_group_rejections += 1;
                if scene.sprite_count != self.sprite_count {
                    stats.sprite_count_rejections += 1;
                }
                continue;
            }
            let origin = previous(old_groups.order[old_run as usize] as usize);
            let gx = v.group_x - origin.group_x;
            let gy = v.group_y - origin.group_y;
            if gx != 0 || gy != 0 {
                stats.moving_sprites += 1;
            }
            let geometry = self.pen_mask == scene.pen_mask
                && valid_sprite(&v)
                && valid_origin(origin)
                && old_groups.transform_unique[old_run as usize]
                && same_transform(&v, origin);
            let jump = motion_delta(origin.group_x, v.group_x, 0)
                && motion_delta(origin.group_y, v.group_y, 0);
            eligible[i] = geometry && jump;
            if eligible[i] {
                dx[i] = if scene.sprite_flipped { -gx } else { gx };
                dy[i] = if scene.sprite_flipped { -gy } else { gy };
            } else if gx != 0 || gy != 0 {
                stats.rejected_sprites += 1;
                if !geometry {
                    stats.sprite_transform_rejections += 1;
                } else {
                    stats.sprite_jump_rejections += 1;
                }
            }
        }

        let mut old_order = [0u32; SceneSnapshot::MAX_SPRITES];
        let mut new_order = [0u32; SceneSnapshot::MAX_SPRITES];
        let mut matches = [NO_SPRITE; SceneSnapshot::MAX_SPRITES];
        let mut old_n = 0usize;
        let mut new_n = 0usize;
        for i in 0..self.sprite_count {
            if previous(i).group == 0 {
                old_order[old_n] = i as u32;
                old_n += 1;
            }
        }
        for (i, sprite) in current[..scene.sprite_count].iter().enumerate() {
            if sprite.group == 0 {
                new_order[new_n] = i as u32;
                new_n += 1;
            }
        }
        old_order[..old_n].sort_by(|&a, &b| {
            appearance_key(previous(a as usize)).cmp(&appearance_key(previous(b as usize)))
        });
        new_order[..new_n].sort_by(|&a, &b| {
            appearance_key(&current[a as usize]).cmp(&appearance_key(&current[b as usize]))
        });
        let mut old_nearest = [NO_SPRITE; SceneSnapshot::MAX_SPRITES];
        let mut new_nearest = [NO_SPRITE; SceneSnapshot::MAX_SPRITES];
        let mut old_distance = [0u32; SceneSnapshot::MAX_SPRITES];
        let mut new_distance = [0u32; SceneSnapshot::MAX_SPRITES];
        let mut old = 0usize;
        let mut fresh = 0usize;
        while old < old_n && fresh < new_n {
            let a = appearance_key(previous(old_order[old] as usize));
            let b = appearance_key(&current[new_order[fresh] as usize]);
            if a < b {
                old += 1;
                continue;
            }
            if b < a {
                fresh += 1;
                continue;
            }
            let mut old_end = old + 1;
            let mut new_end = fresh + 1;
            while old_end < old_n && appearance_key(previous(old_order[old_end] as usize)) == a {
                old_end += 1;
            }
            while new_end < new_n && appearance_key(&current[new_order[new_end] as usize]) == b {
                new_end += 1;
            }
            if old_end - old == 1 && new_end - fresh == 1 {
                matches[new_order[fresh] as usize] = old_order[old];
            } else {
                for p in old..old_end {
                    old_nearest[p] = NO_SPRITE;
                    old_distance[p] = u32::MAX;
                }
                for n in fresh..new_end {
                    new_nearest[n] = NO_SPRITE;
                    new_distance[n] = u32::MAX;
                    let v = &current[new_order[n] as usize];
                    for p in old..old_end {
                        let partner = previous(old_order[p] as usize);
                        let ddx = i64::from(v.x) - i64::from(partner.x);
                        let ddy = i64::from(v.y) - i64::from(partner.y);
                        if ddx.abs() > i64::from(32) * 256 || ddy.abs() > i64::from(32) * 256 {
                            continue;
                        }
                        let distance = (ddx * ddx + ddy * ddy) as u32;
                        if distance < old_distance[p] {
                            old_distance[p] = distance;
                            old_nearest[p] = n as u32;
                        } else if distance == old_distance[p] {
                            old_nearest[p] = NO_SPRITE;
                        }
                        if distance < new_distance[n] {
                            new_distance[n] = distance;
                            new_nearest[n] = p as u32;
                        } else if distance == new_distance[n] {
                            new_nearest[n] = NO_SPRITE;
                        }
                    }
                }
                for n in fresh..new_end {
                    if new_nearest[n] != NO_SPRITE
                        && old_nearest[new_nearest[n] as usize] == n as u32
                    {
                        matches[new_order[n] as usize] = old_order[new_nearest[n] as usize];
                    }
                }
            }
            old = old_end;
            fresh = new_end;
        }

        for (i, v) in current[..scene.sprite_count].iter().enumerate() {
            let v = *v;
            if v.group != 0 {
                continue;
            }
            let matched = matches[i];
            if matched == NO_SPRITE {
                if i < self.sprite_count && v == self.sprites[i].value {
                    continue;
                }
                stats.rejected_sprites += 1;
                stats.sprite_group_rejections += 1;
                if scene.sprite_count != self.sprite_count {
                    stats.sprite_count_rejections += 1;
                }
                continue;
            }
            let partner = previous(matched as usize);
            let moving = v.x != partner.x || v.y != partner.y;
            if moving {
                stats.moving_sprites += 1;
            }
            let geometry = self.pen_mask == scene.pen_mask
                && valid_sprite(&v)
                && valid_sprite(partner)
                && same_transform(&v, partner);
            let jump = motion_delta(partner.x, v.x, 0) && motion_delta(partner.y, v.y, 0);
            eligible[i] = geometry && jump;
            if eligible[i] {
                dx[i] = v.x - partner.x;
                dy[i] = v.y - partner.y;
            } else if moving {
                stats.rejected_sprites += 1;
                if !geometry {
                    stats.sprite_transform_rejections += 1;
                } else {
                    stats.sprite_jump_rejections += 1;
                }
            }
        }

        self.pair_stats = stats;
    }

    pub fn capture(&mut self, scene: &SceneSnapshot, frame: u64) {
        if scene.fallback || scene.sprite_count > self.sprites.len() {
            self.reset();
            return;
        }
        self.paired = self.captured
            && self.frame != u64::MAX
            && frame == self.frame + 1
            && scene.flipped == self.flipped
            && scene.sprite_flipped == self.sprite_flipped;
        self.pair_stats = MotionInterpolationStats::default();
        let current = &scene.sprites;
        let mut sprites_changed = self.sprite_count != scene.sprite_count;
        for (i, sprite) in current[..scene.sprite_count].iter().enumerate() {
            if !self.paired || sprites_changed {
                break;
            }
            sprites_changed = *sprite != self.sprites[i].value;
        }
        let mut dx = [0i32; SceneSnapshot::MAX_SPRITES];
        let mut dy = [0i32; SceneSnapshot::MAX_SPRITES];
        let mut eligible = [false; SceneSnapshot::MAX_SPRITES];
        if self.paired && sprites_changed {
            self.pair_sprites(scene, &mut dx, &mut dy, &mut eligible);
        }
        for i in 0..scene.sprite_count {
            let s = &mut self.sprites[i];
            s.value = current[i];
            s.dx = dx[i];
            s.dy = dy[i];
            s.eligible = eligible[i];
        }
        let geometry = scene.geometry();
        let first_line = geometry.first_line() as usize;
        let end_line = geometry.end_line() as usize;
        for y in first_line..end_line {
            let mut now = Row {
                scene: scene.rows[y],
                order: scene_order(&scene.rows[y]),
                ..Row::default()
            };
            let before = &self.rows[y - first_line];
            for (i, id) in SCROLLED_LAYERS.iter().enumerate() {
                now.valid[i] = valid_layer(now.scene.layer(*id), &self.ranges(&now.scene, *id));
            }
            for (i, id) in SCROLLED_LAYERS.iter().copied().enumerate() {
                let playfield = kind(id) == LayerKind::Playfield;
                // Blend weights and ordering remain current discrete state. Changes to another
                // layer do not invalidate this layer's source geometry.
                let controls = self.paired
                    && (playfield || (!before.scene.bitmap && !now.scene.bitmap))
                    && self.same_controls(&now.scene, &before.scene, id)
                    && now.valid[i]
                    && before.valid[i];
                let (x, px, phase, previous_phase, valid_x, valid_y) = if playfield {
                    let g = now.scene.playfields[i];
                    let p = before.scene.playfields[i];
                    let valid_x = (i64::from(g.source_x)).abs() <= (1i64 << 24)
                        && (i64::from(p.source_x)).abs() <= (1i64 << 24)
                        && (g.x_step as u32) > 0
                        && (g.x_step as u32) <= 256
                        && g.x_step == p.x_step
                        && g.alt_map == p.alt_map;
                    let valid_y = (g.source_y as u32) <= 511
                        && (p.source_y as u32) <= 511
                        && (g.y_step as u32) > 0
                        && (g.y_step as u32) <= 510
                        && g.y_step == p.y_step
                        && g.alt_map == p.alt_map;
                    (
                        g.source_x,
                        p.source_x,
                        (g.source_y as u32 * 256 + u32::from(g.y_fraction)) as i32,
                        (p.source_y as u32 * 256 + u32::from(p.y_fraction)) as i32,
                        valid_x,
                        valid_y,
                    )
                } else {
                    let tx = i32::from(now.scene.text_x);
                    let ty = i32::from(now.scene.text_y);
                    let ptx = i32::from(before.scene.text_x);
                    let pty = i32::from(before.scene.text_y);
                    (
                        tx * 256,
                        ptx * 256,
                        ty * 256,
                        pty * 256,
                        (tx as u32) <= 511 && (ptx as u32) <= 511,
                        (ty as u32) <= 511 && (pty as u32) <= 511,
                    )
                };
                let period = if playfield {
                    (scene.video().pf_map_width_px() as i32) * 256
                } else {
                    512 * 256
                };
                let jump_x = motion_delta(px, x, period);
                let jump_y = motion_delta(previous_phase, phase, 512 * 256);
                now.eligible[i * 2] = controls && valid_x && jump_x;
                now.eligible[i * 2 + 1] = controls && valid_y && jump_y;
                if now.eligible[i * 2] {
                    now.delta[i * 2] = x - px;
                }
                if now.eligible[i * 2 + 1] {
                    now.delta[i * 2 + 1] = phase - previous_phase;
                }
                // Disabled channels and unchanged geometry are neither moving nor rejected.
                let moving_x = x != px;
                let moving_y = phase != previous_phase;
                if !self.paired || !now.scene.layer(id).enabled || (!moving_x && !moving_y) {
                    continue;
                }
                if playfield {
                    self.pair_stats.moving_playfield_rows += 1;
                } else {
                    self.pair_stats.moving_text_rows += 1;
                }
                if (moving_x && !now.eligible[i * 2]) || (moving_y && !now.eligible[i * 2 + 1]) {
                    self.pair_stats.rejected_rows += 1;
                    if !controls {
                        self.pair_stats.row_control_rejections += 1;
                    } else if (moving_x && !valid_x) || (moving_y && !valid_y) {
                        self.pair_stats.row_transform_rejections += 1;
                    } else {
                        self.pair_stats.row_jump_rejections += 1;
                    }
                }
            }
            self.rows[y - first_line] = now;
        }
        self.frame = frame;
        self.sprite_count = scene.sprite_count;
        self.pen_mask = scene.pen_mask;
        self.flipped = scene.flipped;
        self.sprite_flipped = scene.sprite_flipped;
        self.captured = true;
    }

    pub fn apply(&self, scene: &SceneSnapshot, alpha: f32) -> MotionResult {
        let mut result = MotionResult::default();
        let stats = self.apply_into(scene, alpha, &mut result.state);
        result.stats = stats;
        result
    }

    /// Writes the sampled geometry into caller-owned `out` (no 42 KiB return copy). `out` is
    /// meaningful only when the returned `stats.paired`; an unpaired result leaves it untouched.
    pub fn apply_into(
        &self,
        scene: &SceneSnapshot,
        alpha: f32,
        out: &mut MotionState,
    ) -> MotionInterpolationStats {
        if !alpha.is_finite()
            || !(0.0..=1.0).contains(&alpha)
            || !self.paired
            || scene.fallback
            || scene.sprite_count != self.sprite_count
            || scene.pen_mask != self.pen_mask
            || scene.flipped != self.flipped
            || scene.sprite_flipped != self.sprite_flipped
        {
            return MotionInterpolationStats::default();
        }
        let mut stats = self.pair_stats;
        stats.paired = true;
        stats.alpha = alpha;
        for i in 0..self.sprite_count {
            out.sprite_x[i] = scene.sprites[i].x;
            out.sprite_y[i] = scene.sprites[i].y;
        }
        for y in 0..256 {
            let row = &scene.rows[y];
            let out_row = &mut out.rows[y];
            for i in 0..4 {
                out_row.source_x[i] = row.playfields[i].source_x;
                out_row.phase[i] = (row.playfields[i].source_y as u32 * 256
                    + u32::from(row.playfields[i].y_fraction))
                    as i32;
            }
            // Every motion-marked text row has current fixed-point defaults, even snapped rows.
            out_row.text_x = i32::from(row.text_x) * 256;
            out_row.text_y = i32::from(row.text_y) * 256;
        }
        for i in 0..self.sprite_count {
            let s = &self.sprites[i];
            if !s.eligible || (s.dx == 0 && s.dy == 0) || s.value != scene.sprites[i] {
                continue;
            }
            stats.sprites += 1;
            if alpha == 1.0 {
                continue;
            }
            out.sprite_x[i] = interpolate(s.value.x, s.dx, alpha);
            out.sprite_y[i] = interpolate(s.value.y, s.dy, alpha);
        }
        let geometry = scene.geometry();
        let first_line = geometry.first_line() as usize;
        let end_line = geometry.end_line() as usize;
        for y in first_line..end_line {
            let r = &self.rows[y - first_line];
            let row = &scene.rows[y];
            let out_row = &mut out.rows[y];
            if row.bitmap != r.scene.bitmap
                || row.mosaic_period != r.scene.mosaic_period
                || row.blend != r.scene.blend
                || scene_order(row) != r.order
            {
                continue;
            }
            for (i, id) in SCROLLED_LAYERS.iter().copied().enumerate() {
                let playfield = kind(id) == LayerKind::Playfield;
                if (!r.eligible[i * 2] && !r.eligible[i * 2 + 1])
                    || (r.delta[i * 2] == 0 && r.delta[i * 2 + 1] == 0)
                    || !self.same_controls(&r.scene, row, id)
                {
                    continue;
                }
                if playfield {
                    if !same_geometry(&r.scene.playfields[i], &row.playfields[i]) {
                        continue;
                    }
                } else if r.scene.text_x != row.text_x || r.scene.text_y != row.text_y {
                    continue;
                }
                if playfield {
                    stats.playfield_rows += 1;
                } else {
                    stats.text_rows += 1;
                }
                if alpha == 1.0 {
                    continue;
                }
                if playfield {
                    let g = r.scene.playfields[i];
                    out_row.source_x[i] = interpolate(g.source_x, r.delta[i * 2], alpha);
                    out_row.phase[i] = interpolate(
                        (g.source_y as u32 * 256 + u32::from(g.y_fraction)) as i32,
                        r.delta[i * 2 + 1],
                        alpha,
                    );
                } else {
                    out_row.text_x =
                        interpolate(i32::from(r.scene.text_x) * 256, r.delta[8], alpha);
                    out_row.text_y =
                        interpolate(i32::from(r.scene.text_y) * 256, r.delta[9], alpha);
                }
            }
        }
        stats
    }
}

impl Default for GpuMotionHistory {
    fn default() -> Self {
        Self::new(GameVideoOptions::default())
    }
}

fn floor_divide(n: i64, d: i64) -> i64 {
    if n >= 0 { n / d } else { -1 - (-1 - n) / d }
}

fn motion_delta(before: i32, now: i32, period: i32) -> bool {
    (i64::from(now) - i64::from(before)).abs() <= i64::from(32) * 256
        && (period == 0
            || floor_divide(i64::from(before), i64::from(period))
                == floor_divide(i64::from(now), i64::from(period)))
}

fn interpolate(current: i32, delta: i32, alpha: f32) -> i32 {
    (f64::from(current) - f64::from(delta) * (1.0 - f64::from(alpha))).round() as i32
}

fn same_ranges(a: &ClipRanges, b: &ClipRanges) -> bool {
    a.count == b.count && a.as_slice() == b.as_slice()
}

/// A scrolled layer may move only while enabled, unmosaiced and with non-empty, bounded visible
/// ranges.
fn valid_layer(layer: &SceneLayer, clips: &ClipRanges) -> bool {
    if !layer.enabled || layer.mosaic || clips.count == 0 {
        return false;
    }
    for i in 0..clips.count {
        let lo = i32::from(clips.ranges[i].left);
        let hi = i32::from(clips.ranges[i].right);
        if lo >= hi || lo < -4096 || hi > 4096 {
            return false;
        }
    }
    true
}

fn same_geometry(
    a: &crate::game::scene::ScenePlayfield,
    b: &crate::game::scene::ScenePlayfield,
) -> bool {
    a.source_x == b.source_x
        && a.source_y == b.source_y
        && a.x_step == b.x_step
        && a.y_step == b.y_step
        && a.y_fraction == b.y_fraction
        && a.palette_add == b.palette_add
        && a.alt_map == b.alt_map
}

fn valid_sprite(v: &SceneSprite) -> bool {
    v.scale_x > 0
        && v.scale_x <= 256
        && v.scale_y > 0
        && v.scale_y <= 256
        // 17-bit sprite code (MAME y-ack/fdp-collapse tc0630fdp.cpp); the ROM wraps it
        && v.tile < (1u32 << 17)
        && (i64::from(v.x)).abs() <= i64::from(1024) * 256
        && (i64::from(v.y)).abs() <= i64::from(1024) * 256
}

fn same_transform(a: &SceneSprite, b: &SceneSprite) -> bool {
    a.scale_x == b.scale_x && a.scale_y == b.scale_y && a.flip_x == b.flip_x && a.flip_y == b.flip_y
}

fn transform_signature(s: &SceneSprite) -> u64 {
    (u64::from(s.scale_x) << 32)
        | (u64::from(s.scale_y) << 16)
        | (u64::from(s.flip_x) << 1)
        | u64::from(s.flip_y)
}

struct GroupTable {
    order: [u32; SceneSnapshot::MAX_SPRITES],
    origin_unique: [bool; SceneSnapshot::MAX_SPRITES],
    transform_unique: [bool; SceneSnapshot::MAX_SPRITES],
    count: usize,
}

impl Default for GroupTable {
    fn default() -> Self {
        Self {
            order: [0; SceneSnapshot::MAX_SPRITES],
            origin_unique: [false; SceneSnapshot::MAX_SPRITES],
            transform_unique: [false; SceneSnapshot::MAX_SPRITES],
            count: 0,
        }
    }
}

fn build_group_table<'a, F>(count: usize, at: &F, table: &mut GroupTable)
where
    F: Fn(usize) -> &'a SceneSprite,
{
    table.count = 0;
    for i in 0..count {
        if at(i).group != 0 {
            table.order[table.count] = i as u32;
            table.count += 1;
        }
    }
    let order = &mut table.order[..table.count];
    order.sort_by_key(|&a| at(a as usize).group);
    let mut p = 0usize;
    while p < table.count {
        let first = at(table.order[p] as usize);
        let mut same_origin = true;
        let mut same_signature = true;
        let mut e = p + 1;
        while e < table.count && at(table.order[e] as usize).group == first.group {
            let s = at(table.order[e] as usize);
            same_origin = same_origin && s.group_x == first.group_x && s.group_y == first.group_y;
            same_signature = same_signature && transform_signature(s) == transform_signature(first);
            e += 1;
        }
        table.origin_unique[p] = same_origin;
        table.transform_unique[p] = same_signature;
        p = e;
    }
}

fn find_group<'a, F>(table: &GroupTable, at: &F, key: u64) -> u32
where
    F: Fn(usize) -> &'a SceneSprite,
{
    let mut lo = 0usize;
    let mut hi = table.count;
    while lo < hi {
        let mid = lo + (hi - lo) / 2;
        if at(table.order[mid] as usize).group < key {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if lo < table.count && at(table.order[lo] as usize).group == key {
        return lo as u32;
    }
    u32::MAX
}

fn valid_origin(s: &SceneSprite) -> bool {
    (i64::from(s.group_x)).abs() <= i64::from(1024) * 256
        && (i64::from(s.group_y)).abs() <= i64::from(1024) * 256
}

fn appearance_key(s: &SceneSprite) -> u64 {
    (u64::from(s.tile) << 32) | u64::from(s.palette)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::VideoConfig;

    fn snapshot(fallback: bool) -> Box<SceneSnapshot> {
        let mut scene = Box::new(SceneSnapshot::new(&VideoConfig::default()));
        scene.fallback = fallback;
        scene
    }

    fn fresh(scene: &mut SceneSnapshot, slot: usize) -> &mut SceneSprite {
        scene.sprites[slot] = SceneSprite::default();
        scene.sprites[slot].y = 20 * 256;
        scene.sprite_count = scene.sprite_count.max(slot + 1);
        &mut scene.sprites[slot]
    }

    /// Keyless sprite (no group): motion may only pair it by appearance.
    fn sprite(scene: &mut SceneSnapshot, slot: usize, x: i32, tile: u32) {
        let sp = fresh(scene, slot);
        sp.x = x * 256;
        sp.tile = tile;
    }

    /// Member of a rigid group: `group` key, group origin (gx, gy) in pixels, member at x.
    fn grouped(
        scene: &mut SceneSnapshot,
        slot: usize,
        x: i32,
        tile: u32,
        group: u64,
        gx: i32,
        gy: i32,
    ) {
        let sp = fresh(scene, slot);
        sp.x = x * 256;
        sp.tile = tile;
        sp.group = group;
        sp.group_x = gx * 256;
        sp.group_y = gy * 256;
    }

    fn row24(scene: &mut SceneSnapshot) -> &mut SceneRow {
        &mut scene.rows[24]
    }

    fn pf0(scene: &mut SceneSnapshot) -> &mut crate::game::scene::ScenePlayfield {
        &mut scene.rows[24].playfields[0]
    }

    fn layer(scene: &mut SceneSnapshot) {
        let p = pf0(scene);
        p.layer.enabled = true;
        p.layer.clip_inverse = true;
        p.source_x = 100 * 256;
        p.source_y = 100;
        p.x_step = 256;
        p.y_step = 256;
    }

    fn pair(a: &SceneSnapshot, b: &SceneSnapshot, alpha: f32) -> MotionResult {
        let mut history = GpuMotionHistory::default();
        history.capture(a, 10);
        history.capture(b, 11);
        history.apply(b, alpha)
    }

    #[test]
    fn group_members_interpolate_together() {
        let mut a = snapshot(false);
        grouped(&mut a, 0, 20, 10, 1, 20, 20);
        grouped(&mut a, 1, 36, 11, 1, 20, 20);
        let mut b = a.clone();
        grouped(&mut b, 0, 30, 10, 1, 30, 20);
        grouped(&mut b, 1, 46, 11, 1, 30, 20);

        let result = pair(&a, &b, 0.5);
        assert_eq!(result.stats.sprites, 2);
        assert_eq!(result.stats.moving_sprites, 2);
        assert_eq!(result.stats.rejected_sprites, 0);
        assert_eq!(result.stats.sprite_group_rejections, 0);
        assert_eq!(result.state.sprite_x[0], 25 * 256);
        assert_eq!(result.state.sprite_x[1], 41 * 256);

        let result = pair(&a, &b, 0.0);
        assert_eq!(result.state.sprite_x[0], 20 * 256);
        assert_eq!(result.state.sprite_x[1], 36 * 256);
        let result = pair(&a, &b, 1.0);
        assert_eq!(result.state.sprite_x[0], 30 * 256);
    }

    #[test]
    fn body_and_shadow_groups_interpolate_independently() {
        let mut a = snapshot(false);
        grouped(&mut a, 0, 20, 10, 1, 20, 20);
        grouped(&mut a, 1, 18, 10, 2, 18, 20);
        let mut b = a.clone();
        grouped(&mut b, 0, 30, 10, 1, 30, 20);
        grouped(&mut b, 1, 14, 10, 2, 14, 20);

        let result = pair(&a, &b, 0.5);
        assert_eq!(result.stats.sprites, 2);
        assert_eq!(result.state.sprite_x[0], 25 * 256);
        assert_eq!(result.state.sprite_x[1], 16 * 256);

        let mut swapped = b.clone();
        swapped.sprites.swap(0, 1);
        let result = pair(&a, &swapped, 0.5);
        assert_eq!(result.state.sprite_x[0], 16 * 256);
        assert_eq!(result.state.sprite_x[1], 25 * 256);
    }

    #[test]
    fn group_key_change_snaps() {
        let mut a = snapshot(false);
        grouped(&mut a, 0, 20, 10, 7, 20, 20);
        let mut b = a.clone();
        grouped(&mut b, 0, 24, 10, 8, 24, 20);

        let result = pair(&a, &b, 0.5);
        assert_eq!(result.stats.sprites, 0);
        assert_eq!(result.stats.sprite_group_rejections, 1);
        assert_eq!(result.stats.rejected_sprites, 1);
        assert_eq!(result.state.sprite_x[0], 24 * 256);
    }

    #[test]
    fn ambiguous_group_origin_snaps() {
        let mut a = snapshot(false);
        grouped(&mut a, 0, 20, 10, 7, 20, 20);
        grouped(&mut a, 1, 60, 11, 7, 60, 20);
        let mut b = snapshot(false);
        grouped(&mut b, 0, 24, 10, 7, 24, 20);

        let result = pair(&a, &b, 0.5);
        assert_eq!(result.stats.sprites, 0);
        assert_eq!(result.stats.sprite_group_rejections, 1);
        assert_eq!(result.state.sprite_x[0], 24 * 256);

        let mut c = snapshot(false);
        grouped(&mut c, 0, 24, 10, 7, 24, 20);
        grouped(&mut c, 1, 64, 11, 7, 64, 20);
        let mut a = snapshot(false);
        grouped(&mut a, 0, 20, 10, 7, 20, 20);

        let result = pair(&a, &c, 0.5);
        assert_eq!(result.stats.sprites, 0);
        assert_eq!(result.stats.sprite_group_rejections, 2);
        assert_eq!(result.state.sprite_x[0], 24 * 256);
        assert_eq!(result.state.sprite_x[1], 64 * 256);
    }

    #[test]
    fn member_set_change_interpolates() {
        let mut a = snapshot(false);
        grouped(&mut a, 0, 20, 10, 3, 20, 20);
        grouped(&mut a, 1, 36, 11, 3, 20, 20);
        let mut b = a.clone();
        b.sprite_count = 1;
        grouped(&mut b, 0, 40, 11, 3, 24, 20);

        let result = pair(&a, &b, 0.5);
        assert_eq!(result.stats.sprites, 1);
        assert_eq!(result.stats.rejected_sprites, 0);
        assert_eq!(result.state.sprite_x[0], 38 * 256);

        let mut c = snapshot(false);
        grouped(&mut c, 0, 36, 11, 3, 20, 20);
        let mut d = snapshot(false);
        grouped(&mut d, 0, 24, 10, 3, 24, 20);
        grouped(&mut d, 1, 40, 11, 3, 24, 20);
        let result = pair(&c, &d, 0.5);
        assert_eq!(result.stats.sprites, 2);
        assert_eq!(result.state.sprite_x[0], 22 * 256);
        assert_eq!(result.state.sprite_x[1], 38 * 256);
    }

    #[test]
    fn group_transform_change_snaps() {
        let mut a = snapshot(false);
        grouped(&mut a, 0, 20, 10, 9, 20, 20);
        let mut b = a.clone();
        grouped(&mut b, 0, 24, 10, 9, 24, 20);
        b.sprites[0].scale_x = 128;

        let result = pair(&a, &b, 0.5);
        assert_eq!(result.stats.sprites, 0);
        assert_eq!(result.stats.sprite_transform_rejections, 1);
        assert_eq!(result.state.sprite_x[0], 24 * 256);

        b.sprites[0].scale_x = 256;
        b.sprites[0].flip_y = true;
        let result = pair(&a, &b, 0.5);
        assert_eq!(result.stats.sprites, 0);
        assert_eq!(result.stats.sprite_transform_rejections, 1);

        let mut c = snapshot(false);
        grouped(&mut c, 0, 20, 10, 9, 20, 20);
        grouped(&mut c, 1, 36, 11, 9, 20, 20);
        c.sprites[1].scale_y = 128;
        let mut d = snapshot(false);
        grouped(&mut d, 0, 24, 10, 9, 24, 20);

        let result = pair(&c, &d, 0.5);
        assert_eq!(result.stats.sprites, 0);
        assert_eq!(result.stats.sprite_transform_rejections, 1);
    }

    #[test]
    fn keyless_appearance_fallback() {
        let mut a = snapshot(false);
        sprite(&mut a, 0, 20, 10);
        let mut b = a.clone();
        sprite(&mut b, 0, 24, 10);

        let result = pair(&a, &b, 0.5);
        assert_eq!(result.stats.sprites, 1);
        assert_eq!(result.state.sprite_x[0], 22 * 256);

        sprite(&mut b, 0, 24, 99);
        let stats = pair(&a, &b, 0.5).stats;
        assert_eq!(stats.sprites, 0);
        assert_eq!(stats.sprite_group_rejections, 1);

        let mut a = snapshot(false);
        sprite(&mut a, 0, 20, 10);
        sprite(&mut a, 1, 24, 10);
        let mut b = a.clone();
        sprite(&mut b, 0, 22, 10);
        sprite(&mut b, 1, 26, 10);
        let stats = pair(&a, &b, 0.5).stats;
        assert_eq!(stats.sprites, 0);
        assert_eq!(stats.sprite_group_rejections, 2);

        let mut a = snapshot(false);
        sprite(&mut a, 0, 20, 10);
        sprite(&mut a, 1, 36, 10);
        let mut b = a.clone();
        sprite(&mut b, 0, 24, 10);
        sprite(&mut b, 1, 40, 10);
        let result = pair(&a, &b, 0.5);
        assert_eq!(result.stats.sprites, 2);
        assert_eq!(result.state.sprite_x[0], 22 * 256);
        assert_eq!(result.state.sprite_x[1], 38 * 256);
    }

    #[test]
    fn no_group_pair_is_canonical_at_every_alpha() {
        let mut a = snapshot(false);
        grouped(&mut a, 0, 20, 10, 1, 20, 20);
        let mut b = a.clone();
        grouped(&mut b, 0, 24, 10, 2, 24, 20);

        for alpha in [0.0, 0.25, 0.5, 1.0] {
            let result = pair(&a, &b, alpha);
            assert!(result.stats.paired, "frames did not pair at alpha {alpha}");
            assert_eq!(result.stats.sprites, 0);
            assert_eq!(result.state.sprite_x[0], 24 * 256);
        }
    }

    #[test]
    fn group_jump_limit() {
        let mut a = snapshot(false);
        grouped(&mut a, 0, 20, 10, 5, 20, 20);
        let mut b = a.clone();
        grouped(&mut b, 0, 52, 10, 5, 52, 20);

        let result = pair(&a, &b, 0.5);
        assert_eq!(result.stats.sprites, 1);
        assert_eq!(result.state.sprite_x[0], 36 * 256);

        b.sprites[0].group_x += 1;
        let stats = pair(&a, &b, 0.5).stats;
        assert_eq!(stats.sprites, 0);
        assert_eq!(stats.sprite_jump_rejections, 1);
    }

    #[test]
    fn static_grouped_frame_has_no_motion_or_rejections() {
        let mut a = snapshot(false);
        grouped(&mut a, 0, 20, 10, 1, 20, 20);
        grouped(&mut a, 1, 36, 11, 1, 20, 20);

        let stats = pair(&a, &a, 0.5).stats;
        assert_eq!(stats.sprites, 0);
        assert_eq!(stats.moving_sprites, 0);
        assert_eq!(stats.rejected_sprites, 0);
        assert_eq!(stats.sprite_group_rejections, 0);
    }

    #[test]
    fn playfield_scroll_and_independent_guards() {
        let mut a = snapshot(false);
        layer(&mut a);
        let mut b = a.clone();
        pf0(&mut b).source_x += 4 * 256;
        row24(&mut b).blend = [8, 8, 8, 8];
        row24(&mut b).playfields[1].layer.enabled = true;
        row24(&mut b).playfields[1].layer.priority = 9;

        let mut result = pair(&a, &b, 0.5);
        assert_eq!(result.stats.playfield_rows, 1);
        assert_eq!(result.state.rows[24].source_x[0], 102 * 256);

        pf0(&mut b).source_y = 400;
        result = pair(&a, &b, 0.5);
        assert_eq!(result.state.rows[24].source_x[0], 102 * 256);
        assert_eq!(result.state.rows[24].phase[0], 400 * 256);
        assert_eq!(result.stats.row_jump_rejections, 1);

        pf0(&mut b).source_y = 104;
        pf0(&mut b).y_step = 128;
        result = pair(&a, &b, 0.5);
        assert_eq!(result.state.rows[24].source_x[0], 102 * 256);
        assert_eq!(result.state.rows[24].phase[0], 104 * 256);
        assert_eq!(result.stats.row_transform_rejections, 1);

        pf0(&mut b).y_step = 256;
        pf0(&mut a).source_x = 1022 * 256;
        pf0(&mut b).source_x = 2 * 256;
        result = pair(&a, &b, 0.5);
        assert_eq!(result.state.rows[24].source_x[0], 2 * 256);
        assert_eq!(result.state.rows[24].phase[0], 102 * 256);

        let mut a = snapshot(false);
        sprite(&mut a, 0, 20, 10);
        let mut b = a.clone();
        sprite(&mut b, 0, 52, 10);
        let result = pair(&a, &b, 0.5);
        assert_eq!(result.stats.sprites, 1);
        assert_eq!(result.state.sprite_x[0], 36 * 256);
        b.sprites[0].x += 1;
        let stats = pair(&a, &b, 0.5).stats;
        assert_eq!(stats.sprites, 0);
        assert_eq!(stats.sprite_jump_rejections, 1);

        sprite(&mut b, 0, 24, 10);
        b.sprites[0].palette += 1;
        let result = pair(&a, &b, 0.5);
        assert_eq!(result.stats.sprites, 0);
        assert_eq!(result.state.sprite_x[0], b.sprites[0].x);

        b.sprites[0].palette = a.sprites[0].palette;
        b.pen_mask = 63;
        let stats = pair(&a, &b, 0.5).stats;
        assert_eq!(stats.sprites, 0);
        assert_eq!(stats.sprite_transform_rejections, 1);
        b.pen_mask = a.pen_mask;

        let mut a = snapshot(false);
        layer(&mut a);
        pf0(&mut a).source_x = 100 * 256;
        pf0(&mut a).source_y = 100;
        pf0(&mut a).y_fraction = 255;
        let mut b = a.clone();
        pf0(&mut b).source_x += 1;
        pf0(&mut b).source_y = 101;
        pf0(&mut b).y_fraction = 1;
        pf0(&mut b).palette_add = 0x1234;
        let result = pair(&a, &b, 0.5);
        assert_eq!(result.state.rows[24].source_x[0], 100 * 256 + 1);
        assert_eq!(result.state.rows[24].phase[0], 101 * 256);

        pf0(&mut b).layer.clip_enabled = 1;
        pf0(&mut b).layer.clip_inverted = 1;
        pf0(&mut b).layer.clip_inverse = false;
        row24(&mut b).clips[0] = crate::game::scene::SceneClip {
            left: 50,
            right: 300,
        };
        let result = pair(&a, &b, 0.5);
        assert_eq!(result.stats.playfield_rows, 0);
        assert_eq!(result.stats.row_control_rejections, 1);
        assert_eq!(result.state.rows[24].phase[0], 101 * 256 + 1);
    }

    #[test]
    fn text_scroll_and_independent_wrap() {
        let mut a = snapshot(false);
        {
            let text = &mut row24(&mut a).text;
            text.enabled = true;
            text.clip_inverse = true;
            row24(&mut a).text_x = 100;
            row24(&mut a).text_y = 100;
        }
        let mut b = a.clone();
        row24(&mut b).text_x = 101;
        row24(&mut b).text_y = 101;

        let mut result = pair(&a, &b, 0.5);
        assert_eq!(result.stats.text_rows, 1);
        assert_eq!(result.state.rows[24].text_x, 100 * 256 + 128);
        assert_eq!(result.state.rows[24].text_y, 100 * 256 + 128);

        row24(&mut b).text.clip_enabled = 1;
        row24(&mut b).text.clip_inverse = false;
        row24(&mut b).clips[0] = crate::game::scene::SceneClip {
            left: 50,
            right: 300,
        };
        result = pair(&a, &b, 0.5);
        assert_eq!(result.stats.text_rows, 0);
        assert_eq!(result.state.rows[24].text_x, 101 * 256);

        row24(&mut b).text.clip_enabled = 0;
        row24(&mut b).text.clip_inverse = true;
        row24(&mut a).text_x = 511;
        row24(&mut b).text_x = 0;
        result = pair(&a, &b, 0.5);
        assert_eq!(result.state.rows[24].text_x, 0);
        assert_eq!(result.state.rows[24].text_y, 100 * 256 + 128);
    }

    #[test]
    fn history_lifecycle_and_discontinuity() {
        let mut a = snapshot(false);
        sprite(&mut a, 0, 20, 10);
        let mut b = a.clone();
        sprite(&mut b, 0, 24, 10);

        let mut history = GpuMotionHistory::default();
        history.capture(&a, 10);
        history.capture(&b, 11);

        for alpha in [-0.1, 1.1, f32::INFINITY, f32::NAN] {
            assert!(
                !history.apply(&b, alpha).stats.paired,
                "invalid alpha paired: {alpha}"
            );
        }

        history.capture(&b, 11);
        assert!(
            !history.apply(&b, 0.5).stats.paired,
            "duplicate frame paired"
        );

        history.capture(&b, 13);
        assert!(!history.apply(&b, 0.5).stats.paired, "history gap paired");

        b.fallback = true;
        history.capture(&b, 14);
        b.fallback = false;
        history.capture(&b, 15);
        assert!(
            !history.apply(&b, 0.5).stats.paired,
            "fallback recovery paired too early"
        );

        history.capture(&b, 16);
        assert!(
            history.apply(&b, 0.5).stats.paired,
            "fallback recovery never resumed pairing"
        );

        history.reset();
        assert!(
            !history.apply(&b, 0.5).stats.paired,
            "reset retained pairing"
        );
    }

    #[test]
    fn endpoint_identity_and_monotonic_bounds() {
        for x0 in [-500i32, -137, -1, 0, 1, 255, 499] {
            for dx in [-32i32, -7, -1, 1, 5, 32] {
                let mut a = snapshot(false);
                sprite(&mut a, 0, x0, 10);
                let mut b = a.clone();
                sprite(&mut b, 0, x0 + dx, 10);

                let mut history = GpuMotionHistory::default();
                history.capture(&a, 10);
                history.capture(&b, 11);

                let res0 = history.apply(&b, 0.0);
                assert!(res0.stats.paired);
                assert_eq!(res0.stats.sprites, 1);
                assert_eq!(res0.state.sprite_x[0], x0 * 256);

                let res1 = history.apply(&b, 1.0);
                assert!(res1.stats.paired);
                assert_eq!(res1.stats.sprites, 1);
                assert_eq!(res1.state.sprite_x[0], (x0 + dx) * 256);

                for step in 0..=100 {
                    let alpha = step as f32 / 100.0;
                    let res = history.apply(&b, alpha);
                    let min_x = x0.min(x0 + dx) * 256;
                    let max_x = x0.max(x0 + dx) * 256;
                    assert!(res.state.sprite_x[0] >= min_x);
                    assert!(res.state.sprite_x[0] <= max_x);
                }
            }
        }
    }

    #[test]
    fn jump_threshold_rejection() {
        for x0 in [-300i32, 0, 299] {
            for jump_px in [33i32, 64, 100] {
                for sign in [1i32, -1] {
                    let mut a = snapshot(false);
                    sprite(&mut a, 0, x0, 10);
                    let mut b = a.clone();
                    sprite(&mut b, 0, x0 + sign * jump_px, 10);

                    let res = pair(&a, &b, 0.5);
                    assert!(res.stats.paired);
                    assert_eq!(res.stats.sprites, 0);
                    assert_eq!(res.stats.sprite_jump_rejections, 1);
                }
            }
        }
    }

    #[test]
    fn apply_equals_apply_into_and_dirty_output_untouched_when_unpaired() {
        let mut a = snapshot(false);
        sprite(&mut a, 0, 20, 10);
        let mut b = a.clone();
        sprite(&mut b, 0, 24, 10);

        let mut history = GpuMotionHistory::default();
        history.capture(&a, 10);
        history.capture(&b, 11);
        let mut out = MotionState::default();
        let stats = history.apply_into(&b, 0.5, &mut out);
        let result = history.apply(&b, 0.5);
        assert_eq!(stats, result.stats);
        assert_eq!(out.sprite_x, result.state.sprite_x);
        assert_eq!(out.sprite_y, result.state.sprite_y);
        assert_eq!(out.rows[24], result.state.rows[24]);

        let mut dirty = MotionState::default();
        dirty.sprite_x[0] = 12345;
        dirty.rows[24].text_x = -7;
        let fresh = GpuMotionHistory::default();
        let stats = fresh.apply_into(&b, 0.5, &mut dirty);
        assert!(!stats.paired);
        assert_eq!(dirty.sprite_x[0], 12345);
        assert_eq!(dirty.rows[24].text_x, -7);
    }

    #[test]
    fn flip_changes_clear_pairing() {
        let mut a = snapshot(false);
        sprite(&mut a, 0, 20, 10);
        let mut b = a.clone();
        sprite(&mut b, 0, 24, 10);

        let mut history = GpuMotionHistory::default();
        history.capture(&a, 10);
        history.capture(&b, 11);
        assert!(history.apply(&b, 0.5).stats.paired);

        b.flipped = true;
        history.capture(&b, 12);
        assert!(!history.apply(&b, 0.5).stats.paired, "screen flip paired");

        b.flipped = false;
        b.sprite_flipped = true;
        history.capture(&b, 13);
        assert!(!history.apply(&b, 0.5).stats.paired, "sprite flip paired");

        b.sprite_flipped = false;
        history.capture(&b, 14);
        assert!(
            !history.apply(&b, 0.5).stats.paired,
            "second flip recovery paired too early"
        );
        history.capture(&b, 15);
        assert!(history.apply(&b, 0.5).stats.paired);
    }

    #[test]
    fn motion_state_layout_matches_cpp() {
        assert_eq!(SceneSnapshot::MAX_SPRITES, 4096);
        let state = MotionState::default();
        assert_eq!(state.sprite_x.len(), 4096);
        assert_eq!(state.rows.len(), 256);
    }
}
