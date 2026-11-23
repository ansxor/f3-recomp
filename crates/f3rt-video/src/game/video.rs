//! Scanout frame driver and save-state root.

use std::fmt::Write as _;

use crate::config::{Geometry, NATIVE_WIDTH, PixelFormat, SpriteList, VideoConfig};
use crate::decode::SpritePresentation;
use crate::game::SceneSprite;
use crate::game::indexed_compositor::{IndexedScratch, TileRowTable};
use crate::game::lines::GameLines;
use crate::game::palette::GamePalette;
use crate::game::scene::VideoRam;
use crate::game::scene_snapshot::SceneSnapshot;
use crate::game::sprites::{GameSprites, TRAIL_STATE_SIZE};
use crate::game::text::GameText;
use crate::game::tiles::{FullResolutionAltMaps, GameTiles};
use crate::graphics_assets::GraphicsAssets;
use crate::state_io::{StateReader, StateWriter};

/// Pipeline stage reported to a probe (`VideoStage`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VideoStage {
    Idle,
    Decode,
    Scene,
    Capture,
    Latch,
}

/// Render-only sprite post-processing (`RenderEnhancements` / `SpriteStage`).
///
/// `frame` is `None` when the sprite group tracker could not supply a game frame; stages reset and
/// the latched list passes through unchanged.
pub trait SpriteEnhancements {
    fn present(&mut self, latched: &[SceneSprite], frame: Option<bool>, out: &mut Vec<SceneSprite>);
    fn reset(&mut self) {}
    fn report(&self, _out: &mut String) {}
}

struct NativeCompositor {
    tile_rows: TileRowTable,
    scratch: IndexedScratch,
}

impl NativeCompositor {
    fn new(tile_pixels: &[u8]) -> Self {
        Self {
            tile_rows: TileRowTable::new(tile_pixels),
            scratch: IndexedScratch::new(),
        }
    }
}

/// The game-side video root (`GameVideo`).
pub struct GameVideo {
    video: VideoConfig,
    geometry: Geometry,
    rendered_frames: u64,

    tiles: Box<GameTiles>,
    text: Box<GameText>,
    palette: Box<GamePalette>,
    sprites: Box<GameSprites>,
    lines: Box<GameLines>,
    enhancements: Option<Box<dyn SpriteEnhancements>>,
    enhanced: Vec<SceneSprite>,

    alt_maps: Box<FullResolutionAltMaps>,
    alt_rows_remapped: u64,
    alt_rows_fallback: u64,
    presented_frames: u64,
    presented_overflow_frames: u64,

    snapshot_storage: Box<SceneSnapshot>,
    native_pending: bool,
    native_pixels: Vec<u32>,
    native: Option<NativeCompositor>,
}

impl GameVideo {
    pub fn new(video: &VideoConfig, enhancements: Option<Box<dyn SpriteEnhancements>>) -> Self {
        let geometry = Geometry::from_video(video);
        let mut game = Self {
            video: *video,
            geometry,
            rendered_frames: 0,
            tiles: Box::new(GameTiles::new(video)),
            text: Box::new(GameText::new()),
            palette: Box::new(GamePalette::new()),
            sprites: Box::new(GameSprites::new(video)),
            lines: Box::new(GameLines::new(video)),
            enhancements,
            enhanced: Vec::new(),
            alt_maps: Box::new(FullResolutionAltMaps::new(video)),
            alt_rows_remapped: 0,
            alt_rows_fallback: 0,
            presented_frames: 0,
            presented_overflow_frames: 0,
            snapshot_storage: Box::new(SceneSnapshot::new(video)),
            native_pending: false,
            native_pixels: vec![0; NATIVE_WIDTH as usize * geometry.height() as usize],
            native: None,
        };
        game.snapshot_storage.capture_fallback(&[]);
        game
    }

    pub fn reset(&mut self, assets: &GraphicsAssets) {
        self.reset_with(assets, None);
    }

    pub fn reset_with(
        &mut self,
        assets: &GraphicsAssets,
        enhancements: Option<&mut dyn SpriteEnhancements>,
    ) {
        self.materialize_native(assets);
        self.tiles.reset();
        self.text.reset();
        self.palette.reset();
        self.sprites.reset();
        self.lines.reset();
        self.native_pending = false;
        if let Some(enhancements) = enhancements {
            enhancements.reset();
        } else if let Some(enhancements) = self.enhancements.as_mut() {
            enhancements.reset();
        }
        self.snapshot_storage.capture_fallback(&[]);
        self.presented_frames = 0;
        self.presented_overflow_frames = 0;
        self.alt_maps.reset();
        self.alt_rows_remapped = 0;
        self.alt_rows_fallback = 0;
        self.rendered_frames = 0;
    }

    /// Decode, resolve and capture one frame. `snapshot_target` selects an external snapshot slot;
    /// `None` writes into the internal boxed snapshot (the `set_snapshot_target(nullptr)` path).
    pub fn render_frame(
        &mut self,
        vram: &VideoRam<'_>,
        assets: &GraphicsAssets,
        presentation: SpritePresentation<'_>,
        stage_cb: Option<&mut dyn FnMut(VideoStage)>,
        snapshot_target: Option<&mut SceneSnapshot>,
    ) {
        self.render_frame_with(vram, assets, presentation, stage_cb, snapshot_target, None);
    }

    #[allow(clippy::too_many_arguments)]
    pub fn render_frame_with(
        &mut self,
        vram: &VideoRam<'_>,
        assets: &GraphicsAssets,
        presentation: SpritePresentation<'_>,
        stage_cb: Option<&mut dyn FnMut(VideoStage)>,
        snapshot_target: Option<&mut SceneSnapshot>,
        mut enhancements: Option<&mut dyn SpriteEnhancements>,
    ) {
        let mut stage_cb = stage_cb;
        enter(&mut stage_cb, VideoStage::Decode);
        self.tiles.decode(vram);
        self.text.decode(vram);
        self.palette.decode(vram);
        let sprite_assets = assets.sprite_tiles();
        if self
            .sprites
            .before_scanout(vram, presentation, sprite_assets)
            == crate::game::sprites::SpriteLatch::Latched
        {
            match enhancements.as_mut() {
                Some(e) => self.present_enhancements_external(&mut **e),
                None => self.present_enhancements(),
            }
        }
        self.lines.decode(vram, self.sprites.scanout_flipped());

        enter(&mut stage_cb, VideoStage::Scene);
        let alt_counts = self.lines.resolve_scene(
            self.sprites.scanout_flipped(),
            &self.tiles,
            &mut self.alt_maps,
            assets.playfield_tiles(),
        );
        self.alt_rows_remapped += alt_counts.remapped;
        self.alt_rows_fallback += alt_counts.fallback;
        self.rendered_frames += 1;

        enter(&mut stage_cb, VideoStage::Capture);
        let enhanced_active = enhancements.is_some() || self.enhancements.is_some();
        self.capture_with(snapshot_target, enhanced_active);
        self.native_pending = true;

        enter(&mut stage_cb, VideoStage::Latch);
        if self
            .sprites
            .after_scanout(vram, presentation, sprite_assets)
            == crate::game::sprites::SpriteLatch::Latched
        {
            match enhancements.as_mut() {
                Some(e) => self.present_enhancements_external(&mut **e),
                None => self.present_enhancements(),
            }
        }
        if self.sprites.presented_valid() {
            self.presented_frames += 1;
            self.presented_overflow_frames += u64::from(self.sprites.presented_overflow());
        }
        enter(&mut stage_cb, VideoStage::Idle);
    }

    fn present_enhancements(&mut self) {
        if let Some(enhancements) = self.enhancements.as_mut() {
            enhancements.present(
                self.sprites.presented_sprites(),
                Some(self.sprites.flipped()),
                &mut self.enhanced,
            );
        }
    }

    fn present_enhancements_external(&mut self, enhancements: &mut dyn SpriteEnhancements) {
        enhancements.present(
            self.sprites.presented_sprites(),
            Some(self.sprites.flipped()),
            &mut self.enhanced,
        );
    }

    /// Capture the decoded layers into `target`, or into the internal slot when `None`.
    pub fn capture(&mut self, target: Option<&mut SceneSnapshot>) {
        let enhanced_active = self.enhancements.is_some();
        self.capture_with(target, enhanced_active);
    }

    fn capture_with(&mut self, target: Option<&mut SceneSnapshot>, enhanced_active: bool) {
        let Self {
            tiles,
            text,
            palette,
            sprites,
            lines,
            enhanced,
            snapshot_storage,
            ..
        } = self;
        let slot = match target {
            Some(target) => target,
            None => &mut **snapshot_storage,
        };
        let enhanced_view = if enhanced_active {
            Some(&enhanced[..])
        } else {
            None
        };
        slot.capture(tiles, text, palette, sprites, lines, enhanced_view);
    }

    /// The snapshot slot a capture writes to (`next_snapshot_slot`).
    pub fn snapshot_slot<'a>(
        &'a mut self,
        target: Option<&'a mut SceneSnapshot>,
    ) -> &'a mut SceneSnapshot {
        match target {
            Some(target) => target,
            None => &mut self.snapshot_storage,
        }
    }

    pub fn scene_snapshot(&self) -> &SceneSnapshot {
        &self.snapshot_storage
    }

    /// Compose the pending frame into `native_pixels` (scale 1, no border, canonical sprites).
    pub fn materialize_native(&mut self, assets: &GraphicsAssets) {
        self.materialize_native_from(assets, None);
    }

    /// `materialize_native` for a capture written outside the internal slot (`snapshot_target`).
    pub fn materialize_native_from(
        &mut self,
        assets: &GraphicsAssets,
        capture: Option<&SceneSnapshot>,
    ) {
        if !self.native_pending {
            return;
        }
        let geometry = self.geometry;
        let width = NATIVE_WIDTH as usize;
        let height = geometry.height() as usize;
        let sprite_tiles = assets.sprite_tiles();
        let Self {
            native,
            native_pixels,
            snapshot_storage,
            ..
        } = self;
        let snapshot = capture.unwrap_or(snapshot_storage);
        let native = native.get_or_insert_with(|| NativeCompositor::new(assets.playfield_tiles()));
        snapshot
            .compose(
                native_pixels,
                width,
                PixelFormat::Argb8888,
                0,
                &native.tile_rows,
                assets.playfield_tiles(),
                sprite_tiles,
                &mut native.scratch,
                SpriteList::Canonical,
                None,
                None,
            )
            .expect("native composition buffers");
        debug_assert_eq!(native_pixels.len(), width * height);
        self.native_pending = false;
    }

    pub fn native_pixels(&self) -> &[u32] {
        &self.native_pixels
    }

    /// Restore the native pixels, which are serialized separately in the machine state.
    pub fn set_native_pixels(&mut self, pixels: &[u32]) {
        let count = pixels.len().min(self.native_pixels.len());
        self.native_pixels[..count].copy_from_slice(&pixels[..count]);
    }

    pub fn report(&self, out: &mut String) {
        if let Some(enhancements) = self.enhancements.as_ref() {
            enhancements.report(out);
        }
        if self.presented_frames != 0 || self.video.full_resolution_alt_maps {
            let _ = write!(
                out,
                "VIDEO presented_sprite_frames={} presented_sprite_overflow_frames={}",
                self.presented_frames, self.presented_overflow_frames
            );
            if self.video.full_resolution_alt_maps {
                let _ = write!(
                    out,
                    " presented_alt_rows_remapped={} presented_alt_rows_fallback={} \
                     presented_alt_rows_solved={}",
                    self.alt_rows_remapped,
                    self.alt_rows_fallback,
                    self.alt_maps.solved_rows()
                );
            }
            out.push('\n');
        }
        let _ = writeln!(out, "VIDEO game_frames={}", self.rendered_frames);
    }

    /// `GameSprites::TRAIL_STATE_SIZE + sprites + lines`: 367379 bytes for the default geometry.
    pub fn state_size(&self) -> usize {
        TRAIL_STATE_SIZE + self.sprites.state_size() + self.lines.state_size()
    }

    pub fn save_state(&mut self, dst: &mut [u8], assets: &GraphicsAssets) -> Result<(), String> {
        if dst.len() != self.state_size() {
            return Err("GameVideo::save_state size mismatch".to_string());
        }
        self.materialize_native(assets);
        let mut writer = StateWriter::new(dst);
        self.sprites.save_state(&mut writer)?;
        self.lines.save_state(&mut writer)?;
        self.sprites.save_trails(&mut writer)?;
        if writer.remaining() != 0 {
            return Err("GameVideo::save_state remaining unwritten bytes".to_string());
        }
        Ok(())
    }

    pub fn load_state(
        &mut self,
        src: &[u8],
        snapshot_target: Option<&mut SceneSnapshot>,
    ) -> Result<(), String> {
        self.load_state_with(src, snapshot_target, None)
    }

    pub fn load_state_with(
        &mut self,
        src: &[u8],
        snapshot_target: Option<&mut SceneSnapshot>,
        external: Option<&mut dyn SpriteEnhancements>,
    ) -> Result<(), String> {
        if src.len() != self.state_size() {
            return Err("GameVideo::load_state size mismatch".to_string());
        }
        let mut reader = StateReader::new(src);
        self.sprites.load_state(&mut reader)?;
        self.lines.load_state(&mut reader)?;
        self.sprites.load_trails(&mut reader)?;
        self.native_pending = false;
        if let Some(enhancements) = external {
            enhancements.present(self.sprites.presented_sprites(), None, &mut self.enhanced);
        } else if let Some(enhancements) = self.enhancements.as_mut() {
            enhancements.present(self.sprites.presented_sprites(), None, &mut self.enhanced);
        }
        let Self {
            native_pixels,
            snapshot_storage,
            ..
        } = self;
        let slot = match snapshot_target {
            Some(target) => target,
            None => &mut **snapshot_storage,
        };
        slot.capture_fallback(native_pixels);
        if reader.remaining() != 0 {
            return Err("GameVideo::load_state remaining unread bytes".to_string());
        }
        Ok(())
    }

    pub fn tiles(&self) -> &GameTiles {
        &self.tiles
    }

    pub fn text(&self) -> &GameText {
        &self.text
    }

    pub fn sprites(&self) -> &GameSprites {
        &self.sprites
    }

    pub fn lines(&self) -> &GameLines {
        &self.lines
    }

    pub fn video(&self) -> &VideoConfig {
        &self.video
    }

    pub fn geometry(&self) -> Geometry {
        self.geometry
    }
}

fn enter(cb: &mut Option<&mut dyn FnMut(VideoStage)>, stage: VideoStage) {
    if let Some(cb) = cb.as_deref_mut() {
        cb(stage);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::game::scene::VideoRam;
    use crate::memory_map::{GRAPHICS_RAM_SIZE, PALETTE_RAM_SIZE};

    struct RecordingStage {
        calls: usize,
        reports: usize,
    }

    impl SpriteEnhancements for RecordingStage {
        fn present(
            &mut self,
            latched: &[SceneSprite],
            frame: Option<bool>,
            out: &mut Vec<SceneSprite>,
        ) {
            assert!(frame.is_some(), "empty tracker is reported as no frame");
            self.calls += 1;
            out.clear();
            out.extend_from_slice(latched);
        }

        fn report(&self, out: &mut String) {
            let _ = self.reports;
            out.push_str("STAGE report\n");
        }
    }

    fn vram<'a>(graphics: &'a [u8], control: &'a [u8], palette: &'a [u8]) -> VideoRam<'a> {
        VideoRam::with_palette(graphics, control, 1, palette)
    }

    #[test]
    fn render_captures_and_materializes_black() {
        let video = VideoConfig::default();
        let graphics = vec![0u8; GRAPHICS_RAM_SIZE];
        let control = vec![0u8; 0x20];
        let palette = vec![0u8; PALETTE_RAM_SIZE];
        let assets = GraphicsAssets::default();
        let mut game = GameVideo::new(&video, None);
        let mut stages = Vec::new();
        game.render_frame(
            &vram(&graphics, &control, &palette),
            &assets,
            SpritePresentation::default(),
            Some(&mut |stage| stages.push(stage)),
            None,
        );
        assert_eq!(
            stages,
            vec![
                VideoStage::Decode,
                VideoStage::Scene,
                VideoStage::Capture,
                VideoStage::Latch,
                VideoStage::Idle
            ]
        );
        assert!(!game.scene_snapshot().fallback);
        game.materialize_native(&assets);
        assert_eq!(game.native_pixels().len(), 320 * 232);
        assert!(
            game.native_pixels()
                .iter()
                .all(|pixel| *pixel == 0xff00_0000)
        );

        let mut report = String::new();
        game.report(&mut report);
        assert_eq!(report, "VIDEO game_frames=1\n");
    }

    #[test]
    fn external_slot_receives_capture_and_internal_stays_fallback() {
        let video = VideoConfig::default();
        let graphics = vec![0u8; GRAPHICS_RAM_SIZE];
        let control = vec![0u8; 0x20];
        let palette = vec![0u8; PALETTE_RAM_SIZE];
        let assets = GraphicsAssets::default();
        let mut game = GameVideo::new(&video, None);
        assert!(game.scene_snapshot().fallback);
        let mut external = SceneSnapshot::new(&video);
        game.render_frame(
            &vram(&graphics, &control, &palette),
            &assets,
            SpritePresentation::default(),
            None,
            Some(&mut external),
        );
        assert!(!external.fallback);
        assert!(game.scene_snapshot().fallback);
        assert!(
            game.scene_snapshot()
                .native_pixels
                .iter()
                .all(|p| *p == 0xff00_0000)
        );

        game.render_frame(
            &vram(&graphics, &control, &palette),
            &assets,
            SpritePresentation::default(),
            None,
            None,
        );
        assert!(!game.scene_snapshot().fallback);
    }

    fn randomize(bytes: &mut [u8], seed: u64) {
        let mut state = seed | 1;
        for chunk in bytes.chunks_mut(8) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            chunk.copy_from_slice(&state.to_le_bytes()[..chunk.len()]);
        }
    }

    proptest::proptest! {
        #![proptest_config(proptest::test_runner::Config::with_cases(8))]
        #[test]
        fn ram_decodes_and_state_restore_is_deterministic(
            seeds in proptest::array::uniform4(proptest::prelude::any::<u64>()),
            x in -2048i32..2048,
            y in -2048i32..2048,
            alt_maps in proptest::prelude::any::<bool>(),
            sprite_lag in 0u32..=2,
        ) {
            use crate::memory_map::{LINERAM_SIZE, OFFS_LINERAM, OFFS_TEXTRAM_MAP};
            use proptest::prelude::*;
            let mut graphics = vec![0; GRAPHICS_RAM_SIZE];
            let mut control = [0; 0x20];
            let mut palette = vec![0; PALETTE_RAM_SIZE];
            randomize(&mut graphics, seeds[0]);
            randomize(&mut graphics[OFFS_LINERAM..OFFS_LINERAM + LINERAM_SIZE], seeds[1]);
            randomize(&mut control, seeds[2]);
            randomize(&mut palette, seeds[3]);
            let video = VideoConfig { extended_alt_maps: alt_maps, sprite_lag, ..VideoConfig::default() };
            let mut assets = GraphicsAssets::default();
            assert!(assets.load_roms(&[0x78; 384], &[], &[0x56; 384], &[]));
            let render = |game: &mut GameVideo, frame| {
                game.render_frame(&VideoRam::with_palette(&graphics, &control, frame, &palette), &assets, SpritePresentation::default(), None, None);
                game.materialize_native(&assets);
            };
            let mut game = GameVideo::new(&video, None);
            render(&mut game, 1);
            render(&mut game, 2);
            let scene = game.scene_snapshot();
            for (color, bytes) in scene.colors.iter().zip(palette.chunks_exact(4)) {
                prop_assert_eq!(*color, u32::from_be_bytes(bytes.try_into().unwrap()) & 0xff_ffff);
            }
            let text_cell = OFFS_TEXTRAM_MAP + ((y as usize & 511) / 8 * 64 + (x as usize & 511) / 8) * 2;
            prop_assert_eq!(scene.text.map()[(text_cell - OFFS_TEXTRAM_MAP) / 2], u16::from_be_bytes(graphics[text_cell..text_cell + 2].try_into().unwrap()));
            for flipped in [false, true] {
                for bitmap in [false, true] {
                    let pixel = scene.text.pixel(x, y, flipped, bitmap);
                    prop_assert!(pixel.palette < 1024);
                    prop_assert_eq!(pixel.flags != 0, pixel.palette & 15 != 0);
                }
                for layer in 0..4 {
                    let pixel = scene.tiles.playfield_pixel(layer, x, y, flipped, assets.playfield_tiles(), alt_maps && layer >= 2);
                    prop_assert!(pixel.palette < 8192 + 64);
                }
            }
            prop_assert!(scene.rows.iter().all(|row| row.playfields.iter().all(|pf| (0..512).contains(&pf.source_y))));
            prop_assert!(scene.canonical_sprite_list().len() <= crate::game::MAX_HARDWARE_SPRITES);
            prop_assert_eq!(game.state_size(), 367_379);
            let mut saved = vec![0; 367_379];
            game.save_state(&mut saved, &assets).unwrap();
            let mut restored = GameVideo::new(&video, None);
            restored.set_native_pixels(game.native_pixels());
            restored.load_state(&saved, None).unwrap();
            prop_assert_eq!(restored.native_pixels(), game.native_pixels());
            let mut resaved = vec![0; 367_379];
            restored.save_state(&mut resaved, &assets).unwrap();
            prop_assert!(saved == resaved, "save bytes changed after loading");
            render(&mut game, 3);
            render(&mut restored, 3);
            prop_assert!(game.native_pixels() == restored.native_pixels(), "next scanout changed after loading");
            game.save_state(&mut saved, &assets).unwrap();
            restored.save_state(&mut resaved, &assets).unwrap();
            prop_assert!(saved == resaved, "next frame state changed after loading");
        }
    }

    #[test]
    fn save_state_size_matches_the_cpp_root_and_round_trips() {
        let video = VideoConfig::default();
        let graphics = vec![0u8; GRAPHICS_RAM_SIZE];
        let control = vec![0u8; 0x20];
        let palette = vec![0u8; PALETTE_RAM_SIZE];
        let assets = GraphicsAssets::default();
        let mut game = GameVideo::new(&video, None);
        game.render_frame(
            &vram(&graphics, &control, &palette),
            &assets,
            SpritePresentation::default(),
            None,
            None,
        );
        assert_eq!(game.state_size(), 367_379);
        assert_eq!(
            game.state_size(),
            TRAIL_STATE_SIZE + game.sprites().state_size() + game.lines().state_size()
        );

        let mut saved = vec![0u8; game.state_size()];
        game.save_state(&mut saved, &assets).unwrap();
        assert!(saved.contains(&0));

        let native = (0..game.native_pixels().len())
            .map(|i| 0xff00_0000 | i as u32)
            .collect::<Vec<_>>();
        let mut restored = GameVideo::new(&video, None);
        restored.set_native_pixels(&native);
        restored.load_state(&saved, None).unwrap();
        assert!(restored.scene_snapshot().fallback);
        assert_eq!(restored.scene_snapshot().native_pixels, native);

        let mut resaved = vec![0u8; restored.state_size()];
        restored.save_state(&mut resaved, &assets).unwrap();
        assert_eq!(saved, resaved);

        // Post-load frames re-decode and re-materialize.
        restored.render_frame(
            &vram(&graphics, &control, &palette),
            &assets,
            SpritePresentation::default(),
            None,
            None,
        );
        assert!(!restored.scene_snapshot().fallback);
        restored.materialize_native(&assets);
        assert_eq!(restored.report(&mut String::new()), ());
    }

    #[test]
    fn save_state_rejects_wrong_sizes() {
        let video = VideoConfig::default();
        let assets = GraphicsAssets::default();
        let mut game = GameVideo::new(&video, None);
        let mut small = vec![0u8; game.state_size() - 1];
        assert!(game.save_state(&mut small, &assets).is_err());
        assert!(game.load_state(&small, None).is_err());
    }

    #[test]
    fn enhancements_report_and_reset() {
        let video = VideoConfig::default();
        let graphics = vec![0u8; GRAPHICS_RAM_SIZE];
        let control = vec![0u8; 0x20];
        let palette = vec![0u8; PALETTE_RAM_SIZE];
        let assets = GraphicsAssets::default();
        let stage = RecordingStage {
            calls: 0,
            reports: 0,
        };
        let mut game = GameVideo::new(&video, Some(Box::new(stage)));
        game.render_frame(
            &vram(&graphics, &control, &palette),
            &assets,
            SpritePresentation::default(),
            None,
            None,
        );
        let mut report = String::new();
        game.report(&mut report);
        assert!(report.starts_with("STAGE report\n"), "{report}");
        assert!(report.ends_with("VIDEO game_frames=1\n"));

        game.reset(&assets);
        let mut report = String::new();
        game.report(&mut report);
        assert_eq!(report, "STAGE report\nVIDEO game_frames=0\n");
        assert!(game.scene_snapshot().fallback);
        assert!(
            game.scene_snapshot()
                .native_pixels
                .iter()
                .all(|p| *p == 0xff00_0000)
        );
    }

    #[test]
    fn lore_geometry_uses_the_configured_scanout_window() {
        let video = VideoConfig {
            visible_y: 40,
            visible_height: 200,
            ..VideoConfig::default()
        };
        let game = GameVideo::new(&video, None);
        assert_eq!(game.geometry().first_line(), 40);
        assert_eq!(game.geometry().end_line(), 240);
        assert_eq!(game.native_pixels().len(), 320 * 200);
        assert_eq!(game.scene_snapshot().native_pixels.len(), 320 * 200);
    }
}
