//! SDL GPU object lifetimes and presentation, mechanically ported from
//! `runtime/renderer/gpu/sdl_gpu.{hpp,cpp}` with the SDL calls behind [`GpuDriver`].

use super::driver::{
    Blit, DriverRef, Filter, GpuDriver, LoadOp, RawBuffer, RawCommand, RawCopyPass, RawDevice,
    RawPipeline, RawRenderPass, RawSampler, RawTexture, RawTransfer, RawWindow, ShaderKind,
    SwapchainOut, TextureFormat, TextureSamplerBinding, TransferUsage,
};
use super::pacing::DisplayPacer;
use crate::config::{GameVideoOptions, Geometry, VideoScaleMode};
use crate::scale::video_blit_for_window;
use std::ffi::c_void;
use std::rc::Rc;

/// A presented or drawn texture and its pixel size.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct GpuSurface {
    pub texture: RawTexture,
    pub width: u32,
    pub height: u32,
}

impl GpuSurface {
    pub fn is_valid(&self) -> bool {
        !self.texture.is_null()
    }
}

/// Move-only owner of one driver object. The device must outlive every handle.
pub struct GpuBuffer {
    driver: DriverRef,
    device: RawDevice,
    handle: RawBuffer,
}

impl GpuBuffer {
    pub fn new(driver: DriverRef, device: RawDevice, handle: RawBuffer) -> Self {
        Self {
            driver,
            device,
            handle,
        }
    }

    pub fn get(&self) -> RawBuffer {
        self.handle
    }
}

impl Drop for GpuBuffer {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            self.driver.release_buffer(self.device, self.handle);
        }
    }
}

pub struct GpuPipeline {
    driver: DriverRef,
    device: RawDevice,
    handle: RawPipeline,
}

impl GpuPipeline {
    pub fn new(driver: DriverRef, device: RawDevice, handle: RawPipeline) -> Self {
        Self {
            driver,
            device,
            handle,
        }
    }

    pub fn get(&self) -> RawPipeline {
        self.handle
    }
}

impl Drop for GpuPipeline {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            self.driver.release_pipeline(self.device, self.handle);
        }
    }
}

pub struct GpuSampler {
    driver: DriverRef,
    device: RawDevice,
    handle: RawSampler,
}

impl GpuSampler {
    pub fn new(driver: DriverRef, device: RawDevice, handle: RawSampler) -> Self {
        Self {
            driver,
            device,
            handle,
        }
    }

    pub fn get(&self) -> RawSampler {
        self.handle
    }
}

impl Drop for GpuSampler {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            self.driver.release_sampler(self.device, self.handle);
        }
    }
}

/// An owned render texture with the format/size used to decide reallocation.
pub struct GpuTexture {
    driver: Option<DriverRef>,
    device: RawDevice,
    handle: RawTexture,
    format: TextureFormat,
    width: u32,
    height: u32,
}

impl GpuTexture {
    pub const fn empty() -> Self {
        Self {
            driver: None,
            device: RawDevice::null(),
            handle: RawTexture::null(),
            format: TextureFormat::Bgra8Unorm,
            width: 0,
            height: 0,
        }
    }

    pub fn new(
        driver: DriverRef,
        device: RawDevice,
        handle: RawTexture,
        format: TextureFormat,
        width: u32,
        height: u32,
    ) -> Self {
        Self {
            driver: Some(driver),
            device,
            handle,
            format,
            width,
            height,
        }
    }

    pub fn get(&self) -> RawTexture {
        self.handle
    }

    pub fn surface(&self) -> GpuSurface {
        GpuSurface {
            texture: self.handle,
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
        if let Some(driver) = &self.driver
            && self.is_valid()
        {
            driver.release_texture(self.device, self.handle);
        }
    }
}

/// A mapped transfer buffer; unmaps on drop.
pub struct GpuMapping {
    driver: DriverRef,
    device: RawDevice,
    transfer: RawTransfer,
    data: *mut c_void,
}

impl GpuMapping {
    pub fn data(&self) -> *mut c_void {
        self.data
    }

    pub fn words(&mut self, count: usize) -> &mut [u32] {
        // SAFETY: the driver returned a mapping of at least `count` words for this transfer.
        unsafe { std::slice::from_raw_parts_mut(self.data.cast::<u32>(), count) }
    }

    pub fn bytes(&mut self, count: usize) -> &mut [u8] {
        // SAFETY: the driver returned a mapping of at least `count` bytes for this transfer.
        unsafe { std::slice::from_raw_parts_mut(self.data.cast::<u8>(), count) }
    }
}

impl Drop for GpuMapping {
    fn drop(&mut self) {
        self.driver.unmap_transfer(self.device, self.transfer);
    }
}

pub struct GpuTransfer {
    driver: DriverRef,
    device: RawDevice,
    handle: RawTransfer,
    size: u32,
}

impl GpuTransfer {
    pub fn new(driver: DriverRef, device: RawDevice, handle: RawTransfer, size: u32) -> Self {
        Self {
            driver,
            device,
            handle,
            size,
        }
    }

    pub fn get(&self) -> RawTransfer {
        self.handle
    }

    pub fn size(&self) -> u32 {
        self.size
    }

    /// `cycle` lets the driver hand out fresh storage when the previous contents are in flight.
    pub fn map(&self, cycle: bool, operation: &str) -> Result<GpuMapping, String> {
        let data = self
            .driver
            .map_transfer(self.device, self.handle, cycle, operation)?;
        Ok(GpuMapping {
            driver: self.driver.clone(),
            device: self.device,
            transfer: self.handle,
            data,
        })
    }

    pub fn upload_to(
        &self,
        pass: RawCopyPass,
        destination: &GpuBuffer,
        offset: u32,
        size: u32,
        cycle: bool,
    ) {
        self.driver
            .upload_buffer(pass, self.handle, offset, destination.handle, size, cycle);
    }
}

impl Drop for GpuTransfer {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            self.driver.release_transfer(self.device, self.handle);
        }
    }
}

/// How a presenting command buffer got its drawable.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PresentAcquire {
    DrawableReady,
    HeadlessProceed,
    SkipFrame,
}

/// Frame timing for presentation.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub enum PresentTiming {
    /// Static: canonical alpha.
    #[default]
    Canonical,
    /// Fixed alpha.
    Fixed(f32),
    /// Live alpha sampled after acquisition against the shim clock.
    Live { frame_start_ns: u64, period_ns: i64 },
}

impl PresentTiming {
    pub fn is_live(&self) -> bool {
        matches!(self, Self::Live { .. })
    }
}

/// One presentation request. `capture_path` is borrowed for the call.
#[derive(Clone, Copy, Debug)]
pub struct PresentRequest<'a> {
    /// Block for the swapchain and any display tick. False skips the frame when the GPU is busy:
    /// nothing is uploaded or rendered, and no fence is waited on.
    pub wait: bool,
    pub timing: PresentTiming,
    /// Empty: one full-window blit. Otherwise the left half shows this (already oriented)
    /// surface and the right half the presented game image.
    pub left_pane: GpuSurface,
    /// One-shot PNG of the full-window composition submitted to the drawable.
    pub capture_path: Option<&'a str>,
}

impl Default for PresentRequest<'_> {
    fn default() -> Self {
        Self {
            wait: false,
            timing: PresentTiming::Canonical,
            left_pane: GpuSurface::default(),
            capture_path: None,
        }
    }
}

impl PresentRequest<'_> {
    /// Live motion alpha is the elapsed fraction of the native frame period.
    pub fn sample_alpha(&self, driver: &dyn GpuDriver) -> Option<f32> {
        match self.timing {
            PresentTiming::Canonical => None,
            PresentTiming::Fixed(alpha) => Some(alpha),
            PresentTiming::Live {
                frame_start_ns,
                period_ns,
            } => {
                if period_ns <= 0 {
                    return Some(1.0);
                }
                let now = driver.monotonic_ns();
                let elapsed = (i128::from(now) - i128::from(frame_start_ns)) as f64 / 1.0e9;
                let period = period_ns as f64 / 1.0e9;
                Some((elapsed / period).clamp(0.0, 1.0) as f32)
            }
        }
    }
}

/// The device, window claim and sampler every GPU object shares.
pub struct GpuContext {
    driver: DriverRef,
    device: RawDevice,
    window: Option<RawWindow>,
    claimed: bool,
    vsync: bool,
    sampler: Option<GpuSampler>,
}

impl GpuContext {
    /// SDL video must be initialized. Window may be absent for offscreen use.
    pub fn new(driver: DriverRef, window: Option<RawWindow>, vsync: bool) -> Result<Self, String> {
        let device = driver.create_device("Create SDL GPU device")?;
        let mut claimed = false;
        let mut sampler = None;
        let initialized = (|| -> Result<(), String> {
            if window.is_some() {
                driver.claim_window(device, "Claim GPU window")?;
                claimed = true;
                driver.set_swapchain_mode(device, vsync, "Set GPU presentation mode")?;
            }
            sampler = Some(GpuSampler::new(
                driver.clone(),
                device,
                driver.create_sampler(device, "Create GPU sampler")?,
            ));
            Ok(())
        })();
        if let Err(error) = initialized {
            drop(sampler.take());
            if claimed {
                driver.release_window(device);
            }
            driver.destroy_device(device);
            return Err(error);
        }
        Ok(Self {
            driver,
            device,
            window,
            claimed,
            vsync,
            sampler,
        })
    }

    pub fn begin_command(&self) -> Result<GpuCommand, String> {
        let command = self
            .driver
            .acquire_command(self.device, "Acquire GPU command buffer")?;
        Ok(GpuCommand {
            driver: self.driver.clone(),
            device: self.device,
            command: Some(command),
            swapchain: GpuSurface::default(),
            acquired: false,
        })
    }

    pub fn create_buffer(&self, bytes: u32) -> Result<GpuBuffer, String> {
        let handle = self
            .driver
            .create_buffer(self.device, bytes, "Create GPU storage buffer")?;
        Ok(GpuBuffer::new(self.driver.clone(), self.device, handle))
    }

    pub fn create_texture(
        &self,
        format: TextureFormat,
        width: u32,
        height: u32,
    ) -> Result<GpuTexture, String> {
        let handle = self.driver.create_texture(
            self.device,
            format,
            width,
            height,
            "Create GPU render texture",
        )?;
        Ok(GpuTexture::new(
            self.driver.clone(),
            self.device,
            handle,
            format,
            width,
            height,
        ))
    }

    pub fn create_pipeline(
        &self,
        vertex: ShaderKind,
        fragment: ShaderKind,
        format: TextureFormat,
    ) -> Result<GpuPipeline, String> {
        let handle = self.driver.create_pipeline(
            self.device,
            vertex,
            fragment,
            format,
            "Create GPU graphics pipeline",
        )?;
        Ok(GpuPipeline::new(self.driver.clone(), self.device, handle))
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
        let handle = self.driver.create_user_pipeline(
            self.device,
            code,
            bytes,
            entry,
            samplers,
            buffers,
            uniforms,
            format,
            "Create GPU graphics pipeline",
        )?;
        Ok(GpuPipeline::new(self.driver.clone(), self.device, handle))
    }

    pub fn create_transfer(&self, bytes: u32, usage: TransferUsage) -> Result<GpuTransfer, String> {
        let handle =
            self.driver
                .create_transfer(self.device, bytes, usage, "Create GPU transfer buffer")?;
        Ok(GpuTransfer::new(
            self.driver.clone(),
            self.device,
            handle,
            bytes,
        ))
    }

    pub fn wait_idle(&self) {
        self.driver.wait_idle(self.device);
    }

    pub fn is_metal(&self) -> bool {
        self.driver.is_metal(self.device)
    }

    pub fn driver_name(&self) -> String {
        self.driver.driver_name(self.device)
    }

    pub fn driver(&self) -> &dyn GpuDriver {
        &*self.driver
    }

    pub fn driver_ref(&self) -> DriverRef {
        self.driver.clone()
    }

    pub fn device(&self) -> RawDevice {
        self.device
    }

    pub fn window(&self) -> Option<RawWindow> {
        self.window
    }

    pub fn sampler(&self) -> RawSampler {
        self.sampler
            .as_ref()
            .map(GpuSampler::get)
            .unwrap_or_default()
    }

    pub fn vsync(&self) -> bool {
        self.vsync
    }

    pub fn swapchain_format(&self) -> Option<TextureFormat> {
        self.driver.swapchain_format(self.device)
    }
}

impl Drop for GpuContext {
    fn drop(&mut self) {
        self.wait_idle();
        drop(self.sampler.take());
        if self.claimed {
            self.driver.release_window(self.device);
        }
        self.driver.destroy_device(self.device);
    }
}

/// One GPU command buffer. Dropping submits when a swapchain texture was acquired (the driver
/// requires it) and cancels otherwise.
pub struct GpuCommand {
    driver: Rc<dyn GpuDriver>,
    device: RawDevice,
    command: Option<RawCommand>,
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
            if ctx.window().is_none() {
                return Ok(PresentAcquire::SkipFrame);
            }
            let mut out = SwapchainOut::default();
            let acquired = self.driver.acquire_swapchain(
                self.raw(),
                false,
                &mut out,
                "Try acquiring GPU swapchain",
            )?;
            if !acquired {
                return Ok(PresentAcquire::SkipFrame);
            }
            self.acquired = true;
            self.swapchain = GpuSurface {
                texture: RawTexture(out.texture),
                width: out.width,
                height: out.height,
            };
            return Ok(PresentAcquire::DrawableReady);
        }
        if timed && !pace_prepared {
            pacer.wait_tick(ctx.vsync());
        }
        if ctx.window().is_some() {
            let mut out = SwapchainOut::default();
            let acquired = self.driver.acquire_swapchain(
                self.raw(),
                true,
                &mut out,
                if timed {
                    "Acquire timed GPU swapchain"
                } else {
                    "Acquire GPU swapchain"
                },
            )?;
            self.acquired = acquired;
            if acquired {
                self.swapchain = GpuSurface {
                    texture: RawTexture(out.texture),
                    width: out.width,
                    height: out.height,
                };
                return Ok(PresentAcquire::DrawableReady);
            }
        }
        Ok(PresentAcquire::HeadlessProceed)
    }

    pub fn raw(&self) -> RawCommand {
        self.command.unwrap_or_default()
    }

    pub fn driver(&self) -> &dyn GpuDriver {
        &*self.driver
    }

    pub fn begin_copy_pass(&mut self, operation: &str) -> Result<RawCopyPass, String> {
        self.driver.begin_copy_pass(self.raw(), operation)
    }

    pub fn begin_render_pass(
        &mut self,
        target: RawTexture,
        load: bool,
    ) -> Result<RawRenderPass, String> {
        self.driver
            .begin_render_pass(self.raw(), target, load, "Begin GPU render pass")
    }

    /// Full-surface nearest copy; `destination` must be at least as large as `source`.
    pub fn blit_copy(&mut self, source: GpuSurface, destination: GpuSurface) {
        self.driver.blit(
            self.raw(),
            &Blit {
                source: source.texture.0,
                source_x: 0,
                source_y: 0,
                source_width: source.width,
                source_height: source.height,
                destination: destination.texture.0,
                x: 0,
                y: 0,
                width: source.width,
                height: source.height,
                load_op: LoadOp::DontCare as u32,
                filter: Filter::Nearest as u32,
                clear_color: [0.0, 0.0, 0.0, 1.0],
            },
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
        overlay: bool,
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
            composition = capture_texture.get();
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
            self.driver.blit(
                self.raw(),
                &Blit {
                    source: source.0,
                    source_x: viewport.source_x,
                    source_y: viewport.source_y,
                    source_width: viewport.source_width,
                    source_height: viewport.source_height,
                    destination: composition.0,
                    x: offset + viewport.x,
                    y: viewport.y,
                    width: viewport.width,
                    height: viewport.height,
                    load_op: if pane == 0 {
                        LoadOp::Clear as u32
                    } else {
                        LoadOp::Load as u32
                    },
                    filter: if linear && scale_mode != VideoScaleMode::AutoInteger {
                        Filter::Linear as u32
                    } else {
                        Filter::Nearest as u32
                    },
                    clear_color: [0.0, 0.0, 0.0, 1.0],
                },
            );
        }
        if overlay {
            self.driver.invoke_overlay(self.raw(), composition, w, h);
        }
        if !capture_texture.is_valid() {
            self.submit()?;
            return Ok(true);
        }
        // Download the exact full-window composition submitted to the drawable.
        self.driver.blit(
            self.raw(),
            &Blit {
                source: composition.0,
                source_x: 0,
                source_y: 0,
                source_width: w,
                source_height: h,
                destination: self.swapchain.texture.0,
                x: 0,
                y: 0,
                width: w,
                height: h,
                load_op: LoadOp::DontCare as u32,
                filter: Filter::Nearest as u32,
                clear_color: [0.0, 0.0, 0.0, 1.0],
            },
        );
        let copy = self.begin_copy_pass("Begin window capture download")?;
        let download = capture_download
            .as_ref()
            .expect("capture download created with the capture texture");
        self.driver
            .download_texture(copy, composition, w, h, download.get());
        self.driver.end_copy_pass(copy);
        self.submit_and_wait()?;
        let mut mapping = download.map(false, "Map window capture")?;
        let pixels = mapping.words(usize::try_from(u64::from(w) * u64::from(h)).unwrap());
        let path = capture_path.expect("capture path checked above");
        self.driver.save_surface(
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

    pub fn submit(&mut self) -> Result<(), String> {
        let Some(command) = self.command.take() else {
            return Ok(());
        };
        self.driver.submit_command(command, "Submit GPU frame")
    }

    /// Submits and blocks until the GPU finished this command.
    pub fn submit_and_wait(&mut self) -> Result<(), String> {
        let Some(command) = self.command.take() else {
            return Ok(());
        };
        self.driver.submit_and_wait(
            self.device,
            command,
            "Submit GPU command buffer",
            "Wait for GPU fence",
        )
    }
}

impl Drop for GpuCommand {
    fn drop(&mut self) {
        if let Some(command) = self.command.take() {
            if self.acquired {
                let _ = self.driver.submit_command(command, "Submit GPU frame");
            } else {
                self.driver.cancel_command(command);
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
        cmd.driver.download_texture(
            copy,
            surface.texture,
            surface.width,
            surface.height,
            download.get(),
        );
        cmd.driver.end_copy_pass(copy);
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
pub fn sampler_binding(texture: RawTexture, sampler: RawSampler) -> TextureSamplerBinding {
    TextureSamplerBinding {
        texture: texture.0,
        sampler: sampler.0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::gpu::driver::{DriverRef, RawWindow};
    use crate::gpu::mock_driver::{MockGpuDriver, MockSwapchain};
    use crate::gpu::pacing::DisplayPacer;

    fn window() -> Option<RawWindow> {
        Some(RawWindow(std::ptr::dangling_mut::<c_void>()))
    }

    fn context(mock: &std::rc::Rc<MockGpuDriver>) -> GpuContext {
        let driver: DriverRef = mock.clone();
        GpuContext::new(driver, window(), false).expect("mock context")
    }

    #[test]
    fn command_drop_submits_when_acquired_and_cancels_otherwise() {
        let mock = std::rc::Rc::new(MockGpuDriver::default());
        let ctx = context(&mock);
        let mut pacer = DisplayPacer::new(ctx.driver_ref());

        mock.set_swapchain(MockSwapchain::Drawable {
            width: 8,
            height: 8,
        });
        {
            let mut cmd = ctx.begin_command().expect("command");
            assert_eq!(
                cmd.acquire_for_present(&ctx, &mut pacer, &PresentRequest::default())
                    .expect("acquire"),
                PresentAcquire::DrawableReady
            );
        }
        assert_eq!(mock.call_count("submit_command"), 1);
        assert_eq!(mock.call_count("cancel_command"), 0);

        mock.set_swapchain(MockSwapchain::None);
        {
            let mut cmd = ctx.begin_command().expect("command");
            assert_eq!(
                cmd.acquire_for_present(&ctx, &mut pacer, &PresentRequest::default())
                    .expect("acquire"),
                PresentAcquire::SkipFrame
            );
        }
        assert_eq!(mock.call_count("submit_command"), 1);
        assert_eq!(mock.call_count("cancel_command"), 1);
    }

    #[test]
    fn context_cleans_up_after_a_failed_sampler() {
        let mock = std::rc::Rc::new(MockGpuDriver::default());
        mock.fail_after("sampler", 0);
        let driver: DriverRef = mock.clone();
        let error = GpuContext::new(driver, window(), false)
            .err()
            .expect("sampler must fail");
        assert_eq!(error, "Create GPU sampler: mock failure");
        assert_eq!(mock.call_count("release_window"), 1);
        assert_eq!(mock.call_count("destroy_device"), 1);
    }

    #[test]
    fn context_cleans_up_after_a_failed_window_claim() {
        let mock = std::rc::Rc::new(MockGpuDriver::default());
        mock.fail_after("claim", 0);
        let driver: DriverRef = mock.clone();
        let error = GpuContext::new(driver, window(), false)
            .err()
            .expect("claim must fail");
        assert_eq!(error, "Claim GPU window: mock failure");
        assert_eq!(mock.call_count("release_window"), 0);
        assert_eq!(mock.call_count("destroy_device"), 1);
    }

    #[test]
    fn context_reports_device_and_swapchain_mode_failures() {
        let mock = std::rc::Rc::new(MockGpuDriver::default());
        mock.fail_after("device", 0);
        let driver: DriverRef = mock.clone();
        let error = GpuContext::new(driver, None, false)
            .err()
            .expect("device must fail");
        assert_eq!(error, "Create SDL GPU device: mock failure");

        let mock = std::rc::Rc::new(MockGpuDriver::default());
        mock.fail_after("swapchain_mode", 0);
        let driver: DriverRef = mock.clone();
        let error = GpuContext::new(driver, window(), false)
            .err()
            .expect("mode must fail");
        assert_eq!(error, "Set GPU presentation mode: mock failure");
        assert_eq!(mock.call_count("release_window"), 1);
        assert_eq!(mock.call_count("destroy_device"), 1);
    }

    #[test]
    fn live_alpha_is_sampled_on_the_driver_clock() {
        let mock = std::rc::Rc::new(MockGpuDriver::default());
        let driver: DriverRef = mock.clone();
        mock.set_monotonic_ns(1_005_000_000);
        let live = PresentRequest {
            timing: PresentTiming::Live {
                frame_start_ns: 1_000_000_000,
                period_ns: 10_000_000,
            },
            ..PresentRequest::default()
        };
        let alpha = live.sample_alpha(&*driver).expect("live alpha");
        assert!((alpha - 0.5).abs() < 1e-5, "alpha {alpha}");
        // Past the period and before the start both clamp into [0, 1].
        mock.set_monotonic_ns(1_100_000_000);
        assert_eq!(live.sample_alpha(&*driver), Some(1.0));
        mock.set_monotonic_ns(900_000_000);
        assert_eq!(live.sample_alpha(&*driver), Some(0.0));
        let zero = PresentRequest {
            timing: PresentTiming::Live {
                frame_start_ns: 0,
                period_ns: 0,
            },
            ..PresentRequest::default()
        };
        assert_eq!(zero.sample_alpha(&*driver), Some(1.0));
        let fixed = PresentRequest {
            timing: PresentTiming::Fixed(0.25),
            ..PresentRequest::default()
        };
        assert_eq!(fixed.sample_alpha(&*driver), Some(0.25));
        assert_eq!(PresentRequest::default().sample_alpha(&*driver), None);
    }

    #[test]
    fn readback_requires_a_complete_output_buffer() {
        let mock = std::rc::Rc::new(MockGpuDriver::default());
        let ctx = context(&mock);
        let surface = GpuSurface {
            texture: RawTexture(0x9000 as *mut c_void),
            width: 4,
            height: 4,
        };
        let mut cmd = ctx.begin_command().expect("command");
        let mut readback = ReadbackBuffer::default();
        let mut small = [0u32; 15];
        let error = readback
            .finish(&mut cmd, surface, &ctx, &mut small)
            .expect_err("short output must fail");
        assert_eq!(error, "Incomplete GPU output buffer");
        let mut output = [0u32; 16];
        readback
            .finish(&mut cmd, surface, &ctx, &mut output)
            .expect("full output");
        assert_eq!(mock.call_count("download_texture"), 1);
    }
}
