//! In-memory [`GpuDriver`] for `cargo test`: records every call and lets tests inject failures.

use super::driver::{
    Blit, GpuDriver, RawBuffer, RawCommand, RawCopyPass, RawDevice, RawPacing, RawPipeline,
    RawRenderPass, RawSampler, RawTexture, RawTransfer, ShaderKind, SwapchainOut, TextureFormat,
    TextureSamplerBinding, TransferUsage,
};
use std::cell::RefCell;
use std::collections::{BTreeMap, HashMap, VecDeque};
use std::ffi::c_void;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MockSwapchain {
    Drawable { width: u32, height: u32 },
    None,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SavedSurface {
    pub path: String,
    pub width: u32,
    pub height: u32,
    pub pixel_format: u32,
    pub png: bool,
}

#[derive(Default)]
struct State {
    next_handle: usize,
    calls: Vec<String>,
    released: Vec<String>,
    failures: HashMap<&'static str, usize>,
    transfers: BTreeMap<usize, Vec<u8>>,
    textures: Vec<(TextureFormat, u32, u32)>,
    swapchain: Option<MockSwapchain>,
    swapchain_format: Option<TextureFormat>,
    wait_results: VecDeque<bool>,
    poll_results: VecDeque<bool>,
    pacer_enabled: bool,
    saved: Vec<SavedSurface>,
    submit_ok: bool,
    submit_and_wait_status: i32,
    frames_in_flight: Vec<u32>,
    display_hz: f64,
    monotonic_ns: u64,
}

pub struct MockGpuDriver {
    state: RefCell<State>,
}

struct HandleFactory<'a> {
    state: &'a mut State,
}

impl HandleFactory<'_> {
    fn next(&mut self) -> *mut c_void {
        self.state.next_handle += 0x10;
        self.state.next_handle as *mut c_void
    }
}

impl Default for MockGpuDriver {
    fn default() -> Self {
        Self {
            state: RefCell::new(State {
                next_handle: 0x1000,
                swapchain: None,
                swapchain_format: Some(TextureFormat::Bgra8Unorm),
                pacer_enabled: true,
                submit_ok: true,
                display_hz: 60.0,
                ..State::default()
            }),
        }
    }
}

impl MockGpuDriver {
    fn next_handle(state: &mut State) -> *mut c_void {
        let mut factory = HandleFactory { state };
        factory.next()
    }

    fn record(state: &mut State, call: impl Into<String>) {
        state.calls.push(call.into());
    }

    /// Fails the operation once `successes` earlier calls succeeded.
    pub fn fail_after(&self, operation: &'static str, successes: usize) {
        self.state
            .borrow_mut()
            .failures
            .insert(operation, successes);
    }

    pub fn clear_failures(&self) {
        self.state.borrow_mut().failures.clear();
    }

    fn should_fail(state: &mut State, operation: &'static str) -> bool {
        match state.failures.get_mut(operation) {
            Some(remaining) if *remaining == 0 => true,
            Some(remaining) => {
                *remaining -= 1;
                false
            }
            None => false,
        }
    }

    pub fn calls(&self) -> Vec<String> {
        self.state.borrow().calls.clone()
    }

    pub fn call_count(&self, prefix: &str) -> usize {
        self.state
            .borrow()
            .calls
            .iter()
            .filter(|call| call.starts_with(prefix))
            .count()
    }

    pub fn released(&self) -> Vec<String> {
        self.state.borrow().released.clone()
    }

    pub fn textures(&self) -> Vec<(TextureFormat, u32, u32)> {
        self.state.borrow().textures.clone()
    }

    pub fn saved_surfaces(&self) -> Vec<SavedSurface> {
        self.state.borrow().saved.clone()
    }

    pub fn set_swapchain(&self, swapchain: MockSwapchain) {
        self.state.borrow_mut().swapchain = Some(swapchain);
    }

    pub fn set_swapchain_format(&self, format: Option<TextureFormat>) {
        self.state.borrow_mut().swapchain_format = format;
    }

    pub fn set_wait_results(&self, results: impl IntoIterator<Item = bool>) {
        self.state.borrow_mut().wait_results = results.into_iter().collect();
    }

    pub fn set_poll_results(&self, results: impl IntoIterator<Item = bool>) {
        self.state.borrow_mut().poll_results = results.into_iter().collect();
    }

    pub fn set_pacer_enabled(&self, enabled: bool) {
        self.state.borrow_mut().pacer_enabled = enabled;
    }

    pub fn set_submit_and_wait_status(&self, status: i32) {
        self.state.borrow_mut().submit_and_wait_status = status;
    }

    pub fn frames_in_flight(&self) -> Vec<u32> {
        self.state.borrow().frames_in_flight.clone()
    }

    pub fn set_display_hz(&self, hz: f64) {
        self.state.borrow_mut().display_hz = hz;
    }

    pub fn set_monotonic_ns(&self, nanos: u64) {
        self.state.borrow_mut().monotonic_ns = nanos;
    }

    fn fail(_state: &mut State, operation: &str) -> String {
        format!("{operation}: mock failure")
    }
}

impl GpuDriver for MockGpuDriver {
    fn create_device(&self, operation: &str) -> Result<RawDevice, String> {
        let mut state = self.state.borrow_mut();
        Self::record(&mut state, "create_device");
        if Self::should_fail(&mut state, "device") {
            return Err(Self::fail(&mut state, operation));
        }
        Ok(RawDevice(Self::next_handle(&mut state)))
    }

    fn claim_window(&self, _device: RawDevice, operation: &str) -> Result<(), String> {
        let mut state = self.state.borrow_mut();
        Self::record(&mut state, "claim_window");
        if Self::should_fail(&mut state, "claim") {
            return Err(Self::fail(&mut state, operation));
        }
        Ok(())
    }

    fn set_swapchain_mode(
        &self,
        _device: RawDevice,
        _vsync: bool,
        operation: &str,
    ) -> Result<(), String> {
        let mut state = self.state.borrow_mut();
        Self::record(&mut state, "set_swapchain_mode");
        if Self::should_fail(&mut state, "swapchain_mode") {
            return Err(Self::fail(&mut state, operation));
        }
        Ok(())
    }

    fn release_window(&self, _device: RawDevice) {
        Self::record(&mut self.state.borrow_mut(), "release_window");
    }

    fn destroy_device(&self, _device: RawDevice) {
        Self::record(&mut self.state.borrow_mut(), "destroy_device");
    }

    fn wait_idle(&self, _device: RawDevice) {
        Self::record(&mut self.state.borrow_mut(), "wait_idle");
    }

    fn is_metal(&self, _device: RawDevice) -> bool {
        false
    }

    fn driver_name(&self, _device: RawDevice) -> String {
        "mock".to_owned()
    }

    fn display_hz(&self) -> f64 {
        self.state.borrow().display_hz
    }

    fn swapchain_format(&self, _device: RawDevice) -> Option<TextureFormat> {
        self.state.borrow().swapchain_format
    }

    fn create_sampler(&self, _device: RawDevice, operation: &str) -> Result<RawSampler, String> {
        let mut state = self.state.borrow_mut();
        Self::record(&mut state, "create_sampler");
        if Self::should_fail(&mut state, "sampler") {
            return Err(Self::fail(&mut state, operation));
        }
        Ok(RawSampler(Self::next_handle(&mut state)))
    }

    fn create_buffer(
        &self,
        _device: RawDevice,
        bytes: u32,
        operation: &str,
    ) -> Result<RawBuffer, String> {
        let mut state = self.state.borrow_mut();
        Self::record(&mut state, format!("create_buffer:{bytes}"));
        if Self::should_fail(&mut state, "buffer") {
            return Err(Self::fail(&mut state, operation));
        }
        Ok(RawBuffer(Self::next_handle(&mut state)))
    }

    fn create_texture(
        &self,
        _device: RawDevice,
        format: TextureFormat,
        width: u32,
        height: u32,
        operation: &str,
    ) -> Result<RawTexture, String> {
        let mut state = self.state.borrow_mut();
        Self::record(&mut state, format!("create_texture:{width}x{height}"));
        if Self::should_fail(&mut state, "texture") {
            return Err(Self::fail(&mut state, operation));
        }
        let handle = Self::next_handle(&mut state);
        state.textures.push((format, width, height));
        Ok(RawTexture(handle))
    }

    fn create_transfer(
        &self,
        _device: RawDevice,
        bytes: u32,
        usage: TransferUsage,
        operation: &str,
    ) -> Result<RawTransfer, String> {
        let mut state = self.state.borrow_mut();
        Self::record(&mut state, format!("create_transfer:{bytes}"));
        if Self::should_fail(&mut state, "transfer") {
            return Err(Self::fail(&mut state, operation));
        }
        let handle = Self::next_handle(&mut state);
        state
            .transfers
            .insert(handle as usize, vec![0; bytes as usize]);
        let _ = usage;
        Ok(RawTransfer(handle))
    }

    fn create_pipeline(
        &self,
        _device: RawDevice,
        vertex: ShaderKind,
        fragment: ShaderKind,
        _format: TextureFormat,
        operation: &str,
    ) -> Result<RawPipeline, String> {
        let mut state = self.state.borrow_mut();
        Self::record(
            &mut state,
            format!("create_pipeline:{vertex:?}:{fragment:?}"),
        );
        if Self::should_fail(&mut state, "pipeline") {
            return Err(Self::fail(&mut state, operation));
        }
        Ok(RawPipeline(Self::next_handle(&mut state)))
    }

    #[allow(clippy::too_many_arguments)]
    fn create_user_pipeline(
        &self,
        _device: RawDevice,
        code: &[u32],
        bytes: usize,
        entry: &str,
        samplers: u32,
        buffers: u32,
        uniforms: u32,
        _format: TextureFormat,
        operation: &str,
    ) -> Result<RawPipeline, String> {
        let mut state = self.state.borrow_mut();
        Self::record(
            &mut state,
            format!("create_user_pipeline:{entry}:{bytes}:{samplers}:{buffers}:{uniforms}"),
        );
        if code.is_empty() {
            return Err("create_user_pipeline: empty shader".to_owned());
        }
        if Self::should_fail(&mut state, "pipeline") {
            return Err(Self::fail(&mut state, operation));
        }
        Ok(RawPipeline(Self::next_handle(&mut state)))
    }

    fn release_buffer(&self, _device: RawDevice, buffer: RawBuffer) {
        self.state
            .borrow_mut()
            .released
            .push(format!("buffer:{:?}", buffer.0));
    }

    fn release_texture(&self, _device: RawDevice, texture: RawTexture) {
        self.state
            .borrow_mut()
            .released
            .push(format!("texture:{:?}", texture.0));
    }

    fn release_transfer(&self, _device: RawDevice, transfer: RawTransfer) {
        self.state
            .borrow_mut()
            .released
            .push(format!("transfer:{:?}", transfer.0));
    }

    fn release_sampler(&self, _device: RawDevice, sampler: RawSampler) {
        self.state
            .borrow_mut()
            .released
            .push(format!("sampler:{:?}", sampler.0));
    }

    fn release_pipeline(&self, _device: RawDevice, pipeline: RawPipeline) {
        self.state
            .borrow_mut()
            .released
            .push(format!("pipeline:{:?}", pipeline.0));
    }

    fn map_transfer(
        &self,
        _device: RawDevice,
        transfer: RawTransfer,
        _cycle: bool,
        operation: &str,
    ) -> Result<*mut c_void, String> {
        let mut state = self.state.borrow_mut();
        Self::record(&mut state, "map_transfer");
        if Self::should_fail(&mut state, "map") {
            return Err(Self::fail(&mut state, operation));
        }
        state
            .transfers
            .get_mut(&(transfer.0 as usize))
            .map(|bytes| bytes.as_mut_ptr().cast::<c_void>())
            .ok_or_else(|| "map_transfer: unknown transfer".to_owned())
    }

    fn unmap_transfer(&self, _device: RawDevice, _transfer: RawTransfer) {
        Self::record(&mut self.state.borrow_mut(), "unmap_transfer");
    }

    fn acquire_command(&self, _device: RawDevice, operation: &str) -> Result<RawCommand, String> {
        let mut state = self.state.borrow_mut();
        Self::record(&mut state, "acquire_command");
        if Self::should_fail(&mut state, "command") {
            return Err(Self::fail(&mut state, operation));
        }
        Ok(RawCommand(Self::next_handle(&mut state)))
    }

    fn submit_command(&self, _command: RawCommand, operation: &str) -> Result<(), String> {
        let mut state = self.state.borrow_mut();
        Self::record(&mut state, "submit_command");
        if !state.submit_ok {
            return Err(Self::fail(&mut state, operation));
        }
        Ok(())
    }

    fn cancel_command(&self, _command: RawCommand) {
        Self::record(&mut self.state.borrow_mut(), "cancel_command");
    }

    fn submit_and_wait(
        &self,
        _device: RawDevice,
        _command: RawCommand,
        submit_operation: &str,
        wait_operation: &str,
    ) -> Result<(), String> {
        let mut state = self.state.borrow_mut();
        Self::record(&mut state, "submit_and_wait");
        match state.submit_and_wait_status {
            0 => Ok(()),
            1 => Err(Self::fail(&mut state, submit_operation)),
            _ => Err(Self::fail(&mut state, wait_operation)),
        }
    }

    fn acquire_swapchain(
        &self,
        _command: RawCommand,
        _wait: bool,
        out: &mut SwapchainOut,
        operation: &str,
    ) -> Result<bool, String> {
        let mut state = self.state.borrow_mut();
        Self::record(&mut state, "acquire_swapchain");
        if Self::should_fail(&mut state, "swapchain") {
            return Err(Self::fail(&mut state, operation));
        }
        match state.swapchain {
            Some(MockSwapchain::Drawable { width, height }) => {
                *out = SwapchainOut {
                    texture: Self::next_handle(&mut state),
                    width,
                    height,
                };
                Ok(true)
            }
            _ => Ok(false),
        }
    }

    fn begin_copy_pass(
        &self,
        _command: RawCommand,
        _operation: &str,
    ) -> Result<RawCopyPass, String> {
        let mut state = self.state.borrow_mut();
        Self::record(&mut state, "begin_copy_pass");
        Ok(RawCopyPass(Self::next_handle(&mut state)))
    }

    fn end_copy_pass(&self, _pass: RawCopyPass) {
        Self::record(&mut self.state.borrow_mut(), "end_copy_pass");
    }

    fn begin_render_pass(
        &self,
        _command: RawCommand,
        _target: RawTexture,
        load: bool,
        _operation: &str,
    ) -> Result<RawRenderPass, String> {
        let mut state = self.state.borrow_mut();
        Self::record(&mut state, format!("begin_render_pass:{load}"));
        Ok(RawRenderPass(Self::next_handle(&mut state)))
    }

    fn end_render_pass(&self, _pass: RawRenderPass) {
        Self::record(&mut self.state.borrow_mut(), "end_render_pass");
    }

    fn upload_buffer(
        &self,
        _pass: RawCopyPass,
        _source: RawTransfer,
        _source_offset: u32,
        _destination: RawBuffer,
        _size: u32,
        _cycle: bool,
    ) {
        Self::record(&mut self.state.borrow_mut(), "upload_buffer");
    }

    fn upload_texture(
        &self,
        _pass: RawCopyPass,
        _source: RawTransfer,
        _source_offset: u32,
        _width: u32,
        _height: u32,
        _destination: RawTexture,
        _cycle: bool,
    ) {
        Self::record(&mut self.state.borrow_mut(), "upload_texture");
    }

    fn download_texture(
        &self,
        _pass: RawCopyPass,
        _source: RawTexture,
        _width: u32,
        _height: u32,
        _destination: RawTransfer,
    ) {
        Self::record(&mut self.state.borrow_mut(), "download_texture");
    }

    fn push_vertex_uniforms(&self, _command: RawCommand, _slot: u32, _data: &[u8]) {
        Self::record(&mut self.state.borrow_mut(), "push_vertex_uniforms");
    }

    fn push_fragment_uniforms(&self, _command: RawCommand, _slot: u32, _data: &[u8]) {
        Self::record(&mut self.state.borrow_mut(), "push_fragment_uniforms");
    }

    fn bind_pipeline(&self, _pass: RawRenderPass, _pipeline: RawPipeline) {
        Self::record(&mut self.state.borrow_mut(), "bind_pipeline");
    }

    fn bind_vertex_storage_buffers(
        &self,
        _pass: RawRenderPass,
        _first_slot: u32,
        _buffers: &[RawBuffer],
    ) {
        Self::record(&mut self.state.borrow_mut(), "bind_vertex_storage_buffers");
    }

    fn bind_fragment_storage_buffers(
        &self,
        _pass: RawRenderPass,
        _first_slot: u32,
        _buffers: &[RawBuffer],
    ) {
        Self::record(
            &mut self.state.borrow_mut(),
            "bind_fragment_storage_buffers",
        );
    }

    fn bind_fragment_samplers(
        &self,
        _pass: RawRenderPass,
        _first_slot: u32,
        _bindings: &[TextureSamplerBinding],
    ) {
        Self::record(&mut self.state.borrow_mut(), "bind_fragment_samplers");
    }

    fn set_scissor(&self, _pass: RawRenderPass, _x: i32, _y: i32, _w: i32, _h: i32) {
        Self::record(&mut self.state.borrow_mut(), "set_scissor");
    }

    fn draw(&self, _pass: RawRenderPass, _vertices: u32, _instances: u32) {
        Self::record(&mut self.state.borrow_mut(), "draw");
    }

    fn blit(&self, _command: RawCommand, _blit: &Blit) {
        Self::record(&mut self.state.borrow_mut(), "blit");
    }

    fn invoke_overlay(&self, _command: RawCommand, _target: RawTexture, _w: u32, _h: u32) {
        Self::record(&mut self.state.borrow_mut(), "invoke_overlay");
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
        _create_operation: &str,
        _save_operation: &str,
    ) -> Result<(), String> {
        let mut state = self.state.borrow_mut();
        Self::record(&mut state, "save_surface");
        if Self::should_fail(&mut state, "save") {
            return Err(Self::fail(&mut state, "save_surface"));
        }
        if pixels.len() < (width * height) as usize {
            return Err("save_surface: incomplete pixels".to_owned());
        }
        state.saved.push(SavedSurface {
            path: path.to_owned(),
            width,
            height,
            pixel_format,
            png,
        });
        Ok(())
    }

    fn set_frames_in_flight(
        &self,
        _device: RawDevice,
        count: u32,
        operation: &str,
    ) -> Result<(), String> {
        let mut state = self.state.borrow_mut();
        Self::record(&mut state, format!("set_frames_in_flight:{count}"));
        state.frames_in_flight.push(count);
        if Self::should_fail(&mut state, "frames_in_flight") {
            return Err(Self::fail(&mut state, operation));
        }
        Ok(())
    }

    fn create_pacer(&self) -> Option<RawPacing> {
        let mut state = self.state.borrow_mut();
        Self::record(&mut state, "create_pacer");
        state
            .pacer_enabled
            .then(|| RawPacing(Self::next_handle(&mut state)))
    }

    fn destroy_pacer(&self, _pacer: RawPacing) {
        Self::record(&mut self.state.borrow_mut(), "destroy_pacer");
    }

    fn wait_pacer(&self, _pacer: RawPacing) -> bool {
        self.state
            .borrow_mut()
            .wait_results
            .pop_front()
            .unwrap_or(false)
    }

    fn poll_pacer(&self, _pacer: RawPacing) -> bool {
        self.state
            .borrow_mut()
            .poll_results
            .pop_front()
            .unwrap_or(false)
    }

    fn pacer_callback_hz(&self, _pacer: RawPacing) -> f64 {
        0.0
    }

    fn pacer_requested_hz(&self, _pacer: RawPacing) -> f64 {
        0.0
    }

    fn monotonic_ns(&self) -> u64 {
        self.state.borrow().monotonic_ns
    }

    fn ticks_ns(&self) -> u64 {
        self.state.borrow().monotonic_ns
    }
}
