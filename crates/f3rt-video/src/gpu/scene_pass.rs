//! GPU sprite and scanout passes.

use super::api::validate_tile_assets;
use super::encode::{ENCODED_INTERPOLATION_WORDS, EncodedScene, encoded_native_words};
use super::sdl_gpu::{
    CommandBuffer, CopyPass, GpuBuffer, GpuContext, GpuPipeline, GpuTexture, GpuTransfer,
    SamplerBinding, ShaderKind, TextureFormat, TextureRef, TransferUsage,
};
use crate::config::{GameVideoOptions, Geometry};
use crate::gpu::api::TILE_BYTES;
use crate::gpu::scene_layout::{F3_MASK_BLEND_WEIGHTED, F3_MASK_HIDE_WEIGHTED};

// Sprite hardware plane at native resolution: 432 x 256 scanout lines.
const NATIVE_PLANE_WIDTH: u32 = 432;
const NATIVE_PLANE_HEIGHT: u32 = 256;
// Visible columns 46..365 on every game (`geometry::NATIVE_WIDTH` wide).
const NATIVE_FIRST_COLUMN: u32 = 46;
const NATIVE_END_COLUMN: u32 = 366;

fn pack_word(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
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
        validate_tile_assets(pf_rom.len(), sp_rom.len())
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
        let mut staging = ctx.create_transfer(pf_bytes + sp_bytes, TransferUsage::Upload)?;
        {
            let mut mapping = staging.map(false, "Map GPU asset upload")?;
            let words = mapping.words(((pf_bytes + sp_bytes) / 4) as usize);
            for i in (0..pf_bytes as usize).step_by(4) {
                words[i / 4].write(pack_word(pf_rom, i));
                tile_pen_masks[i / TILE_BYTES as usize] |= (1u64 << (pf_rom[i] & 63))
                    | (1u64 << (pf_rom[i + 1] & 63))
                    | (1u64 << (pf_rom[i + 2] & 63))
                    | (1u64 << (pf_rom[i + 3] & 63));
            }
            for i in (0..sp_bytes as usize).step_by(4) {
                words[(pf_bytes as usize + i) / 4].write(pack_word(sp_rom, i));
            }
        }
        let mut cmd = ctx.begin_command()?;
        {
            let mut copy = cmd.begin_copy_pass("Begin GPU asset copy")?;
            copy.upload_buffer(&staging, 0, &pf_tiles, pf_bytes, false);
            copy.upload_buffer(&staging, pf_bytes, &sp_tiles, sp_bytes, false);
        }
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

enum SpritePlane {
    Primary,
    Unweighted,
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

    pub fn upload_buffer(&mut self) -> &mut GpuTransfer {
        &mut self.upload
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
        copy: &mut CopyPass<'_>,
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
                .as_mut()
                .expect("trail upload created above");
            let mut mapping = trail_upload.map(true, "Map GPU sprite trails")?;
            let mapped = mapping.words((width * height) as usize);
            if !expanded {
                for (dst, src) in mapped.iter_mut().zip(seed.iter()) {
                    dst.write(u32::from(*src));
                }
            } else {
                for y in 0..height {
                    let native_y = y / options.scale + geometry.first_line();
                    for x in 0..width {
                        let native_x = (x / options.scale) as i32 + NATIVE_FIRST_COLUMN as i32
                            - options.border as i32;
                        mapped[(y * width + x) as usize].write(
                            if native_x >= NATIVE_FIRST_COLUMN as i32
                                && native_x < NATIVE_END_COLUMN as i32
                            {
                                u32::from(
                                    seed[(native_y * NATIVE_PLANE_WIDTH + native_x as u32)
                                        as usize],
                                )
                            } else {
                                0
                            },
                        );
                    }
                }
            }
        }
        let trail_upload = self
            .trail_upload
            .as_ref()
            .expect("trail upload created above");
        for plane in [&self.targets.sprite_plane, &self.targets.sprite_plane_b] {
            copy.upload_texture(trail_upload, plane.as_ref(), true);
        }
        Ok(())
    }

    pub fn surface(&self) -> TextureRef<'_> {
        self.targets.surface.as_ref()
    }

    pub fn draw(
        &mut self,
        cmd: &mut CommandBuffer<'_>,
        encoded: &EncodedScene<'_>,
        ctx: &GpuContext,
        assets: &GpuTileAssets,
        options: GameVideoOptions,
        geometry: &Geometry,
    ) -> Result<TextureRef<'_>, String> {
        {
            let mut copy = cmd.begin_copy_pass("Begin GPU frame copy")?;
            let destination = if encoded.fallback {
                &self.native_buffer
            } else {
                &self.scene_buffer
            };
            copy.upload_buffer(&self.upload, 0, destination, encoded.upload_bytes, true);
            if encoded.has_trails {
                self.upload_trail_seed(&mut copy, ctx, encoded.trail_seed, options, geometry)?;
            }
        }

        let mut uniforms = encoded.uniforms;
        let native_geometry = (!options.expanded()).then_some(geometry);
        if !encoded.fallback {
            self.draw_sprite_plane(
                cmd,
                SpritePlane::Primary,
                encoded,
                assets,
                ctx,
                native_geometry,
            )?;
            if encoded.blend_weighted {
                self.draw_sprite_plane(
                    cmd,
                    SpritePlane::Unweighted,
                    encoded,
                    assets,
                    ctx,
                    native_geometry,
                )?;
                uniforms.layer_mask |= F3_MASK_BLEND_WEIGHTED;
            }
        }
        cmd.push_fragment_uniforms(0, &uniforms.to_bytes());
        let target = self.surface();
        let mut pass = cmd.begin_render_pass(target, false)?;
        let pipeline = if encoded.use_interpolation {
            self.interpolation_pipeline
                .as_ref()
                .unwrap_or(&self.scene_pipeline)
        } else {
            &self.scene_pipeline
        };
        pass.bind_pipeline(pipeline);
        let primary = self.targets.sprite_plane.as_ref();
        let second = if encoded.blend_weighted {
            self.targets.sprite_plane_b.as_ref()
        } else {
            primary
        };
        let sampler = ctx.sampler();
        pass.bind_fragment_samplers(
            0,
            [
                SamplerBinding {
                    texture: primary,
                    sampler,
                },
                SamplerBinding {
                    texture: second,
                    sampler,
                },
            ],
        );
        pass.bind_fragment_storage_buffers(
            0,
            [&self.scene_buffer, assets.pf_tiles(), &self.native_buffer],
        );
        pass.draw(3, 1);
        Ok(target)
    }

    fn draw_sprite_plane(
        &self,
        cmd: &mut CommandBuffer<'_>,
        plane: SpritePlane,
        encoded: &EncodedScene<'_>,
        assets: &GpuTileAssets,
        ctx: &GpuContext,
        native_geometry: Option<&Geometry>,
    ) -> Result<(), String> {
        let mut uniforms = encoded.uniforms;
        let (target, seed) = match plane {
            SpritePlane::Primary => (&self.targets.sprite_plane, &self.targets.sprite_plane_b),
            SpritePlane::Unweighted => {
                uniforms.layer_mask |= F3_MASK_HIDE_WEIGHTED;
                (&self.targets.sprite_plane_b, &self.targets.sprite_plane)
            }
        };
        cmd.push_vertex_uniforms(0, &uniforms.to_bytes());
        let mut pass = cmd.begin_render_pass(target.as_ref(), encoded.has_trails)?;
        pass.bind_pipeline(&self.sprite_pipeline);
        if let Some(geometry) = native_geometry {
            pass.set_scissor(
                NATIVE_FIRST_COLUMN as i32,
                geometry.first_line() as i32,
                Geometry::NATIVE_WIDTH as i32,
                geometry.height() as i32,
            );
        }
        pass.bind_vertex_storage_buffers(0, [&self.scene_buffer]);
        pass.bind_fragment_samplers(
            0,
            [SamplerBinding {
                texture: seed.as_ref(),
                sampler: ctx.sampler(),
            }],
        );
        pass.bind_fragment_storage_buffers(0, [assets.sp_tiles()]);
        pass.draw(6, uniforms.sprite_count);
        Ok(())
    }
}
