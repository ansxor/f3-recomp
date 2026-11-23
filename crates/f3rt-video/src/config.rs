//! Scanout geometry and video configuration.

/// Visible columns 46..365 on every game (`geometry::NATIVE_WIDTH`).
pub const NATIVE_WIDTH: u32 = 320;
/// Scanout lines the sprite/line RAM address (`geometry::FRAME_LINES`).
pub const FRAME_LINES: u32 = 256;

/// `f3rt::VideoConfig` from `include/f3rt/rom.hpp`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct VideoConfig {
    pub rotation: u32,
    pub sprite_lag: u32,
    pub visible_y: u32,
    pub visible_height: u32,
    pub extend: bool,
    /// Immutable ROM layout capability: extended PF2/PF3 alternates are maps 4/5.
    pub extended_alt_maps: bool,
    /// Presentation-only (never changes emulation): the presented output samples PF2/PF3
    /// alternate-map rows from the full-resolution main map where an exact X2 alias exists.
    /// Requires `extended_alt_maps`.
    pub full_resolution_alt_maps: bool,
}

impl Default for VideoConfig {
    fn default() -> Self {
        Self {
            rotation: 0,
            sprite_lag: 1,
            visible_y: 24,
            visible_height: 232,
            extend: true,
            extended_alt_maps: false,
            full_resolution_alt_maps: false,
        }
    }
}

impl VideoConfig {
    /// Playfield map width in texels (64 columns of 16 in extended mode, 32 columns otherwise).
    pub const fn pf_map_width_px(&self) -> u32 {
        if self.extend { 1024 } else { 512 }
    }

    /// Playfield map columns (tiles of 16 texels).
    pub const fn pf_columns(&self) -> usize {
        if self.extend { 64 } else { 32 }
    }

    /// Bytes per playfield layer in graphics RAM.
    pub const fn pf_layer_stride(&self) -> usize {
        if self.extend {
            crate::memory_map::PLAYFIELD_LAYER_SIZE
        } else {
            crate::memory_map::PLAYFIELD_STANDARD_LAYER_SIZE
        }
    }

    /// Physical playfield map count: 4, or 6 with the PF2/PF3 alternate maps.
    pub const fn map_count(&self) -> usize {
        if !self.extend || self.extended_alt_maps {
            6
        } else {
            4
        }
    }

    /// Per-scanline alternate-map select (line RAM bit 0x200) is live.
    pub const fn alt_map_selectable(&self) -> bool {
        !self.extend || self.extended_alt_maps
    }
}

/// The compile-time `f3rt::geometry` namespace, carried as data.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Geometry {
    pub visible_y: u32,
    pub visible_height: u32,
}

impl Geometry {
    pub const NATIVE_WIDTH: u32 = NATIVE_WIDTH;
    pub const FRAME_LINES: u32 = FRAME_LINES;

    pub const fn from_video(video: &VideoConfig) -> Self {
        Self {
            visible_y: video.visible_y,
            visible_height: video.visible_height,
        }
    }

    pub const fn first_line(&self) -> u32 {
        self.visible_y
    }

    pub const fn height(&self) -> u32 {
        self.visible_height
    }

    pub const fn end_line(&self) -> u32 {
        self.visible_y + self.visible_height
    }

    pub const fn native_pixels(&self) -> u32 {
        NATIVE_WIDTH * self.visible_height
    }
}

impl Default for Geometry {
    fn default() -> Self {
        Self::from_video(&VideoConfig::default())
    }
}

/// `f3rt::GameVideoOptions`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GameVideoOptions {
    pub scale: u32,
    pub border: u32, // Additional native scene columns on EACH side.
}

impl GameVideoOptions {
    pub const MAX_SCALE: u32 = 4;
    pub const MAX_GPU_SCALE: u32 = 8;
    pub const MAX_BORDER: u32 = 160;

    pub const fn width(&self, _geometry: &Geometry) -> u32 {
        (NATIVE_WIDTH + self.border * 2) * self.scale
    }

    pub const fn height(&self, geometry: &Geometry) -> u32 {
        geometry.height() * self.scale
    }

    pub const fn expanded(&self) -> bool {
        self.scale != 1 || self.border != 0
    }
}

impl Default for GameVideoOptions {
    fn default() -> Self {
        Self {
            scale: 1,
            border: 0,
        }
    }
}

/// Presented pixel byte order (`IndexedPalette`/`IndexedScratch`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PixelFormat {
    Argb8888,
    Rgba8888,
}

impl PixelFormat {
    /// `to_pixel_format`: palette ARGB words into the compositor's byte order.
    pub const fn to_pixel_format(self, argb: u32) -> u32 {
        match self {
            Self::Rgba8888 => (argb << 8) | 0xff,
            Self::Argb8888 => argb,
        }
    }

    /// `from_pixel_format`: compositor bytes back into ARGB words.
    pub const fn from_pixel_format(self, pixel: u32) -> u32 {
        match self {
            Self::Rgba8888 => (pixel >> 8) | 0xff00_0000,
            Self::Argb8888 => pixel,
        }
    }
}

/// `f3rt::VideoScaleMode` from `runtime/renderer/scale.hpp`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VideoScaleMode {
    Fixed,
    Auto,
    AutoInteger,
}

/// Which sprite list a presentation consumes (`indexed_compositor.hpp`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SpriteList {
    Presented,
    Canonical,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn geometry_derives_from_video_config() {
        let geometry = Geometry::from_video(&VideoConfig {
            visible_y: 40,
            visible_height: 200,
            ..VideoConfig::default()
        });
        assert_eq!(geometry.first_line(), 40);
        assert_eq!(geometry.height(), 200);
        assert_eq!(geometry.end_line(), 240);
        assert_eq!(geometry.native_pixels(), 320 * 200);
        assert_eq!(Geometry::default().first_line(), 24);
        assert_eq!(Geometry::default().end_line(), 256);
    }

    #[test]
    fn video_config_derives_the_playfield_layout() {
        let extended = VideoConfig::default();
        assert_eq!(extended.pf_map_width_px(), 1024);
        assert_eq!(extended.pf_columns(), 64);
        assert_eq!(
            extended.pf_layer_stride(),
            crate::memory_map::PLAYFIELD_LAYER_SIZE
        );
        assert_eq!(
            extended.map_count(),
            4,
            "landmakrj has no PF2/PF3 alternates"
        );
        assert!(!extended.alt_map_selectable());

        let alternates = VideoConfig {
            extended_alt_maps: true,
            ..extended
        };
        assert_eq!(alternates.map_count(), 6);
        assert!(alternates.alt_map_selectable());

        let standard = VideoConfig {
            extend: false,
            ..extended
        };
        assert_eq!(standard.pf_map_width_px(), 512);
        assert_eq!(standard.pf_columns(), 32);
        assert_eq!(
            standard.pf_layer_stride(),
            crate::memory_map::PLAYFIELD_STANDARD_LAYER_SIZE
        );
        assert_eq!(
            standard.map_count(),
            6,
            "standard mode always exposes the alternates"
        );
        assert!(standard.alt_map_selectable());
    }

    #[test]
    fn game_video_options_scale_and_border() {
        let geometry = Geometry::default();
        let native = GameVideoOptions::default();
        assert!(!native.expanded());
        assert_eq!(native.width(&geometry), 320);
        assert_eq!(native.height(&geometry), 232);

        let expanded = GameVideoOptions {
            scale: 2,
            border: 4,
        };
        assert!(expanded.expanded());
        assert_eq!(expanded.width(&geometry), (320 + 8) * 2);
        assert_eq!(expanded.height(&geometry), 232 * 2);
    }

    #[test]
    fn pixel_format_round_trips() {
        for format in [PixelFormat::Argb8888, PixelFormat::Rgba8888] {
            let argb = 0xff12_3456;
            let pixel = format.to_pixel_format(argb);
            assert_eq!(format.from_pixel_format(pixel), argb);
        }
        assert_eq!(
            PixelFormat::Rgba8888.to_pixel_format(0xff12_3456),
            0x1234_56ff
        );
    }
}
