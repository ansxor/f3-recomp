//! GPU backend driver abstraction, the Rust side of `runtime/renderer/gpu/sdl_driver_shim.cpp`.
//!
//! The renderer never links SDL3: every device, swapchain, pass, transfer and display-link
//! operation crosses this narrow C ABI vtable, which `f3rt-gpu` installs when it constructs a
//! [`super::video::GpuVideo`]. [`GpuDriver`] is also implemented by test doubles, so the whole
//! backend runs under `cargo test` with no GPU.

use std::ffi::{CStr, CString, c_char, c_void};
use std::rc::Rc;

macro_rules! opaque_handles {
    ($($name:ident),* $(,)?) => {$(
        /// Opaque driver handle. Only the driver that produced it may use it.
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        #[repr(transparent)]
        pub struct $name(pub *mut c_void);

        impl $name {
            pub const fn null() -> Self {
                Self(std::ptr::null_mut())
            }

            pub const fn is_null(self) -> bool {
                self.0.is_null()
            }
        }

        impl Default for $name {
            fn default() -> Self {
                Self::null()
            }
        }
    )*};
}

opaque_handles!(
    RawDevice,
    RawWindow,
    RawTexture,
    RawBuffer,
    RawPipeline,
    RawTransfer,
    RawSampler,
    RawCommand,
    RawCopyPass,
    RawRenderPass,
    RawPacing,
);

/// Texture formats the renderer creates. Values are the shim's own tokens, not SDL enums.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum TextureFormat {
    Bgra8Unorm = 1,
    Rgba8Unorm = 2,
    R32Uint = 3,
}

impl TextureFormat {
    /// `0` (or any other token) is not an 8-bit SDR swapchain the renderer can capture.
    pub const fn from_token(token: u32) -> Option<Self> {
        match token {
            1 => Some(Self::Bgra8Unorm),
            2 => Some(Self::Rgba8Unorm),
            _ => None,
        }
    }
}

/// Transfer buffer usage.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum TransferUsage {
    Upload = 1,
    Download = 2,
}

/// Graphics shaders baked into `video_shaders.hpp`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum ShaderKind {
    FullscreenVert = 0,
    SpriteVert = 1,
    SpriteFrag = 2,
    SceneFrag = 3,
    SceneInterpFrag = 4,
    RotateFrag = 5,
    PostprocessFrag = 6,
}

/// Blit load operation, mirroring `SDL_GPULoadOp`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum LoadOp {
    Clear = 0,
    Load = 1,
    DontCare = 2,
}

/// Blit filter, mirroring `SDL_GPUFilter`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum Filter {
    Nearest = 0,
    Linear = 1,
}

/// `SDL_GPUBlitInfo`, reduced to what the renderer uses.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Blit {
    pub source: *mut c_void,
    pub source_x: u32,
    pub source_y: u32,
    pub source_width: u32,
    pub source_height: u32,
    pub destination: *mut c_void,
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
    pub load_op: u32,
    pub filter: u32,
    pub clear_color: [f32; 4],
}

/// `SDL_GPUTextureSamplerBinding`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct TextureSamplerBinding {
    pub texture: *mut c_void,
    pub sampler: *mut c_void,
}

/// Swapchain acquisition result, filled by the shim.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SwapchainOut {
    pub texture: *mut c_void,
    pub width: u32,
    pub height: u32,
}

/// The C ABI the shim installs. Field order and types are duplicated in
/// `runtime/renderer/gpu/sdl_driver_shim.hpp`; both sides assert the size.
#[repr(C)]
pub struct GpuDriverVtable {
    pub create_device: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub last_error: unsafe extern "C" fn(*mut c_void) -> *const c_char,
    pub claim_window: unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool,
    pub set_swapchain_mode: unsafe extern "C" fn(*mut c_void, *mut c_void, bool) -> bool,
    pub release_window: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub destroy_device: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub wait_idle: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub is_metal: unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool,
    pub driver_name: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *const c_char,
    pub display_hz: unsafe extern "C" fn(*mut c_void) -> f64,
    pub swapchain_format: unsafe extern "C" fn(*mut c_void, *mut c_void) -> u32,
    pub create_sampler: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void,
    pub create_buffer: unsafe extern "C" fn(*mut c_void, *mut c_void, u32) -> *mut c_void,
    pub create_texture:
        unsafe extern "C" fn(*mut c_void, *mut c_void, u32, u32, u32) -> *mut c_void,
    pub create_transfer: unsafe extern "C" fn(*mut c_void, *mut c_void, u32, u32) -> *mut c_void,
    pub create_pipeline:
        unsafe extern "C" fn(*mut c_void, *mut c_void, u32, u32, u32) -> *mut c_void,
    pub create_user_pipeline: unsafe extern "C" fn(
        *mut c_void,
        *mut c_void,
        *const u8,
        usize,
        *const c_char,
        u32,
        u32,
        u32,
        u32,
    ) -> *mut c_void,
    pub release_buffer: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void),
    pub release_texture: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void),
    pub release_transfer: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void),
    pub release_sampler: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void),
    pub release_pipeline: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void),
    pub map_transfer:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, bool) -> *mut c_void,
    pub unmap_transfer: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void),
    pub acquire_command: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void,
    pub submit_command: unsafe extern "C" fn(*mut c_void) -> bool,
    pub cancel_command: unsafe extern "C" fn(*mut c_void) -> bool,
    pub submit_and_wait: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void) -> i32,
    pub acquire_swapchain:
        unsafe extern "C" fn(*mut c_void, *mut c_void, bool, *mut SwapchainOut) -> i32,
    pub begin_copy_pass: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub end_copy_pass: unsafe extern "C" fn(*mut c_void),
    pub begin_render_pass: unsafe extern "C" fn(*mut c_void, *mut c_void, bool) -> *mut c_void,
    pub end_render_pass: unsafe extern "C" fn(*mut c_void),
    pub upload_buffer: unsafe extern "C" fn(*mut c_void, *mut c_void, u32, *mut c_void, u32, bool),
    pub upload_texture:
        unsafe extern "C" fn(*mut c_void, *mut c_void, u32, u32, u32, *mut c_void, bool),
    pub download_texture: unsafe extern "C" fn(*mut c_void, *mut c_void, u32, u32, *mut c_void),
    pub push_vertex_uniforms: unsafe extern "C" fn(*mut c_void, u32, *const c_void, usize),
    pub push_fragment_uniforms: unsafe extern "C" fn(*mut c_void, u32, *const c_void, usize),
    pub bind_pipeline: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub bind_vertex_storage_buffers:
        unsafe extern "C" fn(*mut c_void, u32, *const *mut c_void, usize),
    pub bind_fragment_storage_buffers:
        unsafe extern "C" fn(*mut c_void, u32, *const *mut c_void, usize),
    pub bind_fragment_samplers:
        unsafe extern "C" fn(*mut c_void, u32, *const TextureSamplerBinding, usize),
    pub set_scissor: unsafe extern "C" fn(*mut c_void, i32, i32, i32, i32),
    pub draw: unsafe extern "C" fn(*mut c_void, u32, u32),
    pub blit: unsafe extern "C" fn(*mut c_void, *const Blit),
    pub invoke_overlay: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, u32, u32),
    pub save_surface:
        unsafe extern "C" fn(*mut c_void, *const c_char, *const u32, u32, u32, u32, bool) -> i32,
    pub set_frames_in_flight: unsafe extern "C" fn(*mut c_void, *mut c_void, u32) -> bool,
    pub create_pacer: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub destroy_pacer: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub wait_pacer: unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool,
    pub poll_pacer: unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool,
    pub pacer_callback_hz: unsafe extern "C" fn(*mut c_void, *mut c_void) -> f64,
    pub pacer_requested_hz: unsafe extern "C" fn(*mut c_void, *mut c_void) -> f64,
    pub monotonic_ns: unsafe extern "C" fn(*mut c_void) -> u64,
    pub ticks_ns: unsafe extern "C" fn(*mut c_void) -> u64,
}

/// Everything the renderer asks of a GPU backend. All fallible calls take the operation name so
/// failures read exactly like the C++ `gpu_fail(operation)` messages.
pub trait GpuDriver {
    fn create_device(&self, operation: &str) -> Result<RawDevice, String>;
    fn claim_window(&self, device: RawDevice, operation: &str) -> Result<(), String>;
    fn set_swapchain_mode(
        &self,
        device: RawDevice,
        vsync: bool,
        operation: &str,
    ) -> Result<(), String>;
    fn release_window(&self, device: RawDevice);
    fn destroy_device(&self, device: RawDevice);
    fn wait_idle(&self, device: RawDevice);
    fn is_metal(&self, device: RawDevice) -> bool;
    fn driver_name(&self, device: RawDevice) -> String;
    /// Current display mode refresh rate, not observed cadence.
    fn display_hz(&self) -> f64;
    /// Swapchain format, when it is one of the 8-bit SDR capture formats.
    fn swapchain_format(&self, device: RawDevice) -> Option<TextureFormat>;

    fn create_sampler(&self, device: RawDevice, operation: &str) -> Result<RawSampler, String>;
    fn create_buffer(
        &self,
        device: RawDevice,
        bytes: u32,
        operation: &str,
    ) -> Result<RawBuffer, String>;
    fn create_texture(
        &self,
        device: RawDevice,
        format: TextureFormat,
        width: u32,
        height: u32,
        operation: &str,
    ) -> Result<RawTexture, String>;
    fn create_transfer(
        &self,
        device: RawDevice,
        bytes: u32,
        usage: TransferUsage,
        operation: &str,
    ) -> Result<RawTransfer, String>;
    fn create_pipeline(
        &self,
        device: RawDevice,
        vertex: ShaderKind,
        fragment: ShaderKind,
        format: TextureFormat,
        operation: &str,
    ) -> Result<RawPipeline, String>;
    #[allow(clippy::too_many_arguments)]
    fn create_user_pipeline(
        &self,
        device: RawDevice,
        code: &[u32],
        bytes: usize,
        entry: &str,
        samplers: u32,
        buffers: u32,
        uniforms: u32,
        format: TextureFormat,
        operation: &str,
    ) -> Result<RawPipeline, String>;

    fn release_buffer(&self, device: RawDevice, buffer: RawBuffer);
    fn release_texture(&self, device: RawDevice, texture: RawTexture);
    fn release_transfer(&self, device: RawDevice, transfer: RawTransfer);
    fn release_sampler(&self, device: RawDevice, sampler: RawSampler);
    fn release_pipeline(&self, device: RawDevice, pipeline: RawPipeline);

    fn map_transfer(
        &self,
        device: RawDevice,
        transfer: RawTransfer,
        cycle: bool,
        operation: &str,
    ) -> Result<*mut c_void, String>;
    fn unmap_transfer(&self, device: RawDevice, transfer: RawTransfer);

    fn acquire_command(&self, device: RawDevice, operation: &str) -> Result<RawCommand, String>;
    /// Submits and reports success; on failure the command buffer is consumed anyway.
    fn submit_command(&self, command: RawCommand, operation: &str) -> Result<(), String>;
    fn cancel_command(&self, command: RawCommand);
    fn submit_and_wait(
        &self,
        device: RawDevice,
        command: RawCommand,
        submit_operation: &str,
        wait_operation: &str,
    ) -> Result<(), String>;
    /// `Ok(true)` acquired a drawable; `Ok(false)` means no texture this frame.
    fn acquire_swapchain(
        &self,
        command: RawCommand,
        wait: bool,
        out: &mut SwapchainOut,
        operation: &str,
    ) -> Result<bool, String>;

    fn begin_copy_pass(&self, command: RawCommand, operation: &str) -> Result<RawCopyPass, String>;
    fn end_copy_pass(&self, pass: RawCopyPass);
    fn begin_render_pass(
        &self,
        command: RawCommand,
        target: RawTexture,
        load: bool,
        operation: &str,
    ) -> Result<RawRenderPass, String>;
    fn end_render_pass(&self, pass: RawRenderPass);
    fn upload_buffer(
        &self,
        pass: RawCopyPass,
        source: RawTransfer,
        source_offset: u32,
        destination: RawBuffer,
        size: u32,
        cycle: bool,
    );
    #[allow(clippy::too_many_arguments)]
    fn upload_texture(
        &self,
        pass: RawCopyPass,
        source: RawTransfer,
        source_offset: u32,
        width: u32,
        height: u32,
        destination: RawTexture,
        cycle: bool,
    );
    fn download_texture(
        &self,
        pass: RawCopyPass,
        source: RawTexture,
        width: u32,
        height: u32,
        destination: RawTransfer,
    );
    fn push_vertex_uniforms(&self, command: RawCommand, slot: u32, data: &[u8]);
    fn push_fragment_uniforms(&self, command: RawCommand, slot: u32, data: &[u8]);
    fn bind_pipeline(&self, pass: RawRenderPass, pipeline: RawPipeline);
    fn bind_vertex_storage_buffers(
        &self,
        pass: RawRenderPass,
        first_slot: u32,
        buffers: &[RawBuffer],
    );
    fn bind_fragment_storage_buffers(
        &self,
        pass: RawRenderPass,
        first_slot: u32,
        buffers: &[RawBuffer],
    );
    fn bind_fragment_samplers(
        &self,
        pass: RawRenderPass,
        first_slot: u32,
        bindings: &[TextureSamplerBinding],
    );
    fn set_scissor(&self, pass: RawRenderPass, x: i32, y: i32, width: i32, height: i32);
    fn draw(&self, pass: RawRenderPass, vertices: u32, instances: u32);
    fn blit(&self, command: RawCommand, blit: &Blit);
    fn invoke_overlay(&self, command: RawCommand, target: RawTexture, width: u32, height: u32);
    /// `pixel_format`: 1 = ARGB8888, 2 = ABGR8888.
    #[allow(clippy::too_many_arguments)]
    fn save_surface(
        &self,
        path: &str,
        pixels: &[u32],
        width: u32,
        height: u32,
        pixel_format: u32,
        png: bool,
        create_operation: &str,
        save_operation: &str,
    ) -> Result<(), String>;

    fn set_frames_in_flight(
        &self,
        device: RawDevice,
        count: u32,
        operation: &str,
    ) -> Result<(), String>;
    fn create_pacer(&self) -> Option<RawPacing>;
    fn destroy_pacer(&self, pacer: RawPacing);
    fn wait_pacer(&self, pacer: RawPacing) -> bool;
    fn poll_pacer(&self, pacer: RawPacing) -> bool;
    fn pacer_callback_hz(&self, pacer: RawPacing) -> f64;
    fn pacer_requested_hz(&self, pacer: RawPacing) -> f64;

    /// The shim's `steady_clock` in nanoseconds; live motion alpha is sampled against it.
    fn monotonic_ns(&self) -> u64;
    /// `SDL_GetTicksNS`, the postprocess clock.
    fn ticks_ns(&self) -> u64;
}

/// Adapter over the C ABI table installed by `f3rt-gpu`.
pub struct SdlDriver {
    vtable: &'static GpuDriverVtable,
    self_: *mut c_void,
}

impl SdlDriver {
    /// # Safety
    /// `vtable` must point to a live `GpuDriverVtable` and `handle` to the shim that owns it;
    /// both must outlive the returned driver's users.
    pub unsafe fn from_raw(vtable: *const GpuDriverVtable, handle: *mut c_void) -> Self {
        assert!(!vtable.is_null(), "null GPU driver vtable");
        // SAFETY: the caller guarantees the table lives for the driver's users.
        Self {
            vtable: unsafe { &*vtable },
            self_: handle,
        }
    }

    fn detail(&self) -> String {
        // SAFETY: `last_error` returns a NUL-terminated string owned by the shim.
        unsafe {
            let message = (self.vtable.last_error)(self.self_);
            if message.is_null() {
                String::new()
            } else {
                CStr::from_ptr(message).to_string_lossy().into_owned()
            }
        }
    }

    fn failure(&self, operation: &str) -> String {
        format!("{operation}: {}", self.detail())
    }

    fn created(handle: *mut c_void, operation: &str, driver: &Self) -> Result<RawTexture, String> {
        if handle.is_null() {
            Err(driver.failure(operation))
        } else {
            Ok(RawTexture(handle))
        }
    }

    fn c_path(path: &str, operation: &str) -> Result<CString, String> {
        CString::new(path).map_err(|_| format!("{operation}: path contains a NUL byte"))
    }
}

impl GpuDriver for SdlDriver {
    fn create_device(&self, operation: &str) -> Result<RawDevice, String> {
        // SAFETY: vtable entry installed by the shim.
        let device = unsafe { (self.vtable.create_device)(self.self_) };
        if device.is_null() {
            Err(self.failure(operation))
        } else {
            Ok(RawDevice(device))
        }
    }

    fn claim_window(&self, device: RawDevice, operation: &str) -> Result<(), String> {
        // SAFETY: vtable entry installed by the shim.
        if unsafe { (self.vtable.claim_window)(self.self_, device.0) } {
            Ok(())
        } else {
            Err(self.failure(operation))
        }
    }

    fn set_swapchain_mode(
        &self,
        device: RawDevice,
        vsync: bool,
        operation: &str,
    ) -> Result<(), String> {
        // SAFETY: vtable entry installed by the shim.
        if unsafe { (self.vtable.set_swapchain_mode)(self.self_, device.0, vsync) } {
            Ok(())
        } else {
            Err(self.failure(operation))
        }
    }

    fn release_window(&self, device: RawDevice) {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.release_window)(self.self_, device.0) }
    }

    fn destroy_device(&self, device: RawDevice) {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.destroy_device)(self.self_, device.0) }
    }

    fn wait_idle(&self, device: RawDevice) {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.wait_idle)(self.self_, device.0) }
    }

    fn is_metal(&self, device: RawDevice) -> bool {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.is_metal)(self.self_, device.0) }
    }

    fn driver_name(&self, device: RawDevice) -> String {
        // SAFETY: vtable entry installed by the shim returns a shim-owned C string.
        unsafe {
            let name = (self.vtable.driver_name)(self.self_, device.0);
            if name.is_null() {
                String::new()
            } else {
                CStr::from_ptr(name).to_string_lossy().into_owned()
            }
        }
    }

    fn display_hz(&self) -> f64 {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.display_hz)(self.self_) }
    }

    fn swapchain_format(&self, device: RawDevice) -> Option<TextureFormat> {
        // SAFETY: vtable entry installed by the shim.
        let token = unsafe { (self.vtable.swapchain_format)(self.self_, device.0) };
        TextureFormat::from_token(token)
    }

    fn create_sampler(&self, device: RawDevice, operation: &str) -> Result<RawSampler, String> {
        // SAFETY: vtable entry installed by the shim.
        let sampler = unsafe { (self.vtable.create_sampler)(self.self_, device.0) };
        if sampler.is_null() {
            Err(self.failure(operation))
        } else {
            Ok(RawSampler(sampler))
        }
    }

    fn create_buffer(
        &self,
        device: RawDevice,
        bytes: u32,
        operation: &str,
    ) -> Result<RawBuffer, String> {
        // SAFETY: vtable entry installed by the shim.
        let buffer = unsafe { (self.vtable.create_buffer)(self.self_, device.0, bytes) };
        if buffer.is_null() {
            Err(self.failure(operation))
        } else {
            Ok(RawBuffer(buffer))
        }
    }

    fn create_texture(
        &self,
        device: RawDevice,
        format: TextureFormat,
        width: u32,
        height: u32,
        operation: &str,
    ) -> Result<RawTexture, String> {
        // SAFETY: vtable entry installed by the shim.
        let texture = unsafe {
            (self.vtable.create_texture)(self.self_, device.0, format as u32, width, height)
        };
        Self::created(texture, operation, self)
    }

    fn create_transfer(
        &self,
        device: RawDevice,
        bytes: u32,
        usage: TransferUsage,
        operation: &str,
    ) -> Result<RawTransfer, String> {
        // SAFETY: vtable entry installed by the shim.
        let transfer =
            unsafe { (self.vtable.create_transfer)(self.self_, device.0, bytes, usage as u32) };
        if transfer.is_null() {
            Err(self.failure(operation))
        } else {
            Ok(RawTransfer(transfer))
        }
    }

    fn create_pipeline(
        &self,
        device: RawDevice,
        vertex: ShaderKind,
        fragment: ShaderKind,
        format: TextureFormat,
        operation: &str,
    ) -> Result<RawPipeline, String> {
        // SAFETY: vtable entry installed by the shim.
        let pipeline = unsafe {
            (self.vtable.create_pipeline)(
                self.self_,
                device.0,
                vertex as u32,
                fragment as u32,
                format as u32,
            )
        };
        if pipeline.is_null() {
            Err(self.failure(operation))
        } else {
            Ok(RawPipeline(pipeline))
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn create_user_pipeline(
        &self,
        device: RawDevice,
        code: &[u32],
        bytes: usize,
        entry: &str,
        samplers: u32,
        buffers: u32,
        uniforms: u32,
        format: TextureFormat,
        operation: &str,
    ) -> Result<RawPipeline, String> {
        let entry = CString::new(entry)
            .map_err(|_| format!("{operation}: shader entry point contains a NUL byte"))?;
        // SAFETY: vtable entry installed by the shim; `code` outlives the call.
        let pipeline = unsafe {
            (self.vtable.create_user_pipeline)(
                self.self_,
                device.0,
                code.as_ptr().cast::<u8>(),
                bytes,
                entry.as_ptr(),
                samplers,
                buffers,
                uniforms,
                format as u32,
            )
        };
        if pipeline.is_null() {
            Err(self.failure(operation))
        } else {
            Ok(RawPipeline(pipeline))
        }
    }

    fn release_buffer(&self, device: RawDevice, buffer: RawBuffer) {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.release_buffer)(self.self_, device.0, buffer.0) }
    }

    fn release_texture(&self, device: RawDevice, texture: RawTexture) {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.release_texture)(self.self_, device.0, texture.0) }
    }

    fn release_transfer(&self, device: RawDevice, transfer: RawTransfer) {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.release_transfer)(self.self_, device.0, transfer.0) }
    }

    fn release_sampler(&self, device: RawDevice, sampler: RawSampler) {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.release_sampler)(self.self_, device.0, sampler.0) }
    }

    fn release_pipeline(&self, device: RawDevice, pipeline: RawPipeline) {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.release_pipeline)(self.self_, device.0, pipeline.0) }
    }

    fn map_transfer(
        &self,
        device: RawDevice,
        transfer: RawTransfer,
        cycle: bool,
        operation: &str,
    ) -> Result<*mut c_void, String> {
        // SAFETY: vtable entry installed by the shim.
        let data = unsafe { (self.vtable.map_transfer)(self.self_, device.0, transfer.0, cycle) };
        if data.is_null() {
            Err(self.failure(operation))
        } else {
            Ok(data)
        }
    }

    fn unmap_transfer(&self, device: RawDevice, transfer: RawTransfer) {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.unmap_transfer)(self.self_, device.0, transfer.0) }
    }

    fn acquire_command(&self, device: RawDevice, operation: &str) -> Result<RawCommand, String> {
        // SAFETY: vtable entry installed by the shim.
        let command = unsafe { (self.vtable.acquire_command)(self.self_, device.0) };
        if command.is_null() {
            Err(self.failure(operation))
        } else {
            Ok(RawCommand(command))
        }
    }

    fn submit_command(&self, command: RawCommand, operation: &str) -> Result<(), String> {
        // SAFETY: vtable entry installed by the shim.
        if unsafe { (self.vtable.submit_command)(command.0) } {
            Ok(())
        } else {
            Err(self.failure(operation))
        }
    }

    fn cancel_command(&self, command: RawCommand) {
        // SAFETY: vtable entry installed by the shim.
        unsafe {
            (self.vtable.cancel_command)(command.0);
        }
    }

    fn submit_and_wait(
        &self,
        device: RawDevice,
        command: RawCommand,
        submit_operation: &str,
        wait_operation: &str,
    ) -> Result<(), String> {
        // SAFETY: vtable entry installed by the shim.
        match unsafe { (self.vtable.submit_and_wait)(self.self_, device.0, command.0) } {
            0 => Ok(()),
            1 => Err(self.failure(submit_operation)),
            _ => Err(self.failure(wait_operation)),
        }
    }

    fn acquire_swapchain(
        &self,
        command: RawCommand,
        wait: bool,
        out: &mut SwapchainOut,
        operation: &str,
    ) -> Result<bool, String> {
        // SAFETY: vtable entry installed by the shim writes `out` on success.
        match unsafe { (self.vtable.acquire_swapchain)(self.self_, command.0, wait, out) } {
            -1 => Err(self.failure(operation)),
            0 => Ok(true),
            _ => Ok(false),
        }
    }

    fn begin_copy_pass(&self, command: RawCommand, operation: &str) -> Result<RawCopyPass, String> {
        // SAFETY: vtable entry installed by the shim.
        let pass = unsafe { (self.vtable.begin_copy_pass)(command.0) };
        if pass.is_null() {
            Err(self.failure(operation))
        } else {
            Ok(RawCopyPass(pass))
        }
    }

    fn end_copy_pass(&self, pass: RawCopyPass) {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.end_copy_pass)(pass.0) }
    }

    fn begin_render_pass(
        &self,
        command: RawCommand,
        target: RawTexture,
        load: bool,
        operation: &str,
    ) -> Result<RawRenderPass, String> {
        // SAFETY: vtable entry installed by the shim.
        let pass = unsafe { (self.vtable.begin_render_pass)(command.0, target.0, load) };
        if pass.is_null() {
            Err(self.failure(operation))
        } else {
            Ok(RawRenderPass(pass))
        }
    }

    fn end_render_pass(&self, pass: RawRenderPass) {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.end_render_pass)(pass.0) }
    }

    fn upload_buffer(
        &self,
        pass: RawCopyPass,
        source: RawTransfer,
        source_offset: u32,
        destination: RawBuffer,
        size: u32,
        cycle: bool,
    ) {
        // SAFETY: vtable entry installed by the shim.
        unsafe {
            (self.vtable.upload_buffer)(pass.0, source.0, source_offset, destination.0, size, cycle)
        }
    }

    #[allow(clippy::too_many_arguments)]
    #[allow(clippy::too_many_arguments)]
    fn upload_texture(
        &self,
        pass: RawCopyPass,
        source: RawTransfer,
        source_offset: u32,
        width: u32,
        height: u32,
        destination: RawTexture,
        cycle: bool,
    ) {
        // SAFETY: vtable entry installed by the shim.
        unsafe {
            (self.vtable.upload_texture)(
                pass.0,
                source.0,
                source_offset,
                width,
                height,
                destination.0,
                cycle,
            )
        }
    }

    fn download_texture(
        &self,
        pass: RawCopyPass,
        source: RawTexture,
        width: u32,
        height: u32,
        destination: RawTransfer,
    ) {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.download_texture)(pass.0, source.0, width, height, destination.0) }
    }

    fn push_vertex_uniforms(&self, command: RawCommand, slot: u32, data: &[u8]) {
        // SAFETY: vtable entry installed by the shim; `data` outlives the call.
        unsafe {
            (self.vtable.push_vertex_uniforms)(command.0, slot, data.as_ptr().cast(), data.len())
        }
    }

    fn push_fragment_uniforms(&self, command: RawCommand, slot: u32, data: &[u8]) {
        // SAFETY: vtable entry installed by the shim; `data` outlives the call.
        unsafe {
            (self.vtable.push_fragment_uniforms)(command.0, slot, data.as_ptr().cast(), data.len())
        }
    }

    fn bind_pipeline(&self, pass: RawRenderPass, pipeline: RawPipeline) {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.bind_pipeline)(pass.0, pipeline.0) }
    }

    fn bind_vertex_storage_buffers(
        &self,
        pass: RawRenderPass,
        first_slot: u32,
        buffers: &[RawBuffer],
    ) {
        let handles = buffer_handles(buffers);
        // SAFETY: vtable entry installed by the shim; `handles` outlives the call.
        unsafe {
            (self.vtable.bind_vertex_storage_buffers)(
                pass.0,
                first_slot,
                handles.as_ptr(),
                handles.len(),
            )
        }
    }

    fn bind_fragment_storage_buffers(
        &self,
        pass: RawRenderPass,
        first_slot: u32,
        buffers: &[RawBuffer],
    ) {
        let handles = buffer_handles(buffers);
        // SAFETY: vtable entry installed by the shim; `handles` outlives the call.
        unsafe {
            (self.vtable.bind_fragment_storage_buffers)(
                pass.0,
                first_slot,
                handles.as_ptr(),
                handles.len(),
            )
        }
    }

    fn bind_fragment_samplers(
        &self,
        pass: RawRenderPass,
        first_slot: u32,
        bindings: &[TextureSamplerBinding],
    ) {
        // SAFETY: vtable entry installed by the shim; `bindings` outlives the call.
        unsafe {
            (self.vtable.bind_fragment_samplers)(
                pass.0,
                first_slot,
                bindings.as_ptr(),
                bindings.len(),
            )
        }
    }

    fn set_scissor(&self, pass: RawRenderPass, x: i32, y: i32, width: i32, height: i32) {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.set_scissor)(pass.0, x, y, width, height) }
    }

    fn draw(&self, pass: RawRenderPass, vertices: u32, instances: u32) {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.draw)(pass.0, vertices, instances) }
    }

    fn blit(&self, command: RawCommand, blit: &Blit) {
        // SAFETY: vtable entry installed by the shim; `blit` outlives the call.
        unsafe { (self.vtable.blit)(command.0, blit) }
    }

    fn invoke_overlay(&self, command: RawCommand, target: RawTexture, width: u32, height: u32) {
        // SAFETY: vtable entry installed by the shim; it no-ops without a host callback.
        unsafe { (self.vtable.invoke_overlay)(self.self_, command.0, target.0, width, height) }
    }

    #[allow(clippy::too_many_arguments)]
    fn save_surface(
        &self,
        path: &str,
        pixels: &[u32],
        width: u32,
        height: u32,
        pixel_format: u32,
        png: bool,
        create_operation: &str,
        save_operation: &str,
    ) -> Result<(), String> {
        let path = Self::c_path(path, save_operation)?;
        // SAFETY: vtable entry installed by the shim; `pixels` covers width*height words.
        match unsafe {
            (self.vtable.save_surface)(
                self.self_,
                path.as_ptr(),
                pixels.as_ptr(),
                width,
                height,
                pixel_format,
                png,
            )
        } {
            0 => Ok(()),
            1 => Err(self.failure(create_operation)),
            _ => Err(self.failure(save_operation)),
        }
    }

    fn set_frames_in_flight(
        &self,
        device: RawDevice,
        count: u32,
        operation: &str,
    ) -> Result<(), String> {
        // SAFETY: vtable entry installed by the shim.
        if unsafe { (self.vtable.set_frames_in_flight)(self.self_, device.0, count) } {
            Ok(())
        } else {
            Err(self.failure(operation))
        }
    }

    fn create_pacer(&self) -> Option<RawPacing> {
        // SAFETY: vtable entry installed by the shim; null means no display link.
        let pacer = unsafe { (self.vtable.create_pacer)(self.self_) };
        if pacer.is_null() {
            None
        } else {
            Some(RawPacing(pacer))
        }
    }

    fn destroy_pacer(&self, pacer: RawPacing) {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.destroy_pacer)(self.self_, pacer.0) }
    }

    fn wait_pacer(&self, pacer: RawPacing) -> bool {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.wait_pacer)(self.self_, pacer.0) }
    }

    fn poll_pacer(&self, pacer: RawPacing) -> bool {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.poll_pacer)(self.self_, pacer.0) }
    }

    fn pacer_callback_hz(&self, pacer: RawPacing) -> f64 {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.pacer_callback_hz)(self.self_, pacer.0) }
    }

    fn pacer_requested_hz(&self, pacer: RawPacing) -> f64 {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.pacer_requested_hz)(self.self_, pacer.0) }
    }

    fn monotonic_ns(&self) -> u64 {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.monotonic_ns)(self.self_) }
    }

    fn ticks_ns(&self) -> u64 {
        // SAFETY: vtable entry installed by the shim.
        unsafe { (self.vtable.ticks_ns)(self.self_) }
    }
}

fn buffer_handles(buffers: &[RawBuffer]) -> Vec<*mut c_void> {
    buffers.iter().map(|buffer| buffer.0).collect()
}

/// Shared driver reference used by every GPU object.
pub type DriverRef = Rc<dyn GpuDriver>;

pub fn driver_ref<D: GpuDriver + 'static>(driver: D) -> DriverRef {
    Rc::new(driver)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vtable_and_blit_layouts_match_the_cpp_shim() {
        // `runtime/renderer/gpu/sdl_driver_shim.hpp` asserts the same sizes.
        assert_eq!(
            std::mem::size_of::<GpuDriverVtable>(),
            56 * std::mem::size_of::<usize>()
        );
        assert_eq!(std::mem::size_of::<Blit>(), 72);
        assert_eq!(
            std::mem::size_of::<TextureSamplerBinding>(),
            2 * std::mem::size_of::<usize>()
        );
        assert_eq!(std::mem::size_of::<SwapchainOut>(), 16);
        assert_eq!(std::mem::offset_of!(Blit, destination), 24);
        assert_eq!(std::mem::offset_of!(Blit, load_op), 48);
        assert_eq!(std::mem::offset_of!(Blit, clear_color), 56);
    }
}
