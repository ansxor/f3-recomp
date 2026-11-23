//! Scene sprite and scanout passes, mechanically ported from
//! `runtime/renderer/gpu/scene_pass.{hpp,cpp}`.

use super::driver::{
    RawBuffer, RawCopyPass, RawSampler, RawTexture, ShaderKind, TextureFormat,
    TextureSamplerBinding, TransferUsage,
};
use super::encode::{ENCODED_INTERPOLATION_WORDS, EncodedScene, encoded_native_words};
use super::sdl_gpu::{
    GpuBuffer, GpuCommand, GpuContext, GpuPipeline, GpuSurface, GpuTexture, GpuTransfer,
    uniform_bytes,
};
use crate::config::{GameVideoOptions, Geometry};
use crate::gpu::scene_layout::{F3_MASK_BLEND_WEIGHTED, F3_MASK_HIDE_WEIGHTED, SceneUniforms};

/// Bytes per 16x16 tile.
pub const TILE_BYTES: u32 = 256;

// Sprite hardware plane at native resolution: 432 x 256 scanout lines.
const NATIVE_PLANE_WIDTH: u32 = 432;
const NATIVE_PLANE_HEIGHT: u32 = 256;
// Visible columns 46..365 on every game (`geometry::NATIVE_WIDTH` wide).
const NATIVE_FIRST_COLUMN: u32 = 46;
const NATIVE_END_COLUMN: u32 = 366;

fn pack_word(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}

fn validate_lengths(pf_bytes: usize, sp_bytes: usize) -> Result<(), String> {
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

pub struct GpuTileAssets {
    pf_tiles: GpuBuffer,
    sp_tiles: GpuBuffer,
    pf_tile_count: u32,
    sp_tile_count: u32,
    tile_pen_masks: Vec<u64>,
}

impl GpuTileAssets {
    /// Throws unless both ROMs are non-empty whole tiles that fit 32-bit offsets.
    pub fn validate(pf_rom: &[u8], sp_rom: &[u8]) -> Result<(), String> {
        validate_lengths(pf_rom.len(), sp_rom.len())
    }

    /// Packs both ROMs low-byte-first and uploads them once.
    pub fn new(ctx: &GpuContext, pf_rom: &[u8], sp_rom: &[u8]) -> Result<Self, String> {
        Self::validate(pf_rom, sp_rom)?;
        let pf_bytes = pf_rom.len() as u32;
        let sp_bytes = sp_rom.len() as u32;
        let pf_tile_count = pf_bytes / TILE_BYTES;
        let sp_tile_count = sp_bytes / TILE_BYTES;
        let pf_tiles = ctx.create_buffer(pf_bytes)?;
        let sp_tiles = ctx.create_buffer(sp_bytes)?;
        let mut tile_pen_masks = vec![0u64; pf_tile_count as usize];
        let staging = ctx.create_transfer(pf_bytes + sp_bytes, TransferUsage::Upload)?;
        {
            let mut mapping = staging.map(false, "Map GPU asset upload")?;
            let words = mapping.words(((pf_bytes + sp_bytes) / 4) as usize);
            for i in (0..pf_bytes as usize).step_by(4) {
                words[i / 4] = pack_word(pf_rom, i);
                tile_pen_masks[i / TILE_BYTES as usize] |= (1u64 << (pf_rom[i] & 63))
                    | (1u64 << (pf_rom[i + 1] & 63))
                    | (1u64 << (pf_rom[i + 2] & 63))
                    | (1u64 << (pf_rom[i + 3] & 63));
            }
            for i in (0..sp_bytes as usize).step_by(4) {
                words[(pf_bytes as usize + i) / 4] = pack_word(sp_rom, i);
            }
        }
        let mut cmd = ctx.begin_command()?;
        let copy = cmd.begin_copy_pass("Begin GPU asset copy")?;
        staging.upload_to(copy, &pf_tiles, 0, pf_bytes, false);
        staging.upload_to(copy, &sp_tiles, pf_bytes, sp_bytes, false);
        cmd.driver().end_copy_pass(copy);
        cmd.submit()?;
        Ok(Self {
            pf_tiles,
            sp_tiles,
            pf_tile_count,
            sp_tile_count,
            tile_pen_masks,
        })
    }

    pub fn pf_tiles(&self) -> &GpuBuffer {
        &self.pf_tiles
    }

    pub fn sp_tiles(&self) -> &GpuBuffer {
        &self.sp_tiles
    }

    pub fn pf_tile_count(&self) -> u32 {
        self.pf_tile_count
    }

    pub fn sp_tile_count(&self) -> u32 {
        self.sp_tile_count
    }

    /// One 64-bit pen-presence mask per playfield tile.
    pub fn tile_pen_masks(&self) -> &[u64] {
        &self.tile_pen_masks
    }
}

pub struct Targets {
    pub sprite_plane: GpuTexture,
    pub sprite_plane_b: GpuTexture,
    pub surface: GpuTexture,
}

pub struct ScenePass {
    scene_buffer: GpuBuffer,
    native_buffer: GpuBuffer,
    upload: GpuTransfer,
    trail_upload: Option<GpuTransfer>,
    sprite_pipeline: GpuPipeline,
    scene_pipeline: GpuPipeline,
    interpolation_pipeline: Option<GpuPipeline>,
    targets: Targets,
}

fn sprite_plane(
    ctx: &GpuContext,
    geometry: &Geometry,
    options: GameVideoOptions,
) -> Result<GpuTexture, String> {
    if options.expanded() {
        ctx.create_texture(
            TextureFormat::R32Uint,
            options.width(geometry),
            options.height(geometry),
        )
    } else {
        ctx.create_texture(
            TextureFormat::R32Uint,
            NATIVE_PLANE_WIDTH,
            NATIVE_PLANE_HEIGHT,
        )
    }
}

impl ScenePass {
    pub fn new(
        ctx: &GpuContext,
        geometry: &Geometry,
        options: GameVideoOptions,
        enable_interpolation: bool,
    ) -> Result<Self, String> {
        let interpolation_bytes = (ENCODED_INTERPOLATION_WORDS * 4) as u32;
        let native_bytes = (encoded_native_words(geometry) * 4) as u32;
        let scene_buffer = ctx.create_buffer(interpolation_bytes)?;
        let native_buffer = ctx.create_buffer(native_bytes)?;
        let targets = Self::reallocate_targets(ctx, geometry, options)?;
        let sprite_pipeline = ctx.create_pipeline(
            ShaderKind::SpriteVert,
            ShaderKind::SpriteFrag,
            TextureFormat::R32Uint,
        )?;
        let scene_pipeline = ctx.create_pipeline(
            ShaderKind::FullscreenVert,
            ShaderKind::SceneFrag,
            TextureFormat::Bgra8Unorm,
        )?;
        let mut pass = Self {
            scene_buffer,
            native_buffer,
            upload: ctx
                .create_transfer(interpolation_bytes.max(native_bytes), TransferUsage::Upload)?,
            trail_upload: None,
            sprite_pipeline,
            scene_pipeline,
            interpolation_pipeline: None,
            targets,
        };
        if enable_interpolation {
            pass.ensure_interpolation_pipeline(ctx)?;
        }
        Ok(pass)
    }

    /// Created on first use, so an interpolation-off session never compiles the shader. Throws
    /// before changing any state.
    pub fn ensure_interpolation_pipeline(&mut self, ctx: &GpuContext) -> Result<(), String> {
        if self.interpolation_pipeline.is_none() {
            self.interpolation_pipeline = Some(ctx.create_pipeline(
                ShaderKind::FullscreenVert,
                ShaderKind::SceneInterpFrag,
                TextureFormat::Bgra8Unorm,
            )?);
        }
        Ok(())
    }

    pub fn upload_buffer(&self) -> &GpuTransfer {
        &self.upload
    }

    pub fn interpolation_pipeline(&self) -> Option<&GpuPipeline> {
        self.interpolation_pipeline.as_ref()
    }

    pub fn targets(&self) -> &Targets {
        &self.targets
    }

    pub fn reallocate_targets(
        ctx: &GpuContext,
        geometry: &Geometry,
        options: GameVideoOptions,
    ) -> Result<Targets, String> {
        Ok(Targets {
            sprite_plane: sprite_plane(ctx, geometry, options)?,
            sprite_plane_b: sprite_plane(ctx, geometry, options)?,
            surface: ctx.create_texture(
                TextureFormat::Bgra8Unorm,
                options.width(geometry),
                options.height(geometry),
            )?,
        })
    }

    pub fn commit_targets(&mut self, next: Targets) {
        self.targets = next;
        self.trail_upload = None;
    }

    fn upload_trail_seed(
        &mut self,
        copy: RawCopyPass,
        ctx: &GpuContext,
        seed: &[u16],
        options: GameVideoOptions,
        geometry: &Geometry,
    ) -> Result<(), String> {
        let expanded = options.expanded();
        let width = if expanded {
            options.width(geometry)
        } else {
            NATIVE_PLANE_WIDTH
        };
        let height = if expanded {
            options.height(geometry)
        } else {
            NATIVE_PLANE_HEIGHT
        };
        if self.trail_upload.is_none() {
            self.trail_upload =
                Some(ctx.create_transfer(width * height * 4, TransferUsage::Upload)?);
        }
        {
            let trail_upload = self
                .trail_upload
                .as_ref()
                .expect("trail upload created above");
            let mut mapping = trail_upload.map(true, "Map GPU sprite trails")?;
            let mapped = mapping.words((width * height) as usize);
            if !expanded {
                for (dst, src) in mapped.iter_mut().zip(seed.iter()) {
                    *dst = u32::from(*src);
                }
            } else {
                for y in 0..height {
                    let native_y = y / options.scale + geometry.first_line();
                    for x in 0..width {
                        let native_x = (x / options.scale) as i32 + NATIVE_FIRST_COLUMN as i32
                            - options.border as i32;
                        mapped[(y * width + x) as usize] = if native_x >= NATIVE_FIRST_COLUMN as i32
                            && native_x < NATIVE_END_COLUMN as i32
                        {
                            u32::from(
                                seed[(native_y * NATIVE_PLANE_WIDTH + native_x as u32) as usize],
                            )
                        } else {
                            0
                        };
                    }
                }
            }
        }
        let trail_upload = self
            .trail_upload
            .as_ref()
            .expect("trail upload created above");
        for plane in [
            self.targets.sprite_plane.get(),
            self.targets.sprite_plane_b.get(),
        ] {
            ctx.driver()
                .upload_texture(copy, trail_upload.get(), 0, width, height, plane, true);
        }
        Ok(())
    }

    /// Draws the sprite plane(s) and the scene pass; returns the composed scanout surface.
    pub fn draw(
        &mut self,
        cmd: &mut GpuCommand,
        encoded: &EncodedScene<'_>,
        ctx: &GpuContext,
        assets: &GpuTileAssets,
        options: GameVideoOptions,
        geometry: &Geometry,
    ) -> Result<GpuSurface, String> {
        let copy = cmd.begin_copy_pass("Begin GPU frame copy")?;
        self.upload.upload_to(
            copy,
            if encoded.fallback {
                &self.native_buffer
            } else {
                &self.scene_buffer
            },
            0,
            encoded.upload_bytes,
            true,
        );
        if encoded.has_trails {
            self.upload_trail_seed(copy, ctx, encoded.trail_seed, options, geometry)?;
        }
        cmd.driver().end_copy_pass(copy);

        let mut uniforms = encoded.uniforms;
        let scene_buffer = self.scene_buffer.get();
        let sp_tiles = assets.sp_tiles().get();
        let sampler = ctx.sampler();
        if !encoded.fallback {
            let plane = self.targets.sprite_plane.get();
            let plane_b = self.targets.sprite_plane_b.get();
            self.draw_sprite_plane(
                cmd,
                plane,
                plane_b,
                uniforms,
                0,
                encoded.has_trails,
                options,
                geometry,
                scene_buffer,
                sp_tiles,
                sampler,
            )?;
            if encoded.blend_weighted {
                self.draw_sprite_plane(
                    cmd,
                    plane_b,
                    plane,
                    uniforms,
                    F3_MASK_HIDE_WEIGHTED,
                    encoded.has_trails,
                    options,
                    geometry,
                    scene_buffer,
                    sp_tiles,
                    sampler,
                )?;
            }
        }
        if encoded.blend_weighted {
            uniforms.layer_mask |= F3_MASK_BLEND_WEIGHTED;
        }
        cmd.driver()
            .push_fragment_uniforms(cmd.raw(), 0, uniform_bytes(&uniforms));
        let target = self.targets.surface.get();
        let pass = cmd.begin_render_pass(target, false)?;
        let pipeline = if encoded.use_interpolation {
            self.interpolation_pipeline
                .as_ref()
                .map(GpuPipeline::get)
                .unwrap_or_else(|| self.scene_pipeline.get())
        } else {
            self.scene_pipeline.get()
        };
        cmd.driver().bind_pipeline(pass, pipeline);
        let sprite_bindings = [
            TextureSamplerBinding {
                texture: self.targets.sprite_plane.get().0,
                sampler: sampler.0,
            },
            TextureSamplerBinding {
                texture: if encoded.blend_weighted {
                    self.targets.sprite_plane_b.get()
                } else {
                    self.targets.sprite_plane.get()
                }
                .0,
                sampler: sampler.0,
            },
        ];
        cmd.driver()
            .bind_fragment_samplers(pass, 0, &sprite_bindings);
        cmd.driver().bind_fragment_storage_buffers(
            pass,
            0,
            &[
                scene_buffer,
                assets.pf_tiles().get(),
                self.native_buffer.get(),
            ],
        );
        cmd.driver().draw(pass, 3, 1);
        cmd.driver().end_render_pass(pass);
        Ok(self.targets.surface.surface())
    }

    #[allow(clippy::too_many_arguments)]
    fn draw_sprite_plane(
        &self,
        cmd: &mut GpuCommand,
        plane: RawTexture,
        seed: RawTexture,
        uniforms: SceneUniforms,
        mask: u32,
        has_trails: bool,
        options: GameVideoOptions,
        geometry: &Geometry,
        scene_buffer: RawBuffer,
        sp_tiles: RawBuffer,
        sampler: RawSampler,
    ) -> Result<(), String> {
        let mut pass_uniforms = uniforms;
        pass_uniforms.layer_mask |= mask;
        cmd.driver()
            .push_vertex_uniforms(cmd.raw(), 0, uniform_bytes(&pass_uniforms));
        let pass = cmd.begin_render_pass(plane, has_trails)?;
        cmd.driver().bind_pipeline(pass, self.sprite_pipeline.get());
        if !options.expanded() {
            cmd.driver().set_scissor(
                pass,
                NATIVE_FIRST_COLUMN as i32,
                geometry.first_line() as i32,
                Geometry::NATIVE_WIDTH as i32,
                geometry.height() as i32,
            );
        }
        cmd.driver()
            .bind_vertex_storage_buffers(pass, 0, &[scene_buffer]);
        cmd.driver().bind_fragment_samplers(
            pass,
            0,
            &[TextureSamplerBinding {
                texture: seed.0,
                sampler: sampler.0,
            }],
        );
        cmd.driver()
            .bind_fragment_storage_buffers(pass, 0, &[sp_tiles]);
        cmd.driver().draw(pass, 6, uniforms.sprite_count);
        cmd.driver().end_render_pass(pass);
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::gpu::driver::DriverRef;
    use crate::gpu::mock_driver::MockGpuDriver;
    use std::rc::Rc;

    #[test]
    fn tile_assets_reject_incomplete_roms() {
        let tile = [0u8; TILE_BYTES as usize];
        assert_eq!(
            GpuTileAssets::validate(&[], &tile).unwrap_err(),
            "Incomplete GPU tile/sprite assets"
        );
        assert_eq!(
            GpuTileAssets::validate(&tile, &[]).unwrap_err(),
            "Incomplete GPU tile/sprite assets"
        );
        assert!(GpuTileAssets::validate(&tile[..255], &tile).is_err());
        assert!(GpuTileAssets::validate(&tile, &tile).is_ok());
    }

    #[test]
    fn tile_assets_reject_offsets_that_do_not_fit_32_bits() {
        // Each ROM must fit half of a 32-bit offset; the pair sums without overflow.
        assert!(validate_lengths(0x8000_0000, 256).is_err());
        assert!(validate_lengths(256, 0x8000_0000).is_err());
        assert!(validate_lengths(0x7FFF_FF00, 0x7FFF_FF00).is_ok());
        assert!(validate_lengths(510, 256).is_err());
    }

    #[test]
    fn interpolation_pipeline_is_lazy_and_keeps_state_on_failure() {
        let mock = Rc::new(MockGpuDriver::default());
        let driver: DriverRef = mock.clone();
        let ctx = GpuContext::new(driver, None, false).expect("context");
        let geometry = Geometry::default();
        let options = GameVideoOptions::default();
        let mut pass = ScenePass::new(&ctx, &geometry, options, false).expect("scene pass");
        assert!(pass.interpolation_pipeline().is_none());
        mock.fail_after("pipeline", 0);
        assert!(pass.ensure_interpolation_pipeline(&ctx).is_err());
        assert!(pass.interpolation_pipeline().is_none());
        mock.clear_failures();
        pass.ensure_interpolation_pipeline(&ctx).expect("pipeline");
        assert!(pass.interpolation_pipeline().is_some());
    }
}
