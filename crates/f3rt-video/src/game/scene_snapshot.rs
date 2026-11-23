//! Render-only snapshot of the layers latched at VBSTART.
//! Dirty versions let capture reuse unchanged tiles, text, rows and palette.

use crate::config::{GameVideoOptions, Geometry, NATIVE_WIDTH, SpriteList, VideoConfig};
use crate::game::indexed_compositor::{
    IndexedOptions, IndexedOutput, IndexedScratch, IndexedTiles, RowExecution, SceneSnapshotView,
    compose_indexed,
};
use crate::game::lines::GameLines;
use crate::game::palette::GamePalette;
use crate::game::scene::{MAX_HARDWARE_SPRITES, MAX_PRESENTED_SPRITES, SceneRow, SceneSprite};
use crate::game::sprites::GameSprites;
use crate::game::text::GameText;
use crate::game::tiles::GameTiles;

/// `SceneSnapshot`: the last decoded scanout frame.
#[derive(Clone)]
pub struct SceneSnapshot {
    pub tiles: GameTiles,
    pub text: GameText,
    pub rows: [SceneRow; 256],
    pub sprites: Vec<SceneSprite>,
    pub sprite_count: usize,
    pub canonical_sprites: Vec<SceneSprite>,
    pub canonical_sprite_count: usize,
    /// `0x00RRGGBB`, indexed by palette entry.
    pub colors: [u32; 8192],
    pub rows_version: u64,
    pub colors_version: u64,
    pub pen_mask: u8,
    pub flipped: bool,
    pub sprite_flipped: bool,
    pub sprite_trail_seed: Vec<u16>,
    /// Oracle-fallback frame: only `native_pixels` is meaningful.
    pub fallback: bool,
    /// `320 x visible_height` ARGB.
    pub native_pixels: Vec<u32>,
    video: VideoConfig,
}

impl SceneSnapshot {
    pub const MAX_SPRITES: usize = MAX_PRESENTED_SPRITES;
    pub const PALETTE_SIZE: usize = GamePalette::SIZE;

    pub fn new(video: &VideoConfig) -> Self {
        let geometry = Geometry::from_video(video);
        Self {
            tiles: GameTiles::new(video),
            text: GameText::new(),
            rows: [SceneRow::default(); 256],
            sprites: vec![SceneSprite::default(); MAX_PRESENTED_SPRITES],
            sprite_count: 0,
            canonical_sprites: vec![SceneSprite::default(); MAX_HARDWARE_SPRITES],
            canonical_sprite_count: 0,
            colors: [0; Self::PALETTE_SIZE],
            rows_version: 0,
            colors_version: 0,
            pen_mask: 15,
            flipped: false,
            sprite_flipped: false,
            sprite_trail_seed: Vec::new(),
            fallback: true,
            native_pixels: vec![0; NATIVE_WIDTH as usize * geometry.height() as usize],
            video: *video,
        }
    }

    pub fn sprite_list(&self) -> &[SceneSprite] {
        &self.sprites[..self.sprite_count]
    }

    pub fn sprite_list_for(&self, list: SpriteList) -> &[SceneSprite] {
        match list {
            SpriteList::Canonical => self.canonical_sprite_list(),
            SpriteList::Presented => self.sprite_list(),
        }
    }

    pub fn canonical_sprite_list(&self) -> &[SceneSprite] {
        &self.canonical_sprites[..self.canonical_sprite_count]
    }

    pub fn presents_enhanced_sprites(&self) -> bool {
        !self.fallback
            && (self.sprite_list() != self.canonical_sprite_list()
                || self.sprite_list().iter().any(SceneSprite::weighted))
    }

    pub fn view(&self) -> SceneSnapshotView<'_> {
        SceneSnapshotView {
            tiles: &self.tiles,
            text: &self.text,
            rows: &self.rows,
            sprites: self.sprite_list(),
            canonical_sprites: self.canonical_sprite_list(),
            colors: &self.colors,
            pen_mask: self.pen_mask,
            flipped: self.flipped,
            sprite_flipped: self.sprite_flipped,
            sprite_trail_seed: &self.sprite_trail_seed,
            fallback: self.fallback,
        }
    }

    /// Capture the decoded layers. `enhanced_sprites` is the render-only splice list, if any
    /// (matching `GameSprites::presented_for`).
    pub fn capture(
        &mut self,
        tiles: &GameTiles,
        text: &GameText,
        palette: &GamePalette,
        sprites: &GameSprites,
        lines: &GameLines,
        enhanced_sprites: Option<&[SceneSprite]>,
    ) -> &mut Self {
        self.fallback = false;
        self.sync_versioned_layers(tiles, text, palette, lines);
        self.copy_frame_sprites(sprites, enhanced_sprites);
        self
    }

    fn sync_versioned_layers(
        &mut self,
        source_tiles: &GameTiles,
        source_text: &GameText,
        palette: &GamePalette,
        lines: &GameLines,
    ) {
        if self.tiles.version() != source_tiles.version() {
            self.tiles = source_tiles.clone();
        }
        if self.text.version() != source_text.version() {
            self.text = source_text.clone();
        }
        if self.rows_version != lines.rows_version() {
            self.rows.copy_from_slice(lines.rows());
            self.rows_version = lines.rows_version();
        }
        if self.colors_version != palette.version() {
            self.colors.copy_from_slice(palette.colors());
            self.colors_version = palette.version();
        }
    }

    fn copy_frame_sprites(&mut self, source: &GameSprites, enhanced: Option<&[SceneSprite]>) {
        let presented = source.presented_for(enhanced);
        self.sprite_count = presented.len();
        self.sprites[..self.sprite_count].copy_from_slice(presented);
        let canonical = source.sprites();
        self.canonical_sprite_count = canonical.len();
        self.canonical_sprites[..self.canonical_sprite_count].copy_from_slice(canonical);
        self.pen_mask = source.pen_mask();
        self.flipped = source.scanout_flipped();
        self.sprite_flipped = source.flipped();
        let trail_seed = source.trail_seed();
        self.sprite_trail_seed.clear();
        self.sprite_trail_seed.extend_from_slice(trail_seed);
    }

    /// Make this an oracle-fallback frame: copies `native` (`320 x visible_height` ARGB), or
    /// opaque black when empty.
    pub fn capture_fallback(&mut self, native: &[u32]) -> &mut Self {
        self.fallback = true;
        if native.is_empty() {
            self.native_pixels.fill(0xff00_0000);
        } else {
            let count = native.len().min(self.native_pixels.len());
            self.native_pixels[..count].copy_from_slice(&native[..count]);
        }
        self
    }

    /// Scale-1 CPU indexed composition with optional symmetric horizontal `border` columns.
    pub fn compose(
        &self,
        output: IndexedOutput<'_>,
        border: u32,
        tiles: IndexedTiles<'_>,
        scratch: &mut IndexedScratch,
        list: SpriteList,
        execution: RowExecution<'_>,
    ) -> Result<(), String> {
        let stride = output.layout.stride;
        let format = output.layout.format;
        if border > GameVideoOptions::MAX_BORDER {
            return Err("Indexed composition border out of range".to_string());
        }
        let options = GameVideoOptions { scale: 1, border };
        let geometry = self.geometry();
        if stride < options.width(&geometry) as usize
            || output.pixels.len() < stride * options.height(&geometry) as usize
        {
            return Err("Incomplete indexed frame render buffers".to_string());
        }
        if self.fallback {
            let left = border as usize;
            for y in 0..options.height(&geometry) as usize {
                let row =
                    &mut output.pixels[y * stride..y * stride + options.width(&geometry) as usize];
                for (x, pixel) in row.iter_mut().enumerate() {
                    *pixel = if x < left || x >= left + NATIVE_WIDTH as usize {
                        format.to_pixel_format(0xff00_0000)
                    } else {
                        format.to_pixel_format(
                            self.native_pixels[y * NATIVE_WIDTH as usize + (x - left)],
                        )
                    };
                }
            }
            return Ok(());
        }
        let view = self.view();
        compose_indexed(
            &view,
            output,
            IndexedOptions {
                raster: options,
                sprites: list,
            },
            &geometry,
            tiles,
            scratch,
            execution,
        );
        Ok(())
    }

    pub fn geometry(&self) -> Geometry {
        Geometry::from_video(&self.video)
    }

    pub fn video(&self) -> &VideoConfig {
        &self.video
    }

    pub fn row(&self, y: usize) -> &SceneRow {
        &self.rows[y & 255]
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::PixelFormat;
    use crate::game::indexed_compositor::{IndexedLayout, TileRowTable};
    use crate::game::row_workers::COMPOSE_CHUNK_ROWS;
    use crate::game::scene::{LayerId, LayerKind, ScenePlayfield, kind, presented_playfield};

    fn video() -> VideoConfig {
        VideoConfig::default()
    }

    fn snapshot() -> SceneSnapshot {
        SceneSnapshot::new(&video())
    }

    #[test]
    fn fallback_fills_black_or_copies_native() {
        let mut scene = snapshot();
        scene.capture_fallback(&[]);
        assert!(scene.fallback);
        assert!(scene.native_pixels.iter().all(|p| *p == 0xff00_0000));

        let native = (0..scene.native_pixels.len())
            .map(|i| 0xff00_0000 | i as u32)
            .collect::<Vec<_>>();
        scene.capture_fallback(&native);
        assert_eq!(scene.native_pixels, native);

        let short = vec![0xdead_beefu32; 3];
        scene.capture_fallback(&short);
        assert_eq!(&scene.native_pixels[..3], &short[..]);
    }

    #[test]
    fn presents_enhanced_only_for_live_frames_with_differences_or_weights() {
        let mut scene = snapshot();
        scene.fallback = false;
        let a = SceneSprite {
            x: 1,
            ..SceneSprite::default()
        };
        scene.sprites[0] = a;
        scene.canonical_sprites[0] = a;
        scene.sprite_count = 1;
        scene.canonical_sprite_count = 1;
        assert!(!scene.presents_enhanced_sprites());

        scene.sprites[0].x = 2;
        assert!(scene.presents_enhanced_sprites());

        scene.sprites[0] = a;
        scene.sprites[0].blend_weight = 0.5;
        assert!(scene.presents_enhanced_sprites());

        scene.fallback = true;
        assert!(!scene.presents_enhanced_sprites());
    }

    #[test]
    fn compose_rejects_bad_buffers_and_borders() {
        let scene = snapshot();
        let tiles = TileRowTable::new(&[]);
        let mut scratch = IndexedScratch::new();
        let mut output = vec![0u32; 320 * 232];
        assert!(
            scene
                .compose(
                    IndexedOutput {
                        pixels: &mut output,
                        layout: IndexedLayout {
                            stride: 320,
                            format: PixelFormat::Argb8888
                        }
                    },
                    GameVideoOptions::MAX_BORDER + 1,
                    IndexedTiles {
                        playfield_rows: &tiles,
                        playfield_pixels: &[],
                        sprite_pixels: &[]
                    },
                    &mut scratch,
                    SpriteList::Presented,
                    RowExecution::default(),
                )
                .is_err()
        );
        assert!(
            scene
                .compose(
                    IndexedOutput {
                        pixels: &mut output,
                        layout: IndexedLayout {
                            stride: 320,
                            format: PixelFormat::Argb8888
                        }
                    },
                    0,
                    IndexedTiles {
                        playfield_rows: &tiles,
                        playfield_pixels: &[],
                        sprite_pixels: &[]
                    },
                    &mut scratch,
                    SpriteList::Presented,
                    RowExecution::default(),
                )
                .is_ok()
        );
    }

    #[test]
    fn chunk_count_matches_visible_window() {
        let scene = snapshot();
        assert_eq!(
            crate::game::row_workers::compose_chunk_count(
                scene.geometry().first_line(),
                scene.geometry().end_line()
            ),
            29
        );
        assert_eq!(COMPOSE_CHUNK_ROWS, 8);
    }

    #[test]
    fn serially_composed_fallback_uses_border_columns() {
        let mut scene = snapshot();
        let native = vec![0xff11_2233u32; scene.native_pixels.len()];
        scene.capture_fallback(&native);
        let tiles = TileRowTable::new(&[]);
        let mut scratch = IndexedScratch::new();
        let border = 4u32;
        let width = (320 + border * 2) as usize;
        let height = scene.geometry().height() as usize;
        let mut output = vec![0u32; width * height];
        scene
            .compose(
                IndexedOutput {
                    pixels: &mut output,
                    layout: IndexedLayout {
                        stride: width,
                        format: PixelFormat::Rgba8888,
                    },
                },
                border,
                IndexedTiles {
                    playfield_rows: &tiles,
                    playfield_pixels: &[],
                    sprite_pixels: &[],
                },
                &mut scratch,
                SpriteList::Presented,
                RowExecution::default(),
            )
            .unwrap();
        // Border columns are opaque black in the requested byte order.
        assert_eq!(output[0], 0x0000_00ff);
        assert_eq!(output[border as usize], 0x1122_33ff);
        assert_eq!(output[width - border as usize - 1], 0x1122_33ff);
        assert_eq!(output[width - 1], 0x0000_00ff);
    }

    #[test]
    fn layer_helpers_agree_with_kind() {
        assert_eq!(kind(LayerId::Sp2), LayerKind::Sprite);
        let pf = ScenePlayfield::default();
        assert_eq!(presented_playfield(&pf, &video()), pf);
        let scene = snapshot();
        assert_eq!(scene.row(24).background, 0);
    }
}
