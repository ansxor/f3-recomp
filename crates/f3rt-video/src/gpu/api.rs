//! SDL-free renderer request types and validation, shared by the GPU backend and the ABI stubs.

use crate::config::{GameVideoOptions, SpriteList};
use crate::gpu::encode::ALL_LAYERS;

/// A presented or drawn texture token and its pixel size. The GPU backend casts `texture` to
/// `SDL_GPUTexture *`; every other module treats it as opaque.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct GpuSurface {
    pub texture: usize,
    pub width: u32,
    pub height: u32,
}

impl GpuSurface {
    pub fn is_valid(&self) -> bool {
        self.texture != 0
    }
}

/// Presentation postprocessing preset.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Postprocess {
    #[default]
    Off,
    Crt,
    User,
}

/// One offscreen draw request.
#[derive(Clone, Copy, Debug)]
pub struct RenderRequest {
    pub sprites: SpriteList,
    /// Output/layer-mask diagnostics remain baseline unless `postprocess` is explicitly true.
    pub layer_mask: u32,
    /// Temporal geometry at this alpha (needs captured motion history); `None` renders the
    /// canonical frame.
    pub alpha: Option<f32>,
    pub postprocess: bool,
}

impl Default for RenderRequest {
    fn default() -> Self {
        Self {
            sprites: SpriteList::Presented,
            layer_mask: ALL_LAYERS,
            alpha: None,
            postprocess: false,
        }
    }
}

/// How a presenting command buffer got its drawable.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PresentAcquire {
    DrawableReady,
    HeadlessProceed,
    SkipFrame,
}

/// Frame timing for presentation.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub enum PresentTiming {
    /// Static: canonical alpha.
    #[default]
    Canonical,
    /// Fixed alpha.
    Fixed(f32),
    /// Live alpha sampled after acquisition against the monotonic clock.
    Live { frame_start_ns: u64, period_ns: i64 },
}

impl PresentTiming {
    pub fn is_live(&self) -> bool {
        matches!(self, Self::Live { .. })
    }
}

/// One presentation request. `capture_path` is borrowed for the call.
#[derive(Clone, Copy, Debug)]
pub struct PresentRequest<'a> {
    /// Block for the swapchain and any display tick. False skips the frame when the GPU is busy:
    /// nothing is uploaded or rendered, and no fence is waited on.
    pub wait: bool,
    pub timing: PresentTiming,
    /// Empty: one full-window blit. Otherwise the left half shows this (already oriented)
    /// surface and the right half the presented game image.
    pub left_pane: GpuSurface,
    /// One-shot PNG of the full-window composition submitted to the drawable.
    pub capture_path: Option<&'a str>,
}

impl Default for PresentRequest<'_> {
    fn default() -> Self {
        Self {
            wait: false,
            timing: PresentTiming::Canonical,
            left_pane: GpuSurface::default(),
            capture_path: None,
        }
    }
}

impl PresentRequest<'_> {
    /// Live motion alpha is the elapsed fraction of the native frame period.
    pub fn sample_alpha(&self, now_ns: u64) -> Option<f32> {
        match self.timing {
            PresentTiming::Canonical => None,
            PresentTiming::Fixed(alpha) => Some(alpha),
            PresentTiming::Live {
                frame_start_ns,
                period_ns,
            } => {
                if period_ns <= 0 {
                    return Some(1.0);
                }
                let elapsed = (i128::from(now_ns) - i128::from(frame_start_ns)) as f64 / 1.0e9;
                let period = period_ns as f64 / 1.0e9;
                Some((elapsed / period).clamp(0.0, 1.0) as f32)
            }
        }
    }
}

/// Bytes per 16x16 tile.
pub const TILE_BYTES: u32 = 256;

/// Throws unless both ROMs are non-empty whole tiles that fit 32-bit offsets.
pub fn validate_tile_assets(pf_bytes: usize, sp_bytes: usize) -> Result<(), String> {
    let tile_bytes = TILE_BYTES as usize;
    let half = u32::MAX as usize / 2;
    if pf_bytes == 0
        || sp_bytes == 0
        || !pf_bytes.is_multiple_of(tile_bytes)
        || !sp_bytes.is_multiple_of(tile_bytes)
        || pf_bytes > half
        || sp_bytes > half
        || pf_bytes + sp_bytes > u32::MAX as usize
    {
        return Err("Incomplete GPU tile/sprite assets".to_owned());
    }
    Ok(())
}

/// Throws unless `rotation` is 0, 90 or 270.
pub fn validate_rotation(rotation: u32) -> Result<(), String> {
    if rotation != 0 && rotation != 90 && rotation != 270 {
        return Err("Unsupported GPU presentation rotation".to_owned());
    }
    Ok(())
}

/// Host-only GPU presentation options and asset validation, independent of any device.
pub fn validate_gpu_video_config(
    options: GameVideoOptions,
    rotation: u32,
    playfield_assets: &[u8],
    sprite_assets: &[u8],
) -> Result<(), String> {
    validate_rotation(rotation)?;
    if options.scale == 0
        || options.scale > GameVideoOptions::MAX_GPU_SCALE
        || options.border > GameVideoOptions::MAX_BORDER
    {
        return Err("Game GPU presentation scale/border out of range".to_owned());
    }
    validate_tile_assets(playfield_assets.len(), sprite_assets.len())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn live_alpha_is_sampled_on_the_monotonic_clock() {
        let live = PresentRequest {
            timing: PresentTiming::Live {
                frame_start_ns: 1_000_000_000,
                period_ns: 10_000_000,
            },
            ..PresentRequest::default()
        };
        let alpha = live.sample_alpha(1_005_000_000).expect("live alpha");
        assert!((alpha - 0.5).abs() < 1e-5, "alpha {alpha}");
        // Past the period and before the start both clamp into [0, 1].
        assert_eq!(live.sample_alpha(1_100_000_000), Some(1.0));
        assert_eq!(live.sample_alpha(900_000_000), Some(0.0));
        let zero = PresentRequest {
            timing: PresentTiming::Live {
                frame_start_ns: 0,
                period_ns: 0,
            },
            ..PresentRequest::default()
        };
        assert_eq!(zero.sample_alpha(0), Some(1.0));
        let fixed = PresentRequest {
            timing: PresentTiming::Fixed(0.25),
            ..PresentRequest::default()
        };
        assert_eq!(fixed.sample_alpha(0), Some(0.25));
        assert_eq!(PresentRequest::default().sample_alpha(0), None);
    }

    #[test]
    fn tile_assets_reject_incomplete_roms() {
        let tile = TILE_BYTES as usize;
        assert_eq!(
            validate_tile_assets(0, tile).unwrap_err(),
            "Incomplete GPU tile/sprite assets"
        );
        assert_eq!(
            validate_tile_assets(tile, 0).unwrap_err(),
            "Incomplete GPU tile/sprite assets"
        );
        assert!(validate_tile_assets(tile - 1, tile).is_err());
        assert!(validate_tile_assets(tile, tile).is_ok());
    }

    #[test]
    fn tile_assets_reject_offsets_that_do_not_fit_32_bits() {
        // Each ROM must fit half of a 32-bit offset; the pair sums without overflow.
        assert!(validate_tile_assets(0x8000_0000, 256).is_err());
        assert!(validate_tile_assets(256, 0x8000_0000).is_err());
        assert!(validate_tile_assets(0x7FFF_FF00, 0x7FFF_FF00).is_ok());
        assert!(validate_tile_assets(510, 256).is_err());
    }

    #[test]
    fn rotation_accepts_only_portrait_right_angles() {
        assert!(validate_rotation(0).is_ok());
        assert!(validate_rotation(90).is_ok());
        assert!(validate_rotation(270).is_ok());
        for rotation in [1, 45, 180, 360] {
            assert_eq!(
                validate_rotation(rotation).unwrap_err(),
                "Unsupported GPU presentation rotation"
            );
        }
    }
}
