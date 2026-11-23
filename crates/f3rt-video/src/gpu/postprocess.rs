//! Presentation postprocessing and rotation.

use super::api::{Postprocess, validate_rotation};
use super::sdl_gpu::{
    GpuCommand, GpuContext, GpuPipeline, GpuSurface, GpuTexture, ShaderKind, TextureFormat,
    sampler_binding, uniform_bytes,
};
use super::user_spirv::load_user_shader;
use crate::config::{GameVideoOptions, Geometry};

const TARGET_FORMAT: TextureFormat = TextureFormat::Bgra8Unorm;

fn user_pipeline(ctx: &GpuContext, path: &str) -> Result<GpuPipeline, String> {
    let metal = ctx.is_metal();
    let loaded = load_user_shader(path, metal)?;
    ctx.create_user_pipeline(
        &loaded.code,
        loaded.size,
        "f3_postprocess",
        1,
        0,
        1,
        TARGET_FORMAT,
    )
}

pub struct Postprocessor {
    preset: Postprocess,
    pipeline: Option<GpuPipeline>,
    surface: Option<GpuTexture>,
    start_ns: u64,
}

impl Default for Postprocessor {
    fn default() -> Self {
        Self {
            preset: Postprocess::Off,
            pipeline: None,
            surface: None,
            start_ns: 0,
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
            user_pipeline(ctx, user_shader)?
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
        // SAFETY: SDL is initialized by the host.
        self.start_ns = unsafe { sdl3_sys::timer::SDL_GetTicksNS() };
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

    /// Returns `input` unchanged unless a preset is active and `apply_now`.
    pub fn apply(
        &mut self,
        cmd: &mut GpuCommand,
        input: GpuSurface,
        ctx: &GpuContext,
        geometry: &Geometry,
        options: GameVideoOptions,
        apply_now: bool,
    ) -> Result<GpuSurface, String> {
        if !self.active() || !apply_now {
            return Ok(input);
        }
        // SAFETY: SDL is initialized by the host.
        let now = unsafe { sdl3_sys::timer::SDL_GetTicksNS() };
        let elapsed = (now.saturating_sub(self.start_ns)) as f64 / 1.0e9;
        let parameters = [
            options.width(geometry) as f32,
            options.height(geometry) as f32,
            options.scale as f32,
            elapsed as f32,
        ];
        // SAFETY: The f32 array is initialized and has no padding.
        let bytes = unsafe { uniform_bytes(&parameters) };
        cmd.push_fragment_uniforms(0, bytes);
        let target = self
            .surface
            .as_ref()
            .ok_or_else(|| "GPU postprocess target missing".to_owned())?;
        let pipeline = self
            .pipeline
            .as_ref()
            .ok_or_else(|| "GPU postprocess pipeline missing".to_owned())?;
        let pass = cmd.begin_render_pass(target.get(), false)?;
        cmd.bind_pipeline(pass, pipeline.get());
        cmd.bind_fragment_samplers(pass, 0, &[sampler_binding(input.texture, ctx.sampler())]);
        cmd.draw(pass, 3, 1);
        cmd.end_render_pass(pass);
        Ok(target.surface())
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

    pub fn apply(
        &mut self,
        cmd: &mut GpuCommand,
        input: GpuSurface,
        ctx: &GpuContext,
    ) -> Result<GpuSurface, String> {
        if self.rotation == 0 {
            return Ok(input);
        }
        // rotate.frag reads a uvec4 even though only its first component is used.
        let uniform = [self.rotation, 0, 0, 0];
        // SAFETY: The u32 array is initialized and has no padding.
        let bytes = unsafe { uniform_bytes(&uniform) };
        cmd.push_fragment_uniforms(0, bytes);
        let target = self
            .surface
            .as_ref()
            .ok_or_else(|| "GPU rotation target missing".to_owned())?;
        let pipeline = self
            .pipeline
            .as_ref()
            .ok_or_else(|| "GPU rotation pipeline missing".to_owned())?;
        let pass = cmd.begin_render_pass(target.get(), false)?;
        cmd.bind_pipeline(pass, pipeline.get());
        cmd.bind_fragment_samplers(pass, 0, &[sampler_binding(input.texture, ctx.sampler())]);
        cmd.draw(pass, 3, 1);
        cmd.end_render_pass(pass);
        Ok(target.surface())
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
