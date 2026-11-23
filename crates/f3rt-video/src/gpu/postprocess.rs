//! Presentation postprocessing and rotation.

use super::api::{Postprocess, validate_rotation};
use super::scene_layout::uniform_words;
use super::sdl_gpu::{
    CommandBuffer, GpuContext, GpuPipeline, GpuTexture, SamplerBinding, ShaderKind, TextureFormat,
    TextureRef,
};
use super::user_spirv::load_user_shader;
use crate::config::{GameVideoOptions, Geometry};
use std::time::Instant;

const TARGET_FORMAT: TextureFormat = TextureFormat::Bgra8Unorm;

struct PostprocessUniforms {
    width: f32,
    height: f32,
    scale: f32,
    elapsed_seconds: f32,
}

impl PostprocessUniforms {
    fn to_bytes(&self) -> [u8; 16] {
        uniform_words([self.width, self.height, self.scale, self.elapsed_seconds].map(f32::to_bits))
    }
}

pub struct Postprocessor {
    preset: Postprocess,
    pipeline: Option<GpuPipeline>,
    surface: Option<GpuTexture>,
    start: Instant,
}

impl Default for Postprocessor {
    fn default() -> Self {
        Self {
            preset: Postprocess::Off,
            pipeline: None,
            surface: None,
            start: Instant::now(),
        }
    }
}

impl Postprocessor {
    /// User ABI: `.metal` entry `f3_postprocess` on Metal, `.spv` entry `main` on Vulkan.
    /// Files are limited to 1 MiB. Throws before mutating anything, retaining the last valid
    /// preset.
    pub fn configure(
        &mut self,
        ctx: &GpuContext,
        geometry: &Geometry,
        options: GameVideoOptions,
        preset: Postprocess,
        user_shader: &str,
    ) -> Result<(), String> {
        if preset == Postprocess::Off {
            self.pipeline = None;
            self.surface = None;
            self.preset = preset;
            return Ok(());
        }
        let next_pipeline = if preset == Postprocess::Crt {
            ctx.create_pipeline(
                ShaderKind::FullscreenVert,
                ShaderKind::PostprocessFrag,
                TARGET_FORMAT,
            )?
        } else {
            let loaded = load_user_shader(user_shader, ctx.is_metal())?;
            ctx.create_user_pipeline(&loaded, TARGET_FORMAT)?
        };
        let next_surface = if self.surface.is_none() {
            Some(ctx.create_texture(
                TARGET_FORMAT,
                options.width(geometry),
                options.height(geometry),
            )?)
        } else {
            None
        };
        // SDL defers destruction of the replaced pipeline until pending commands finish.
        self.pipeline = Some(next_pipeline);
        if let Some(surface) = next_surface {
            self.surface = Some(surface);
        }
        self.preset = preset;
        self.start = Instant::now();
        Ok(())
    }

    /// A new target when a postprocess surface exists, else an empty texture.
    pub fn reallocate_target(
        &self,
        ctx: &GpuContext,
        geometry: &Geometry,
        options: GameVideoOptions,
    ) -> Result<GpuTexture, String> {
        if self.surface.is_none() {
            return Ok(GpuTexture::empty());
        }
        ctx.create_texture(
            TARGET_FORMAT,
            options.width(geometry),
            options.height(geometry),
        )
    }

    pub fn commit_target(&mut self, next: GpuTexture) {
        self.surface = next.is_valid().then_some(next);
    }

    pub fn active(&self) -> bool {
        self.preset != Postprocess::Off
    }

    pub fn preset(&self) -> Postprocess {
        self.preset
    }

    pub fn surface(&self) -> TextureRef<'_> {
        self.surface
            .as_ref()
            .expect("active postprocess target")
            .as_ref()
    }

    pub fn apply<'a>(
        &'a self,
        cmd: &mut CommandBuffer<'_>,
        input: TextureRef<'a>,
        ctx: &GpuContext,
        scale: u32,
        apply_now: bool,
    ) -> Result<TextureRef<'a>, String> {
        if !self.active() || !apply_now {
            return Ok(input);
        }
        let elapsed = self.start.elapsed().as_nanos() as f64 / 1.0e9;
        let uniforms = PostprocessUniforms {
            width: input.width() as f32,
            height: input.height() as f32,
            scale: scale as f32,
            elapsed_seconds: elapsed as f32,
        };
        cmd.push_fragment_uniforms(0, &uniforms.to_bytes());
        let target = self.surface();
        let pipeline = self.pipeline.as_ref().expect("active postprocess pipeline");
        let mut pass = cmd.begin_render_pass(target, false)?;
        pass.bind_pipeline(pipeline);
        pass.bind_fragment_samplers(
            0,
            [SamplerBinding {
                texture: input,
                sampler: ctx.sampler(),
            }],
        );
        pass.draw(3, 1);
        Ok(target)
    }
}

/// Presentation rotation (90 or 270 degrees) into a portrait surface; passthrough when upright.
pub struct SurfaceRotator {
    rotation: u32,
    pipeline: Option<GpuPipeline>,
    surface: Option<GpuTexture>,
}

impl SurfaceRotator {
    /// Throws unless `rotation` is 0, 90 or 270.
    pub fn validate(rotation: u32) -> Result<(), String> {
        validate_rotation(rotation)
    }

    pub fn new(
        ctx: &GpuContext,
        geometry: &Geometry,
        options: GameVideoOptions,
        rotation: u32,
    ) -> Result<Self, String> {
        Self::validate(rotation)?;
        if rotation == 0 {
            return Ok(Self {
                rotation,
                pipeline: None,
                surface: None,
            });
        }
        let pipeline = Some(ctx.create_pipeline(
            ShaderKind::FullscreenVert,
            ShaderKind::RotateFrag,
            TARGET_FORMAT,
        )?);
        let surface = Some(Self::target_texture(ctx, geometry, options)?);
        Ok(Self {
            rotation,
            pipeline,
            surface,
        })
    }

    fn target_texture(
        ctx: &GpuContext,
        geometry: &Geometry,
        options: GameVideoOptions,
    ) -> Result<GpuTexture, String> {
        ctx.create_texture(
            TARGET_FORMAT,
            options.height(geometry),
            options.width(geometry),
        )
    }

    pub fn reallocate_target(
        &self,
        ctx: &GpuContext,
        geometry: &Geometry,
        options: GameVideoOptions,
    ) -> Result<GpuTexture, String> {
        if self.rotation == 0 {
            return Ok(GpuTexture::empty());
        }
        Self::target_texture(ctx, geometry, options)
    }

    pub fn commit_target(&mut self, next: GpuTexture) {
        self.surface = next.is_valid().then_some(next);
    }

    pub fn rotated(&self) -> bool {
        self.rotation != 0
    }

    pub fn apply<'a>(
        &'a self,
        cmd: &mut CommandBuffer<'_>,
        input: TextureRef<'a>,
        ctx: &GpuContext,
    ) -> Result<TextureRef<'a>, String> {
        if self.rotation == 0 {
            return Ok(input);
        }
        // rotate.frag reads a uvec4 even though only its first component is used.
        let bytes: [u8; 16] = uniform_words([self.rotation, 0, 0, 0]);
        cmd.push_fragment_uniforms(0, &bytes);
        let target = self
            .surface
            .as_ref()
            .expect("active rotation target")
            .as_ref();
        let pipeline = self.pipeline.as_ref().expect("active rotation pipeline");
        let mut pass = cmd.begin_render_pass(target, false)?;
        pass.bind_pipeline(pipeline);
        pass.bind_fragment_samplers(
            0,
            [SamplerBinding {
                texture: input,
                sampler: ctx.sampler(),
            }],
        );
        pass.draw(3, 1);
        Ok(target)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rotation_accepts_only_portrait_right_angles() {
        assert!(SurfaceRotator::validate(0).is_ok());
        assert!(SurfaceRotator::validate(90).is_ok());
        assert!(SurfaceRotator::validate(270).is_ok());
        for rotation in [1, 45, 180, 360] {
            assert_eq!(
                SurfaceRotator::validate(rotation).unwrap_err(),
                "Unsupported GPU presentation rotation"
            );
        }
    }
}
