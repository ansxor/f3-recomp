//! SDL GPU resource ownership and command submission.

use super::api::{GpuSurface, PresentAcquire, PresentRequest};
use super::pacing::DisplayPacer;
use super::shaders::Shader;
use super::user_spirv::LoadedUserShader;
use crate::config::{GameVideoOptions, Geometry, VideoScaleMode};
use crate::scale::video_blit_for_window;
use sdl3_sys::error::SDL_GetError;
use sdl3_sys::gpu::*;
use sdl3_sys::pixels::SDL_FColor;
use sdl3_sys::rect::SDL_Rect;
use sdl3_sys::video::{SDL_GetCurrentDisplayMode, SDL_GetDisplayForWindow, SDL_Window};
use std::ffi::{CStr, c_void};
use std::marker::PhantomData;
use std::mem::MaybeUninit;
use std::ptr::NonNull;
use std::rc::Rc;

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
    let message = SDL_GetError();
    if message.is_null() {
        String::new()
    } else {
        // SAFETY: SDL returned a non-null NUL-terminated thread-local error string.
        unsafe { CStr::from_ptr(message) }
            .to_string_lossy()
            .into_owned()
    }
}

fn failure(operation: &str) -> String {
    format!("{operation}: {}", detail())
}

fn created<T>(handle: *mut T, operation: &str) -> Result<NonNull<T>, String> {
    NonNull::new(handle).ok_or_else(|| failure(operation))
}

struct Device(NonNull<SDL_GPUDevice>);

impl Device {
    fn raw(&self) -> *mut SDL_GPUDevice {
        self.0.as_ptr()
    }
}

impl Drop for Device {
    fn drop(&mut self) {
        // SAFETY: the last owner has released its resources.
        unsafe { SDL_DestroyGPUDevice(self.raw()) };
    }
}

pub struct GpuBuffer {
    device: Rc<Device>,
    handle: NonNull<SDL_GPUBuffer>,
    size: u32,
}

impl Drop for GpuBuffer {
    fn drop(&mut self) {
        // SAFETY: the buffer is live; it shares ownership of the device.
        unsafe { SDL_ReleaseGPUBuffer(self.device.raw(), self.handle.as_ptr()) };
    }
}

pub struct GpuPipeline {
    device: Rc<Device>,
    handle: NonNull<SDL_GPUGraphicsPipeline>,
}

impl Drop for GpuPipeline {
    fn drop(&mut self) {
        // SAFETY: the pipeline is live; it shares ownership of the device.
        unsafe { SDL_ReleaseGPUGraphicsPipeline(self.device.raw(), self.handle.as_ptr()) };
    }
}

pub struct GpuSampler {
    device: Rc<Device>,
    handle: NonNull<SDL_GPUSampler>,
}

impl Drop for GpuSampler {
    fn drop(&mut self) {
        // SAFETY: the sampler is live; it shares ownership of the device.
        unsafe { SDL_ReleaseGPUSampler(self.device.raw(), self.handle.as_ptr()) };
    }
}

#[derive(Clone, Copy)]
struct TextureHandle {
    device: NonNull<SDL_GPUDevice>,
    handle: NonNull<SDL_GPUTexture>,
    width: u32,
    height: u32,
}

impl TextureHandle {
    fn as_ref(&self) -> TextureRef<'_> {
        TextureRef {
            device: self.device,
            handle: self.handle,
            width: self.width,
            height: self.height,
            owner: PhantomData,
        }
    }
}

#[derive(Clone, Copy)]
pub struct TextureRef<'a> {
    device: NonNull<SDL_GPUDevice>,
    handle: NonNull<SDL_GPUTexture>,
    width: u32,
    height: u32,
    owner: PhantomData<&'a TextureHandle>,
}

impl<'a> TextureRef<'a> {
    /// # Safety
    /// A nonempty host token must identify a live texture on this device for the borrow.
    pub(super) unsafe fn from_host(surface: &'a GpuSurface, ctx: &GpuContext) -> Option<Self> {
        Some(Self {
            device: ctx.device.0,
            handle: NonNull::new(surface.texture as *mut SDL_GPUTexture)?,
            width: surface.width,
            height: surface.height,
            owner: PhantomData,
        })
    }

    pub fn width(self) -> u32 {
        self.width
    }

    pub fn height(self) -> u32 {
        self.height
    }

    fn region(self) -> SDL_GPUTextureRegion {
        SDL_GPUTextureRegion {
            texture: self.handle.as_ptr(),
            w: self.width,
            h: self.height,
            d: 1,
            ..Default::default()
        }
    }

    fn bytes(self) -> u64 {
        u64::from(self.width) * u64::from(self.height) * 4
    }
}

pub struct SamplerBinding<'a> {
    pub texture: TextureRef<'a>,
    pub sampler: &'a GpuSampler,
}

struct OwnedTexture {
    device: Rc<Device>,
    texture: TextureHandle,
    format: TextureFormat,
}

impl Drop for OwnedTexture {
    fn drop(&mut self) {
        // SAFETY: this texture is live and retains the device that created it.
        unsafe { SDL_ReleaseGPUTexture(self.device.raw(), self.texture.handle.as_ptr()) };
    }
}

pub struct GpuTexture(Option<OwnedTexture>);

impl GpuTexture {
    pub const fn empty() -> Self {
        Self(None)
    }

    pub(super) fn as_ref(&self) -> TextureRef<'_> {
        self.0
            .as_ref()
            .expect("allocated GPU texture")
            .texture
            .as_ref()
    }

    pub fn texture_handle(&self) -> usize {
        self.0
            .as_ref()
            .map_or(0, |owned| owned.texture.handle.as_ptr() as usize)
    }

    pub(super) fn is_valid(&self) -> bool {
        self.0.is_some()
    }

    pub fn width(&self) -> u32 {
        self.0.as_ref().map_or(0, |owned| owned.texture.width)
    }

    pub fn height(&self) -> u32 {
        self.0.as_ref().map_or(0, |owned| owned.texture.height)
    }

    pub(super) fn ensure(
        &mut self,
        ctx: &GpuContext,
        format: TextureFormat,
        width: u32,
        height: u32,
    ) -> Result<(), String> {
        if let Some(owned) = &self.0 {
            if !Rc::ptr_eq(&owned.device, &ctx.device) {
                return Err("Capture texture belongs to a different GPU device".to_owned());
            }
            if owned.format == format
                && width == owned.texture.width
                && height == owned.texture.height
            {
                return Ok(());
            }
        }
        *self = ctx.create_texture(format, width, height)?;
        Ok(())
    }
}

/// A mapped transfer buffer; unmaps on drop.
pub struct MappedTransfer<'a> {
    transfer: &'a mut GpuTransfer,
    data: NonNull<c_void>,
}

impl MappedTransfer<'_> {
    pub fn words(&mut self, count: usize) -> &mut [MaybeUninit<u32>] {
        assert!(count <= self.transfer.size as usize / size_of::<u32>());
        // SAFETY: SDL supplies aligned, mapped storage; the transfer is exclusively borrowed.
        unsafe { std::slice::from_raw_parts_mut(self.data.cast().as_ptr(), count) }
    }
}

impl Drop for MappedTransfer<'_> {
    fn drop(&mut self) {
        // SAFETY: this object owns the live mapping, and the transfer outlives it.
        unsafe {
            SDL_UnmapGPUTransferBuffer(self.transfer.device.raw(), self.transfer.handle.as_ptr())
        };
    }
}

pub struct GpuTransfer {
    device: Rc<Device>,
    handle: NonNull<SDL_GPUTransferBuffer>,
    size: u32,
}

impl GpuTransfer {
    pub fn size(&self) -> u32 {
        self.size
    }

    /// `cycle` lets SDL hand out fresh storage when the previous contents are in flight.
    pub fn map(&mut self, cycle: bool, operation: &str) -> Result<MappedTransfer<'_>, String> {
        // SAFETY: the transfer is live and its exclusive borrow prevents a second mapping.
        let data =
            unsafe { SDL_MapGPUTransferBuffer(self.device.raw(), self.handle.as_ptr(), cycle) };
        let data = NonNull::new(data).ok_or_else(|| failure(operation))?;
        Ok(MappedTransfer {
            transfer: self,
            data,
        })
    }
}

impl Drop for GpuTransfer {
    fn drop(&mut self) {
        // SAFETY: the device owns the transfer buffer and outlives it.
        unsafe { SDL_ReleaseGPUTransferBuffer(self.device.raw(), self.handle.as_ptr()) };
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
    device: Rc<Device>,
    window: *mut SDL_Window,
    claimed: bool,
    vsync: bool,
    sampler: Option<GpuSampler>,
}

impl GpuContext {
    /// # Safety
    /// SDL video must be initialized. A nonzero window must remain live until drop.
    pub(super) unsafe fn new(window: usize, vsync: bool) -> Result<Self, String> {
        let window = window as *mut SDL_Window;
        let device = created(
            // SAFETY: SDL video is initialized by contract.
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
        let device = Rc::new(Device(device));
        let mut claimed = false;
        let mut sampler = None;
        let initialized = (|| -> Result<(), String> {
            if !window.is_null() {
                // SAFETY: `device` and `window` are live.
                if !unsafe { SDL_ClaimWindowForGPUDevice(device.raw(), window) } {
                    return Err(failure("Claim GPU window"));
                }
                claimed = true;
                set_swapchain_mode(device.raw(), window, vsync)?;
            }
            let handle = created(create_sampler(device.raw()), "Create GPU sampler")?;
            sampler = Some(GpuSampler {
                device: Rc::clone(&device),
                handle,
            });
            Ok(())
        })();
        if let Err(error) = initialized {
            drop(sampler.take());
            if claimed {
                // SAFETY: `device` and `window` are live.
                unsafe { SDL_ReleaseWindowFromGPUDevice(device.raw(), window) };
            }
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

    pub fn begin_command(&self) -> Result<CommandBuffer<'_>, String> {
        // SAFETY: the context keeps the device live for the command's lifetime.
        let command = created(
            unsafe { SDL_AcquireGPUCommandBuffer(self.device.raw()) },
            "Acquire GPU command buffer",
        )?;
        Ok(CommandBuffer {
            ctx: self,
            command: Some(command),
            swapchain: None,
            active: None,
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
            unsafe { SDL_CreateGPUBuffer(self.device.raw(), &info) },
            "Create GPU storage buffer",
        )?;
        Ok(GpuBuffer {
            device: Rc::clone(&self.device),
            handle,
            size: bytes,
        })
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
            unsafe { SDL_CreateGPUTexture(self.device.raw(), &info) },
            "Create GPU render texture",
        )?;
        Ok(GpuTexture(Some(OwnedTexture {
            device: Rc::clone(&self.device),
            texture: TextureHandle {
                device: self.device.0,
                handle,
                width,
                height,
            },
            format,
        })))
    }

    pub fn create_pipeline(
        &self,
        vertex: ShaderKind,
        fragment: ShaderKind,
        format: TextureFormat,
    ) -> Result<GpuPipeline, String> {
        let vs = create_shader(
            self.device.raw(),
            shader(vertex),
            SDL_GPU_SHADERSTAGE_VERTEX,
        );
        if vs.is_null() {
            return Err(failure("Create GPU graphics pipeline"));
        }
        let fs = create_shader(
            self.device.raw(),
            shader(fragment),
            SDL_GPU_SHADERSTAGE_FRAGMENT,
        );
        let mut pipeline = std::ptr::null_mut();
        if !fs.is_null() {
            pipeline = build_pipeline(self.device.raw(), vs, fs, format.sdl());
        }
        if !fs.is_null() {
            // SAFETY: the fragment shader is live and the pipeline holds its own reference.
            unsafe { SDL_ReleaseGPUShader(self.device.raw(), fs) };
        }
        // SAFETY: the vertex shader is live and the pipeline holds its own reference.
        unsafe { SDL_ReleaseGPUShader(self.device.raw(), vs) };
        let handle = created(pipeline, "Create GPU graphics pipeline")?;
        Ok(GpuPipeline {
            device: Rc::clone(&self.device),
            handle,
        })
    }

    pub fn create_user_pipeline(
        &self,
        source: &LoadedUserShader,
        format: TextureFormat,
    ) -> Result<GpuPipeline, String> {
        let (code, code_size, entrypoint, shader_format) = match source {
            LoadedUserShader::Metal(text) => (
                text.as_ptr(),
                text.len() - 1,
                c"f3_postprocess",
                SDL_GPU_SHADERFORMAT_MSL,
            ),
            LoadedUserShader::Spirv(words) => (
                words.as_ptr().cast(),
                std::mem::size_of_val(words.as_slice()),
                c"main",
                SDL_GPU_SHADERFORMAT_SPIRV,
            ),
        };
        let info = SDL_GPUShaderCreateInfo {
            code_size,
            code,
            entrypoint: entrypoint.as_ptr(),
            format: shader_format,
            stage: SDL_GPU_SHADERSTAGE_FRAGMENT,
            num_samplers: 1,
            num_uniform_buffers: 1,
            ..Default::default()
        };
        let vs = create_shader(
            self.device.raw(),
            shader(ShaderKind::FullscreenVert),
            SDL_GPU_SHADERSTAGE_VERTEX,
        );
        if vs.is_null() {
            return Err(failure("Create GPU graphics pipeline"));
        }
        // SAFETY: `info` borrows `code` for the call only.
        let fs = unsafe { SDL_CreateGPUShader(self.device.raw(), &info) };
        let mut pipeline = std::ptr::null_mut();
        if !fs.is_null() {
            pipeline = build_pipeline(self.device.raw(), vs, fs, format.sdl());
            // SAFETY: the shader was created above; the pipeline retained its state.
            unsafe { SDL_ReleaseGPUShader(self.device.raw(), fs) };
        }
        // SAFETY: the vertex shader was created above.
        unsafe { SDL_ReleaseGPUShader(self.device.raw(), vs) };
        let handle = created(pipeline, "Create GPU graphics pipeline")?;
        Ok(GpuPipeline {
            device: Rc::clone(&self.device),
            handle,
        })
    }

    pub fn create_transfer(&self, bytes: u32, usage: TransferUsage) -> Result<GpuTransfer, String> {
        let info = SDL_GPUTransferBufferCreateInfo {
            usage: usage.sdl(),
            size: bytes,
            ..Default::default()
        };
        // SAFETY: `info` is fully initialized and the device is live.
        let handle = created(
            unsafe { SDL_CreateGPUTransferBuffer(self.device.raw(), &info) },
            "Create GPU transfer buffer",
        )?;
        Ok(GpuTransfer {
            device: Rc::clone(&self.device),
            handle,
            size: bytes,
        })
    }

    pub fn wait_idle(&self) {
        // SAFETY: the device is live.
        unsafe { SDL_WaitForGPUIdle(self.device.raw()) };
    }

    pub fn is_metal(&self) -> bool {
        is_metal_device(self.device.raw())
    }

    pub fn driver_name(&self) -> String {
        // SAFETY: the device is live and the name is a static string owned by SDL.
        let name = unsafe { SDL_GetGPUDeviceDriver(self.device.raw()) };
        if name.is_null() {
            String::new()
        } else {
            // SAFETY: SDL's non-null driver name is NUL-terminated and lives with the device.
            unsafe { CStr::from_ptr(name) }
                .to_string_lossy()
                .into_owned()
        }
    }

    pub fn device_handle(&self) -> usize {
        self.device.raw() as usize
    }

    #[cfg(target_os = "macos")]
    pub(crate) fn window_raw(&self) -> *mut SDL_Window {
        self.window
    }

    pub fn has_window(&self) -> bool {
        self.claimed
    }

    pub fn set_allowed_frames_in_flight(&self, frames: u32) -> Result<(), String> {
        // SAFETY: this context owns the live device.
        if unsafe { SDL_SetGPUAllowedFramesInFlight(self.device.raw(), frames) } {
            Ok(())
        } else {
            Err(failure("Set temporal GPU queue depth"))
        }
    }

    pub fn display_hz(&self) -> f64 {
        if !self.has_window() {
            return 0.0;
        }
        // SAFETY: the host keeps the window live; SDL owns the mode through this lookup.
        unsafe {
            let display = SDL_GetDisplayForWindow(self.window);
            SDL_GetCurrentDisplayMode(display)
                .as_ref()
                .map_or(0.0, |mode| f64::from(mode.refresh_rate))
        }
    }

    pub fn sampler(&self) -> &GpuSampler {
        self.sampler
            .as_ref()
            .expect("context initialized its sampler")
    }

    pub fn vsync(&self) -> bool {
        self.vsync
    }

    pub fn swapchain_format(&self) -> Option<TextureFormat> {
        if self.window.is_null() {
            return None;
        }
        // SAFETY: the window is claimed and the device is live.
        let format = unsafe { SDL_GetGPUSwapchainTextureFormat(self.device.raw(), self.window) };
        TextureFormat::from_sdl(format)
    }
}

impl Drop for GpuContext {
    fn drop(&mut self) {
        self.wait_idle();
        drop(self.sampler.take());
        if self.claimed {
            // SAFETY: the device is live and the window was claimed by it.
            unsafe { SDL_ReleaseWindowFromGPUDevice(self.device.raw(), self.window) };
        }
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

type OverlayFn =
    unsafe extern "C" fn(*mut c_void, *mut SDL_GPUCommandBuffer, *mut SDL_GPUTexture, u32, u32);

#[derive(Clone, Copy)]
pub(super) struct Overlay {
    callback: OverlayFn,
    userdata: *mut c_void,
}

impl Overlay {
    /// # Safety
    /// A nonzero callback must have the host overlay ABI and remain valid with its userdata.
    pub(super) unsafe fn from_host(callback: usize, userdata: usize) -> Option<Self> {
        (callback != 0).then(|| Self {
            // SAFETY: the host guarantees the address and signature.
            callback: unsafe { std::mem::transmute::<usize, OverlayFn>(callback) },
            userdata: userdata as *mut c_void,
        })
    }

    fn invoke(self, cmd: &mut CommandBuffer<'_>, target: TextureRef<'_>) {
        assert_eq!(cmd.ctx.device.0, target.device);
        // SAFETY: the callback contract covers userdata and the command and target are live.
        unsafe {
            (self.callback)(
                self.userdata,
                cmd.idle_raw(),
                target.handle.as_ptr(),
                target.width,
                target.height,
            )
        };
    }
}

pub(super) struct PresentLayout<'a> {
    pub geometry: &'a Geometry,
    pub options: GameVideoOptions,
    pub scale_mode: VideoScaleMode,
    pub linear: bool,
    pub rotated: bool,
}

struct BlitRegion<'a> {
    texture: TextureRef<'a>,
    x: u32,
    y: u32,
    width: u32,
    height: u32,
}

impl BlitRegion<'_> {
    fn sdl(&self) -> SDL_GPUBlitRegion {
        SDL_GPUBlitRegion {
            texture: self.texture.handle.as_ptr(),
            x: self.x,
            y: self.y,
            w: self.width,
            h: self.height,
            ..Default::default()
        }
    }
}

const OPAQUE_BLACK: SDL_FColor = SDL_FColor {
    r: 0.0,
    g: 0.0,
    b: 0.0,
    a: 1.0,
};

enum ActivePass {
    Copy(NonNull<SDL_GPUCopyPass>),
    Render(NonNull<SDL_GPURenderPass>),
}

fn end_pass(active: &mut Option<ActivePass>) {
    // SAFETY: taking the active handle ensures each pass is ended just once.
    unsafe {
        match active.take() {
            Some(ActivePass::Copy(pass)) => SDL_EndGPUCopyPass(pass.as_ptr()),
            Some(ActivePass::Render(pass)) => SDL_EndGPURenderPass(pass.as_ptr()),
            None => {}
        }
    }
}

pub struct CommandBuffer<'a> {
    ctx: &'a GpuContext,
    command: Option<NonNull<SDL_GPUCommandBuffer>>,
    swapchain: Option<TextureHandle>,
    active: Option<ActivePass>,
}

impl CommandBuffer<'_> {
    pub fn acquire_for_present(
        &mut self,
        pacer: &mut DisplayPacer,
        request: &PresentRequest<'_>,
    ) -> Result<PresentAcquire, String> {
        let paced = pacer.consume_prepared();
        let timed = request.timing.is_live();
        if request.wait && timed && !paced {
            pacer.wait_tick(self.ctx.vsync());
        }
        let idle = if request.wait {
            PresentAcquire::HeadlessProceed
        } else {
            PresentAcquire::SkipFrame
        };
        if !self.ctx.has_window() {
            return Ok(idle);
        }
        assert!(self.swapchain.is_none());
        let acquire = if request.wait {
            SDL_WaitAndAcquireGPUSwapchainTexture
        } else {
            SDL_AcquireGPUSwapchainTexture
        };
        let mut texture = std::ptr::null_mut();
        let mut width = 0;
        let mut height = 0;
        // SAFETY: the window is claimed by this command's device; the output pointers are writable.
        let ok = unsafe {
            acquire(
                self.idle_raw(),
                self.ctx.window,
                &mut texture,
                &mut width,
                &mut height,
            )
        };
        if !ok {
            return Err(failure(match (request.wait, timed) {
                (false, _) => "Try acquiring GPU swapchain",
                (true, true) => "Acquire timed GPU swapchain",
                (true, false) => "Acquire GPU swapchain",
            }));
        }
        let Some(handle) = NonNull::new(texture) else {
            return Ok(idle);
        };
        self.swapchain = Some(TextureHandle {
            device: self.ctx.device.0,
            handle,
            width,
            height,
        });
        Ok(PresentAcquire::DrawableReady)
    }

    fn idle_raw(&self) -> *mut SDL_GPUCommandBuffer {
        assert!(self.active.is_none(), "GPU pass still active");
        self.command.expect("unsubmitted GPU command").as_ptr()
    }

    pub fn begin_copy_pass(&mut self, operation: &str) -> Result<CopyPass<'_>, String> {
        // SAFETY: the command is live and has no active pass.
        let handle = created(unsafe { SDL_BeginGPUCopyPass(self.idle_raw()) }, operation)?;
        self.active = Some(ActivePass::Copy(handle));
        Ok(CopyPass {
            handle,
            active: &mut self.active,
            device: self.ctx.device.0,
        })
    }

    pub fn begin_render_pass<'pass>(
        &'pass mut self,
        target: TextureRef<'pass>,
        load: bool,
    ) -> Result<RenderPass<'pass>, String> {
        assert_eq!(self.ctx.device.0, target.device);
        let info = SDL_GPUColorTargetInfo {
            texture: target.handle.as_ptr(),
            load_op: if load {
                SDL_GPU_LOADOP_LOAD
            } else {
                SDL_GPU_LOADOP_CLEAR
            },
            store_op: SDL_GPU_STOREOP_STORE,
            clear_color: OPAQUE_BLACK,
            cycle: !load,
            ..Default::default()
        };
        // SAFETY: the command has no active pass and the target belongs to its device.
        let handle = created(
            unsafe { SDL_BeginGPURenderPass(self.idle_raw(), &info, 1, std::ptr::null()) },
            "Begin GPU render pass",
        )?;
        self.active = Some(ActivePass::Render(handle));
        Ok(RenderPass {
            handle,
            active: &mut self.active,
            device: self.ctx.device.0,
        })
    }

    pub fn blit_copy(&mut self, source: TextureRef<'_>, destination: TextureRef<'_>) {
        assert!(destination.width >= source.width && destination.height >= source.height);
        self.blit(
            BlitRegion {
                texture: source,
                x: 0,
                y: 0,
                width: source.width,
                height: source.height,
            },
            BlitRegion {
                texture: destination,
                x: 0,
                y: 0,
                width: source.width,
                height: source.height,
            },
            SDL_GPU_LOADOP_DONT_CARE,
            SDL_GPU_FILTER_NEAREST,
        );
    }

    pub(super) fn present(
        mut self,
        surface: TextureRef<'_>,
        left_pane: Option<TextureRef<'_>>,
        layout: PresentLayout<'_>,
        overlay: Option<Overlay>,
        capture_path: Option<&str>,
    ) -> Result<bool, String> {
        let Some(swapchain) = self.swapchain else {
            self.submit()?;
            return Ok(false);
        };
        let drawable = swapchain.as_ref();
        let capture = capture_path
            .map(|_| WindowCapture::new(self.ctx, drawable))
            .transpose()?;
        let composition = capture
            .as_ref()
            .map_or(drawable, |capture| capture.texture.as_ref());
        self.compose_window(surface, left_pane, composition, layout);
        if let Some(overlay) = overlay {
            overlay.invoke(&mut self, composition);
        }
        if let Some((path, mut capture)) = capture_path.zip(capture) {
            self.blit_copy(capture.texture.as_ref(), drawable);
            capture.save(self, path)?;
        } else {
            self.submit()?;
        }
        Ok(true)
    }

    fn compose_window(
        &mut self,
        surface: TextureRef<'_>,
        left_pane: Option<TextureRef<'_>>,
        target: TextureRef<'_>,
        layout: PresentLayout<'_>,
    ) {
        let w = target.width;
        let h = target.height;
        let left_pane = left_pane.filter(|_| w >= 2);
        let panes = if let Some(left) = left_pane {
            [(left, 0, w / 2), (surface, w / 2, w - w / 2)]
        } else {
            [(surface, 0, w), (surface, 0, 0)]
        };
        let filter = if layout.linear && layout.scale_mode != VideoScaleMode::AutoInteger {
            SDL_GPU_FILTER_LINEAR
        } else {
            SDL_GPU_FILTER_NEAREST
        };
        for (source, offset, width) in panes.into_iter().filter(|&(_, _, width)| width != 0) {
            let viewport = video_blit_for_window(
                layout.scale_mode,
                layout.options,
                width,
                h,
                layout.rotated,
                layout.geometry,
            );
            self.blit(
                BlitRegion {
                    texture: source,
                    x: viewport.source_x,
                    y: viewport.source_y,
                    width: viewport.source_width,
                    height: viewport.source_height,
                },
                BlitRegion {
                    texture: target,
                    x: offset + viewport.x,
                    y: viewport.y,
                    width: viewport.width,
                    height: viewport.height,
                },
                if offset == 0 {
                    SDL_GPU_LOADOP_CLEAR
                } else {
                    SDL_GPU_LOADOP_LOAD
                },
                filter,
            );
        }
    }

    fn blit(
        &mut self,
        source: BlitRegion<'_>,
        destination: BlitRegion<'_>,
        load_op: SDL_GPULoadOp,
        filter: SDL_GPUFilter,
    ) {
        assert_eq!(self.ctx.device.0, source.texture.device);
        assert_eq!(self.ctx.device.0, destination.texture.device);
        let info = SDL_GPUBlitInfo {
            source: source.sdl(),
            destination: destination.sdl(),
            load_op,
            clear_color: OPAQUE_BLACK,
            filter,
            ..Default::default()
        };
        // SAFETY: the command has no active pass and both textures belong to its device.
        unsafe { SDL_BlitGPUTexture(self.idle_raw(), &info) };
    }

    pub fn push_vertex_uniforms(&mut self, slot: u32, data: &[u8]) {
        // SAFETY: the command is live and SDL copies the uniform bytes in this call.
        unsafe {
            SDL_PushGPUVertexUniformData(
                self.idle_raw(),
                slot,
                data.as_ptr().cast(),
                data.len() as u32,
            )
        };
    }

    pub fn push_fragment_uniforms(&mut self, slot: u32, data: &[u8]) {
        // SAFETY: the command is live and SDL copies the uniform bytes in this call.
        unsafe {
            SDL_PushGPUFragmentUniformData(
                self.idle_raw(),
                slot,
                data.as_ptr().cast(),
                data.len() as u32,
            )
        };
    }

    fn take_command(&mut self) -> NonNull<SDL_GPUCommandBuffer> {
        end_pass(&mut self.active);
        self.command.take().expect("unsubmitted GPU command")
    }

    pub fn submit(mut self) -> Result<(), String> {
        let command = self.take_command();
        // SAFETY: the command is live, no pass is active, and submission consumes the handle.
        if unsafe { SDL_SubmitGPUCommandBuffer(command.as_ptr()) } {
            Ok(())
        } else {
            Err(failure("Submit GPU frame"))
        }
    }

    fn submit_and_wait(mut self) -> Result<(), String> {
        let command = self.take_command();
        // SAFETY: the command is live, no pass is active, and submission consumes the handle.
        let fence = unsafe { SDL_SubmitGPUCommandBufferAndAcquireFence(command.as_ptr()) };
        if fence.is_null() {
            return Err(failure("Submit GPU command buffer"));
        }
        let device = self.ctx.device.raw();
        // SAFETY: the submitted commands and fence belong to this device.
        let signaled = unsafe { SDL_WaitForGPUFences(device, true, &fence, 1) };
        // SAFETY: this call owns the acquired fence.
        unsafe { SDL_ReleaseGPUFence(device, fence) };
        if signaled {
            Ok(())
        } else {
            Err(failure("Wait for GPU fence"))
        }
    }
}

impl Drop for CommandBuffer<'_> {
    fn drop(&mut self) {
        // A forgotten pass must not leave SDL in a recording pass when the command is dropped.
        end_pass(&mut self.active);
        if let Some(command) = self.command.take() {
            if self.swapchain.is_some() {
                // SAFETY: SDL requires submission once a drawable has been acquired.
                let _ = unsafe { SDL_SubmitGPUCommandBuffer(command.as_ptr()) };
            } else {
                // SAFETY: this unsubmitted command has no drawable and can be cancelled.
                let _ = unsafe { SDL_CancelGPUCommandBuffer(command.as_ptr()) };
            }
        }
    }
}

pub struct CopyPass<'a> {
    handle: NonNull<SDL_GPUCopyPass>,
    active: &'a mut Option<ActivePass>,
    device: NonNull<SDL_GPUDevice>,
}

impl CopyPass<'_> {
    pub fn upload_buffer(
        &mut self,
        source: &GpuTransfer,
        source_offset: u32,
        destination: &GpuBuffer,
        size: u32,
        cycle: bool,
    ) {
        assert_eq!(self.device, source.device.0);
        assert_eq!(self.device, destination.device.0);
        assert!(u64::from(source_offset) + u64::from(size) <= u64::from(source.size));
        assert!(size <= destination.size);
        let from = SDL_GPUTransferBufferLocation {
            transfer_buffer: source.handle.as_ptr(),
            offset: source_offset,
        };
        let to = SDL_GPUBufferRegion {
            buffer: destination.handle.as_ptr(),
            offset: 0,
            size,
        };
        // SAFETY: both live resources belong to this device and the copy is in bounds.
        unsafe { SDL_UploadToGPUBuffer(self.handle.as_ptr(), &from, &to, cycle) };
    }

    pub fn upload_texture(
        &mut self,
        source: &GpuTransfer,
        destination: TextureRef<'_>,
        cycle: bool,
    ) {
        assert_eq!(self.device, source.device.0);
        assert_eq!(self.device, destination.device);
        assert!(destination.bytes() <= u64::from(source.size));
        let from = SDL_GPUTextureTransferInfo {
            transfer_buffer: source.handle.as_ptr(),
            pixels_per_row: destination.width,
            rows_per_layer: destination.height,
            ..Default::default()
        };
        // SAFETY: both live resources belong to this device and the transfer covers the texture.
        unsafe {
            SDL_UploadToGPUTexture(self.handle.as_ptr(), &from, &destination.region(), cycle)
        };
    }

    fn download_texture(&mut self, source: TextureRef<'_>, destination: &mut GpuTransfer) {
        assert_eq!(self.device, source.device);
        assert_eq!(self.device, destination.device.0);
        assert!(source.bytes() <= u64::from(destination.size));
        let to = SDL_GPUTextureTransferInfo {
            transfer_buffer: destination.handle.as_ptr(),
            pixels_per_row: source.width,
            rows_per_layer: source.height,
            ..Default::default()
        };
        // SAFETY: both live resources belong to this device and the transfer covers the texture.
        unsafe { SDL_DownloadFromGPUTexture(self.handle.as_ptr(), &source.region(), &to) };
    }
}

impl Drop for CopyPass<'_> {
    fn drop(&mut self) {
        end_pass(self.active);
    }
}

pub struct RenderPass<'a> {
    handle: NonNull<SDL_GPURenderPass>,
    active: &'a mut Option<ActivePass>,
    device: NonNull<SDL_GPUDevice>,
}

impl RenderPass<'_> {
    pub fn bind_pipeline(&mut self, pipeline: &GpuPipeline) {
        assert_eq!(self.device, pipeline.device.0);
        // SAFETY: the pass and pipeline are live and belong to the same device.
        unsafe { SDL_BindGPUGraphicsPipeline(self.handle.as_ptr(), pipeline.handle.as_ptr()) };
    }

    pub fn bind_vertex_storage_buffers<const N: usize>(
        &mut self,
        first_slot: u32,
        buffers: [&GpuBuffer; N],
    ) {
        let handles = buffers.map(|buffer| {
            assert_eq!(self.device, buffer.device.0);
            buffer.handle.as_ptr()
        });
        // SAFETY: the handles describe live buffers on the pass's device.
        unsafe {
            SDL_BindGPUVertexStorageBuffers(
                self.handle.as_ptr(),
                first_slot,
                handles.as_ptr(),
                N as u32,
            )
        };
    }

    pub fn bind_fragment_storage_buffers<const N: usize>(
        &mut self,
        first_slot: u32,
        buffers: [&GpuBuffer; N],
    ) {
        let handles = buffers.map(|buffer| {
            assert_eq!(self.device, buffer.device.0);
            buffer.handle.as_ptr()
        });
        // SAFETY: the handles describe live buffers on the pass's device.
        unsafe {
            SDL_BindGPUFragmentStorageBuffers(
                self.handle.as_ptr(),
                first_slot,
                handles.as_ptr(),
                N as u32,
            )
        };
    }

    pub fn bind_fragment_samplers<const N: usize>(
        &mut self,
        first_slot: u32,
        bindings: [SamplerBinding<'_>; N],
    ) {
        let bindings = bindings.map(|binding| {
            assert_eq!(self.device, binding.texture.device);
            assert_eq!(self.device, binding.sampler.device.0);
            SDL_GPUTextureSamplerBinding {
                texture: binding.texture.handle.as_ptr(),
                sampler: binding.sampler.handle.as_ptr(),
            }
        });
        // SAFETY: the binding array describes live resources on the pass's device.
        unsafe {
            SDL_BindGPUFragmentSamplers(
                self.handle.as_ptr(),
                first_slot,
                bindings.as_ptr(),
                N as u32,
            )
        };
    }

    pub fn set_scissor(&mut self, x: i32, y: i32, width: i32, height: i32) {
        let scissor = SDL_Rect {
            x,
            y,
            w: width,
            h: height,
        };
        // SAFETY: the pass is live and SDL copies the rectangle during the call.
        unsafe { SDL_SetGPUScissor(self.handle.as_ptr(), &scissor) };
    }

    pub fn draw(&mut self, vertices: u32, instances: u32) {
        // SAFETY: the pass has a graphics pipeline and bindings for this draw.
        unsafe { SDL_DrawGPUPrimitives(self.handle.as_ptr(), vertices, instances, 0, 0) };
    }
}

impl Drop for RenderPass<'_> {
    fn drop(&mut self) {
        end_pass(self.active);
    }
}

struct WindowCapture {
    texture: GpuTexture,
    download: GpuTransfer,
    format: TextureFormat,
}

impl WindowCapture {
    fn new(ctx: &GpuContext, drawable: TextureRef<'_>) -> Result<Self, String> {
        let format = ctx
            .swapchain_format()
            .filter(|format| *format != TextureFormat::R32Uint)
            .ok_or_else(|| "Window capture requires an 8-bit SDR swapchain".to_owned())?;
        let bytes = u32::try_from(drawable.bytes())
            .map_err(|_| "Window capture exceeds GPU transfer size".to_owned())?;
        Ok(Self {
            texture: ctx.create_texture(format, drawable.width, drawable.height)?,
            download: ctx.create_transfer(bytes, TransferUsage::Download)?,
            format,
        })
    }

    fn save(&mut self, mut cmd: CommandBuffer<'_>, path: &str) -> Result<(), String> {
        let surface = self.texture.as_ref();
        cmd.begin_copy_pass("Begin window capture download")?
            .download_texture(surface, &mut self.download);
        cmd.submit_and_wait()?;
        let count = (surface.bytes() / 4) as usize;
        let mut mapping = self.download.map(false, "Map window capture")?;
        // SAFETY: the full texture download was fenced before mapping these pixel words.
        let pixels = unsafe { mapping.words(count).assume_init_mut() };
        save_surface(path, pixels, surface, self.format, CaptureImage::Window)
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

    pub fn finish(
        &mut self,
        mut cmd: CommandBuffer<'_>,
        surface: TextureRef<'_>,
        output: &mut [u32],
    ) -> Result<(), String> {
        let bytes = u32::try_from(surface.bytes())
            .map_err(|_| "Incomplete GPU output buffer".to_owned())?;
        let pixels = (bytes / 4) as usize;
        if output.len() < pixels {
            return Err("Incomplete GPU output buffer".to_owned());
        }
        if self.download.as_ref().map(GpuTransfer::size) != Some(bytes) {
            self.download = Some(cmd.ctx.create_transfer(bytes, TransferUsage::Download)?);
        }
        let download = self
            .download
            .as_mut()
            .expect("download transfer created above");
        cmd.begin_copy_pass("Begin GPU readback copy")?
            .download_texture(surface, download);
        cmd.submit_and_wait()?;
        let mut mapping = download.map(false, "Map GPU readback")?;
        // SAFETY: the full texture download was fenced before mapping these pixel words.
        let source = unsafe { mapping.words(pixels).assume_init_ref() };
        output[..pixels].copy_from_slice(source);
        Ok(())
    }
}

pub(super) enum CaptureImage {
    Window,
    Game { png: bool },
}

pub(super) fn save_surface(
    path: &str,
    pixels: &mut [u32],
    surface: TextureRef<'_>,
    format: TextureFormat,
    image: CaptureImage,
) -> Result<(), String> {
    use sdl3_sys::pixels::{SDL_PIXELFORMAT_ABGR8888, SDL_PIXELFORMAT_ARGB8888};
    use sdl3_sys::surface::{SDL_CreateSurfaceFrom, SDL_DestroySurface, SDL_SaveBMP, SDL_SavePNG};
    let (png, create_operation, save_operation) = match image {
        CaptureImage::Window => (
            true,
            "Create window capture image",
            "Save window capture PNG",
        ),
        CaptureImage::Game { png } => (png, "Create GPU capture surface", "Save GPU surface"),
    };
    let format = match format {
        TextureFormat::Bgra8Unorm => SDL_PIXELFORMAT_ARGB8888,
        TextureFormat::Rgba8Unorm => SDL_PIXELFORMAT_ABGR8888,
        TextureFormat::R32Uint => unreachable!("capture requires a color texture"),
    };
    let path = std::ffi::CString::new(path)
        .map_err(|_| format!("{save_operation}: path contains a NUL byte"))?;
    let width = surface.width;
    let height = surface.height;
    assert!(pixels.len() >= width as usize * height as usize);
    // SAFETY: the pixel buffer covers the surface and is exclusively borrowed until destruction.
    let image = unsafe {
        SDL_CreateSurfaceFrom(
            width as i32,
            height as i32,
            format,
            pixels.as_mut_ptr().cast(),
            (width * 4) as i32,
        )
    };
    if image.is_null() {
        return Err(failure(create_operation));
    }
    // SAFETY: the surface is live and the path is NUL-terminated.
    let saved = unsafe {
        if png {
            SDL_SavePNG(image, path.as_ptr())
        } else {
            SDL_SaveBMP(image, path.as_ptr())
        }
    };
    // SAFETY: this function owns the surface created above.
    unsafe { SDL_DestroySurface(image) };
    if saved {
        Ok(())
    } else {
        Err(failure(save_operation))
    }
}
