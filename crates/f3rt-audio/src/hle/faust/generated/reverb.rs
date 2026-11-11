/* ------------------------------------------------------------
name: "reverb"
Code generated with Faust 2.88.0 (https://faust.grame.fr)
Compilation options: -a crates/f3rt-audio/faust/architecture.rs -lang rust -fpga-mem-th 4 -ct 1 -cn ReverbDsp -es 1 -mcd 16 -mdd 1024 -mdy 33 -double -ftz 0
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
pub struct ReverbDsp {
	iVec0: [i32;2],
	fHslider0: FaustFloat,
	fHslider1: FaustFloat,
	fRec16: [F64;2],
	fRec17: [F64;2],
	fHslider2: FaustFloat,
	fHslider3: FaustFloat,
	fHslider4: FaustFloat,
	fVec1: [F64;4],
	IOTA0: i32,
	fRec0: [F64;1024],
	fRec1: [F64;2],
	fRec2: [F64;2048],
	fRec3: [F64;1024],
	fRec4: [F64;2],
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
	fHslider12: FaustFloat,
	fHslider13: FaustFloat,
	fRec18: [F64;2],
	fRec9: [F64;256],
	fRec10: [F64;1024],
	fRec11: [F64;256],
	fRec12: [F64;2],
	fRec13: [F64;2],
	fRec14: [F64;512],
	fRec15: [F64;2048],
	fHslider14: FaustFloat,
	fHslider15: FaustFloat,
	fHslider16: FaustFloat,
	fVec2: [F64;2],
	fHslider17: FaustFloat,
	fHslider18: FaustFloat,
	fHslider19: FaustFloat,
	fHslider20: FaustFloat,
	fHslider21: FaustFloat,
	fVec3: [F64;2],
	fSampleRate: i32,
}


pub const FAUST_INPUTS: usize = 6;
pub const FAUST_OUTPUTS: usize = 3;
pub const FAUST_ACTIVES: usize = 22;
pub const FAUST_PASSIVES: usize = 0;

impl ReverbDsp {
		
	pub fn new() -> ReverbDsp { 
		ReverbDsp {
			iVec0: [0;2],
			fHslider0: 0.0,
			fHslider1: 0.0,
			fRec16: [0.0;2],
			fRec17: [0.0;2],
			fHslider2: 0.0,
			fHslider3: 0.0,
			fHslider4: 0.0,
			fVec1: [0.0;4],
			IOTA0: 0,
			fRec0: [0.0;1024],
			fRec1: [0.0;2],
			fRec2: [0.0;2048],
			fRec3: [0.0;1024],
			fRec4: [0.0;2],
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
			fHslider12: 0.0,
			fHslider13: 0.0,
			fRec18: [0.0;2],
			fRec9: [0.0;256],
			fRec10: [0.0;1024],
			fRec11: [0.0;256],
			fRec12: [0.0;2],
			fRec13: [0.0;2],
			fRec14: [0.0;512],
			fRec15: [0.0;2048],
			fHslider14: 0.0,
			fHslider15: 0.0,
			fHslider16: 0.0,
			fVec2: [0.0;2],
			fHslider17: 0.0,
			fHslider18: 0.0,
			fHslider19: 0.0,
			fHslider20: 0.0,
			fHslider21: 0.0,
			fVec3: [0.0;2],
			fSampleRate: 0,
		}
	}
	pub fn metadata(&self, m: &mut dyn Meta) { 
		m.declare("compile_options", r"-a crates/f3rt-audio/faust/architecture.rs -lang rust -fpga-mem-th 4 -ct 1 -cn ReverbDsp -es 1 -mcd 16 -mdd 1024 -mdy 33 -double -ftz 0");
		m.declare("filename", r"reverb.dsp");
		m.declare("name", r"reverb");
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
		self.fHslider10 = (0.0075) as FaustFloat;
		self.fHslider11 = (0.2448) as FaustFloat;
		self.fHslider12 = (0.2448) as FaustFloat;
		self.fHslider13 = (0.7071) as FaustFloat;
		self.fHslider14 = (0.46667) as FaustFloat;
		self.fHslider15 = (0.66667) as FaustFloat;
		self.fHslider16 = (0.13333) as FaustFloat;
		self.fHslider17 = (0.9695) as FaustFloat;
		self.fHslider18 = (0.9695) as FaustFloat;
		self.fHslider19 = (0.66667) as FaustFloat;
		self.fHslider20 = (0.66667) as FaustFloat;
		self.fHslider21 = (0.13333) as FaustFloat;
	}
	pub fn instance_clear(&mut self) {
		for l0 in 0..2 {
			self.iVec0[l0 as usize] = 0;
		}
		for l1 in 0..2 {
			self.fRec16[l1 as usize] = 0.0;
		}
		for l2 in 0..2 {
			self.fRec17[l2 as usize] = 0.0;
		}
		for l3 in 0..4 {
			self.fVec1[l3 as usize] = 0.0;
		}
		self.IOTA0 = 0;
		for l4 in 0..1024 {
			self.fRec0[l4 as usize] = 0.0;
		}
		for l5 in 0..2 {
			self.fRec1[l5 as usize] = 0.0;
		}
		for l6 in 0..2048 {
			self.fRec2[l6 as usize] = 0.0;
		}
		for l7 in 0..1024 {
			self.fRec3[l7 as usize] = 0.0;
		}
		for l8 in 0..2 {
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
			self.fRec18[l12 as usize] = 0.0;
		}
		for l13 in 0..256 {
			self.fRec9[l13 as usize] = 0.0;
		}
		for l14 in 0..1024 {
			self.fRec10[l14 as usize] = 0.0;
		}
		for l15 in 0..256 {
			self.fRec11[l15 as usize] = 0.0;
		}
		for l16 in 0..2 {
			self.fRec12[l16 as usize] = 0.0;
		}
		for l17 in 0..2 {
			self.fRec13[l17 as usize] = 0.0;
		}
		for l18 in 0..512 {
			self.fRec14[l18 as usize] = 0.0;
		}
		for l19 in 0..2048 {
			self.fRec15[l19 as usize] = 0.0;
		}
		for l20 in 0..2 {
			self.fVec2[l20 as usize] = 0.0;
		}
		for l21 in 0..2 {
			self.fVec3[l21 as usize] = 0.0;
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
		ReverbDsp::class_init(sample_rate);
		self.instance_init(sample_rate);
	}
	
	pub fn build_user_interface(&self, ui_interface: &mut dyn UI<FaustFloat>) {
		Self::build_user_interface_static(ui_interface);
	}
	
	pub fn build_user_interface_static(ui_interface: &mut dyn UI<FaustFloat>) {
		ui_interface.open_vertical_box("reverb");
		ui_interface.add_horizontal_slider("c1", ParamIndex(0), 0.00703, 0.0, 1.0, 1e-05);
		ui_interface.add_horizontal_slider("c2", ParamIndex(1), 0.04922, 0.0, 1.0, 1e-05);
		ui_interface.add_horizontal_slider("chorus_out", ParamIndex(2), 0.9695, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("chorus_to_reverb", ParamIndex(3), 0.2448, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("damp", ParamIndex(4), 0.3125, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("decay", ParamIndex(5), 0.25, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("depth", ParamIndex(6), 8.0, 0.0, 128.0, 0.001);
		ui_interface.add_horizontal_slider("dry_send", ParamIndex(7), 0.9695, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("g", ParamIndex(8), 0.4, 0.0, 2.0, 0.0001);
		ui_interface.add_horizontal_slider("leak", ParamIndex(9), 0.0075, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("lfo_c_eps_hi", ParamIndex(10), 0.0, -1.0, 1.0, 1e-09);
		ui_interface.add_horizontal_slider("lfo_c_eps_lo", ParamIndex(11), 0.0, -1e-06, 1e-06, 1e-15);
		ui_interface.add_horizontal_slider("lfo_c_ph_hi", ParamIndex(12), 0.0, -7.0, 7.0, 1e-09);
		ui_interface.add_horizontal_slider("lfo_c_ph_lo", ParamIndex(13), 0.0, -1e-06, 1e-06, 1e-15);
		ui_interface.add_horizontal_slider("scale", ParamIndex(14), 0.7071, 0.0, 2.0, 0.0001);
		ui_interface.add_horizontal_slider("send", ParamIndex(15), 0.2448, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("tap_l0", ParamIndex(16), 0.13333, -2.0, 2.0, 0.0001);
		ui_interface.add_horizontal_slider("tap_l1", ParamIndex(17), 0.66667, -2.0, 2.0, 0.0001);
		ui_interface.add_horizontal_slider("tap_l2", ParamIndex(18), 0.46667, -2.0, 2.0, 0.0001);
		ui_interface.add_horizontal_slider("tap_r0", ParamIndex(19), 0.13333, -2.0, 2.0, 0.0001);
		ui_interface.add_horizontal_slider("tap_r1", ParamIndex(20), 0.66667, -2.0, 2.0, 0.0001);
		ui_interface.add_horizontal_slider("tap_r2", ParamIndex(21), 0.66667, -2.0, 2.0, 0.0001);
		ui_interface.close_box();
	}
	
	pub fn get_param(&self, param: ParamIndex) -> Option<FaustFloat> {
		match param.0 {
			10 => Some(self.fHslider0),
			11 => Some(self.fHslider1),
			9 => Some(self.fHslider10),
			3 => Some(self.fHslider11),
			15 => Some(self.fHslider12),
			14 => Some(self.fHslider13),
			18 => Some(self.fHslider14),
			17 => Some(self.fHslider15),
			16 => Some(self.fHslider16),
			7 => Some(self.fHslider17),
			2 => Some(self.fHslider18),
			21 => Some(self.fHslider19),
			12 => Some(self.fHslider2),
			20 => Some(self.fHslider20),
			19 => Some(self.fHslider21),
			13 => Some(self.fHslider3),
			6 => Some(self.fHslider4),
			8 => Some(self.fHslider5),
			1 => Some(self.fHslider6),
			0 => Some(self.fHslider7),
			4 => Some(self.fHslider8),
			5 => Some(self.fHslider9),
			_ => None,
		}
	}
	
	pub fn set_param(&mut self, param: ParamIndex, value: FaustFloat) {
		match param.0 {
			10 => { self.fHslider0 = value }
			11 => { self.fHslider1 = value }
			9 => { self.fHslider10 = value }
			3 => { self.fHslider11 = value }
			15 => { self.fHslider12 = value }
			14 => { self.fHslider13 = value }
			18 => { self.fHslider14 = value }
			17 => { self.fHslider15 = value }
			16 => { self.fHslider16 = value }
			7 => { self.fHslider17 = value }
			2 => { self.fHslider18 = value }
			21 => { self.fHslider19 = value }
			12 => { self.fHslider2 = value }
			20 => { self.fHslider20 = value }
			19 => { self.fHslider21 = value }
			13 => { self.fHslider3 = value }
			6 => { self.fHslider4 = value }
			8 => { self.fHslider5 = value }
			1 => { self.fHslider6 = value }
			0 => { self.fHslider7 = value }
			4 => { self.fHslider8 = value }
			5 => { self.fHslider9 = value }
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
		let mut fSlow1: F64 = F64::cos(fSlow0);
		let mut fSlow2: F64 = F64::sin(fSlow0);
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
		let mut fSlow13: F64 = (self.fHslider10) as F64;
		let mut fSlow14: F64 = (self.fHslider11) as F64;
		let mut fSlow15: F64 = (self.fHslider12) as F64;
		let mut fSlow16: F64 = (self.fHslider13) as F64 * (1.0 - fSlow13);
		let mut fSlow17: F64 = (self.fHslider14) as F64;
		let mut fSlow18: F64 = (self.fHslider15) as F64;
		let mut fSlow19: F64 = (self.fHslider16) as F64;
		let mut fSlow20: F64 = (self.fHslider17) as F64;
		let mut fSlow21: F64 = (self.fHslider18) as F64;
		let mut fSlow22: F64 = (self.fHslider19) as F64;
		let mut fSlow23: F64 = (self.fHslider20) as F64;
		let mut fSlow24: F64 = (self.fHslider21) as F64;
		let zipped_iterators = inputs0.zip(inputs1).zip(inputs2).zip(inputs3).zip(inputs4).zip(inputs5).zip(outputs0).zip(outputs1).zip(outputs2);
		for ((((((((input0, input1), input2), input3), input4), input5), output0), output1), output2) in zipped_iterators {
			self.iVec0[0] = 1;
			let mut fTemp0: F64 = (i32::wrapping_sub(1, self.iVec0[1])) as F64 + self.fRec17[1];
			self.fRec16[0] = fSlow2 * fTemp0 + fSlow1 * self.fRec16[1];
			self.fRec17[0] = fSlow1 * fTemp0 - fSlow2 * self.fRec16[1];
			self.fVec1[0] = fSlow6 * (fSlow5 * fTemp0 - fSlow4 * self.fRec16[1]);
			let mut fTemp1: F64 = 921.0 + self.fVec1[3];
			let mut fTemp2: F64 = F64::max(0.0, F64::min(1.1e+03, fTemp1));
			let mut iTemp3: i32 = (fTemp2) as i32;
			let mut fTemp4: F64 = self.fRec2[((i32::wrapping_sub(self.IOTA0, i32::wrapping_add(1, iTemp3))) & 2047) as usize];
			let mut fTemp5: F64 = (if (fTemp1 >= 0.0) as i32 != 0 {fTemp4 + (fTemp2 - (iTemp3) as F64) * (self.fRec2[((i32::wrapping_sub(self.IOTA0, i32::wrapping_add(2, iTemp3))) & 2047) as usize] - fTemp4)} else {0.0});
			let mut fTemp6: F64 = self.fRec4[1] + 0.25 * fTemp5;
			self.fRec0[(self.IOTA0 & 1023) as usize] = fTemp5 - 0.25 * fTemp6;
			let mut fTemp7: F64 = self.fRec15[((i32::wrapping_sub(self.IOTA0, 1105)) & 2047) as usize];
			self.fRec1[0] = 0.375 * self.fRec1[1] + 0.625 * fTemp7;
			self.fRec2[(self.IOTA0 & 2047) as usize] = fTemp6;
			let mut fTemp8: F64 = self.fRec0[((i32::wrapping_sub(self.IOTA0, 695)) & 1023) as usize];
			let mut fTemp9: F64 = self.fRec12[1] + 0.25 * fTemp8;
			self.fRec3[(self.IOTA0 & 1023) as usize] = fTemp8 - 0.25 * fTemp9;
			self.fRec4[0] = fTemp9;
			let mut fTemp10: F64 = self.fRec6[((i32::wrapping_sub(self.IOTA0, 1369)) & 2047) as usize];
			let mut fTemp11: F64 = self.fRec13[1] + 0.25 * fTemp10;
			self.fRec5[(self.IOTA0 & 1023) as usize] = fTemp10 - 0.25 * fTemp11;
			self.fRec6[(self.IOTA0 & 2047) as usize] = fTemp11;
			self.fRec7[0] = 0.875 * self.fRec7[1] + 0.125 * self.fRec1[1];
			let mut fTemp12: F64 = self.fRec9[((i32::wrapping_sub(self.IOTA0, 168)) & 255) as usize];
			let mut fTemp13: F64 = (*input3) as F64;
			let mut fTemp14: F64 = (*input2) as F64;
			let mut fTemp15: F64 = (*input5) as F64;
			let mut fTemp16: F64 = (*input4) as F64;
			self.fRec18[0] = fSlow16 * (fSlow15 * (fTemp16 + fTemp15) + fSlow14 * (fTemp14 + fTemp13)) + fSlow13 * self.fRec18[1];
			let mut fTemp17: F64 = self.fRec18[0] + fSlow12 * (fSlow11 * fTemp7 + fSlow10 * self.fRec1[1]) + fSlow9 * self.fRec1[1] + fSlow8 * self.fRec7[1] + fSlow7 * fTemp12;
			let mut fRec8: F64 = fTemp12 - fSlow7 * fTemp17;
			self.fRec9[(self.IOTA0 & 255) as usize] = fTemp17;
			let mut fTemp18: F64 = self.fRec14[((i32::wrapping_sub(self.IOTA0, 276)) & 511) as usize];
			let mut fTemp19: F64 = fRec8 + fSlow7 * fTemp18;
			self.fRec10[(self.IOTA0 & 1023) as usize] = fTemp18 - fSlow7 * fTemp19;
			let mut fTemp20: F64 = self.fRec5[((i32::wrapping_sub(self.IOTA0, 548)) & 1023) as usize];
			let mut fTemp21: F64 = self.fRec10[((i32::wrapping_sub(self.IOTA0, 755)) & 1023) as usize] + 0.5 * fTemp20;
			self.fRec11[(self.IOTA0 & 255) as usize] = fTemp20 - 0.5 * fTemp21;
			let mut fTemp22: F64 = self.fRec3[((i32::wrapping_sub(self.IOTA0, 1011)) & 1023) as usize];
			let mut fTemp23: F64 = self.fRec11[((i32::wrapping_sub(self.IOTA0, 242)) & 255) as usize] + 0.5 * fTemp22;
			self.fRec12[0] = fTemp23;
			self.fRec13[0] = fTemp21;
			self.fRec14[(self.IOTA0 & 511) as usize] = fTemp19;
			self.fRec15[(self.IOTA0 & 2047) as usize] = fTemp22 - 0.5 * fTemp23;
			let mut fTemp24: F64 = fSlow19 * self.fRec15[((i32::wrapping_sub(self.IOTA0, 384)) & 2047) as usize] + fSlow18 * self.fRec10[((i32::wrapping_sub(self.IOTA0, 4)) & 1023) as usize] + fSlow17 * self.fRec11[((i32::wrapping_sub(self.IOTA0, 26)) & 255) as usize];
			self.fVec2[0] = fTemp24;
			*output0 = (fSlow21 * fTemp14 + (*input0) as F64 + fSlow20 * fTemp16 + self.fVec2[1]) as FaustFloat;
			let mut fTemp25: F64 = fSlow24 * fTemp7 + fSlow23 * self.fRec10[((i32::wrapping_sub(self.IOTA0, 242)) & 1023) as usize] + fSlow22 * self.fRec11[((i32::wrapping_sub(self.IOTA0, 180)) & 255) as usize];
			self.fVec3[0] = fTemp25;
			*output1 = (fSlow21 * fTemp13 + (*input1) as F64 + fSlow20 * fTemp15 + self.fVec3[1]) as FaustFloat;
			*output2 = (F64::max(F64::max(F64::abs(fTemp24), F64::abs(fTemp25)), F64::max(F64::abs(fTemp10), F64::max(F64::abs(fTemp7), F64::abs(self.fRec7[0]))))) as FaustFloat;
			self.iVec0[1] = self.iVec0[0];
			self.fRec16[1] = self.fRec16[0];
			self.fRec17[1] = self.fRec17[0];
			for j0 in (1..=3).rev() {
				self.fVec1[j0 as usize] = self.fVec1[(i32::wrapping_sub(j0, 1)) as usize];
			}
			self.IOTA0 = i32::wrapping_add(self.IOTA0, 1);
			self.fRec1[1] = self.fRec1[0];
			self.fRec4[1] = self.fRec4[0];
			self.fRec7[1] = self.fRec7[0];
			self.fRec18[1] = self.fRec18[0];
			self.fRec12[1] = self.fRec12[0];
			self.fRec13[1] = self.fRec13[0];
			self.fVec2[1] = self.fVec2[0];
			self.fVec3[1] = self.fVec3[0];
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

impl FaustDsp for ReverbDsp {
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
