/* ------------------------------------------------------------
name: "hall"
Code generated with Faust 2.88.0 (https://faust.grame.fr)
Compilation options: -a crates/f3rt-audio/faust/architecture.rs -lang rust -fpga-mem-th 4 -ct 1 -cn HallDsp -es 1 -mcd 16 -mdd 1024 -mdy 33 -double -ftz 0
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
pub struct HallDsp {
	iVec0: [i32;2],
	fHslider0: FaustFloat,
	fHslider1: FaustFloat,
	fRec32: [F64;2],
	fRec33: [F64;2],
	fHslider2: FaustFloat,
	fHslider3: FaustFloat,
	fHslider4: FaustFloat,
	fVec1: [F64;4],
	IOTA0: i32,
	fRec0: [F64;2048],
	fRec1: [F64;2048],
	fHslider5: FaustFloat,
	fRec2: [F64;2],
	fRec3: [F64;2],
	fRec4: [F64;512],
	fRec5: [F64;2],
	fRec6: [F64;2],
	fRec7: [F64;1024],
	fRec8: [F64;1024],
	fRec9: [F64;1024],
	fHslider6: FaustFloat,
	fHslider7: FaustFloat,
	fHslider8: FaustFloat,
	fHslider9: FaustFloat,
	fHslider10: FaustFloat,
	fRec34: [F64;2],
	fRec11: [F64;512],
	fRec12: [F64;256],
	fRec13: [F64;512],
	fRec14: [F64;128],
	fRec35: [F64;2],
	fRec15: [F64;128],
	fHslider11: FaustFloat,
	fRec17: [F64;256],
	fRec18: [F64;512],
	fHslider12: FaustFloat,
	fRec19: [F64;2],
	fRec20: [F64;256],
	fRec21: [F64;2],
	fRec22: [F64;2],
	fRec23: [F64;1024],
	fRec24: [F64;1024],
	fRec25: [F64;256],
	fRec26: [F64;256],
	fRec27: [F64;2],
	fRec28: [F64;2],
	fRec29: [F64;2],
	fRec30: [F64;8],
	fRec31: [F64;1024],
	fVec2: [F64;2],
	fHslider13: FaustFloat,
	fHslider14: FaustFloat,
	fVec3: [F64;2],
	fSampleRate: i32,
}


pub const FAUST_INPUTS: usize = 6;
pub const FAUST_OUTPUTS: usize = 3;
pub const FAUST_ACTIVES: usize = 15;
pub const FAUST_PASSIVES: usize = 0;

impl HallDsp {
		
	pub fn new() -> HallDsp { 
		HallDsp {
			iVec0: [0;2],
			fHslider0: 0.0,
			fHslider1: 0.0,
			fRec32: [0.0;2],
			fRec33: [0.0;2],
			fHslider2: 0.0,
			fHslider3: 0.0,
			fHslider4: 0.0,
			fVec1: [0.0;4],
			IOTA0: 0,
			fRec0: [0.0;2048],
			fRec1: [0.0;2048],
			fHslider5: 0.0,
			fRec2: [0.0;2],
			fRec3: [0.0;2],
			fRec4: [0.0;512],
			fRec5: [0.0;2],
			fRec6: [0.0;2],
			fRec7: [0.0;1024],
			fRec8: [0.0;1024],
			fRec9: [0.0;1024],
			fHslider6: 0.0,
			fHslider7: 0.0,
			fHslider8: 0.0,
			fHslider9: 0.0,
			fHslider10: 0.0,
			fRec34: [0.0;2],
			fRec11: [0.0;512],
			fRec12: [0.0;256],
			fRec13: [0.0;512],
			fRec14: [0.0;128],
			fRec35: [0.0;2],
			fRec15: [0.0;128],
			fHslider11: 0.0,
			fRec17: [0.0;256],
			fRec18: [0.0;512],
			fHslider12: 0.0,
			fRec19: [0.0;2],
			fRec20: [0.0;256],
			fRec21: [0.0;2],
			fRec22: [0.0;2],
			fRec23: [0.0;1024],
			fRec24: [0.0;1024],
			fRec25: [0.0;256],
			fRec26: [0.0;256],
			fRec27: [0.0;2],
			fRec28: [0.0;2],
			fRec29: [0.0;2],
			fRec30: [0.0;8],
			fRec31: [0.0;1024],
			fVec2: [0.0;2],
			fHslider13: 0.0,
			fHslider14: 0.0,
			fVec3: [0.0;2],
			fSampleRate: 0,
		}
	}
	pub fn metadata(&self, m: &mut dyn Meta) { 
		m.declare("compile_options", r"-a crates/f3rt-audio/faust/architecture.rs -lang rust -fpga-mem-th 4 -ct 1 -cn HallDsp -es 1 -mcd 16 -mdd 1024 -mdy 33 -double -ftz 0");
		m.declare("filename", r"hall.dsp");
		m.declare("name", r"hall");
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
		self.fHslider5 = (0.3125) as FaustFloat;
		self.fHslider6 = (0.65625) as FaustFloat;
		self.fHslider7 = (0.04689) as FaustFloat;
		self.fHslider8 = (0.25) as FaustFloat;
		self.fHslider9 = (0.2448) as FaustFloat;
		self.fHslider10 = (0.2448) as FaustFloat;
		self.fHslider11 = (0.52295) as FaustFloat;
		self.fHslider12 = (0.5) as FaustFloat;
		self.fHslider13 = (0.9695) as FaustFloat;
		self.fHslider14 = (0.9695) as FaustFloat;
	}
	pub fn instance_clear(&mut self) {
		for l0 in 0..2 {
			self.iVec0[l0 as usize] = 0;
		}
		for l1 in 0..2 {
			self.fRec32[l1 as usize] = 0.0;
		}
		for l2 in 0..2 {
			self.fRec33[l2 as usize] = 0.0;
		}
		for l3 in 0..4 {
			self.fVec1[l3 as usize] = 0.0;
		}
		self.IOTA0 = 0;
		for l4 in 0..2048 {
			self.fRec0[l4 as usize] = 0.0;
		}
		for l5 in 0..2048 {
			self.fRec1[l5 as usize] = 0.0;
		}
		for l6 in 0..2 {
			self.fRec2[l6 as usize] = 0.0;
		}
		for l7 in 0..2 {
			self.fRec3[l7 as usize] = 0.0;
		}
		for l8 in 0..512 {
			self.fRec4[l8 as usize] = 0.0;
		}
		for l9 in 0..2 {
			self.fRec5[l9 as usize] = 0.0;
		}
		for l10 in 0..2 {
			self.fRec6[l10 as usize] = 0.0;
		}
		for l11 in 0..1024 {
			self.fRec7[l11 as usize] = 0.0;
		}
		for l12 in 0..1024 {
			self.fRec8[l12 as usize] = 0.0;
		}
		for l13 in 0..1024 {
			self.fRec9[l13 as usize] = 0.0;
		}
		for l14 in 0..2 {
			self.fRec34[l14 as usize] = 0.0;
		}
		for l15 in 0..512 {
			self.fRec11[l15 as usize] = 0.0;
		}
		for l16 in 0..256 {
			self.fRec12[l16 as usize] = 0.0;
		}
		for l17 in 0..512 {
			self.fRec13[l17 as usize] = 0.0;
		}
		for l18 in 0..128 {
			self.fRec14[l18 as usize] = 0.0;
		}
		for l19 in 0..2 {
			self.fRec35[l19 as usize] = 0.0;
		}
		for l20 in 0..128 {
			self.fRec15[l20 as usize] = 0.0;
		}
		for l21 in 0..256 {
			self.fRec17[l21 as usize] = 0.0;
		}
		for l22 in 0..512 {
			self.fRec18[l22 as usize] = 0.0;
		}
		for l23 in 0..2 {
			self.fRec19[l23 as usize] = 0.0;
		}
		for l24 in 0..256 {
			self.fRec20[l24 as usize] = 0.0;
		}
		for l25 in 0..2 {
			self.fRec21[l25 as usize] = 0.0;
		}
		for l26 in 0..2 {
			self.fRec22[l26 as usize] = 0.0;
		}
		for l27 in 0..1024 {
			self.fRec23[l27 as usize] = 0.0;
		}
		for l28 in 0..1024 {
			self.fRec24[l28 as usize] = 0.0;
		}
		for l29 in 0..256 {
			self.fRec25[l29 as usize] = 0.0;
		}
		for l30 in 0..256 {
			self.fRec26[l30 as usize] = 0.0;
		}
		for l31 in 0..2 {
			self.fRec27[l31 as usize] = 0.0;
		}
		for l32 in 0..2 {
			self.fRec28[l32 as usize] = 0.0;
		}
		for l33 in 0..2 {
			self.fRec29[l33 as usize] = 0.0;
		}
		for l34 in 0..8 {
			self.fRec30[l34 as usize] = 0.0;
		}
		for l35 in 0..1024 {
			self.fRec31[l35 as usize] = 0.0;
		}
		for l36 in 0..2 {
			self.fVec2[l36 as usize] = 0.0;
		}
		for l37 in 0..2 {
			self.fVec3[l37 as usize] = 0.0;
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
		HallDsp::class_init(sample_rate);
		self.instance_init(sample_rate);
	}
	
	pub fn build_user_interface(&self, ui_interface: &mut dyn UI<FaustFloat>) {
		Self::build_user_interface_static(ui_interface);
	}
	
	pub fn build_user_interface_static(ui_interface: &mut dyn UI<FaustFloat>) {
		ui_interface.open_vertical_box("hall");
		ui_interface.add_horizontal_slider("chorus_out", ParamIndex(0), 0.9695, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("chorus_to_reverb", ParamIndex(1), 0.2448, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("damp", ParamIndex(2), 0.3125, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("decay", ParamIndex(3), 0.25, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("depth", ParamIndex(4), 8.0, 0.0, 128.0, 0.001);
		ui_interface.add_horizontal_slider("dry_send", ParamIndex(5), 0.9695, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("hall_a", ParamIndex(6), 0.5, 0.0, 2.0, 0.0001);
		ui_interface.add_horizontal_slider("hall_g", ParamIndex(7), 0.65625, 0.0, 2.0, 0.0001);
		ui_interface.add_horizontal_slider("hall_g108", ParamIndex(8), 0.04689, 0.0, 1.0, 1e-05);
		ui_interface.add_horizontal_slider("hall_g2", ParamIndex(9), 0.52295, 0.0, 2.0, 0.0001);
		ui_interface.add_horizontal_slider("lfo_c_eps_hi", ParamIndex(10), 0.0, -1.0, 1.0, 1e-09);
		ui_interface.add_horizontal_slider("lfo_c_eps_lo", ParamIndex(11), 0.0, -1e-06, 1e-06, 1e-15);
		ui_interface.add_horizontal_slider("lfo_c_ph_hi", ParamIndex(12), 0.0, -7.0, 7.0, 1e-09);
		ui_interface.add_horizontal_slider("lfo_c_ph_lo", ParamIndex(13), 0.0, -1e-06, 1e-06, 1e-15);
		ui_interface.add_horizontal_slider("send", ParamIndex(14), 0.2448, 0.0, 1.0, 0.0001);
		ui_interface.close_box();
	}
	
	pub fn get_param(&self, param: ParamIndex) -> Option<FaustFloat> {
		match param.0 {
			10 => Some(self.fHslider0),
			11 => Some(self.fHslider1),
			14 => Some(self.fHslider10),
			9 => Some(self.fHslider11),
			6 => Some(self.fHslider12),
			5 => Some(self.fHslider13),
			0 => Some(self.fHslider14),
			12 => Some(self.fHslider2),
			13 => Some(self.fHslider3),
			4 => Some(self.fHslider4),
			2 => Some(self.fHslider5),
			7 => Some(self.fHslider6),
			8 => Some(self.fHslider7),
			3 => Some(self.fHslider8),
			1 => Some(self.fHslider9),
			_ => None,
		}
	}
	
	pub fn set_param(&mut self, param: ParamIndex, value: FaustFloat) {
		match param.0 {
			10 => { self.fHslider0 = value }
			11 => { self.fHslider1 = value }
			14 => { self.fHslider10 = value }
			9 => { self.fHslider11 = value }
			6 => { self.fHslider12 = value }
			5 => { self.fHslider13 = value }
			0 => { self.fHslider14 = value }
			12 => { self.fHslider2 = value }
			13 => { self.fHslider3 = value }
			4 => { self.fHslider4 = value }
			2 => { self.fHslider5 = value }
			7 => { self.fHslider6 = value }
			8 => { self.fHslider7 = value }
			3 => { self.fHslider8 = value }
			1 => { self.fHslider9 = value }
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
		let mut fSlow8: F64 = 1.0 - fSlow7;
		let mut fSlow9: F64 = (self.fHslider6) as F64;
		let mut fSlow10: F64 = (self.fHslider7) as F64;
		let mut fSlow11: F64 = (self.fHslider8) as F64;
		let mut fSlow12: F64 = (self.fHslider9) as F64;
		let mut fSlow13: F64 = (self.fHslider10) as F64;
		let mut fSlow14: F64 = (self.fHslider11) as F64;
		let mut fSlow15: F64 = (self.fHslider12) as F64;
		let mut fSlow16: F64 = (self.fHslider13) as F64;
		let mut fSlow17: F64 = (self.fHslider14) as F64;
		let zipped_iterators = inputs0.zip(inputs1).zip(inputs2).zip(inputs3).zip(inputs4).zip(inputs5).zip(outputs0).zip(outputs1).zip(outputs2);
		for ((((((((input0, input1), input2), input3), input4), input5), output0), output1), output2) in zipped_iterators {
			self.iVec0[0] = 1;
			let mut fTemp0: F64 = (i32::wrapping_sub(1, self.iVec0[1])) as F64 + self.fRec32[1];
			self.fRec32[0] = fSlow2 * fTemp0 - fSlow1 * self.fRec33[1];
			self.fRec33[0] = fSlow1 * fTemp0 + fSlow2 * self.fRec33[1];
			self.fVec1[0] = fSlow6 * (fSlow5 * fTemp0 - fSlow4 * self.fRec33[1]);
			let mut fTemp1: F64 = 611.0 + self.fVec1[3];
			let mut fTemp2: F64 = F64::max(0.0, F64::min(1.1e+03, fTemp1));
			let mut iTemp3: i32 = (fTemp2) as i32;
			let mut fTemp4: F64 = self.fRec0[((i32::wrapping_sub(self.IOTA0, i32::wrapping_add(1, iTemp3))) & 2047) as usize];
			let mut fTemp5: F64 = (if (fTemp1 >= 0.0) as i32 != 0 {fTemp4 + (fTemp2 - (iTemp3) as F64) * (self.fRec0[((i32::wrapping_sub(self.IOTA0, i32::wrapping_add(2, iTemp3))) & 2047) as usize] - fTemp4)} else {0.0});
			let mut fTemp6: F64 = self.fRec22[1] + 0.25 * fTemp5;
			self.fRec0[(self.IOTA0 & 2047) as usize] = fTemp6;
			let mut fTemp7: F64 = 475.0 + self.fVec1[3];
			let mut fTemp8: F64 = F64::max(0.0, F64::min(1.1e+03, fTemp7));
			let mut iTemp9: i32 = (fTemp8) as i32;
			let mut fTemp10: F64 = self.fRec1[((i32::wrapping_sub(self.IOTA0, i32::wrapping_add(1, iTemp9))) & 2047) as usize];
			let mut fTemp11: F64 = (if (fTemp7 >= 0.0) as i32 != 0 {fTemp10 + (fTemp8 - (iTemp9) as F64) * (self.fRec1[((i32::wrapping_sub(self.IOTA0, i32::wrapping_add(2, iTemp9))) & 2047) as usize] - fTemp10)} else {0.0});
			let mut fTemp12: F64 = self.fRec21[1] + 0.25 * fTemp11;
			self.fRec1[(self.IOTA0 & 2047) as usize] = fTemp12;
			self.fRec2[0] = fSlow7 * self.fRec2[1] + fSlow8 * self.fRec30[7];
			let mut fTemp13: F64 = self.fRec31[((i32::wrapping_sub(self.IOTA0, 675)) & 1023) as usize];
			self.fRec3[0] = fSlow7 * self.fRec3[1] + fSlow8 * fTemp13;
			self.fRec4[(self.IOTA0 & 511) as usize] = fTemp5 - 0.25 * fTemp6;
			self.fRec5[0] = 0.875 * self.fRec5[1] + 0.125 * self.fRec30[7];
			self.fRec6[0] = 0.875 * self.fRec6[1] + 0.125 * fTemp13;
			let mut fTemp14: F64 = self.fRec4[((i32::wrapping_sub(self.IOTA0, 368)) & 511) as usize];
			let mut fTemp15: F64 = self.fRec27[1] + 0.25 * fTemp14;
			self.fRec7[(self.IOTA0 & 1023) as usize] = fTemp14 - 0.25 * fTemp15;
			let mut fTemp16: F64 = self.fRec8[((i32::wrapping_sub(self.IOTA0, 874)) & 1023) as usize];
			let mut fTemp17: F64 = self.fRec29[1] + 0.25 * fTemp16;
			self.fRec8[(self.IOTA0 & 1023) as usize] = fTemp17;
			let mut fTemp18: F64 = self.fRec9[((i32::wrapping_sub(self.IOTA0, 836)) & 1023) as usize];
			let mut fTemp19: F64 = self.fRec19[1] + 0.25 * fTemp18;
			self.fRec9[(self.IOTA0 & 1023) as usize] = fTemp19;
			let mut fTemp20: F64 = self.fRec14[((i32::wrapping_sub(self.IOTA0, 102)) & 127) as usize];
			let mut fTemp21: F64 = (*input2) as F64;
			let mut fTemp22: F64 = (*input4) as F64;
			self.fRec34[0] = 0.9921875 * (fSlow13 * fTemp22 + fSlow12 * fTemp21) + 0.0078125 * self.fRec34[1];
			let mut fTemp23: F64 = 0.5 * self.fRec34[1] + fSlow11 * self.fRec2[1] + fSlow10 * self.fRec5[1] + fSlow9 * fTemp20;
			let mut fRec10: F64 = fTemp20 - fSlow9 * fTemp23;
			self.fRec11[(self.IOTA0 & 511) as usize] = fTemp18 - 0.25 * fTemp19;
			self.fRec12[(self.IOTA0 & 255) as usize] = fTemp16 - 0.25 * fTemp17;
			self.fRec13[(self.IOTA0 & 511) as usize] = fTemp11 - 0.25 * fTemp12;
			self.fRec14[(self.IOTA0 & 127) as usize] = fTemp23;
			let mut fTemp24: F64 = self.fRec15[((i32::wrapping_sub(self.IOTA0, 88)) & 127) as usize];
			let mut fTemp25: F64 = (*input3) as F64;
			let mut fTemp26: F64 = (*input5) as F64;
			self.fRec35[0] = 0.9921875 * (fSlow13 * fTemp26 + fSlow12 * fTemp25) + 0.0078125 * self.fRec35[1];
			let mut fTemp27: F64 = 0.5 * self.fRec35[1] + fSlow11 * self.fRec3[1] + fSlow10 * self.fRec6[1] + fSlow9 * fTemp24;
			self.fRec15[(self.IOTA0 & 127) as usize] = fTemp27;
			let mut fRec16: F64 = fTemp24 - fSlow9 * fTemp27;
			let mut fTemp28: F64 = self.fRec17[((i32::wrapping_sub(self.IOTA0, 141)) & 255) as usize];
			let mut fTemp29: F64 = fRec16 + fSlow14 * fTemp28;
			self.fRec17[(self.IOTA0 & 255) as usize] = fTemp29;
			let mut fTemp30: F64 = self.fRec20[((i32::wrapping_sub(self.IOTA0, 168)) & 255) as usize];
			let mut fTemp31: F64 = fRec10 + fSlow14 * fTemp30;
			self.fRec18[(self.IOTA0 & 511) as usize] = fTemp30 - fSlow14 * fTemp31;
			let mut fTemp32: F64 = self.fRec11[((i32::wrapping_sub(self.IOTA0, 334)) & 511) as usize];
			let mut fTemp33: F64 = self.fRec18[((i32::wrapping_sub(self.IOTA0, 449)) & 511) as usize] + fSlow15 * fTemp32;
			self.fRec19[0] = fTemp33;
			self.fRec20[(self.IOTA0 & 255) as usize] = fTemp31;
			let mut fTemp34: F64 = self.fRec13[((i32::wrapping_sub(self.IOTA0, 423)) & 511) as usize];
			let mut fTemp35: F64 = self.fRec28[1] + 0.25 * fTemp34;
			self.fRec21[0] = fTemp35;
			self.fRec22[0] = fTemp15;
			self.fRec23[(self.IOTA0 & 1023) as usize] = fTemp34 - 0.25 * fTemp35;
			self.fRec24[(self.IOTA0 & 1023) as usize] = fTemp28 - fSlow14 * fTemp29;
			self.fRec25[(self.IOTA0 & 255) as usize] = fTemp32 - fSlow15 * fTemp33;
			let mut fTemp36: F64 = self.fRec12[((i32::wrapping_sub(self.IOTA0, 231)) & 255) as usize];
			let mut fTemp37: F64 = self.fRec24[((i32::wrapping_sub(self.IOTA0, 628)) & 1023) as usize] + fSlow15 * fTemp36;
			self.fRec26[(self.IOTA0 & 255) as usize] = fTemp36 - fSlow15 * fTemp37;
			let mut fTemp38: F64 = self.fRec7[((i32::wrapping_sub(self.IOTA0, 649)) & 1023) as usize];
			let mut fTemp39: F64 = self.fRec26[((i32::wrapping_sub(self.IOTA0, 214)) & 255) as usize] + fSlow15 * fTemp38;
			self.fRec27[0] = fTemp39;
			let mut fTemp40: F64 = self.fRec23[((i32::wrapping_sub(self.IOTA0, 616)) & 1023) as usize];
			let mut fTemp41: F64 = self.fRec25[((i32::wrapping_sub(self.IOTA0, 145)) & 255) as usize] + fSlow15 * fTemp40;
			self.fRec28[0] = fTemp41;
			self.fRec29[0] = fTemp37;
			self.fRec30[0] = fTemp38 - fSlow15 * fTemp39;
			self.fRec31[(self.IOTA0 & 1023) as usize] = fTemp40 - fSlow15 * fTemp41;
			let mut fTemp42: F64 = 0.4375 * (self.fRec26[((i32::wrapping_sub(self.IOTA0, 163)) & 255) as usize] - self.fRec25[((i32::wrapping_sub(self.IOTA0, 131)) & 255) as usize]) + 0.125 * self.fRec30[3] + 0.9375 * self.fRec24[((i32::wrapping_sub(self.IOTA0, 101)) & 1023) as usize];
			self.fVec2[0] = fTemp42;
			*output0 = (fSlow17 * fTemp21 + (*input0) as F64 + fSlow16 * fTemp22 + self.fVec2[1]) as FaustFloat;
			let mut fTemp43: F64 = 0.125 * self.fRec31[((i32::wrapping_sub(self.IOTA0, 225)) & 1023) as usize] + 0.5 * self.fRec18[((i32::wrapping_sub(self.IOTA0, 1)) & 511) as usize] - 0.4375 * (self.fRec26[((i32::wrapping_sub(self.IOTA0, 74)) & 255) as usize] - self.fRec25[((i32::wrapping_sub(self.IOTA0, 10)) & 255) as usize]);
			self.fVec3[0] = fTemp43;
			*output1 = (fSlow17 * fTemp25 + (*input1) as F64 + fSlow16 * fTemp26 + self.fVec3[1]) as FaustFloat;
			*output2 = (F64::max(F64::max(F64::abs(fTemp42), F64::max(F64::abs(fTemp43), F64::abs(self.fRec30[7]))), F64::max(F64::abs(fTemp13), F64::max(F64::abs(self.fRec6[0]), F64::abs(self.fRec5[0]))))) as FaustFloat;
			self.iVec0[1] = self.iVec0[0];
			self.fRec32[1] = self.fRec32[0];
			self.fRec33[1] = self.fRec33[0];
			for j0 in (1..=3).rev() {
				self.fVec1[j0 as usize] = self.fVec1[(i32::wrapping_sub(j0, 1)) as usize];
			}
			self.IOTA0 = i32::wrapping_add(self.IOTA0, 1);
			self.fRec2[1] = self.fRec2[0];
			self.fRec3[1] = self.fRec3[0];
			self.fRec5[1] = self.fRec5[0];
			self.fRec6[1] = self.fRec6[0];
			self.fRec34[1] = self.fRec34[0];
			self.fRec35[1] = self.fRec35[0];
			self.fRec19[1] = self.fRec19[0];
			self.fRec21[1] = self.fRec21[0];
			self.fRec22[1] = self.fRec22[0];
			self.fRec27[1] = self.fRec27[0];
			self.fRec28[1] = self.fRec28[0];
			self.fRec29[1] = self.fRec29[0];
			for j1 in (1..=7).rev() {
				self.fRec30[j1 as usize] = self.fRec30[(i32::wrapping_sub(j1, 1)) as usize];
			}
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

impl FaustDsp for HallDsp {
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
