//! Temporal playfield interpolation analysis, mechanically ported from
//! `runtime/renderer/gpu/interp.{hpp,cpp}`.
//!
//! On the canonical frame the coefficients are zero; the shader evaluates the native-anchored
//! cubic from them. Motion-effective geometry comes from [`MotionState`] (`None`: the canonical
//! frame).

use crate::config::{GameVideoOptions, NATIVE_WIDTH};
use crate::game::clip::{ClipRanges, clip_ranges_for_row};
use crate::game::scene::{SceneRow, wrap_tile_index};
use crate::game::scene_snapshot::SceneSnapshot;
use crate::gpu::motion::MotionState;
use crate::gpu::scene_layout::{
    F3_INTERP_FLAG_PALETTE, F3_INTERP_FLAG_SOURCE, F3_INTERP_FLAG_VERTICAL, F3_INTERP_FLAG_ZOOM,
};

/// `VideoInterpolation`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum VideoInterpolation {
    #[default]
    Off,
    Linear,
    Fit,
}

/// `InterpolationFields`: which coefficient families are written.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct InterpolationFields(u8);

#[allow(non_upper_case_globals)]
impl InterpolationFields {
    pub const None: Self = Self(0);
    pub const Geometry: Self = Self(1);
    pub const Palette: Self = Self(2);
    pub const All: Self = Self(3);

    pub const fn bits(self) -> u8 {
        self.0
    }

    pub const fn contains(self, other: Self) -> bool {
        self.0 & other.0 == other.0
    }

    pub const fn from_bits_truncate(bits: u8) -> Self {
        Self(bits & 3)
    }
}

/// `parse_interpolation_fields`.
pub fn parse_interpolation_fields(value: &str) -> Option<InterpolationFields> {
    match value {
        "none" => Some(InterpolationFields::None),
        "geometry" => Some(InterpolationFields::Geometry),
        "palette" => Some(InterpolationFields::Palette),
        "geometry,palette" | "palette,geometry" => Some(InterpolationFields::All),
        _ => None,
    }
}

/// `interpolation_fields_name`.
pub const fn interpolation_fields_name(fields: InterpolationFields) -> &'static str {
    match fields.bits() {
        0 => "none",
        1 => "geometry",
        2 => "palette",
        3 => "geometry,palette",
        _ => "unknown",
    }
}

/// `InterpolationReason`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum InterpolationReason {
    #[default]
    Off,
    NativeScale,
    Oracle,
    NoEffect,
    InvalidRows,
    DiscreteBoundary,
    PaletteDiscontinuity,
    Applied,
}

/// `InterpolationConfig`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct InterpolationConfig {
    pub mode: VideoInterpolation,
    pub fields: InterpolationFields,
}

impl Default for InterpolationConfig {
    fn default() -> Self {
        Self {
            mode: VideoInterpolation::Off,
            fields: InterpolationFields::Geometry,
        }
    }
}

/// `LayerInterpolationStats`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LayerInterpolationStats {
    pub reason: InterpolationReason,
    pub source_rows: u32,
    pub zoom_rows: u32,
    pub vertical_rows: u32,
    pub palette_rows: u32,
    pub invalid_rows: u32,
    pub discontinuities: u32,
    pub unsafe_palette_pairs: u32,
}

impl Default for LayerInterpolationStats {
    fn default() -> Self {
        Self {
            reason: InterpolationReason::NoEffect,
            source_rows: 0,
            zoom_rows: 0,
            vertical_rows: 0,
            palette_rows: 0,
            invalid_rows: 0,
            discontinuities: 0,
            unsafe_palette_pairs: 0,
        }
    }
}

/// `InterpolationStats`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct InterpolationStats {
    pub reason: InterpolationReason,
    pub layers: [LayerInterpolationStats; 4],
    pub row_fields: [[u8; 256]; 4],
}

impl Default for InterpolationStats {
    fn default() -> Self {
        Self {
            reason: InterpolationReason::Off,
            layers: [LayerInterpolationStats::default(); 4],
            row_fields: [[0; 256]; 4],
        }
    }
}

/// Native-anchored polynomial increments for one playfield row; the canonical frame stays
/// unchanged. Each triple is the cubic's (c1, c2, c3); a triple is zero unless its flag is set.
pub const INTERPOLATION_SOURCE: u32 = F3_INTERP_FLAG_SOURCE;
pub const INTERPOLATION_ZOOM: u32 = F3_INTERP_FLAG_ZOOM;
pub const INTERPOLATION_VERTICAL: u32 = F3_INTERP_FLAG_VERTICAL;
pub const INTERPOLATION_PALETTE: u32 = F3_INTERP_FLAG_PALETTE;

/// `PlayfieldCoefficients`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PlayfieldCoefficients {
    /// interpolation_* bits
    pub flags: u8,
    /// bank stride, meaningful with [`INTERPOLATION_PALETTE`]
    pub palette_stride: u16,
    pub source: [f32; 3],
    pub zoom: [f32; 3],
    pub vertical: [f32; 3],
    pub palette: [f32; 3],
}

/// `InterpolationCoefficients`: `[scanline][playfield]`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct InterpolationCoefficients {
    pub rows: [[PlayfieldCoefficients; 4]; 256],
}

impl Default for InterpolationCoefficients {
    fn default() -> Self {
        Self {
            rows: [[PlayfieldCoefficients::default(); 4]; 256],
        }
    }
}

/// `InterpolationAnalysis`.
#[derive(Clone, Default)]
pub struct InterpolationAnalysis {
    pub stats: InterpolationStats,
    pub coefficients: InterpolationCoefficients,
}

/// Motion-effective geometry comes from `motion` (`None`: the canonical frame). Writes the
/// coefficients into caller-owned `out` (no 56 KiB return copy). `out` is rewritten whenever
/// the frame can upload a coefficient region; the Off, NativeScale and Oracle rejections leave
/// it untouched because nothing reads it.
pub fn analyze_gpu_interpolation_into(
    frame: &SceneSnapshot,
    options: GameVideoOptions,
    mode: VideoInterpolation,
    fields: InterpolationFields,
    motion: Option<&MotionState>,
    tile_pen_masks: &[u64],
    out: &mut InterpolationCoefficients,
) -> InterpolationStats {
    let mut stats = InterpolationStats::default();
    let reject = |stats: &mut InterpolationStats, reason| {
        stats.reason = reason;
        for layer in &mut stats.layers {
            layer.reason = reason;
        }
    };
    if mode == VideoInterpolation::Off {
        reject(&mut stats, InterpolationReason::Off);
        return stats;
    }
    if options.scale == 1 {
        reject(&mut stats, InterpolationReason::NativeScale);
        return stats;
    }
    if frame.fallback {
        reject(&mut stats, InterpolationReason::Oracle);
        return stats;
    }
    *out = InterpolationCoefficients::default();
    if options.scale == 0
        || options.scale > GameVideoOptions::MAX_GPU_SCALE
        || options.border > GameVideoOptions::MAX_BORDER
    {
        reject(&mut stats, InterpolationReason::InvalidRows);
        return stats;
    }
    stats.reason = InterpolationReason::NoEffect;
    let geometry = fields.contains(InterpolationFields::Geometry);
    let palette = fields.contains(InterpolationFields::Palette);
    if !geometry && !palette {
        return stats;
    }

    let frame_geometry = frame.geometry();
    let first_row = frame_geometry.first_line() as usize;
    let end_row = frame_geometry.end_line() as usize;
    let extend = frame.video().extend;

    for pf in 0..4 {
        let mut layer = LayerInterpolationStats::default();
        let mut rows = [Row::default(); 256];
        let mut clips = [ClipRanges::default(); 256];
        let mut links = [false; 256];
        for y in first_row..end_row {
            let row = &frame.rows[y];
            let p = &row.playfields[pf];
            let source = motion.map_or(p.source_x, |m| m.rows[y].source_x[pf]);
            let phase = motion.map_or(
                (p.source_y as u32 * 256 + u32::from(p.y_fraction)) as i32,
                |m| m.rows[y].phase[pf],
            );
            let mut clip = ClipRanges::default();
            let valid = valid_row(row, pf, options, &mut clip);
            clips[y] = clip;
            rows[y] = Row {
                source,
                zoom: p.x_step as u32 as i32,
                phase,
                vertical: p.y_step as u32 as i32,
                palette: u32::from(p.palette_add) as i32,
                valid,
            };
            if !valid && p.layer.enabled {
                layer.invalid_rows += 1;
            }
            if y == first_row || !valid || !rows[y - 1].valid {
                continue;
            }
            links[y - 1] = same_controls(&frame.rows[y - 1], row, &clips[y - 1], &clips[y], pf)
                && phase_delta(rows[y - 1].phase, rows[y].phase, 512 * 256) == rows[y - 1].vertical;
            if !links[y - 1] {
                layer.discontinuities += 1;
            }
        }

        // Each field gets its own maximal valid run: palette cannot disable geometry.
        for field in 0..4usize {
            if (field < 3 && !geometry) || (field == 3 && !palette) {
                continue;
            }
            let mut delta = [0i32; 256];
            let mut continuous = [false; 256];
            for y in first_row..end_row.saturating_sub(1) {
                if !links[y] {
                    continue;
                }
                let a = rows[y];
                let b = rows[y + 1];
                if field == 0 {
                    delta[y] =
                        phase_delta(a.source, b.source, (if extend { 1024 } else { 512 }) * 256);
                }
                if field == 1 {
                    delta[y] = b.zoom - a.zoom;
                }
                if field == 2 {
                    delta[y] = phase_delta(a.phase, b.phase, 512 * 256);
                }
                if field == 3 {
                    delta[y] = b.palette - a.palette;
                }
                continuous[y] = (field != 0 || delta[y].abs() <= 4096)
                    && (field != 1 || delta[y].abs() <= 8)
                    && (field != 2 || (b.vertical - a.vertical).abs() <= 8)
                    && (field != 3 || (a.palette % 16 == 0 && b.palette % 16 == 0));
                if !continuous[y] {
                    layer.discontinuities += 1;
                }
            }
            let mut start = first_row;
            while start < end_row {
                let mut last = start;
                let mut direction = 0i32;
                while last + 1 < end_row && continuous[last] {
                    if field == 3 && delta[last] != 0 {
                        let sign = if delta[last] > 0 { 1 } else { -1 };
                        if direction != 0 && direction != sign {
                            break;
                        }
                        direction = sign;
                    }
                    last += 1;
                }
                let mut steps = 0u32;
                let mut stride = 0u32;
                for y in start..last {
                    let stepped = if field == 2 {
                        rows[y].vertical != rows[y + 1].vertical
                    } else {
                        delta[y] != 0
                    };
                    if stepped {
                        steps += 1;
                    }
                    if field == 3 {
                        stride = gcd(stride, delta[y].unsigned_abs());
                    }
                }
                // Palette consumer banks are rooted at zero; include the run's origin so an
                // offset ramp cannot accidentally interpolate different pens.
                if field == 3 {
                    stride = gcd(stride, rows[start].palette as u32);
                }
                if last - start >= 3
                    && steps >= 2
                    && (field != 3 || (stride != 0 && stride.is_multiple_of(16)))
                {
                    let mut safe = [false; 256];
                    if field == 3 {
                        for y in start..last {
                            safe[y] = delta[y] == 0
                                || safe_palette(
                                    frame,
                                    &rows[y],
                                    &rows[y + 1],
                                    pf,
                                    stride,
                                    options,
                                    tile_pen_masks,
                                );
                            if !safe[y] {
                                layer.unsafe_palette_pairs += 1;
                            }
                        }
                    }
                    for y in (start + 1)..=last {
                        // Four real neighbors only, with raw run endpoints and no unsafe
                        // derivative samples.
                        if y + 2 > last
                            || delta[y] == 0
                            || (field == 3 && (!safe[y - 1] || !safe[y] || !safe[y + 1]))
                        {
                            continue;
                        }
                        let coeffs = &mut out.rows[y][pf];
                        let triple = match field {
                            0 => &mut coeffs.source,
                            1 => &mut coeffs.zoom,
                            2 => &mut coeffs.vertical,
                            _ => &mut coeffs.palette,
                        };
                        coefficients(
                            triple,
                            delta[y - 1] as f32,
                            delta[y] as f32,
                            delta[y + 1] as f32,
                            mode,
                        );
                        let flag = 1u8 << field;
                        coeffs.flags |= flag;
                        if field == 3 {
                            coeffs.palette_stride = stride as u16;
                        }
                        stats.row_fields[pf][y] |= flag;
                        match field {
                            0 => layer.source_rows += 1,
                            1 => layer.zoom_rows += 1,
                            2 => layer.vertical_rows += 1,
                            _ => layer.palette_rows += 1,
                        }
                    }
                }
                start = last + 1;
            }
        }

        if layer.source_rows != 0
            || layer.zoom_rows != 0
            || layer.vertical_rows != 0
            || layer.palette_rows != 0
        {
            layer.reason = InterpolationReason::Applied;
        } else if layer.unsafe_palette_pairs != 0 {
            layer.reason = InterpolationReason::PaletteDiscontinuity;
        } else if layer.discontinuities != 0 {
            layer.reason = InterpolationReason::DiscreteBoundary;
        } else if layer.invalid_rows != 0 {
            layer.reason = InterpolationReason::InvalidRows;
        }
        if layer.reason == InterpolationReason::Applied {
            stats.reason = InterpolationReason::Applied;
        } else if stats.reason != InterpolationReason::Applied
            && layer.reason != InterpolationReason::NoEffect
        {
            stats.reason = layer.reason;
        }
        stats.layers[pf] = layer;
    }
    stats
}

/// `analyze_gpu_interpolation`.
pub fn analyze_gpu_interpolation(
    frame: &SceneSnapshot,
    options: GameVideoOptions,
    mode: VideoInterpolation,
    fields: InterpolationFields,
    motion: Option<&MotionState>,
    tile_pen_masks: &[u64],
) -> InterpolationAnalysis {
    let mut result = InterpolationAnalysis::default();
    result.stats = analyze_gpu_interpolation_into(
        frame,
        options,
        mode,
        fields,
        motion,
        tile_pen_masks,
        &mut result.coefficients,
    );
    result
}

/// Prints one `video_interp ...` line to stdout whenever the observed reason/layer outcome
/// changes.
#[derive(Clone, Default)]
pub struct InterpolationLog {
    layers: [LayerInterpolationStats; 4],
    reason: InterpolationReason,
    observed: bool,
}

impl InterpolationLog {
    pub fn reset(&mut self) {
        self.observed = false;
    }

    pub fn observe(&mut self, config: InterpolationConfig, stats: &InterpolationStats) {
        if config.mode == VideoInterpolation::Off {
            return;
        }
        if self.observed && self.reason == stats.reason && self.layers == stats.layers {
            return;
        }
        println!("{}", Self::line(config, stats));
        self.reason = stats.reason;
        self.layers = stats.layers;
        self.observed = true;
    }

    fn line(config: InterpolationConfig, stats: &InterpolationStats) -> String {
        let mut line = format!(
            "video_interp fields={} reason={}",
            interpolation_fields_name(config.fields),
            interpolation_reason_name(stats.reason)
        );
        for (i, layer) in stats.layers.iter().enumerate() {
            line.push_str(&format!(
                " pf{i}={}:{}/{}/{}/{} invalid{i}={} boundaries{i}={} unsafe_palette{i}={}",
                interpolation_reason_name(layer.reason),
                layer.source_rows,
                layer.zoom_rows,
                layer.vertical_rows,
                layer.palette_rows,
                layer.invalid_rows,
                layer.discontinuities,
                layer.unsafe_palette_pairs
            ));
        }
        line
    }
}

/// `interpolation_reason_name`.
pub const fn interpolation_reason_name(reason: InterpolationReason) -> &'static str {
    match reason {
        InterpolationReason::Off => "off",
        InterpolationReason::NativeScale => "native-scale",
        InterpolationReason::Oracle => "oracle",
        InterpolationReason::NoEffect => "no-effect",
        InterpolationReason::InvalidRows => "invalid-rows",
        InterpolationReason::DiscreteBoundary => "discrete-boundary",
        InterpolationReason::PaletteDiscontinuity => "palette-discontinuity",
        InterpolationReason::Applied => "applied",
    }
}

#[derive(Clone, Copy, Default)]
struct Row {
    source: i32,
    zoom: i32,
    phase: i32,
    vertical: i32,
    palette: i32,
    valid: bool,
}

fn phase_delta(a: i32, b: i32, period: i32) -> i32 {
    let mut delta = (b - a) % period;
    if delta > period / 2 {
        delta -= period;
    }
    if delta < -period / 2 {
        delta += period;
    }
    delta
}

fn floor_divide(n: i64, d: i64) -> i64 {
    if n >= 0 { n / d } else { -1 - (-1 - n) / d }
}

fn tangent(a: f32, b: f32) -> f32 {
    if a * b <= 0.0 {
        0.0
    } else {
        2.0 * a * b / (a + b)
    }
}

fn coefficients(
    out: &mut [f32; 3],
    previous: f32,
    delta: f32,
    next: f32,
    mode: VideoInterpolation,
) {
    let m0 = if mode == VideoInterpolation::Fit {
        tangent(previous, delta)
    } else {
        delta
    };
    let m1 = if mode == VideoInterpolation::Fit {
        tangent(delta, next)
    } else {
        delta
    };
    *out = [m0, 3.0 * delta - 2.0 * m0 - m1, m0 + m1 - 2.0 * delta];
}

/// Fills `clips` (visible ranges at the render border) for rows that pass the cheap checks.
fn valid_row(row: &SceneRow, pf: usize, options: GameVideoOptions, clips: &mut ClipRanges) -> bool {
    let p = &row.playfields[pf];
    let layer = p.layer;
    // Alternate-map rows switch the sampled map per scanline; never interpolated across.
    if row.bitmap
        || row.palette_15bit
        || !layer.enabled
        || layer.mosaic
        || p.alt_map
        || (p.source_y as u32) > 511
        || p.x_step as u32 == 0
        || p.x_step as u32 > 256
        || p.y_step as u32 == 0
        || p.y_step as u32 > 510
        || p.palette_add > 8191
        || (i64::from(p.source_x)).abs() > (1i64 << 24)
    {
        return false;
    }
    for value in row.blend {
        if value > 8 {
            return false;
        }
    }
    let left_edge = (46 - options.border as i32) as i16;
    let right_edge = (366 + options.border as i32) as i16;
    *clips = clip_ranges_for_row(row, &layer, left_edge, right_edge);
    for i in 0..clips.count {
        let left = i32::from(clips.ranges[i].left);
        let right = i32::from(clips.ranges[i].right);
        if left >= right || left < i32::from(left_edge) || right > i32::from(right_edge) {
            return false;
        }
    }
    true
}

fn same_controls(a: &SceneRow, b: &SceneRow, ca: &ClipRanges, cb: &ClipRanges, pf: usize) -> bool {
    a.blend == b.blend
        && a.playfields[pf].layer == b.playfields[pf].layer
        && ca.count == cb.count
        && ca.as_slice() == cb.as_slice()
}

fn rgb_jump(a: u32, b: u32) -> u32 {
    let mut maximum = 0u32;
    for shift in [0u32, 8, 16] {
        maximum = maximum.max(((a >> shift) & 255).abs_diff((b >> shift) & 255));
    }
    maximum
}

/// Conservative endpoint-bounded tile footprint, including border and fractional taps. Every
/// bank traversed by the polynomial is checked against actual tile pen sets.
fn safe_palette(
    frame: &SceneSnapshot,
    a: &Row,
    b: &Row,
    pf: usize,
    stride: u32,
    options: GameVideoOptions,
    masks: &[u64],
) -> bool {
    if masks.is_empty() {
        return false;
    }
    let pf_period = frame.video().pf_map_width_px() as i32 * 256;
    let source_b = a.source + phase_delta(a.source, b.source, pf_period);
    // Source and zoom have independent cubics: all endpoint combinations, not only paired
    // endpoints, bound their combined horizontal footprint.
    let maximum_zoom = i64::from(a.zoom.max(b.zoom));
    let x0 = i64::from(a.source.min(source_b)) - i64::from(options.border) * maximum_zoom;
    let x1 =
        i64::from(a.source.max(source_b)) + i64::from(NATIVE_WIDTH + options.border) * maximum_zoom;
    let phase_b = a.phase + phase_delta(a.phase, b.phase, 512 * 256);
    let y0 = floor_divide(i64::from(a.phase.min(phase_b)), 16 * 256) as i32;
    let y1 = floor_divide(i64::from(a.phase.max(phase_b)) + 256, 16 * 256) as i32;
    let low = a.palette.min(b.palette);
    let high = a.palette.max(b.palette);
    let flip = if frame.flipped { u32::MAX } else { 0 };
    let columns = frame.tiles.columns() as u32;
    let cells = frame.tiles.cells(pf);
    for cy in y0..=y1 {
        let mut cx = floor_divide(x0 - 256, 16 * 256);
        while cx <= floor_divide(x1 + 256, 16 * 256) {
            let cell = ((cy as u32 ^ flip) & 31) * columns + ((cx as u32 ^ flip) & (columns - 1));
            // Raw 4-byte PF cell (attributes<<16 | code); see runtime/renderer/game/tiles.hpp.
            let raw = cells[cell as usize];
            let attr = raw >> 16;
            let code = raw & 65535;
            let base = (attr & 511) * 16;
            let mask = (((attr >> 10) & 3 & !attr) << 4) | 15;
            if base >= 8192 || mask > 63 {
                return false;
            }
            let mut pens = masks[wrap_tile_index(code, masks.len() as u32) as usize];
            let step = stride as usize;
            if step == 0 {
                return false;
            }
            while pens != 0 {
                let pen = pens.trailing_zeros() & mask;
                pens &= pens - 1;
                if pen == 0 {
                    continue;
                }
                let mut bank = low;
                while bank < high {
                    if rgb_jump(
                        frame.colors[((base + pen + bank as u32) & 8191) as usize],
                        frame.colors[((base + pen + bank as u32 + stride) & 8191) as usize],
                    ) > 32
                    {
                        return false;
                    }
                    bank += stride as i32;
                }
            }
            cx += 1;
        }
    }
    true
}

fn gcd(a: u32, b: u32) -> u32 {
    let mut a = a;
    let mut b = b;
    while b != 0 {
        let t = a % b;
        a = b;
        b = t;
    }
    a
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::VideoConfig;
    use crate::game::scene::ScenePlayfield;

    fn scene(first_line: usize, end_line: usize, delta: i32) -> Box<SceneSnapshot> {
        let mut scene = Box::new(SceneSnapshot::new(&VideoConfig::default()));
        scene.fallback = false;
        for y in first_line..end_line {
            let row = y - first_line;
            let p = &mut scene.rows[y].playfields[0];
            p.layer.enabled = true;
            p.layer.clip_inverse = true;
            p.source_x = (100 + delta * row as i32) * 256;
            p.source_y = 100 + row as i32;
            p.x_step = 256;
            p.y_step = 256;
        }
        scene
    }

    fn options(scale: u32, border: u32) -> GameVideoOptions {
        GameVideoOptions { scale, border }
    }

    #[test]
    fn linear_and_fit_modes_apply_source_ramp() {
        let scene = scene(24, 256, 2);
        let mut out = InterpolationCoefficients::default();
        let stats = analyze_gpu_interpolation_into(
            &scene,
            options(2, 0),
            VideoInterpolation::Linear,
            InterpolationFields::Geometry,
            None,
            &[],
            &mut out,
        );
        assert_eq!(stats.reason, InterpolationReason::Applied);
        assert!(stats.layers[0].source_rows > 0);
        let row = 34;
        assert_eq!(
            out.rows[row][0].source,
            [512.0, 0.0, 0.0],
            "linear ramp is a constant 2.0 increment"
        );
        assert_eq!(out.rows[row][0].flags & 1, 1);
        assert_eq!(out.rows[row][1], PlayfieldCoefficients::default());

        let fit = analyze_gpu_interpolation(
            &scene,
            options(2, 0),
            VideoInterpolation::Fit,
            InterpolationFields::Geometry,
            None,
            &[],
        );
        assert_eq!(fit.stats.reason, InterpolationReason::Applied);
        assert_eq!(fit.coefficients.rows[row][0].source, [512.0, 0.0, 0.0]);
    }

    #[test]
    fn rejects_off_native_scale_and_oracle_without_touching_out() {
        let scene = scene(24, 256, 2);
        for (mode, scale, reason) in [
            (VideoInterpolation::Off, 2, InterpolationReason::Off),
            (VideoInterpolation::Fit, 1, InterpolationReason::NativeScale),
        ] {
            let mut out = InterpolationCoefficients::default();
            out.rows[10][0].flags = 0xff;
            let stats = analyze_gpu_interpolation_into(
                &scene,
                options(scale, 0),
                mode,
                InterpolationFields::All,
                None,
                &[],
                &mut out,
            );
            assert_eq!(stats.reason, reason);
            assert_eq!(out.rows[10][0].flags, 0xff, "rejection rewrote out");
        }

        let mut fallback = scene.clone();
        fallback.fallback = true;
        let mut out = InterpolationCoefficients::default();
        out.rows[10][0].flags = 0xff;
        let stats = analyze_gpu_interpolation_into(
            &fallback,
            options(2, 0),
            VideoInterpolation::Linear,
            InterpolationFields::All,
            None,
            &[],
            &mut out,
        );
        assert_eq!(stats.reason, InterpolationReason::Oracle);
        assert_eq!(out.rows[10][0].flags, 0xff);
    }

    #[test]
    fn invalid_options_zero_out_then_reject() {
        let scene = scene(24, 256, 2);
        let mut out = InterpolationCoefficients::default();
        out.rows[10][0].flags = 0xff;
        let stats = analyze_gpu_interpolation_into(
            &scene,
            options(9, 0),
            VideoInterpolation::Linear,
            InterpolationFields::Geometry,
            None,
            &[],
            &mut out,
        );
        assert_eq!(stats.reason, InterpolationReason::InvalidRows);
        assert_eq!(out.rows[10][0], PlayfieldCoefficients::default());
    }

    #[test]
    fn fields_gate_geometry_and_palette() {
        let scene = scene(24, 256, 2);
        let mut out = InterpolationCoefficients::default();
        let stats = analyze_gpu_interpolation_into(
            &scene,
            options(2, 0),
            VideoInterpolation::Linear,
            InterpolationFields::Palette,
            None,
            &[],
            &mut out,
        );
        assert_eq!(stats.layers[0].source_rows, 0);
        assert_eq!(stats.reason, InterpolationReason::NoEffect);

        let stats = analyze_gpu_interpolation_into(
            &scene,
            options(2, 0),
            VideoInterpolation::Linear,
            InterpolationFields::None,
            None,
            &[],
            &mut out,
        );
        assert_eq!(stats.reason, InterpolationReason::NoEffect);

        let stats = analyze_gpu_interpolation_into(
            &scene,
            options(2, 0),
            VideoInterpolation::Linear,
            InterpolationFields::All,
            None,
            &[],
            &mut out,
        );
        assert!(stats.layers[0].source_rows > 0, "All must include geometry");
        assert_eq!(
            stats.layers[0].palette_rows, 0,
            "empty pen masks gate palette"
        );
    }

    #[test]
    fn vertical_discontinuity_splits_runs() {
        let mut scene = scene(24, 256, 2);
        // Break the vertical phase ramp at row 140.
        let p = &mut scene.rows[140].playfields[0];
        p.source_y += 100;
        let stats = analyze_gpu_interpolation(
            &scene,
            options(2, 0),
            VideoInterpolation::Linear,
            InterpolationFields::Geometry,
            None,
            &[],
        );
        assert!(stats.stats.layers[0].discontinuities > 0);
    }

    #[test]
    fn pen_mask_gates_palette_and_motion_state_is_used() {
        let mut scene = scene(24, 256, 2);
        for y in 24..256 {
            let p = &mut scene.rows[y].playfields[0];
            p.palette_add = ((y - 24) as u16) * 16;
        }
        for c in 0..8192 {
            scene.colors[c] = 0x0010_1010;
        }
        // No pen masks: palette interpolation is unsafe, so no palette coefficients.
        let stats = analyze_gpu_interpolation(
            &scene,
            options(2, 0),
            VideoInterpolation::Linear,
            InterpolationFields::Palette,
            None,
            &[],
        );
        assert_eq!(stats.stats.layers[0].palette_rows, 0);
        assert_eq!(
            stats.stats.layers[0].reason,
            InterpolationReason::PaletteDiscontinuity
        );

        let mut motion = MotionState::default();
        for y in 24..256 {
            motion.rows[y].source_x[0] = (100 + 4 * (y - 24) as i32) * 256;
            motion.rows[y].phase[0] = (100 + (y - 24) as i32) * 256;
        }
        let analyzed = analyze_gpu_interpolation(
            &scene,
            options(2, 0),
            VideoInterpolation::Linear,
            InterpolationFields::Geometry,
            Some(&motion),
            &[],
        );
        assert_eq!(analyzed.stats.reason, InterpolationReason::Applied);
        assert_eq!(analyzed.coefficients.rows[30][0].source, [1024.0, 0.0, 0.0]);
    }

    #[test]
    fn dirty_coefficient_scratch_matches_fresh_scratch() {
        let scene = scene(24, 256, 2);
        let mut fresh = InterpolationCoefficients::default();
        let fresh_stats = analyze_gpu_interpolation_into(
            &scene,
            options(2, 0),
            VideoInterpolation::Linear,
            InterpolationFields::Geometry,
            None,
            &[],
            &mut fresh,
        );

        let mut dirty = InterpolationCoefficients::default();
        dirty.rows[30][0].flags = 0xff;
        dirty.rows[30][0].source = [7.0, 8.0, 9.0];
        dirty.rows[200][2].palette = [1.0, 2.0, 3.0];
        let dirty_stats = analyze_gpu_interpolation_into(
            &scene,
            options(2, 0),
            VideoInterpolation::Linear,
            InterpolationFields::Geometry,
            None,
            &[],
            &mut dirty,
        );
        assert_eq!(fresh_stats, dirty_stats);
        assert_eq!(fresh, dirty);
    }

    #[test]
    fn log_formats_and_deduplicates() {
        let stats = InterpolationStats {
            reason: InterpolationReason::Applied,
            layers: [LayerInterpolationStats {
                reason: InterpolationReason::Applied,
                source_rows: 3,
                zoom_rows: 1,
                vertical_rows: 2,
                palette_rows: 0,
                invalid_rows: 4,
                discontinuities: 5,
                unsafe_palette_pairs: 6,
            }; 4],
            ..InterpolationStats::default()
        };
        let config = InterpolationConfig {
            mode: VideoInterpolation::Linear,
            fields: InterpolationFields::All,
        };
        assert_eq!(
            InterpolationLog::line(config, &stats),
            "video_interp fields=geometry,palette reason=applied \
pf0=applied:3/1/2/0 invalid0=4 boundaries0=5 unsafe_palette0=6 \
pf1=applied:3/1/2/0 invalid1=4 boundaries1=5 unsafe_palette1=6 \
pf2=applied:3/1/2/0 invalid2=4 boundaries2=5 unsafe_palette2=6 \
pf3=applied:3/1/2/0 invalid3=4 boundaries3=5 unsafe_palette3=6"
        );
        assert_eq!(
            interpolation_reason_name(InterpolationReason::PaletteDiscontinuity),
            "palette-discontinuity"
        );
        assert_eq!(
            interpolation_fields_name(InterpolationFields::All),
            "geometry,palette"
        );
        assert!(matches!(
            parse_interpolation_fields("palette,geometry"),
            Some(InterpolationFields::All)
        ));
        assert!(parse_interpolation_fields("bogus").is_none());
    }

    #[test]
    fn palette_ramp_with_safe_masks_gets_palette_coefficients() {
        let mut scene = scene(24, 256, 0);
        for y in 24..256 {
            let p = &mut scene.rows[y].playfields[0];
            p.palette_add = (y as u16 - 24) * 16;
            p.source_x = 0;
        }
        for c in 0..8192 {
            let bank = (c / 16) as u32;
            scene.colors[c] = 0x0010_1010 + bank;
        }
        let cell = 0x0000_0000u32;
        {
            let cells = scene.tiles.cells_mut(0);
            for c in cells.iter_mut() {
                *c = cell;
            }
        }
        // Tile 0 has no nonzero pens, so every bank pair is safe.
        let stats = analyze_gpu_interpolation(
            &scene,
            options(2, 0),
            VideoInterpolation::Linear,
            InterpolationFields::Palette,
            None,
            &[0],
        );
        assert!(stats.stats.layers[0].palette_rows > 0);
        assert_eq!(stats.stats.layers[0].reason, InterpolationReason::Applied);
        let row = 40;
        assert_eq!(stats.coefficients.rows[row][0].palette_stride, 16);
        assert_eq!(stats.coefficients.rows[row][0].flags & 8, 8);
        assert_eq!(stats.coefficients.rows[row][0].palette, [16.0, 0.0, 0.0]);
    }

    #[test]
    fn scene_playfield_defaults_are_intact_for_analysis() {
        assert_eq!(ScenePlayfield::default().x_step, 256);
    }
}
