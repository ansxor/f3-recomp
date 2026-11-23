//! SDL3 GPU object lifetimes and presentation, calling `sdl3-sys` directly.
//!
//! Replaces the former `runtime/renderer/gpu/sdl_gpu.{hpp,cpp}` and the C driver vtable
//! (`sdl_driver_shim.{hpp,cpp}`, `driver.rs`). Every handle here is an SDL3 GPU pointer; the
//! device owns all of them.

// Raw GPU handles are only ever passed back to SDL3 or overlaid on C++-owned hosts.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

pub use super::api::GpuSurface;
use super::api::{PresentAcquire, PresentRequest};
use super::pacing::DisplayPacer;
use super::shaders::Shader;
use crate::config::{GameVideoOptions, Geometry, VideoScaleMode};
use crate::scale::video_blit_for_window;
use sdl3_sys::error::SDL_GetError;
use sdl3_sys::gpu::*;
use sdl3_sys::pixels::SDL_FColor;
use sdl3_sys::rect::SDL_Rect;
use std::ffi::{CStr, CString, c_void};

pub use sdl3_sys::video::SDL_Window;

/// Texture formats the renderer creates.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TextureFormat {
    Bgra8Unorm,
    Rgba8Unorm,
    R32Uint,
}

impl TextureFormat {
    pub fn from_sdl(format: SDL_GPUTextureFormat) -> Option<Self> {
        match format {
            SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM => Some(Self::Bgra8Unorm),
            SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM => Some(Self::Rgba8Unorm),
            SDL_GPU_TEXTUREFORMAT_R32_UINT => Some(Self::R32Uint),
            _ => None,
        }
    }

    fn sdl(self) -> SDL_GPUTextureFormat {
        match self {
            Self::Bgra8Unorm => SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
            Self::Rgba8Unorm => SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            Self::R32Uint => SDL_GPU_TEXTUREFORMAT_R32_UINT,
        }
    }
}

/// Transfer buffer usage.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TransferUsage {
    Upload,
    Download,
}

impl TransferUsage {
    fn sdl(self) -> SDL_GPUTransferBufferUsage {
        match self {
            Self::Upload => SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
            Self::Download => SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
        }
    }
}

/// Graphics shaders baked into the generated `video_shaders.rs`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ShaderKind {
    FullscreenVert,
    SpriteVert,
    SpriteFrag,
    SceneFrag,
    SceneInterpFrag,
    RotateFrag,
    PostprocessFrag,
}

fn shader(kind: ShaderKind) -> &'static Shader {
    use super::shaders as s;
    match kind {
        ShaderKind::FullscreenVert => &s::FULLSCREEN_VERT,
        ShaderKind::SpriteVert => &s::SPRITE_VERT,
        ShaderKind::SpriteFrag => &s::SPRITE_FRAG,
        ShaderKind::SceneFrag => &s::SCENE_FRAG,
        ShaderKind::SceneInterpFrag => &s::SCENE_INTERP_FRAG,
        ShaderKind::RotateFrag => &s::ROTATE_FRAG,
        ShaderKind::PostprocessFrag => &s::POSTPROCESS_FRAG,
    }
}

fn detail() -> String {
    // SAFETY: SDL_GetError returns a NUL-terminated, thread-local string.
    let message = SDL_GetError();
    if message.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(message) }
            .to_string_lossy()
            .into_owned()
    }
}

pub(crate) fn failure(operation: &str) -> String {
    format!("{operation}: {}", detail())
}

fn created<T>(handle: *mut T, operation: &str) -> Result<*mut T, String> {
    if handle.is_null() {
        Err(failure(operation))
    } else {
        Ok(handle)
    }
}

pub fn c_path(path: &str, operation: &str) -> Result<CString, String> {
    CString::new(path).map_err(|_| format!("{operation}: path contains a NUL byte"))
}

/// Move-only owner of one GPU buffer. The device must outlive every handle.
pub struct GpuBuffer {
    device: *mut SDL_GPUDevice,
    handle: *mut SDL_GPUBuffer,
}

impl GpuBuffer {
    pub fn new(device: *mut SDL_GPUDevice, handle: *mut SDL_GPUBuffer) -> Self {
        Self { device, handle }
    }

    pub fn get(&self) -> *mut SDL_GPUBuffer {
        self.handle
    }
}

impl Drop for GpuBuffer {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            // SAFETY: the device owns the buffer and outlives it.
            unsafe { SDL_ReleaseGPUBuffer(self.device, self.handle) };
        }
    }
}

pub struct GpuPipeline {
    device: *mut SDL_GPUDevice,
    handle: *mut SDL_GPUGraphicsPipeline,
}

impl GpuPipeline {
    pub fn new(device: *mut SDL_GPUDevice, handle: *mut SDL_GPUGraphicsPipeline) -> Self {
        Self { device, handle }
    }

    pub fn get(&self) -> *mut SDL_GPUGraphicsPipeline {
        self.handle
    }
}

impl Drop for GpuPipeline {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            // SAFETY: the device owns the pipeline and outlives it.
            unsafe { SDL_ReleaseGPUGraphicsPipeline(self.device, self.handle) };
        }
    }
}

pub struct GpuSampler {
    device: *mut SDL_GPUDevice,
    handle: *mut SDL_GPUSampler,
}

impl GpuSampler {
    pub fn new(device: *mut SDL_GPUDevice, handle: *mut SDL_GPUSampler) -> Self {
        Self { device, handle }
    }

    pub fn get(&self) -> *mut SDL_GPUSampler {
        self.handle
    }
}

impl Drop for GpuSampler {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            // SAFETY: the device owns the sampler and outlives it.
            unsafe { SDL_ReleaseGPUSampler(self.device, self.handle) };
        }
    }
}

/// An owned render texture with the format/size used to decide reallocation.
pub struct GpuTexture {
    device: *mut SDL_GPUDevice,
    handle: *mut SDL_GPUTexture,
    format: TextureFormat,
    width: u32,
    height: u32,
}

impl GpuTexture {
    pub const fn empty() -> Self {
        Self {
            device: std::ptr::null_mut(),
            handle: std::ptr::null_mut(),
            format: TextureFormat::Bgra8Unorm,
            width: 0,
            height: 0,
        }
    }

    pub fn new(
        device: *mut SDL_GPUDevice,
        handle: *mut SDL_GPUTexture,
        format: TextureFormat,
        width: u32,
        height: u32,
    ) -> Self {
        Self {
            device,
            handle,
            format,
            width,
            height,
        }
    }

    pub fn get(&self) -> *mut SDL_GPUTexture {
        self.handle
    }

    pub fn texture_handle(&self) -> usize {
        self.handle as usize
    }

    pub fn surface(&self) -> GpuSurface {
        GpuSurface {
            texture: self.handle as usize,
            width: self.width,
            height: self.height,
        }
    }

    pub fn is_valid(&self) -> bool {
        !self.handle.is_null()
    }

    pub fn width(&self) -> u32 {
        self.width
    }

    pub fn height(&self) -> u32 {
        self.height
    }

    /// Keeps the current texture when it already has this format and size; otherwise replaces it.
    pub fn ensure(
        &mut self,
        ctx: &GpuContext,
        format: TextureFormat,
        width: u32,
        height: u32,
    ) -> Result<(), String> {
        if self.is_valid() && format == self.format && width == self.width && height == self.height
        {
            return Ok(());
        }
        *self = ctx.create_texture(format, width, height)?;
        Ok(())
    }
}

impl Drop for GpuTexture {
    fn drop(&mut self) {
        if !self.handle.is_null() && !self.device.is_null() {
            // SAFETY: the device owns the texture and outlives it.
            unsafe { SDL_ReleaseGPUTexture(self.device, self.handle) };
        }
    }
}

/// A mapped transfer buffer; unmaps on drop.
pub struct GpuMapping {
    device: *mut SDL_GPUDevice,
    transfer: *mut SDL_GPUTransferBuffer,
    data: *mut c_void,
}

impl GpuMapping {
    pub fn words(&mut self, count: usize) -> &mut [u32] {
        // SAFETY: SDL returned a mapping of at least the transfer buffer's requested size.
        unsafe { std::slice::from_raw_parts_mut(self.data.cast::<u32>(), count) }
    }
}

impl Drop for GpuMapping {
    fn drop(&mut self) {
        // SAFETY: the buffer is mapped by this object and the device outlives it.
        unsafe { SDL_UnmapGPUTransferBuffer(self.device, self.transfer) };
    }
}

pub struct GpuTransfer {
    device: *mut SDL_GPUDevice,
    handle: *mut SDL_GPUTransferBuffer,
    size: u32,
}

impl GpuTransfer {
    pub fn new(device: *mut SDL_GPUDevice, handle: *mut SDL_GPUTransferBuffer, size: u32) -> Self {
        Self {
            device,
            handle,
            size,
        }
    }

    pub fn get(&self) -> *mut SDL_GPUTransferBuffer {
        self.handle
    }

    pub fn size(&self) -> u32 {
        self.size
    }

    /// `cycle` lets SDL hand out fresh storage when the previous contents are in flight.
    pub fn map(&self, cycle: bool, operation: &str) -> Result<GpuMapping, String> {
        // SAFETY: the device owns the transfer buffer and outlives the mapping.
        let data = unsafe { SDL_MapGPUTransferBuffer(self.device, self.handle, cycle) };
        if data.is_null() {
            return Err(failure(operation));
        }
        Ok(GpuMapping {
            device: self.device,
            transfer: self.handle,
            data,
        })
    }
}

impl Drop for GpuTransfer {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            // SAFETY: the device owns the transfer buffer and outlives it.
            unsafe { SDL_ReleaseGPUTransferBuffer(self.device, self.handle) };
        }
    }
}

fn is_metal_device(gpu: *mut SDL_GPUDevice) -> bool {
    // SAFETY: `gpu` is a live device.
    let formats = unsafe { SDL_GetGPUShaderFormats(gpu) };
    formats.0 & SDL_GPU_SHADERFORMAT_MSL.0 != 0
}

fn create_shader(
    gpu: *mut SDL_GPUDevice,
    source: &Shader,
    stage: SDL_GPUShaderStage,
) -> *mut SDL_GPUShader {
    let metal = is_metal_device(gpu);
    let info = SDL_GPUShaderCreateInfo {
        code_size: if metal {
            source.msl.len()
        } else {
            source.spirv.len()
        },
        code: if metal {
            source.msl.as_ptr()
        } else {
            source.spirv.as_ptr()
        },
        entrypoint: if metal {
            source.msl_entry.as_ptr()
        } else {
            c"main".as_ptr()
        },
        format: if metal {
            SDL_GPU_SHADERFORMAT_MSL
        } else {
            SDL_GPU_SHADERFORMAT_SPIRV
        },
        stage,
        num_samplers: source.samplers,
        num_storage_buffers: source.buffers,
        num_uniform_buffers: source.uniforms,
        ..Default::default()
    };
    // SAFETY: `info` points at shader code owned by the static shader table.
    unsafe { SDL_CreateGPUShader(gpu, &info) }
}

fn build_pipeline(
    gpu: *mut SDL_GPUDevice,
    vertex: *mut SDL_GPUShader,
    fragment: *mut SDL_GPUShader,
    format: SDL_GPUTextureFormat,
) -> *mut SDL_GPUGraphicsPipeline {
    let target = SDL_GPUColorTargetDescription {
        format,
        ..Default::default()
    };
    let info = SDL_GPUGraphicsPipelineCreateInfo {
        vertex_shader: vertex,
        fragment_shader: fragment,
        primitive_type: SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        rasterizer_state: SDL_GPURasterizerState {
            fill_mode: SDL_GPU_FILLMODE_FILL,
            cull_mode: SDL_GPU_CULLMODE_NONE,
            enable_depth_clip: true,
            ..Default::default()
        },
        multisample_state: SDL_GPUMultisampleState {
            sample_count: SDL_GPU_SAMPLECOUNT_1,
            ..Default::default()
        },
        target_info: SDL_GPUGraphicsPipelineTargetInfo {
            color_target_descriptions: &target,
            num_color_targets: 1,
            ..Default::default()
        },
        ..Default::default()
    };
    // SAFETY: `info` borrows `target` and the shaders only for the call.
    unsafe { SDL_CreateGPUGraphicsPipeline(gpu, &info) }
}

/// The device, window claim and sampler every GPU object shares.
pub struct GpuContext {
    device: *mut SDL_GPUDevice,
    window: *mut SDL_Window,
    claimed: bool,
    vsync: bool,
    sampler: Option<GpuSampler>,
}

impl GpuContext {
    /// SDL video must be initialized. Window may be null for offscreen use.
    pub fn new(window: usize, vsync: bool) -> Result<Self, String> {
        let window = window as *mut SDL_Window;
        // SAFETY: SDL was initialized by the host before constructing the renderer.
        let device = created(
            unsafe {
                SDL_CreateGPUDevice(
                    SDL_GPU_SHADERFORMAT_SPIRV
                        | SDL_GPU_SHADERFORMAT_MSL
                        | SDL_GPU_SHADERFORMAT_METALLIB,
                    false,
                    std::ptr::null(),
                )
            },
            "Create SDL GPU device",
        )?;
        let mut claimed = false;
        let mut sampler = None;
        let initialized = (|| -> Result<(), String> {
            if !window.is_null() {
                // SAFETY: `device` and `window` are live.
                if !unsafe { SDL_ClaimWindowForGPUDevice(device, window) } {
                    return Err(failure("Claim GPU window"));
                }
                claimed = true;
                set_swapchain_mode(device, window, vsync)?;
            }
            // SAFETY: `device` is live.
            let handle = created(create_sampler(device), "Create GPU sampler")?;
            sampler = Some(GpuSampler::new(device, handle));
            Ok(())
        })();
        if let Err(error) = initialized {
            drop(sampler.take());
            if claimed {
                // SAFETY: `device` and `window` are live.
                unsafe { SDL_ReleaseWindowFromGPUDevice(device, window) };
            }
            // SAFETY: this context owns the device.
            unsafe { SDL_DestroyGPUDevice(device) };
            return Err(error);
        }
        Ok(Self {
            device,
            window,
            claimed,
            vsync,
            sampler,
        })
    }

    pub fn begin_command(&self) -> Result<GpuCommand, String> {
        // SAFETY: the device is live.
        let command = created(
            unsafe { SDL_AcquireGPUCommandBuffer(self.device) },
            "Acquire GPU command buffer",
        )?;
        Ok(GpuCommand {
            device: self.device,
            command: Some(command),
            swapchain: GpuSurface::default(),
            acquired: false,
        })
    }

    pub fn create_buffer(&self, bytes: u32) -> Result<GpuBuffer, String> {
        let info = SDL_GPUBufferCreateInfo {
            usage: SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
            size: bytes,
            ..Default::default()
        };
        // SAFETY: `info` is fully initialized and the device is live.
        let handle = created(
            unsafe { SDL_CreateGPUBuffer(self.device, &info) },
            "Create GPU storage buffer",
        )?;
        Ok(GpuBuffer::new(self.device, handle))
    }

    pub fn create_texture(
        &self,
        format: TextureFormat,
        width: u32,
        height: u32,
    ) -> Result<GpuTexture, String> {
        let info = SDL_GPUTextureCreateInfo {
            r#type: SDL_GPU_TEXTURETYPE_2D,
            format: format.sdl(),
            usage: SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
            width,
            height,
            layer_count_or_depth: 1,
            num_levels: 1,
            sample_count: SDL_GPU_SAMPLECOUNT_1,
            ..Default::default()
        };
        // SAFETY: `info` is fully initialized and the device is live.
        let handle = created(
            unsafe { SDL_CreateGPUTexture(self.device, &info) },
            "Create GPU render texture",
        )?;
        Ok(GpuTexture::new(self.device, handle, format, width, height))
    }

    pub fn create_pipeline(
        &self,
        vertex: ShaderKind,
        fragment: ShaderKind,
        format: TextureFormat,
    ) -> Result<GpuPipeline, String> {
        let vs = create_shader(self.device, shader(vertex), SDL_GPU_SHADERSTAGE_VERTEX);
        if vs.is_null() {
            return Err(failure("Create GPU graphics pipeline"));
        }
        let fs = create_shader(self.device, shader(fragment), SDL_GPU_SHADERSTAGE_FRAGMENT);
        let mut pipeline = std::ptr::null_mut();
        if !fs.is_null() {
            pipeline = build_pipeline(self.device, vs, fs, format.sdl());
        }
        // SAFETY: both shaders were created above and the pipeline keeps what it needs.
        unsafe {
            if !fs.is_null() {
                SDL_ReleaseGPUShader(self.device, fs);
            }
            SDL_ReleaseGPUShader(self.device, vs);
        }
        let handle = created(pipeline, "Create GPU graphics pipeline")?;
        Ok(GpuPipeline::new(self.device, handle))
    }

    #[allow(clippy::too_many_arguments)]
    pub fn create_user_pipeline(
        &self,
        code: &[u32],
        bytes: usize,
        entry: &str,
        samplers: u32,
        buffers: u32,
        uniforms: u32,
        format: TextureFormat,
    ) -> Result<GpuPipeline, String> {
        let metal = self.is_metal();
        let entry = CString::new(entry).map_err(|_| {
            "Create GPU graphics pipeline: entry point contains a NUL byte".to_owned()
        })?;
        let info = SDL_GPUShaderCreateInfo {
            code_size: bytes,
            code: code.as_ptr().cast::<u8>(),
            entrypoint: if metal {
                entry.as_ptr()
            } else {
                c"main".as_ptr()
            },
            format: if metal {
                SDL_GPU_SHADERFORMAT_MSL
            } else {
                SDL_GPU_SHADERFORMAT_SPIRV
            },
            stage: SDL_GPU_SHADERSTAGE_FRAGMENT,
            num_samplers: samplers,
            num_storage_buffers: buffers,
            num_uniform_buffers: uniforms,
            ..Default::default()
        };
        let vs = create_shader(
            self.device,
            shader(ShaderKind::FullscreenVert),
            SDL_GPU_SHADERSTAGE_VERTEX,
        );
        if vs.is_null() {
            return Err(failure("Create GPU graphics pipeline"));
        }
        // SAFETY: `info` borrows `code` for the call only.
        let fs = unsafe { SDL_CreateGPUShader(self.device, &info) };
        let mut pipeline = std::ptr::null_mut();
        if !fs.is_null() {
            pipeline = build_pipeline(self.device, vs, fs, format.sdl());
            // SAFETY: the shader was created above; the pipeline retained its state.
            unsafe { SDL_ReleaseGPUShader(self.device, fs) };
        }
        // SAFETY: the vertex shader was created above.
        unsafe { SDL_ReleaseGPUShader(self.device, vs) };
        let handle = created(pipeline, "Create GPU graphics pipeline")?;
        Ok(GpuPipeline::new(self.device, handle))
    }

    pub fn create_transfer(&self, bytes: u32, usage: TransferUsage) -> Result<GpuTransfer, String> {
        let info = SDL_GPUTransferBufferCreateInfo {
            usage: usage.sdl(),
            size: bytes,
            ..Default::default()
        };
        // SAFETY: `info` is fully initialized and the device is live.
        let handle = created(
            unsafe { SDL_CreateGPUTransferBuffer(self.device, &info) },
            "Create GPU transfer buffer",
        )?;
        Ok(GpuTransfer::new(self.device, handle, bytes))
    }

    pub fn wait_idle(&self) {
        // SAFETY: the device is live.
        unsafe { SDL_WaitForGPUIdle(self.device) };
    }

    pub fn is_metal(&self) -> bool {
        is_metal_device(self.device)
    }

    pub fn driver_name(&self) -> String {
        // SAFETY: the device is live and the name is a static string owned by SDL.
        let name = unsafe { SDL_GetGPUDeviceDriver(self.device) };
        if name.is_null() {
            String::new()
        } else {
            unsafe { CStr::from_ptr(name) }
                .to_string_lossy()
                .into_owned()
        }
    }

    pub fn device(&self) -> *mut SDL_GPUDevice {
        self.device
    }

    pub fn has_window(&self) -> bool {
        !self.window.is_null()
    }

    pub fn sampler(&self) -> *mut SDL_GPUSampler {
        self.sampler
            .as_ref()
            .map(GpuSampler::get)
            .unwrap_or(std::ptr::null_mut())
    }

    pub fn vsync(&self) -> bool {
        self.vsync
    }

    pub fn swapchain_format(&self) -> Option<TextureFormat> {
        if self.window.is_null() {
            return None;
        }
        // SAFETY: the window is claimed and the device is live.
        let format = unsafe { SDL_GetGPUSwapchainTextureFormat(self.device, self.window) };
        TextureFormat::from_sdl(format)
    }
}

impl Drop for GpuContext {
    fn drop(&mut self) {
        self.wait_idle();
        drop(self.sampler.take());
        if self.claimed {
            // SAFETY: the device is live and the window was claimed by it.
            unsafe { SDL_ReleaseWindowFromGPUDevice(self.device, self.window) };
        }
        // SAFETY: this context owns the device and all objects are gone.
        unsafe { SDL_DestroyGPUDevice(self.device) };
    }
}

fn create_sampler(device: *mut SDL_GPUDevice) -> *mut SDL_GPUSampler {
    let info = SDL_GPUSamplerCreateInfo {
        min_filter: SDL_GPU_FILTER_NEAREST,
        mag_filter: SDL_GPU_FILTER_NEAREST,
        mipmap_mode: SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
        address_mode_u: SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        address_mode_v: SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        address_mode_w: SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        ..Default::default()
    };
    // SAFETY: `info` is fully initialized and the device is live.
    unsafe { SDL_CreateGPUSampler(device, &info) }
}

fn set_swapchain_mode(
    device: *mut SDL_GPUDevice,
    window: *mut SDL_Window,
    vsync: bool,
) -> Result<(), String> {
    if vsync {
        return Ok(());
    }
    // FIFO can make a slow display throttle native simulation/audio.
    // Mailbox remains tear-free without queuing obsolete display frames.
    // SAFETY: the window is claimed and the device is live.
    let mode = unsafe {
        if SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_IMMEDIATE) {
            SDL_GPU_PRESENTMODE_IMMEDIATE
        } else if SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_MAILBOX) {
            SDL_GPU_PRESENTMODE_MAILBOX
        } else {
            SDL_GPU_PRESENTMODE_VSYNC
        }
    };
    if mode == SDL_GPU_PRESENTMODE_VSYNC {
        return Ok(());
    }
    // SAFETY: composition/mode were checked for support above.
    if unsafe {
        SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode)
    } {
        Ok(())
    } else {
        Err(failure("Set GPU presentation mode"))
    }
}

/// One GPU command buffer. Dropping submits when a swapchain texture was acquired (SDL requires
/// it) and cancels otherwise.
pub struct GpuCommand {
    device: *mut SDL_GPUDevice,
    command: Option<*mut SDL_GPUCommandBuffer>,
    swapchain: GpuSurface,
    acquired: bool,
}

impl GpuCommand {
    pub fn acquire_for_present(
        &mut self,
        ctx: &GpuContext,
        pacer: &mut DisplayPacer,
        request: &PresentRequest<'_>,
    ) -> Result<PresentAcquire, String> {
        let pace_prepared = pacer.consume_prepared();
        let timed = request.timing.is_live();
        if !request.wait {
            if !ctx.has_window() {
                return Ok(PresentAcquire::SkipFrame);
            }
            let (texture, width, height) = unsafe {
                let command = self.raw();
                let mut texture = std::ptr::null_mut();
                let mut width = 0;
                let mut height = 0;
                if !SDL_AcquireGPUSwapchainTexture(
                    command,
                    ctx.window,
                    &mut texture,
                    &mut width,
                    &mut height,
                ) {
                    return Err(failure("Try acquiring GPU swapchain"));
                }
                (texture, width, height)
            };
            if texture.is_null() {
                return Ok(PresentAcquire::SkipFrame);
            }
            self.acquired = true;
            self.swapchain = GpuSurface {
                texture: texture as usize,
                width,
                height,
            };
            return Ok(PresentAcquire::DrawableReady);
        }
        if timed && !pace_prepared {
            pacer.wait_tick(ctx.vsync());
        }
        if ctx.has_window() {
            let operation = if timed {
                "Acquire timed GPU swapchain"
            } else {
                "Acquire GPU swapchain"
            };
            // SAFETY: command buffer, device and window are live.
            let (texture, width, height) = unsafe {
                let command = self.raw();
                let mut texture = std::ptr::null_mut();
                let mut width = 0;
                let mut height = 0;
                if !SDL_WaitAndAcquireGPUSwapchainTexture(
                    command,
                    ctx.window,
                    &mut texture,
                    &mut width,
                    &mut height,
                ) {
                    return Err(failure(operation));
                }
                (texture, width, height)
            };
            self.acquired = !texture.is_null();
            if self.acquired {
                self.swapchain = GpuSurface {
                    texture: texture as usize,
                    width,
                    height,
                };
                return Ok(PresentAcquire::DrawableReady);
            }
        }
        Ok(PresentAcquire::HeadlessProceed)
    }

    pub fn raw(&self) -> *mut SDL_GPUCommandBuffer {
        self.command.unwrap_or(std::ptr::null_mut())
    }

    pub fn begin_copy_pass(&mut self, operation: &str) -> Result<*mut SDL_GPUCopyPass, String> {
        // SAFETY: the command buffer is live.
        let pass = unsafe { SDL_BeginGPUCopyPass(self.raw()) };
        created(pass, operation)
    }

    pub fn end_copy_pass(&self, pass: *mut SDL_GPUCopyPass) {
        // SAFETY: `pass` came from `begin_copy_pass`.
        unsafe { SDL_EndGPUCopyPass(pass) };
    }

    pub fn begin_render_pass(
        &mut self,
        target: *mut SDL_GPUTexture,
        load: bool,
    ) -> Result<*mut SDL_GPURenderPass, String> {
        let info = SDL_GPUColorTargetInfo {
            texture: target,
            load_op: if load {
                SDL_GPU_LOADOP_LOAD
            } else {
                SDL_GPU_LOADOP_CLEAR
            },
            store_op: SDL_GPU_STOREOP_STORE,
            clear_color: SDL_FColor {
                r: 0.0,
                g: 0.0,
                b: 0.0,
                a: 1.0,
            },
            cycle: !load,
            ..Default::default()
        };
        // SAFETY: the command buffer and target are live; `info` is fully initialized.
        let pass = unsafe { SDL_BeginGPURenderPass(self.raw(), &info, 1, std::ptr::null()) };
        created(pass, "Begin GPU render pass")
    }

    pub fn end_render_pass(&self, pass: *mut SDL_GPURenderPass) {
        // SAFETY: `pass` came from `begin_render_pass`.
        unsafe { SDL_EndGPURenderPass(pass) };
    }

    /// Full-surface nearest copy; `destination` must be at least as large as `source`.
    pub fn blit_copy(&mut self, source: GpuSurface, destination: GpuSurface) {
        self.blit(
            source.texture,
            0,
            0,
            source.width,
            source.height,
            destination.texture,
            0,
            0,
            source.width,
            source.height,
            SDL_GPU_LOADOP_DONT_CARE,
            SDL_GPU_FILTER_NEAREST,
        );
    }

    /// Blits `surface` (and `left_pane`, when set) onto the acquired swapchain, runs the overlay,
    /// submits, and optionally saves the composed window to `capture_path`. Returns whether a
    /// swapchain texture was submitted. `rotated`: the surfaces are portrait.
    #[allow(clippy::too_many_arguments)]
    pub fn present(
        &mut self,
        ctx: &GpuContext,
        surface: GpuSurface,
        left_pane: GpuSurface,
        scale_mode: VideoScaleMode,
        options: GameVideoOptions,
        linear: bool,
        rotated: bool,
        overlay: Option<super::video::Overlay>,
        capture_path: Option<&str>,
        geometry: &Geometry,
    ) -> Result<bool, String> {
        if !self.swapchain.is_valid() {
            self.submit()?;
            return Ok(false);
        }
        let w = self.swapchain.width;
        let h = self.swapchain.height;
        let mut capture_texture = GpuTexture::empty();
        let mut capture_download = None;
        let mut capture_format = 0u32;
        let mut composition = self.swapchain.texture;
        if capture_path.is_some() {
            let format = ctx
                .swapchain_format()
                .ok_or_else(|| "Window capture requires an 8-bit SDR swapchain".to_owned())?;
            capture_format = match format {
                TextureFormat::Bgra8Unorm => 1,
                TextureFormat::Rgba8Unorm => 2,
                TextureFormat::R32Uint => {
                    return Err("Window capture requires an 8-bit SDR swapchain".to_owned());
                }
            };
            let bytes = u64::from(w) * u64::from(h) * 4;
            if bytes > u64::from(u32::MAX) {
                return Err("Window capture exceeds GPU transfer size".to_owned());
            }
            capture_texture = ctx.create_texture(format, w, h)?;
            composition = capture_texture.get() as usize;
            capture_download = Some(ctx.create_transfer(bytes as u32, TransferUsage::Download)?);
        }
        let panes = if left_pane.is_valid() && w >= 2 { 2 } else { 1 };
        for pane in 0..panes {
            let offset = if panes == 2 && pane == 1 { w / 2 } else { 0 };
            let pane_width = if panes == 1 {
                w
            } else if pane == 0 {
                w / 2
            } else {
                w - w / 2
            };
            let viewport =
                video_blit_for_window(scale_mode, options, pane_width, h, rotated, geometry);
            let source = if panes == 2 && pane == 0 {
                left_pane.texture
            } else {
                surface.texture
            };
            self.blit(
                source,
                viewport.source_x,
                viewport.source_y,
                viewport.source_width,
                viewport.source_height,
                composition,
                offset + viewport.x,
                viewport.y,
                viewport.width,
                viewport.height,
                if pane == 0 {
                    SDL_GPU_LOADOP_CLEAR
                } else {
                    SDL_GPU_LOADOP_LOAD
                },
                if linear && scale_mode != VideoScaleMode::AutoInteger {
                    SDL_GPU_FILTER_LINEAR
                } else {
                    SDL_GPU_FILTER_NEAREST
                },
            );
        }
        if let Some(overlay) = overlay {
            overlay.invoke(self.raw(), composition, w, h);
        }
        if !capture_texture.is_valid() {
            self.submit()?;
            return Ok(true);
        }
        // Download the exact full-window composition submitted to the drawable.
        self.blit(
            composition,
            0,
            0,
            w,
            h,
            self.swapchain.texture,
            0,
            0,
            w,
            h,
            SDL_GPU_LOADOP_DONT_CARE,
            SDL_GPU_FILTER_NEAREST,
        );
        let copy = self.begin_copy_pass("Begin window capture download")?;
        let download = capture_download
            .as_ref()
            .expect("capture download created with the capture texture");
        let from = SDL_GPUTextureRegion {
            texture: composition as *mut SDL_GPUTexture,
            w,
            h,
            d: 1,
            ..Default::default()
        };
        let to = SDL_GPUTextureTransferInfo {
            transfer_buffer: download.get(),
            pixels_per_row: w,
            rows_per_layer: h,
            ..Default::default()
        };
        // SAFETY: the copy pass is live and both regions describe live resources.
        unsafe { SDL_DownloadFromGPUTexture(copy, &from, &to) };
        self.end_copy_pass(copy);
        self.submit_and_wait()?;
        let mut mapping = download.map(false, "Map window capture")?;
        let pixels = mapping.words(usize::try_from(u64::from(w) * u64::from(h)).unwrap());
        let path = capture_path.expect("capture path checked above");
        save_surface(
            path,
            pixels,
            w,
            h,
            capture_format,
            true,
            "Create window capture image",
            "Save window capture PNG",
        )?;
        Ok(true)
    }

    #[allow(clippy::too_many_arguments)]
    fn blit(
        &mut self,
        source: usize,
        source_x: u32,
        source_y: u32,
        width: u32,
        height: u32,
        destination: usize,
        x: u32,
        y: u32,
        destination_width: u32,
        destination_height: u32,
        load_op: SDL_GPULoadOp,
        filter: SDL_GPUFilter,
    ) {
        let info = SDL_GPUBlitInfo {
            source: SDL_GPUBlitRegion {
                texture: source as *mut SDL_GPUTexture,
                x: source_x,
                y: source_y,
                w: width,
                h: height,
                ..Default::default()
            },
            destination: SDL_GPUBlitRegion {
                texture: destination as *mut SDL_GPUTexture,
                x,
                y,
                w: destination_width,
                h: destination_height,
                ..Default::default()
            },
            load_op,
            clear_color: SDL_FColor {
                r: 0.0,
                g: 0.0,
                b: 0.0,
                a: 1.0,
            },
            filter,
            ..Default::default()
        };
        // SAFETY: the command buffer is live and both regions describe live textures.
        unsafe { SDL_BlitGPUTexture(self.raw(), &info) };
    }

    pub fn upload_buffer(
        &self,
        pass: *mut SDL_GPUCopyPass,
        source: *mut SDL_GPUTransferBuffer,
        source_offset: u32,
        destination: *mut SDL_GPUBuffer,
        size: u32,
        cycle: bool,
    ) {
        let from = SDL_GPUTransferBufferLocation {
            transfer_buffer: source,
            offset: source_offset,
        };
        let to = SDL_GPUBufferRegion {
            buffer: destination,
            offset: 0,
            size,
        };
        // SAFETY: the pass and both resources are live.
        unsafe { SDL_UploadToGPUBuffer(pass, &from, &to, cycle) };
    }

    #[allow(clippy::too_many_arguments)]
    pub fn upload_texture(
        &self,
        pass: *mut SDL_GPUCopyPass,
        source: *mut SDL_GPUTransferBuffer,
        source_offset: u32,
        width: u32,
        height: u32,
        destination: *mut SDL_GPUTexture,
        cycle: bool,
    ) {
        let from = SDL_GPUTextureTransferInfo {
            transfer_buffer: source,
            offset: source_offset,
            pixels_per_row: width,
            rows_per_layer: height,
        };
        let to = SDL_GPUTextureRegion {
            texture: destination,
            w: width,
            h: height,
            d: 1,
            ..Default::default()
        };
        // SAFETY: the pass and both resources are live.
        unsafe { SDL_UploadToGPUTexture(pass, &from, &to, cycle) };
    }

    pub fn download_texture(
        &self,
        pass: *mut SDL_GPUCopyPass,
        source: *mut SDL_GPUTexture,
        width: u32,
        height: u32,
        destination: *mut SDL_GPUTransferBuffer,
    ) {
        let from = SDL_GPUTextureRegion {
            texture: source,
            w: width,
            h: height,
            d: 1,
            ..Default::default()
        };
        let to = SDL_GPUTextureTransferInfo {
            transfer_buffer: destination,
            pixels_per_row: width,
            rows_per_layer: height,
            ..Default::default()
        };
        // SAFETY: the pass and both resources are live.
        unsafe { SDL_DownloadFromGPUTexture(pass, &from, &to) };
    }

    pub fn push_vertex_uniforms(&self, slot: u32, data: &[u8]) {
        // SAFETY: the command buffer is live and `data` outlives the call.
        unsafe {
            SDL_PushGPUVertexUniformData(self.raw(), slot, data.as_ptr().cast(), data.len() as u32)
        };
    }

    pub fn push_fragment_uniforms(&self, slot: u32, data: &[u8]) {
        // SAFETY: the command buffer is live and `data` outlives the call.
        unsafe {
            SDL_PushGPUFragmentUniformData(
                self.raw(),
                slot,
                data.as_ptr().cast(),
                data.len() as u32,
            )
        };
    }

    pub fn bind_pipeline(
        &self,
        pass: *mut SDL_GPURenderPass,
        pipeline: *mut SDL_GPUGraphicsPipeline,
    ) {
        // SAFETY: the pass is live and the pipeline belongs to the same device.
        unsafe { SDL_BindGPUGraphicsPipeline(pass, pipeline) };
    }

    pub fn bind_vertex_storage_buffers(
        &self,
        pass: *mut SDL_GPURenderPass,
        first_slot: u32,
        buffers: &[*mut SDL_GPUBuffer],
    ) {
        // SAFETY: the pass is live and the buffers belong to the same device.
        unsafe {
            SDL_BindGPUVertexStorageBuffers(
                pass,
                first_slot,
                buffers.as_ptr(),
                buffers.len() as u32,
            )
        };
    }

    pub fn bind_fragment_storage_buffers(
        &self,
        pass: *mut SDL_GPURenderPass,
        first_slot: u32,
        buffers: &[*mut SDL_GPUBuffer],
    ) {
        // SAFETY: the pass is live and the buffers belong to the same device.
        unsafe {
            SDL_BindGPUFragmentStorageBuffers(
                pass,
                first_slot,
                buffers.as_ptr(),
                buffers.len() as u32,
            )
        };
    }

    pub fn bind_fragment_samplers(
        &self,
        pass: *mut SDL_GPURenderPass,
        first_slot: u32,
        bindings: &[SDL_GPUTextureSamplerBinding],
    ) {
        // SAFETY: the pass is live and the bindings describe live resources.
        unsafe {
            SDL_BindGPUFragmentSamplers(pass, first_slot, bindings.as_ptr(), bindings.len() as u32)
        };
    }

    pub fn set_scissor(
        &self,
        pass: *mut SDL_GPURenderPass,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
    ) {
        let scissor = SDL_Rect {
            x,
            y,
            w: width,
            h: height,
        };
        // SAFETY: the pass is live.
        unsafe { SDL_SetGPUScissor(pass, &scissor) };
    }

    pub fn draw(&self, pass: *mut SDL_GPURenderPass, vertices: u32, instances: u32) {
        // SAFETY: the pass is live and a pipeline was bound.
        unsafe { SDL_DrawGPUPrimitives(pass, vertices, instances, 0, 0) };
    }

    pub fn submit(&mut self) -> Result<(), String> {
        let Some(command) = self.command.take() else {
            return Ok(());
        };
        // SAFETY: `command` was acquired from this device and is consumed here.
        if unsafe { SDL_SubmitGPUCommandBuffer(command) } {
            Ok(())
        } else {
            Err(failure("Submit GPU frame"))
        }
    }

    /// Submits and blocks until the GPU finished this command.
    pub fn submit_and_wait(&mut self) -> Result<(), String> {
        let Some(command) = self.command.take() else {
            return Ok(());
        };
        // SAFETY: `command` was acquired from this device and is consumed here.
        let fence = unsafe { SDL_SubmitGPUCommandBufferAndAcquireFence(command) };
        if fence.is_null() {
            return Err(failure("Submit GPU command buffer"));
        }
        // SAFETY: the fence belongs to the device.
        let signaled = unsafe { SDL_WaitForGPUFences(self.device, true, &fence, 1) };
        // SAFETY: the fence was acquired above.
        unsafe { SDL_ReleaseGPUFence(self.device, fence) };
        if signaled {
            Ok(())
        } else {
            Err(failure("Wait for GPU fence"))
        }
    }
}

impl Drop for GpuCommand {
    fn drop(&mut self) {
        if let Some(command) = self.command.take() {
            if self.acquired {
                // SAFETY: a swapchain texture was acquired; SDL requires submission.
                let _ = unsafe { SDL_SubmitGPUCommandBuffer(command) };
            } else {
                // SAFETY: no swapchain texture was acquired.
                let _ = unsafe { SDL_CancelGPUCommandBuffer(command) };
            }
        }
    }
}

/// Reusable download staging for readbacks.
#[derive(Default)]
pub struct ReadbackBuffer {
    download: Option<GpuTransfer>,
}

impl ReadbackBuffer {
    pub fn reset(&mut self) {
        self.download = None;
    }

    /// Consumes `cmd`, copies `surface` into `output` (ARGB8888, width*height pixels) and waits.
    pub fn finish(
        &mut self,
        cmd: &mut GpuCommand,
        surface: GpuSurface,
        ctx: &GpuContext,
        output: &mut [u32],
    ) -> Result<(), String> {
        let pixels = usize::try_from(u64::from(surface.width) * u64::from(surface.height))
            .map_err(|_| "Incomplete GPU output buffer".to_owned())?;
        if output.len() < pixels {
            return Err("Incomplete GPU output buffer".to_owned());
        }
        let bytes =
            u32::try_from(pixels * 4).map_err(|_| "Incomplete GPU output buffer".to_owned())?;
        if self.download.as_ref().map(GpuTransfer::size) != Some(bytes) {
            self.download = Some(ctx.create_transfer(bytes, TransferUsage::Download)?);
        }
        let copy = cmd.begin_copy_pass("Begin GPU readback copy")?;
        let download = self
            .download
            .as_ref()
            .expect("download transfer created above");
        cmd.download_texture(
            copy,
            surface.texture as *mut SDL_GPUTexture,
            surface.width,
            surface.height,
            download.get(),
        );
        cmd.end_copy_pass(copy);
        cmd.submit_and_wait()?;
        let mut mapping = download.map(false, "Map GPU readback")?;
        let source = mapping.words(pixels);
        output[..pixels].copy_from_slice(source);
        Ok(())
    }
}

/// `cpy` helper: reinterpret a `#[repr(C)]` uniform block as bytes.
pub fn uniform_bytes<T>(value: &T) -> &[u8] {
    // SAFETY: `T` is a plain `#[repr(C)]` POD.
    unsafe {
        std::slice::from_raw_parts((value as *const T).cast::<u8>(), std::mem::size_of::<T>())
    }
}

/// `SDL_GPUTextureSamplerBinding` built from renderer handles.
pub fn sampler_binding(
    texture: usize,
    sampler: *mut SDL_GPUSampler,
) -> SDL_GPUTextureSamplerBinding {
    SDL_GPUTextureSamplerBinding {
        texture: texture as *mut SDL_GPUTexture,
        sampler,
    }
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn save_surface(
    path: &str,
    pixels: &[u32],
    width: u32,
    height: u32,
    pixel_format: u32,
    png: bool,
    create_operation: &str,
    save_operation: &str,
) -> Result<(), String> {
    use sdl3_sys::pixels::{SDL_PIXELFORMAT_ABGR8888, SDL_PIXELFORMAT_ARGB8888};
    use sdl3_sys::surface::{SDL_CreateSurfaceFrom, SDL_DestroySurface, SDL_SaveBMP, SDL_SavePNG};
    let format = if pixel_format == 2 {
        SDL_PIXELFORMAT_ABGR8888
    } else {
        SDL_PIXELFORMAT_ARGB8888
    };
    // SAFETY: `pixels` is a live ARGB/ABGR image of width*height texels.
    let image = unsafe {
        SDL_CreateSurfaceFrom(
            width as i32,
            height as i32,
            format,
            pixels.as_ptr().cast_mut().cast(),
            (width * 4) as i32,
        )
    };
    if image.is_null() {
        return Err(failure(create_operation));
    }
    let path = c_path(path, save_operation)?;
    // SAFETY: the surface is live and the path is NUL-terminated.
    let saved = unsafe {
        if png {
            SDL_SavePNG(image, path.as_ptr())
        } else {
            SDL_SaveBMP(image, path.as_ptr())
        }
    };
    // SAFETY: the surface was created above.
    unsafe { SDL_DestroySurface(image) };
    if saved {
        Ok(())
    } else {
        Err(failure(save_operation))
    }
}
