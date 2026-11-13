/* ------------------------------------------------------------
name: "chorus"
Code generated with Faust 2.88.0 (https://faust.grame.fr)
Compilation options: -a crates/f3rt-audio/faust/architecture.rs -lang rust -fpga-mem-th 4 -ct 1 -cn ChorusDsp -es 1 -mcd 16 -mdd 1024 -mdy 33 -double -ftz 0
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
pub struct ChorusDsp {
	IOTA0: i32,
	iVec0: [i32;2],
	fRec0: [F64;2],
	fHslider0: FaustFloat,
	fHslider1: FaustFloat,
	fRec16: [F64;2],
	fRec17: [F64;2],
	fHslider2: FaustFloat,
	fHslider3: FaustFloat,
	fHslider4: FaustFloat,
	fVec1: [F64;4],
	fRec1: [F64;1024],
	fRec2: [F64;2],
	fRec3: [F64;2048],
	fRec4: [F64;1024],
	fRec5: [F64;1024],
	fRec6: [F64;2048],
	fRec7: [F64;2],
	fHslider5: FaustFloat,
	fHslider6: FaustFloat,
	fHslider7: FaustFloat,
	fHslider8: FaustFloat,
	fHslider9: FaustFloat,
	fHslider10: FaustFloat,
	fHslider11: FaustFloat,
	fRec19: [F64;2],
	fRec20: [F64;2],
	fHslider12: FaustFloat,
	fHslider13: FaustFloat,
	fHslider14: FaustFloat,
	fHslider15: FaustFloat,
	fRec23: [F64;2],
	fRec24: [F64;2],
	fHslider16: FaustFloat,
	fHslider17: FaustFloat,
	fHslider18: FaustFloat,
	fRec21: [F64;512],
	fRec22: [F64;512],
	fHslider19: FaustFloat,
	fHslider20: FaustFloat,
	fVec2: [F64;2],
	fVec3: [F64;2],
	fHslider21: FaustFloat,
	fHslider22: FaustFloat,
	fHslider23: FaustFloat,
	fHslider24: FaustFloat,
	fRec18: [F64;2],
	fRec9: [F64;512],
	fRec10: [F64;256],
	fRec11: [F64;1024],
	fRec12: [F64;256],
	fRec13: [F64;2],
	fRec14: [F64;2],
	fRec15: [F64;2048],
	fHslider25: FaustFloat,
	fHslider26: FaustFloat,
	fHslider27: FaustFloat,
	fVec4: [F64;2],
	fHslider28: FaustFloat,
	fHslider29: FaustFloat,
	fHslider30: FaustFloat,
	fHslider31: FaustFloat,
	fHslider32: FaustFloat,
	fVec5: [F64;2],
	fSampleRate: i32,
}


pub const FAUST_INPUTS: usize = 6;
pub const FAUST_OUTPUTS: usize = 3;
pub const FAUST_ACTIVES: usize = 33;
pub const FAUST_PASSIVES: usize = 0;

impl ChorusDsp {
		
	pub fn new() -> ChorusDsp { 
		ChorusDsp {
			IOTA0: 0,
			iVec0: [0;2],
			fRec0: [0.0;2],
			fHslider0: 0.0,
			fHslider1: 0.0,
			fRec16: [0.0;2],
			fRec17: [0.0;2],
			fHslider2: 0.0,
			fHslider3: 0.0,
			fHslider4: 0.0,
			fVec1: [0.0;4],
			fRec1: [0.0;1024],
			fRec2: [0.0;2],
			fRec3: [0.0;2048],
			fRec4: [0.0;1024],
			fRec5: [0.0;1024],
			fRec6: [0.0;2048],
			fRec7: [0.0;2],
			fHslider5: 0.0,
			fHslider6: 0.0,
			fHslider7: 0.0,
			fHslider8: 0.0,
			fHslider9: 0.0,
			fHslider10: 0.0,
			fHslider11: 0.0,
			fRec19: [0.0;2],
			fRec20: [0.0;2],
			fHslider12: 0.0,
			fHslider13: 0.0,
			fHslider14: 0.0,
			fHslider15: 0.0,
			fRec23: [0.0;2],
			fRec24: [0.0;2],
			fHslider16: 0.0,
			fHslider17: 0.0,
			fHslider18: 0.0,
			fRec21: [0.0;512],
			fRec22: [0.0;512],
			fHslider19: 0.0,
			fHslider20: 0.0,
			fVec2: [0.0;2],
			fVec3: [0.0;2],
			fHslider21: 0.0,
			fHslider22: 0.0,
			fHslider23: 0.0,
			fHslider24: 0.0,
			fRec18: [0.0;2],
			fRec9: [0.0;512],
			fRec10: [0.0;256],
			fRec11: [0.0;1024],
			fRec12: [0.0;256],
			fRec13: [0.0;2],
			fRec14: [0.0;2],
			fRec15: [0.0;2048],
			fHslider25: 0.0,
			fHslider26: 0.0,
			fHslider27: 0.0,
			fVec4: [0.0;2],
			fHslider28: 0.0,
			fHslider29: 0.0,
			fHslider30: 0.0,
			fHslider31: 0.0,
			fHslider32: 0.0,
			fVec5: [0.0;2],
			fSampleRate: 0,
		}
	}
	pub fn metadata(&self, m: &mut dyn Meta) { 
		m.declare("compile_options", r"-a crates/f3rt-audio/faust/architecture.rs -lang rust -fpga-mem-th 4 -ct 1 -cn ChorusDsp -es 1 -mcd 16 -mdd 1024 -mdy 33 -double -ftz 0");
		m.declare("filename", r"chorus.dsp");
		m.declare("name", r"chorus");
	}

	pub fn get_sample_rate(&self) -> i32 { self.fSampleRate as i32}
	
	pub fn class_init(sample_rate: i32) {
		// Obtaining locks on 0 static var(s)
	}
	pub fn instance_reset_params(&mut self) {
		self.fHslider0 = (0.0) as FaustFloat;
		self.fHslider1 = (0.0) as FaustFloat;
		self.fHslider2 = (0.0) as FaustFloat;
		self.fHslider3 = (0.0) as FaustFloat;
		self.fHslider4 = (8.0) as FaustFloat;
		self.fHslider5 = (0.4) as FaustFloat;
		self.fHslider6 = (0.04922) as FaustFloat;
		self.fHslider7 = (0.00703) as FaustFloat;
		self.fHslider8 = (0.3125) as FaustFloat;
		self.fHslider9 = (0.25) as FaustFloat;
		self.fHslider10 = (0.0) as FaustFloat;
		self.fHslider11 = (0.0) as FaustFloat;
		self.fHslider12 = (0.0) as FaustFloat;
		self.fHslider13 = (0.0) as FaustFloat;
		self.fHslider14 = (0.0) as FaustFloat;
		self.fHslider15 = (0.0) as FaustFloat;
		self.fHslider16 = (0.0) as FaustFloat;
		self.fHslider17 = (0.0) as FaustFloat;
		self.fHslider18 = (0.0) as FaustFloat;
		self.fHslider19 = (0.0) as FaustFloat;
		self.fHslider20 = (1.0) as FaustFloat;
		self.fHslider21 = (0.2448) as FaustFloat;
		self.fHslider22 = (0.2448) as FaustFloat;
		self.fHslider23 = (0.0075) as FaustFloat;
		self.fHslider24 = (0.7071) as FaustFloat;
		self.fHslider25 = (0.46667) as FaustFloat;
		self.fHslider26 = (0.66667) as FaustFloat;
		self.fHslider27 = (0.13333) as FaustFloat;
		self.fHslider28 = (0.9695) as FaustFloat;
		self.fHslider29 = (0.9695) as FaustFloat;
		self.fHslider30 = (0.66667) as FaustFloat;
		self.fHslider31 = (0.66667) as FaustFloat;
		self.fHslider32 = (0.13333) as FaustFloat;
	}
	pub fn instance_clear(&mut self) {
		self.IOTA0 = 0;
		for l0 in 0..2 {
			self.iVec0[l0 as usize] = 0;
		}
		for l1 in 0..2 {
			self.fRec0[l1 as usize] = 0.0;
		}
		for l2 in 0..2 {
			self.fRec16[l2 as usize] = 0.0;
		}
		for l3 in 0..2 {
			self.fRec17[l3 as usize] = 0.0;
		}
		for l4 in 0..4 {
			self.fVec1[l4 as usize] = 0.0;
		}
		for l5 in 0..1024 {
			self.fRec1[l5 as usize] = 0.0;
		}
		for l6 in 0..2 {
			self.fRec2[l6 as usize] = 0.0;
		}
		for l7 in 0..2048 {
			self.fRec3[l7 as usize] = 0.0;
		}
		for l8 in 0..1024 {
			self.fRec4[l8 as usize] = 0.0;
		}
		for l9 in 0..1024 {
			self.fRec5[l9 as usize] = 0.0;
		}
		for l10 in 0..2048 {
			self.fRec6[l10 as usize] = 0.0;
		}
		for l11 in 0..2 {
			self.fRec7[l11 as usize] = 0.0;
		}
		for l12 in 0..2 {
			self.fRec19[l12 as usize] = 0.0;
		}
		for l13 in 0..2 {
			self.fRec20[l13 as usize] = 0.0;
		}
		for l14 in 0..2 {
			self.fRec23[l14 as usize] = 0.0;
		}
		for l15 in 0..2 {
			self.fRec24[l15 as usize] = 0.0;
		}
		for l16 in 0..512 {
			self.fRec21[l16 as usize] = 0.0;
		}
		for l17 in 0..512 {
			self.fRec22[l17 as usize] = 0.0;
		}
		for l18 in 0..2 {
			self.fVec2[l18 as usize] = 0.0;
		}
		for l19 in 0..2 {
			self.fVec3[l19 as usize] = 0.0;
		}
		for l20 in 0..2 {
			self.fRec18[l20 as usize] = 0.0;
		}
		for l21 in 0..512 {
			self.fRec9[l21 as usize] = 0.0;
		}
		for l22 in 0..256 {
			self.fRec10[l22 as usize] = 0.0;
		}
		for l23 in 0..1024 {
			self.fRec11[l23 as usize] = 0.0;
		}
		for l24 in 0..256 {
			self.fRec12[l24 as usize] = 0.0;
		}
		for l25 in 0..2 {
			self.fRec13[l25 as usize] = 0.0;
		}
		for l26 in 0..2 {
			self.fRec14[l26 as usize] = 0.0;
		}
		for l27 in 0..2048 {
			self.fRec15[l27 as usize] = 0.0;
		}
		for l28 in 0..2 {
			self.fVec4[l28 as usize] = 0.0;
		}
		for l29 in 0..2 {
			self.fVec5[l29 as usize] = 0.0;
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
		ChorusDsp::class_init(sample_rate);
		self.instance_init(sample_rate);
	}
	
	pub fn build_user_interface(&self, ui_interface: &mut dyn UI<FaustFloat>) {
		Self::build_user_interface_static(ui_interface);
	}
	
	pub fn build_user_interface_static(ui_interface: &mut dyn UI<FaustFloat>) {
		ui_interface.open_vertical_box("chorus");
		ui_interface.add_horizontal_slider("c1", ParamIndex(0), 0.00703, 0.0, 1.0, 1e-05);
		ui_interface.add_horizontal_slider("c2", ParamIndex(1), 0.04922, 0.0, 1.0, 1e-05);
		ui_interface.add_horizontal_slider("chorus_out", ParamIndex(2), 0.9695, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("chorus_to_reverb", ParamIndex(3), 0.2448, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("cos_mix", ParamIndex(4), 1.0, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("damp", ParamIndex(5), 0.3125, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("decay", ParamIndex(6), 0.25, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("depth", ParamIndex(7), 8.0, 0.0, 128.0, 0.001);
		ui_interface.add_horizontal_slider("dry_send", ParamIndex(8), 0.9695, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("feedback", ParamIndex(9), 0.0, -0.95, 0.95, 0.0001);
		ui_interface.add_horizontal_slider("g", ParamIndex(10), 0.4, 0.0, 2.0, 0.0001);
		ui_interface.add_horizontal_slider("leak", ParamIndex(11), 0.0075, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("lfo_a_eps_hi", ParamIndex(12), 0.0, -1.0, 1.0, 1e-09);
		ui_interface.add_horizontal_slider("lfo_a_eps_lo", ParamIndex(13), 0.0, -1e-06, 1e-06, 1e-15);
		ui_interface.add_horizontal_slider("lfo_a_ph_hi", ParamIndex(14), 0.0, -7.0, 7.0, 1e-09);
		ui_interface.add_horizontal_slider("lfo_a_ph_lo", ParamIndex(15), 0.0, -1e-06, 1e-06, 1e-15);
		ui_interface.add_horizontal_slider("lfo_b_eps_hi", ParamIndex(16), 0.0, -1.0, 1.0, 1e-09);
		ui_interface.add_horizontal_slider("lfo_b_eps_lo", ParamIndex(17), 0.0, -1e-06, 1e-06, 1e-15);
		ui_interface.add_horizontal_slider("lfo_b_ph_hi", ParamIndex(18), 0.0, -7.0, 7.0, 1e-09);
		ui_interface.add_horizontal_slider("lfo_b_ph_lo", ParamIndex(19), 0.0, -1e-06, 1e-06, 1e-15);
		ui_interface.add_horizontal_slider("lfo_c_eps_hi", ParamIndex(20), 0.0, -1.0, 1.0, 1e-09);
		ui_interface.add_horizontal_slider("lfo_c_eps_lo", ParamIndex(21), 0.0, -1e-06, 1e-06, 1e-15);
		ui_interface.add_horizontal_slider("lfo_c_ph_hi", ParamIndex(22), 0.0, -7.0, 7.0, 1e-09);
		ui_interface.add_horizontal_slider("lfo_c_ph_lo", ParamIndex(23), 0.0, -1e-06, 1e-06, 1e-15);
		ui_interface.add_horizontal_slider("scale", ParamIndex(24), 0.7071, 0.0, 2.0, 0.0001);
		ui_interface.add_horizontal_slider("send", ParamIndex(25), 0.2448, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("sin_mix", ParamIndex(26), 0.0, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("tap_l0", ParamIndex(27), 0.13333, -2.0, 2.0, 0.0001);
		ui_interface.add_horizontal_slider("tap_l1", ParamIndex(28), 0.66667, -2.0, 2.0, 0.0001);
		ui_interface.add_horizontal_slider("tap_l2", ParamIndex(29), 0.46667, -2.0, 2.0, 0.0001);
		ui_interface.add_horizontal_slider("tap_r0", ParamIndex(30), 0.13333, -2.0, 2.0, 0.0001);
		ui_interface.add_horizontal_slider("tap_r1", ParamIndex(31), 0.66667, -2.0, 2.0, 0.0001);
		ui_interface.add_horizontal_slider("tap_r2", ParamIndex(32), 0.66667, -2.0, 2.0, 0.0001);
		ui_interface.close_box();
	}
	
	pub fn get_param(&self, param: ParamIndex) -> Option<FaustFloat> {
		match param.0 {
			20 => Some(self.fHslider0),
			21 => Some(self.fHslider1),
			16 => Some(self.fHslider10),
			17 => Some(self.fHslider11),
			18 => Some(self.fHslider12),
			19 => Some(self.fHslider13),
			12 => Some(self.fHslider14),
			13 => Some(self.fHslider15),
			14 => Some(self.fHslider16),
			15 => Some(self.fHslider17),
			9 => Some(self.fHslider18),
			26 => Some(self.fHslider19),
			22 => Some(self.fHslider2),
			4 => Some(self.fHslider20),
			3 => Some(self.fHslider21),
			25 => Some(self.fHslider22),
			11 => Some(self.fHslider23),
			24 => Some(self.fHslider24),
			29 => Some(self.fHslider25),
			28 => Some(self.fHslider26),
			27 => Some(self.fHslider27),
			2 => Some(self.fHslider28),
			8 => Some(self.fHslider29),
			23 => Some(self.fHslider3),
			32 => Some(self.fHslider30),
			31 => Some(self.fHslider31),
			30 => Some(self.fHslider32),
			7 => Some(self.fHslider4),
			10 => Some(self.fHslider5),
			1 => Some(self.fHslider6),
			0 => Some(self.fHslider7),
			5 => Some(self.fHslider8),
			6 => Some(self.fHslider9),
			_ => None,
		}
	}
	
	pub fn set_param(&mut self, param: ParamIndex, value: FaustFloat) {
		match param.0 {
			20 => { self.fHslider0 = value }
			21 => { self.fHslider1 = value }
			16 => { self.fHslider10 = value }
			17 => { self.fHslider11 = value }
			18 => { self.fHslider12 = value }
			19 => { self.fHslider13 = value }
			12 => { self.fHslider14 = value }
			13 => { self.fHslider15 = value }
			14 => { self.fHslider16 = value }
			15 => { self.fHslider17 = value }
			9 => { self.fHslider18 = value }
			26 => { self.fHslider19 = value }
			22 => { self.fHslider2 = value }
			4 => { self.fHslider20 = value }
			3 => { self.fHslider21 = value }
			25 => { self.fHslider22 = value }
			11 => { self.fHslider23 = value }
			24 => { self.fHslider24 = value }
			29 => { self.fHslider25 = value }
			28 => { self.fHslider26 = value }
			27 => { self.fHslider27 = value }
			2 => { self.fHslider28 = value }
			8 => { self.fHslider29 = value }
			23 => { self.fHslider3 = value }
			32 => { self.fHslider30 = value }
			31 => { self.fHslider31 = value }
			30 => { self.fHslider32 = value }
			7 => { self.fHslider4 = value }
			10 => { self.fHslider5 = value }
			1 => { self.fHslider6 = value }
			0 => { self.fHslider7 = value }
			5 => { self.fHslider8 = value }
			6 => { self.fHslider9 = value }
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
		let [inputs0, inputs1, inputs2, inputs3, inputs4, inputs5, .. ] = inputs.as_ref() else { panic!("wrong number of input buffers"); };
		let inputs0 = inputs0.as_ref()[..count].iter();
		let inputs1 = inputs1.as_ref()[..count].iter();
		let inputs2 = inputs2.as_ref()[..count].iter();
		let inputs3 = inputs3.as_ref()[..count].iter();
		let inputs4 = inputs4.as_ref()[..count].iter();
		let inputs5 = inputs5.as_ref()[..count].iter();
		let [outputs0, outputs1, outputs2, .. ] = outputs.as_mut() else { panic!("wrong number of output buffers"); };
		let outputs0 = outputs0.as_mut()[..count].iter_mut();
		let outputs1 = outputs1.as_mut()[..count].iter_mut();
		let outputs2 = outputs2.as_mut()[..count].iter_mut();
		let mut fSlow0: F64 = (self.fHslider1) as F64 + (self.fHslider0) as F64;
		let mut fSlow1: F64 = F64::sin(fSlow0);
		let mut fSlow2: F64 = F64::cos(fSlow0);
		let mut fSlow3: F64 = (self.fHslider3) as F64 + (self.fHslider2) as F64;
		let mut fSlow4: F64 = F64::sin(fSlow3);
		let mut fSlow5: F64 = F64::cos(fSlow3);
		let mut fSlow6: F64 = (self.fHslider4) as F64;
		let mut fSlow7: F64 = (self.fHslider5) as F64;
		let mut fSlow8: F64 = (self.fHslider6) as F64;
		let mut fSlow9: F64 = (self.fHslider7) as F64;
		let mut fSlow10: F64 = (self.fHslider8) as F64;
		let mut fSlow11: F64 = 1.0 - fSlow10;
		let mut fSlow12: F64 = (self.fHslider9) as F64;
		let mut fSlow13: F64 = (self.fHslider11) as F64 + (self.fHslider10) as F64;
		let mut fSlow14: F64 = F64::cos(fSlow13);
		let mut fSlow15: F64 = F64::sin(fSlow13);
		let mut fSlow16: F64 = (self.fHslider13) as F64 + (self.fHslider12) as F64;
		let mut fSlow17: F64 = F64::cos(fSlow16);
		let mut fSlow18: F64 = F64::sin(fSlow16);
		let mut fSlow19: F64 = (self.fHslider15) as F64 + (self.fHslider14) as F64;
		let mut fSlow20: F64 = F64::sin(fSlow19);
		let mut fSlow21: F64 = F64::cos(fSlow19);
		let mut fSlow22: F64 = (self.fHslider17) as F64 + (self.fHslider16) as F64;
		let mut fSlow23: F64 = F64::cos(fSlow22);
		let mut fSlow24: F64 = F64::sin(fSlow22);
		let mut fSlow25: F64 = (self.fHslider18) as F64;
		let mut fSlow26: F64 = 0.7071 * (self.fHslider19) as F64;
		let mut fSlow27: F64 = (self.fHslider20) as F64;
		let mut fSlow28: F64 = (self.fHslider21) as F64;
		let mut fSlow29: F64 = (self.fHslider22) as F64;
		let mut fSlow30: F64 = (self.fHslider23) as F64;
		let mut fSlow31: F64 = (self.fHslider24) as F64 * (1.0 - fSlow30);
		let mut fSlow32: F64 = (self.fHslider25) as F64;
		let mut fSlow33: F64 = (self.fHslider26) as F64;
		let mut fSlow34: F64 = (self.fHslider27) as F64;
		let mut fSlow35: F64 = (self.fHslider28) as F64;
		let mut fSlow36: F64 = (self.fHslider29) as F64;
		let mut fSlow37: F64 = (self.fHslider30) as F64;
		let mut fSlow38: F64 = (self.fHslider31) as F64;
		let mut fSlow39: F64 = (self.fHslider32) as F64;
		let zipped_iterators = inputs0.zip(inputs1).zip(inputs2).zip(inputs3).zip(inputs4).zip(inputs5).zip(outputs0).zip(outputs1).zip(outputs2);
		for ((((((((input0, input1), input2), input3), input4), input5), output0), output1), output2) in zipped_iterators {
			let mut fTemp0: F64 = self.fRec15[((i32::wrapping_sub(self.IOTA0, 1105)) & 2047) as usize];
			self.iVec0[0] = 1;
			self.fRec0[0] = 0.375 * self.fRec0[1] + 0.625 * fTemp0;
			let mut fTemp1: F64 = (i32::wrapping_sub(1, self.iVec0[1])) as F64;
			let mut fTemp2: F64 = fTemp1 + self.fRec16[1];
			self.fRec16[0] = fSlow2 * fTemp2 - fSlow1 * self.fRec17[1];
			self.fRec17[0] = fSlow1 * fTemp2 + fSlow2 * self.fRec17[1];
			self.fVec1[0] = fSlow6 * (fSlow5 * fTemp2 - fSlow4 * self.fRec17[1]);
			let mut fTemp3: F64 = 921.0 + self.fVec1[3];
			let mut fTemp4: F64 = F64::max(0.0, F64::min(1.1e+03, fTemp3));
			let mut iTemp5: i32 = (fTemp4) as i32;
			let mut fTemp6: F64 = self.fRec3[((i32::wrapping_sub(self.IOTA0, i32::wrapping_add(1, iTemp5))) & 2047) as usize];
			let mut fTemp7: F64 = (if (fTemp3 >= 0.0) as i32 != 0 {fTemp6 + (fTemp4 - (iTemp5) as F64) * (self.fRec3[((i32::wrapping_sub(self.IOTA0, i32::wrapping_add(2, iTemp5))) & 2047) as usize] - fTemp6)} else {0.0});
			let mut fTemp8: F64 = self.fRec2[1] + 0.25 * fTemp7;
			self.fRec1[(self.IOTA0 & 1023) as usize] = fTemp7 - 0.25 * fTemp8;
			let mut fTemp9: F64 = self.fRec1[((i32::wrapping_sub(self.IOTA0, 695)) & 1023) as usize];
			let mut fTemp10: F64 = self.fRec13[1] + 0.25 * fTemp9;
			self.fRec2[0] = fTemp10;
			self.fRec3[(self.IOTA0 & 2047) as usize] = fTemp8;
			self.fRec4[(self.IOTA0 & 1023) as usize] = fTemp9 - 0.25 * fTemp10;
			let mut fTemp11: F64 = self.fRec6[((i32::wrapping_sub(self.IOTA0, 1369)) & 2047) as usize];
			let mut fTemp12: F64 = self.fRec14[1] + 0.25 * fTemp11;
			self.fRec5[(self.IOTA0 & 1023) as usize] = fTemp11 - 0.25 * fTemp12;
			self.fRec6[(self.IOTA0 & 2047) as usize] = fTemp12;
			self.fRec7[0] = 0.875 * self.fRec7[1] + 0.125 * self.fRec0[1];
			let mut fTemp13: F64 = self.fRec10[((i32::wrapping_sub(self.IOTA0, 168)) & 255) as usize];
			let mut fTemp14: F64 = fTemp1 + self.fRec20[1];
			self.fRec19[0] = fSlow15 * fTemp14 + fSlow14 * self.fRec19[1];
			self.fRec20[0] = fSlow14 * fTemp14 - fSlow15 * self.fRec19[1];
			let mut fTemp15: F64 = 428.0 + 4e+01 * (fSlow18 * fTemp14 + fSlow17 * self.fRec19[1]);
			let mut fTemp16: F64 = fTemp1 + self.fRec23[1];
			self.fRec23[0] = fSlow21 * fTemp16 - fSlow20 * self.fRec24[1];
			self.fRec24[0] = fSlow20 * fTemp16 + fSlow21 * self.fRec24[1];
			let mut fTemp17: F64 = 261.0 + 4e+01 * (fSlow24 * fTemp16 + fSlow23 * self.fRec24[1]);
			let mut fTemp18: F64 = F64::max(0.0, F64::min(4.7e+02, fTemp17));
			let mut iTemp19: i32 = (fTemp18) as i32;
			let mut fTemp20: F64 = self.fRec22[((i32::wrapping_sub(self.IOTA0, i32::wrapping_add(1, iTemp19))) & 511) as usize];
			let mut fTemp21: F64 = (if (fTemp17 >= 0.0) as i32 != 0 {fTemp20 + (fTemp18 - (iTemp19) as F64) * (self.fRec22[((i32::wrapping_sub(self.IOTA0, i32::wrapping_add(2, iTemp19))) & 511) as usize] - fTemp20)} else {0.0});
			let mut fTemp22: F64 = (*input2) as F64;
			self.fRec21[(self.IOTA0 & 511) as usize] = fTemp22 + fSlow25 * fTemp21;
			let mut fTemp23: F64 = 303.0 + 4e+01 * (fSlow17 * fTemp14 - fSlow18 * self.fRec19[1]);
			let mut fTemp24: F64 = F64::max(0.0, F64::min(4.7e+02, fTemp23));
			let mut iTemp25: i32 = (fTemp24) as i32;
			let mut fTemp26: F64 = self.fRec21[((i32::wrapping_sub(self.IOTA0, i32::wrapping_add(1, iTemp25))) & 511) as usize];
			let mut fTemp27: F64 = (if (fTemp23 >= 0.0) as i32 != 0 {fTemp26 + (fTemp24 - (iTemp25) as F64) * (self.fRec21[((i32::wrapping_sub(self.IOTA0, i32::wrapping_add(2, iTemp25))) & 511) as usize] - fTemp26)} else {0.0});
			let mut fTemp28: F64 = (*input3) as F64;
			self.fRec22[(self.IOTA0 & 511) as usize] = fTemp28 + fSlow25 * fTemp27;
			let mut fTemp29: F64 = F64::max(0.0, F64::min(4.7e+02, fTemp15));
			let mut iTemp30: i32 = (fTemp29) as i32;
			let mut fTemp31: F64 = self.fRec22[((i32::wrapping_sub(self.IOTA0, i32::wrapping_add(1, iTemp30))) & 511) as usize];
			let mut fTemp32: F64 = fSlow27 * fTemp28 + fSlow26 * (fTemp21 + (if (fTemp15 >= 0.0) as i32 != 0 {fTemp31 + (fTemp29 - (iTemp30) as F64) * (self.fRec22[((i32::wrapping_sub(self.IOTA0, i32::wrapping_add(2, iTemp30))) & 511) as usize] - fTemp31)} else {0.0}));
			self.fVec2[0] = fTemp32;
			let mut fTemp33: F64 = 7e+01 + 4e+01 * (fSlow23 * fTemp16 - fSlow24 * self.fRec24[1]);
			let mut fTemp34: F64 = F64::max(0.0, F64::min(4.7e+02, fTemp33));
			let mut iTemp35: i32 = (fTemp34) as i32;
			let mut fTemp36: F64 = self.fRec21[((i32::wrapping_sub(self.IOTA0, i32::wrapping_add(1, iTemp35))) & 511) as usize];
			let mut fTemp37: F64 = fSlow27 * fTemp22 + fSlow26 * (fTemp27 + (if (fTemp33 >= 0.0) as i32 != 0 {fTemp36 + (fTemp34 - (iTemp35) as F64) * (self.fRec21[((i32::wrapping_sub(self.IOTA0, i32::wrapping_add(2, iTemp35))) & 511) as usize] - fTemp36)} else {0.0}));
			self.fVec3[0] = fTemp37;
			let mut fTemp38: F64 = (*input5) as F64;
			let mut fTemp39: F64 = (*input4) as F64;
			self.fRec18[0] = fSlow30 * self.fRec18[1] + fSlow31 * (fSlow29 * (fTemp39 + fTemp38) + fSlow28 * (self.fVec3[1] + self.fVec2[1]));
			let mut fTemp40: F64 = self.fRec18[0] + fSlow12 * (fSlow11 * fTemp0 + fSlow10 * self.fRec0[1]) + fSlow9 * self.fRec0[1] + fSlow8 * self.fRec7[1] + fSlow7 * fTemp13;
			let mut fRec8: F64 = fTemp13 - fSlow7 * fTemp40;
			let mut fTemp41: F64 = self.fRec9[((i32::wrapping_sub(self.IOTA0, 276)) & 511) as usize];
			let mut fTemp42: F64 = fRec8 + fSlow7 * fTemp41;
			self.fRec9[(self.IOTA0 & 511) as usize] = fTemp42;
			self.fRec10[(self.IOTA0 & 255) as usize] = fTemp40;
			self.fRec11[(self.IOTA0 & 1023) as usize] = fTemp41 - fSlow7 * fTemp42;
			let mut fTemp43: F64 = self.fRec5[((i32::wrapping_sub(self.IOTA0, 548)) & 1023) as usize];
			let mut fTemp44: F64 = self.fRec11[((i32::wrapping_sub(self.IOTA0, 755)) & 1023) as usize] + 0.5 * fTemp43;
			self.fRec12[(self.IOTA0 & 255) as usize] = fTemp43 - 0.5 * fTemp44;
			let mut fTemp45: F64 = self.fRec4[((i32::wrapping_sub(self.IOTA0, 1011)) & 1023) as usize];
			let mut fTemp46: F64 = self.fRec12[((i32::wrapping_sub(self.IOTA0, 242)) & 255) as usize] + 0.5 * fTemp45;
			self.fRec13[0] = fTemp46;
			self.fRec14[0] = fTemp44;
			self.fRec15[(self.IOTA0 & 2047) as usize] = fTemp45 - 0.5 * fTemp46;
			let mut fTemp47: F64 = fSlow34 * self.fRec15[((i32::wrapping_sub(self.IOTA0, 384)) & 2047) as usize] + fSlow33 * self.fRec11[((i32::wrapping_sub(self.IOTA0, 4)) & 1023) as usize] + fSlow32 * self.fRec12[((i32::wrapping_sub(self.IOTA0, 26)) & 255) as usize];
			self.fVec4[0] = fTemp47;
			*output0 = ((*input0) as F64 + fSlow36 * fTemp39 + fSlow35 * self.fVec3[1] + self.fVec4[1]) as FaustFloat;
			let mut fTemp48: F64 = fSlow39 * fTemp0 + fSlow38 * self.fRec11[((i32::wrapping_sub(self.IOTA0, 242)) & 1023) as usize] + fSlow37 * self.fRec12[((i32::wrapping_sub(self.IOTA0, 180)) & 255) as usize];
			self.fVec5[0] = fTemp48;
			*output1 = ((*input1) as F64 + fSlow36 * fTemp38 + fSlow35 * self.fVec2[1] + self.fVec5[1]) as FaustFloat;
			*output2 = (F64::max(F64::max(F64::max(F64::abs(fTemp47), F64::abs(fTemp48)), F64::max(F64::abs(fTemp11), F64::max(F64::abs(fTemp0), F64::abs(self.fRec7[0])))), F64::max(F64::abs(fTemp37), F64::abs(fTemp32)))) as FaustFloat;
			self.IOTA0 = i32::wrapping_add(self.IOTA0, 1);
			self.iVec0[1] = self.iVec0[0];
			self.fRec0[1] = self.fRec0[0];
			self.fRec16[1] = self.fRec16[0];
			self.fRec17[1] = self.fRec17[0];
			for j0 in (1..=3).rev() {
				self.fVec1[j0 as usize] = self.fVec1[(i32::wrapping_sub(j0, 1)) as usize];
			}
			self.fRec2[1] = self.fRec2[0];
			self.fRec7[1] = self.fRec7[0];
			self.fRec19[1] = self.fRec19[0];
			self.fRec20[1] = self.fRec20[0];
			self.fRec23[1] = self.fRec23[0];
			self.fRec24[1] = self.fRec24[0];
			self.fVec2[1] = self.fVec2[0];
			self.fVec3[1] = self.fVec3[0];
			self.fRec18[1] = self.fRec18[0];
			self.fRec13[1] = self.fRec13[0];
			self.fRec14[1] = self.fRec14[0];
			self.fVec4[1] = self.fVec4[0];
			self.fVec5[1] = self.fVec5[0];
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

impl FaustDsp for ChorusDsp {
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
