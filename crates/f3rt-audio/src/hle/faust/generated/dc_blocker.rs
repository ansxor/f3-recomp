/* ------------------------------------------------------------
name: "dc_blocker"
Code generated with Faust 2.88.0 (https://faust.grame.fr)
Compilation options: -a crates/f3rt-audio/faust/architecture.rs -lang rust -fpga-mem-th 4 -ct 1 -cn DcBlockerDsp -es 1 -mcd 16 -mdd 1024 -mdy 33 -double -ftz 0
------------------------------------------------------------ */
// FAUST Rust architecture for f3rt DSP modules. faust/regen.sh runs:
//   faust -lang rust -a crates/f3rt-audio/faust/architecture.rs -cn <Class> [-double] <file>.dsp
// The generated module lives in crate::hle::faust::generated and takes the FAUST interfaces
// (FaustDsp, UI, Meta, FaustFloat, F32, F64, ParamIndex) from crate::hle::faust::support, mirroring
// runtime/audio/hle/faust/architecture.hpp + support.hpp.
//
// `use super::*` is the Rust counterpart of the C++ architecture's `#include "support.hpp"`: the
// generated file is declared in hle::faust::generated, which re-exports hle::faust's items.
#![allow(non_snake_case)]
#![allow(non_camel_case_types)]
#![allow(dead_code)]
#![allow(unused_variables)]
#![allow(unused_mut)]
#![allow(unused_parens)]
#![allow(non_upper_case_globals)]
// The generated body is machine output; its lints are suppressed the way architecture.hpp pushes
// and pops -Wunused-parameter around the class body.
#![allow(clippy::all)]

use super::*;



#[repr(C)]
pub struct DcBlockerDsp {
	fHslider0: FaustFloat,
	fHslider1: FaustFloat,
	fVec0: [F64;2],
	fRec0: [F64;2],
	fHslider2: FaustFloat,
	fVec1: [F64;2],
	fRec1: [F64;2],
	fSampleRate: i32,
}


pub const FAUST_INPUTS: usize = 2;
pub const FAUST_OUTPUTS: usize = 2;
pub const FAUST_ACTIVES: usize = 3;
pub const FAUST_PASSIVES: usize = 0;

impl DcBlockerDsp {
		
	pub fn new() -> DcBlockerDsp { 
		DcBlockerDsp {
			fHslider0: 0.0,
			fHslider1: 0.0,
			fVec0: [0.0;2],
			fRec0: [0.0;2],
			fHslider2: 0.0,
			fVec1: [0.0;2],
			fRec1: [0.0;2],
			fSampleRate: 0,
		}
	}
	pub fn metadata(&self, m: &mut dyn Meta) { 
		m.declare("compile_options", r"-a crates/f3rt-audio/faust/architecture.rs -lang rust -fpga-mem-th 4 -ct 1 -cn DcBlockerDsp -es 1 -mcd 16 -mdd 1024 -mdy 33 -double -ftz 0");
		m.declare("filename", r"dc_blocker.dsp");
		m.declare("name", r"dc_blocker");
	}

	pub fn get_sample_rate(&self) -> i32 { self.fSampleRate as i32}
	
	pub fn class_init(sample_rate: i32) {
		// Obtaining locks on 0 static var(s)
	}
	pub fn instance_reset_params(&mut self) {
		self.fHslider0 = (0.99578) as FaustFloat;
		self.fHslider1 = (1.0) as FaustFloat;
		self.fHslider2 = (1.0) as FaustFloat;
	}
	pub fn instance_clear(&mut self) {
		for l0 in 0..2 {
			self.fVec0[l0 as usize] = 0.0;
		}
		for l1 in 0..2 {
			self.fRec0[l1 as usize] = 0.0;
		}
		for l2 in 0..2 {
			self.fVec1[l2 as usize] = 0.0;
		}
		for l3 in 0..2 {
			self.fRec1[l3 as usize] = 0.0;
		}
	}
	pub fn instance_constants(&mut self, sample_rate: i32) {
		// Obtaining locks on 0 static var(s)
		self.fSampleRate = sample_rate;
	}
	pub fn instance_init(&mut self, sample_rate: i32) {
		self.instance_constants(sample_rate);
		self.instance_reset_params();
		self.instance_clear();
	}
	pub fn init(&mut self, sample_rate: i32) {
		DcBlockerDsp::class_init(sample_rate);
		self.instance_init(sample_rate);
	}
	
	pub fn build_user_interface(&self, ui_interface: &mut dyn UI<FaustFloat>) {
		Self::build_user_interface_static(ui_interface);
	}
	
	pub fn build_user_interface_static(ui_interface: &mut dyn UI<FaustFloat>) {
		ui_interface.open_vertical_box("dc_blocker");
		ui_interface.add_horizontal_slider("gain_l", ParamIndex(0), 1.0, 0.0, 64.0, 0.0001);
		ui_interface.add_horizontal_slider("gain_r", ParamIndex(1), 1.0, 0.0, 64.0, 0.0001);
		ui_interface.add_horizontal_slider("r", ParamIndex(2), 0.99578, 0.0, 1.0, 1e-06);
		ui_interface.close_box();
	}
	
	pub fn get_param(&self, param: ParamIndex) -> Option<FaustFloat> {
		match param.0 {
			2 => Some(self.fHslider0),
			0 => Some(self.fHslider1),
			1 => Some(self.fHslider2),
			_ => None,
		}
	}
	
	pub fn set_param(&mut self, param: ParamIndex, value: FaustFloat) {
		match param.0 {
			2 => { self.fHslider0 = value }
			0 => { self.fHslider1 = value }
			1 => { self.fHslider2 = value }
			_ => {}
		}
	}
	
	pub fn compute(
		&mut self,
		count: usize,
		inputs: &[impl AsRef<[FaustFloat]>],
		outputs: &mut[impl AsMut<[FaustFloat]>],
	) {
		
		// Obtaining locks on 0 static var(s)
		let [inputs0, inputs1, .. ] = inputs.as_ref() else { panic!("wrong number of input buffers"); };
		let inputs0 = inputs0.as_ref()[..count].iter();
		let inputs1 = inputs1.as_ref()[..count].iter();
		let [outputs0, outputs1, .. ] = outputs.as_mut() else { panic!("wrong number of output buffers"); };
		let outputs0 = outputs0.as_mut()[..count].iter_mut();
		let outputs1 = outputs1.as_mut()[..count].iter_mut();
		let mut fSlow0: F64 = (self.fHslider0) as F64;
		let mut fSlow1: F64 = (self.fHslider1) as F64;
		let mut fSlow2: F64 = (self.fHslider2) as F64;
		let zipped_iterators = inputs0.zip(inputs1).zip(outputs0).zip(outputs1);
		for (((input0, input1), output0), output1) in zipped_iterators {
			let mut fTemp0: F64 = fSlow1 * (*input0) as F64;
			self.fVec0[0] = fTemp0;
			self.fRec0[0] = fTemp0 - self.fVec0[1] + fSlow0 * (if (F64::abs(self.fRec0[1]) < 1e-08) as i32 != 0 {0.0} else {self.fRec0[1]});
			*output0 = (self.fRec0[0]) as FaustFloat;
			let mut fTemp1: F64 = fSlow2 * (*input1) as F64;
			self.fVec1[0] = fTemp1;
			self.fRec1[0] = fTemp1 - self.fVec1[1] + fSlow0 * (if (F64::abs(self.fRec1[1]) < 1e-08) as i32 != 0 {0.0} else {self.fRec1[1]});
			*output1 = (self.fRec1[0]) as FaustFloat;
			self.fVec0[1] = self.fVec0[0];
			self.fRec0[1] = self.fRec0[0];
			self.fVec1[1] = self.fVec1[0];
			self.fRec1[1] = self.fRec1[0];
		}
		
	}

}

#[cfg(not(target_arch = "wasm32"))] // Compile ffi bindings only on non-wasm targets
	mod ffi {
	use core::ffi::c_double;
	// Conditionally compile the link attribute only on non-Windows platforms
	#[cfg_attr(not(target_os="windows"), link(name="m"))]
	unsafe extern "C" {
		pub fn remainder(from: c_double, to: c_double) -> c_double;
		pub fn rint(val: c_double) -> c_double;
	}
}
fn remainder(from: f64, to: f64) -> f64 {
	#[cfg(not(target_arch = "wasm32"))] // non-wasm targets use ffi bindings
	unsafe { ffi::remainder(from, to) }
	#[cfg(target_arch = "wasm32")] // wasm relies on libm
	libm::remainder(from, to)
}
fn rint(val: f64) -> f64 {
	#[cfg(not(target_arch = "wasm32"))] // non-wasm targets use ffi bindings
	unsafe { ffi::rint(val) }
	#[cfg(target_arch = "wasm32")] // wasm relies on libm
	libm::rint(val)
}

impl FaustDsp for DcBlockerDsp {
	type T = FaustFloat;
	fn new() -> Self where Self: Sized {
		Self::new()
	}
	fn metadata(&self, m: &mut dyn Meta) {
		self.metadata(m)
	}
	fn get_sample_rate(&self) -> i32 {
		self.get_sample_rate()
	}
	fn get_num_inputs(&self) -> i32 {
		FAUST_INPUTS as i32
	}
	fn get_num_outputs(&self) -> i32 {
		FAUST_OUTPUTS as i32
	}
	fn class_init(sample_rate: i32) where Self: Sized {
		Self::class_init(sample_rate);
	}
	fn instance_reset_params(&mut self) {
		self.instance_reset_params()
	}
	fn instance_clear(&mut self) {
		self.instance_clear()
	}
	fn instance_constants(&mut self, sample_rate: i32) {
		self.instance_constants(sample_rate)
	}
	fn instance_init(&mut self, sample_rate: i32) {
		self.instance_init(sample_rate)
	}
	fn init(&mut self, sample_rate: i32) {
		self.init(sample_rate)
	}
	fn build_user_interface(&self, ui_interface: &mut dyn UI<Self::T>) {
		self.build_user_interface(ui_interface)
	}
	fn build_user_interface_static(ui_interface: &mut dyn UI<Self::T>) where Self: Sized {
		Self::build_user_interface_static(ui_interface);
	}
	fn get_param(&self, param: ParamIndex) -> Option<Self::T> {
		self.get_param(param)
	}
	fn set_param(&mut self, param: ParamIndex, value: Self::T) {
		self.set_param(param, value)
	}
	fn compute(&mut self, count: i32, inputs: &[&[Self::T]], outputs: &mut [&mut [Self::T]]) {
		self.compute(count as usize, inputs, outputs)
	}
}
