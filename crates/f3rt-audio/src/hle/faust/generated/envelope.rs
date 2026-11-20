/* ------------------------------------------------------------
name: "envelope"
Code generated with Faust 2.88.0 (https://faust.grame.fr)
Compilation options: -a crates/f3rt-audio/faust/architecture.rs -lang rust -fpga-mem-th 4 -it -ct 1 -cn EnvelopeDsp -es 1 -mcd 16 -mdd 1024 -mdy 33 -double -ftz 0
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
pub struct EnvelopeDsp {
	iVec0: [i32;3],
	fHslider0: FaustFloat,
	IOTA0: i32,
	fHslider1: FaustFloat,
	fHslider2: FaustFloat,
	fHslider3: FaustFloat,
	fRec0: [F64;2],
	fRec1: [F64;1024],
	fHslider4: FaustFloat,
	fRec2: [F64;1024],
	fHslider5: FaustFloat,
	fRec3: [F64;2048],
	fRec4: [F64;2048],
	fRec5: [F64;512],
	fVec1: [F64;2],
	fVec2: [F64;2],
	fRec6: [F64;2],
	fHslider6: FaustFloat,
	fVec3: [F64;3],
	fRec18: [F64;2],
	fRec16: [F64;2],
	fRec17: [F64;2],
	iRec15: [i32;2],
	fHslider7: FaustFloat,
	itbl0EnvelopeDspSIG0: [i32;131],
	fVec4: [F64;2],
	fRec14: [F64;2],
	fHslider8: FaustFloat,
	fHslider9: FaustFloat,
	fRec20: [F64;2],
	fRec21: [F64;2],
	fHslider10: FaustFloat,
	fHslider11: FaustFloat,
	fVec5: [F64;2],
	fVec6: [F64;512],
	fVec7: [F64;2],
	fVec8: [F64;2],
	fVec9: [F64;2],
	fRec7: [F64;2],
	fRec8: [F64;2048],
	fHslider12: FaustFloat,
	fVec10: [F64;2],
	fVec11: [F64;2],
	fVec12: [F64;512],
	fHslider13: FaustFloat,
	fVec13: [F64;2],
	fHslider14: FaustFloat,
	fRec9: [F64;2],
	fRec10: [F64;2],
	fHslider15: FaustFloat,
	fVec14: [F64;2],
	fRec11: [F64;2],
	fRec12: [F64;2],
	fRec13: [F64;2],
	fHslider16: FaustFloat,
	fHslider17: FaustFloat,
	fSampleRate: i32,
	iRec19: [i32;2],
}


pub const FAUST_INPUTS: usize = 6;
pub const FAUST_OUTPUTS: usize = 3;
pub const FAUST_ACTIVES: usize = 18;
pub const FAUST_PASSIVES: usize = 0;

impl EnvelopeDsp {
		
	pub fn new() -> EnvelopeDsp { 
		EnvelopeDsp {
			iVec0: [0;3],
			fHslider0: 0.0,
			IOTA0: 0,
			fHslider1: 0.0,
			fHslider2: 0.0,
			fHslider3: 0.0,
			fRec0: [0.0;2],
			fRec1: [0.0;1024],
			fHslider4: 0.0,
			fRec2: [0.0;1024],
			fHslider5: 0.0,
			fRec3: [0.0;2048],
			fRec4: [0.0;2048],
			fRec5: [0.0;512],
			fVec1: [0.0;2],
			fVec2: [0.0;2],
			fRec6: [0.0;2],
			fHslider6: 0.0,
			fVec3: [0.0;3],
			fRec18: [0.0;2],
			fRec16: [0.0;2],
			fRec17: [0.0;2],
			iRec15: [0;2],
			fHslider7: 0.0,
			itbl0EnvelopeDspSIG0: [0;131],
			fVec4: [0.0;2],
			fRec14: [0.0;2],
			fHslider8: 0.0,
			fHslider9: 0.0,
			fRec20: [0.0;2],
			fRec21: [0.0;2],
			fHslider10: 0.0,
			fHslider11: 0.0,
			fVec5: [0.0;2],
			fVec6: [0.0;512],
			fVec7: [0.0;2],
			fVec8: [0.0;2],
			fVec9: [0.0;2],
			fRec7: [0.0;2],
			fRec8: [0.0;2048],
			fHslider12: 0.0,
			fVec10: [0.0;2],
			fVec11: [0.0;2],
			fVec12: [0.0;512],
			fHslider13: 0.0,
			fVec13: [0.0;2],
			fHslider14: 0.0,
			fRec9: [0.0;2],
			fRec10: [0.0;2],
			fHslider15: 0.0,
			fVec14: [0.0;2],
			fRec11: [0.0;2],
			fRec12: [0.0;2],
			fRec13: [0.0;2],
			fHslider16: 0.0,
			fHslider17: 0.0,
			fSampleRate: 0,
			iRec19: [0;2],
		}
	}
	pub fn metadata(&self, m: &mut dyn Meta) { 
		m.declare("compile_options", r"-a crates/f3rt-audio/faust/architecture.rs -lang rust -fpga-mem-th 4 -it -ct 1 -cn EnvelopeDsp -es 1 -mcd 16 -mdd 1024 -mdy 33 -double -ftz 0");
		m.declare("filename", r"envelope.dsp");
		m.declare("name", r"envelope");
	}

	pub fn get_sample_rate(&self) -> i32 { self.fSampleRate as i32}
	pub fn class_init(sample_rate: i32) {}
	
	pub fn static_init(&mut self,sample_rate: i32) {
		for l15 in 0..2 {
			self.iRec19[l15 as usize] = 0;
		}
		for i1_re0 in 0..131 {
			self.iRec19[0] = i32::wrapping_add(1, self.iRec19[1]);
			self.itbl0EnvelopeDspSIG0[i1_re0 as usize] = envelope_rom_word(i32::wrapping_add(-1, self.iRec19[0]));
			self.iRec19[1] = self.iRec19[0];
		}
	}
	pub fn instance_reset_params(&mut self) {
		self.fHslider0 = (0.0) as FaustFloat;
		self.fHslider1 = (0.0) as FaustFloat;
		self.fHslider2 = (0.2448) as FaustFloat;
		self.fHslider3 = (0.2448) as FaustFloat;
		self.fHslider4 = (0.0) as FaustFloat;
		self.fHslider5 = (0.0) as FaustFloat;
		self.fHslider6 = (0.0) as FaustFloat;
		self.fHslider7 = (0.0) as FaustFloat;
		self.fHslider8 = (0.0) as FaustFloat;
		self.fHslider9 = (0.0) as FaustFloat;
		self.fHslider10 = (0.0) as FaustFloat;
		self.fHslider11 = (0.0) as FaustFloat;
		self.fHslider12 = (0.0) as FaustFloat;
		self.fHslider13 = (0.0) as FaustFloat;
		self.fHslider14 = (0.0) as FaustFloat;
		self.fHslider15 = (0.0) as FaustFloat;
		self.fHslider16 = (0.9695) as FaustFloat;
		self.fHslider17 = (0.9695) as FaustFloat;
	}
	pub fn instance_clear(&mut self) {
		for l0 in 0..3 {
			self.iVec0[l0 as usize] = 0;
		}
		self.IOTA0 = 0;
		for l1 in 0..2 {
			self.fRec0[l1 as usize] = 0.0;
		}
		for l2 in 0..1024 {
			self.fRec1[l2 as usize] = 0.0;
		}
		for l3 in 0..1024 {
			self.fRec2[l3 as usize] = 0.0;
		}
		for l4 in 0..2048 {
			self.fRec3[l4 as usize] = 0.0;
		}
		for l5 in 0..2048 {
			self.fRec4[l5 as usize] = 0.0;
		}
		for l6 in 0..512 {
			self.fRec5[l6 as usize] = 0.0;
		}
		for l7 in 0..2 {
			self.fVec1[l7 as usize] = 0.0;
		}
		for l8 in 0..2 {
			self.fVec2[l8 as usize] = 0.0;
		}
		for l9 in 0..2 {
			self.fRec6[l9 as usize] = 0.0;
		}
		for l10 in 0..3 {
			self.fVec3[l10 as usize] = 0.0;
		}
		for l11 in 0..2 {
			self.fRec18[l11 as usize] = 0.0;
		}
		for l12 in 0..2 {
			self.fRec16[l12 as usize] = 0.0;
		}
		for l13 in 0..2 {
			self.fRec17[l13 as usize] = 0.0;
		}
		for l14 in 0..2 {
			self.iRec15[l14 as usize] = 0;
		}
		for l16 in 0..2 {
			self.fVec4[l16 as usize] = 0.0;
		}
		for l17 in 0..2 {
			self.fRec14[l17 as usize] = 0.0;
		}
		for l18 in 0..2 {
			self.fRec20[l18 as usize] = 0.0;
		}
		for l19 in 0..2 {
			self.fRec21[l19 as usize] = 0.0;
		}
		for l20 in 0..2 {
			self.fVec5[l20 as usize] = 0.0;
		}
		for l21 in 0..512 {
			self.fVec6[l21 as usize] = 0.0;
		}
		for l22 in 0..2 {
			self.fVec7[l22 as usize] = 0.0;
		}
		for l23 in 0..2 {
			self.fVec8[l23 as usize] = 0.0;
		}
		for l24 in 0..2 {
			self.fVec9[l24 as usize] = 0.0;
		}
		for l25 in 0..2 {
			self.fRec7[l25 as usize] = 0.0;
		}
		for l26 in 0..2048 {
			self.fRec8[l26 as usize] = 0.0;
		}
		for l27 in 0..2 {
			self.fVec10[l27 as usize] = 0.0;
		}
		for l28 in 0..2 {
			self.fVec11[l28 as usize] = 0.0;
		}
		for l29 in 0..512 {
			self.fVec12[l29 as usize] = 0.0;
		}
		for l30 in 0..2 {
			self.fVec13[l30 as usize] = 0.0;
		}
		for l31 in 0..2 {
			self.fRec9[l31 as usize] = 0.0;
		}
		for l32 in 0..2 {
			self.fRec10[l32 as usize] = 0.0;
		}
		for l33 in 0..2 {
			self.fVec14[l33 as usize] = 0.0;
		}
		for l34 in 0..2 {
			self.fRec11[l34 as usize] = 0.0;
		}
		for l35 in 0..2 {
			self.fRec12[l35 as usize] = 0.0;
		}
		for l36 in 0..2 {
			self.fRec13[l36 as usize] = 0.0;
		}
	}
	pub fn instance_constants(&mut self, sample_rate: i32) {
		// Obtaining locks on 0 static var(s)
		self.fSampleRate = sample_rate;
	}
	pub fn instance_init(&mut self, sample_rate: i32) {
		self.static_init(sample_rate);
		self.instance_constants(sample_rate);
		self.instance_reset_params();
		self.instance_clear();
	}
	pub fn init(&mut self, sample_rate: i32) {
		self.instance_init(sample_rate);
	}
	
	pub fn build_user_interface(&self, ui_interface: &mut dyn UI<FaustFloat>) {
		Self::build_user_interface_static(ui_interface);
	}
	
	pub fn build_user_interface_static(ui_interface: &mut dyn UI<FaustFloat>) {
		ui_interface.open_vertical_box("envelope");
		ui_interface.add_horizontal_slider("address_gain", ParamIndex(0), 0.0, -8388608.0, 8388607.0, 1.0);
		ui_interface.add_horizontal_slider("allpass", ParamIndex(1), 0.0, -8388608.0, 8388607.0, 1.0);
		ui_interface.add_horizontal_slider("allpass_complement", ParamIndex(2), 0.0, -8388608.0, 8388607.0, 1.0);
		ui_interface.add_horizontal_slider("chorus_out", ParamIndex(3), 0.9695, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("chorus_to_reverb", ParamIndex(4), 0.2448, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("curve_gain", ParamIndex(5), 0.0, -8388608.0, 8388607.0, 1.0);
		ui_interface.add_horizontal_slider("diffusion_pole", ParamIndex(6), 0.0, -8388608.0, 8388607.0, 1.0);
		ui_interface.add_horizontal_slider("dry_send", ParamIndex(7), 0.9695, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("envelope_limit", ParamIndex(8), 0.0, -8388608.0, 8388607.0, 1.0);
		ui_interface.add_horizontal_slider("filter_pole", ParamIndex(9), 0.0, -8388608.0, 8388607.0, 1.0);
		ui_interface.add_horizontal_slider("lfo_c_eps_hi", ParamIndex(10), 0.0, -1.0, 1.0, 1e-09);
		ui_interface.add_horizontal_slider("lfo_c_eps_lo", ParamIndex(11), 0.0, -1e-06, 1e-06, 1e-15);
		ui_interface.add_horizontal_slider("lfo_c_ph_hi", ParamIndex(12), 0.0, -7.0, 7.0, 1e-09);
		ui_interface.add_horizontal_slider("lfo_c_ph_lo", ParamIndex(13), 0.0, -1e-06, 1e-06, 1e-15);
		ui_interface.add_horizontal_slider("send", ParamIndex(14), 0.2448, 0.0, 1.0, 0.0001);
		ui_interface.add_horizontal_slider("short_feedback", ParamIndex(15), 0.0, -8388608.0, 8388607.0, 1.0);
		ui_interface.add_horizontal_slider("tank_damp", ParamIndex(16), 0.0, -8388608.0, 8388607.0, 1.0);
		ui_interface.add_horizontal_slider("tank_decay", ParamIndex(17), 0.0, -8388608.0, 8388607.0, 1.0);
		ui_interface.close_box();
	}
	
	pub fn get_param(&self, param: ParamIndex) -> Option<FaustFloat> {
		match param.0 {
			16 => Some(self.fHslider0),
			17 => Some(self.fHslider1),
			12 => Some(self.fHslider10),
			13 => Some(self.fHslider11),
			9 => Some(self.fHslider12),
			0 => Some(self.fHslider13),
			5 => Some(self.fHslider14),
			6 => Some(self.fHslider15),
			3 => Some(self.fHslider16),
			7 => Some(self.fHslider17),
			4 => Some(self.fHslider2),
			14 => Some(self.fHslider3),
			1 => Some(self.fHslider4),
			2 => Some(self.fHslider5),
			15 => Some(self.fHslider6),
			8 => Some(self.fHslider7),
			10 => Some(self.fHslider8),
			11 => Some(self.fHslider9),
			_ => None,
		}
	}
	
	pub fn set_param(&mut self, param: ParamIndex, value: FaustFloat) {
		match param.0 {
			16 => { self.fHslider0 = value }
			17 => { self.fHslider1 = value }
			12 => { self.fHslider10 = value }
			13 => { self.fHslider11 = value }
			9 => { self.fHslider12 = value }
			0 => { self.fHslider13 = value }
			5 => { self.fHslider14 = value }
			6 => { self.fHslider15 = value }
			3 => { self.fHslider16 = value }
			7 => { self.fHslider17 = value }
			4 => { self.fHslider2 = value }
			14 => { self.fHslider3 = value }
			1 => { self.fHslider4 = value }
			2 => { self.fHslider5 = value }
			15 => { self.fHslider6 = value }
			8 => { self.fHslider7 = value }
			10 => { self.fHslider8 = value }
			11 => { self.fHslider9 = value }
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
		let mut fSlow0: F64 = (self.fHslider0) as F64;
		let mut fSlow1: F64 = (self.fHslider1) as F64;
		let mut fSlow2: F64 = 1.1920928955078125e-07 * fSlow1;
		let mut fSlow3: F64 = 8388608.0 * (self.fHslider2) as F64;
		let mut fSlow4: F64 = 2147483648.0 * (self.fHslider3) as F64;
		let mut fSlow5: F64 = 8388607.0 - fSlow0;
		let mut fSlow6: F64 = (self.fHslider4) as F64;
		let mut fSlow7: F64 = (self.fHslider5) as F64;
		let mut fSlow8: F64 = (self.fHslider6) as F64;
		let mut fSlow9: F64 = (self.fHslider7) as F64;
		let mut fSlow10: F64 = 1.1920928955078125e-07 * fSlow9;
		let mut fSlow11: F64 = (self.fHslider9) as F64 + (self.fHslider8) as F64;
		let mut fSlow12: F64 = (self.fHslider11) as F64 + (self.fHslider10) as F64;
		let mut fSlow13: F64 = F64::sin(fSlow12);
		let mut fSlow14: F64 = F64::cos(fSlow12);
		let mut fSlow15: F64 = (self.fHslider12) as F64;
		let mut fSlow16: F64 = 1.1920928955078125e-07 * (self.fHslider13) as F64;
		let mut fSlow17: F64 = 1.1920928955078125e-07 * (self.fHslider14) as F64;
		let mut fSlow18: F64 = (self.fHslider15) as F64;
		let mut fSlow19: F64 = 8388608.0 * (self.fHslider16) as F64;
		let mut fSlow20: F64 = 2147483648.0 * (self.fHslider17) as F64;
		let zipped_iterators = inputs0.zip(inputs1).zip(inputs2).zip(inputs3).zip(inputs4).zip(inputs5).zip(outputs0).zip(outputs1).zip(outputs2);
		for ((((((((input0, input1), input2), input3), input4), input5), output0), output1), output2) in zipped_iterators {
			self.iVec0[0] = 1;
			let mut fTemp0: F64 = self.fRec8[((i32::wrapping_sub(self.IOTA0, 1494)) & 2047) as usize];
			let mut fTemp1: F64 = 3.93216e+06 * F64::min(8388607.0, F64::max(-8388608.0, F64::floor(fSlow2 * fTemp0)));
			let mut fTemp2: F64 = self.fRec4[((i32::wrapping_sub(self.IOTA0, 1469)) & 2047) as usize];
			let mut fTemp3: F64 = 6545408.0 * fTemp2;
			let mut fTemp4: F64 = F64::floor(1.1920928955078125e-07 * (fTemp3 - fTemp1));
			let mut fTemp5: F64 = fSlow3 * self.fRec13[1];
			let mut fTemp6: F64 = (*input5) as F64;
			let mut fTemp7: F64 = F64::min(32767.0, F64::max(-32768.0, ((if (fTemp6 < 0.0) as i32 != 0 {-0.5} else {0.5}) + 32768.0 * fTemp6) as i32 as F64));
			let mut fTemp8: F64 = (*input4) as F64;
			let mut fTemp9: F64 = F64::min(32767.0, F64::max(-32768.0, ((if (fTemp8 < 0.0) as i32 != 0 {-0.5} else {0.5}) + 32768.0 * fTemp8) as i32 as F64));
			self.fRec0[0] = fSlow5 * F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (fTemp3 + 8388608.0 * (F64::floor(0.5 * F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (fSlow4 * fTemp9 + fTemp5))))) + F64::floor(0.5 * F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (fSlow4 * fTemp7 + fTemp5))))) - fTemp4) + fSlow1 * self.fRec1[((i32::wrapping_sub(self.IOTA0, 799)) & 1023) as usize] - fTemp1)))) + fSlow0 * F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * self.fRec0[1])));
			self.fRec1[(self.IOTA0 & 1023) as usize] = 256.0 * F64::floor(0.00390625 * F64::min(8388607.0, F64::max(-8388608.0, fTemp4)));
			let mut fTemp10: F64 = self.fRec2[((i32::wrapping_sub(self.IOTA0, 773)) & 1023) as usize];
			let mut fTemp11: F64 = self.fRec3[((i32::wrapping_sub(self.IOTA0, 1866)) & 2047) as usize];
			self.fRec2[(self.IOTA0 & 1023) as usize] = 256.0 * F64::floor(0.00390625 * F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (fSlow1 * fTemp11 + fSlow6 * fTemp10)))));
			let mut fTemp12: F64 = self.fRec5[((i32::wrapping_sub(self.IOTA0, 402)) & 511) as usize];
			self.fRec3[(self.IOTA0 & 2047) as usize] = 256.0 * F64::floor(0.00390625 * F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (fSlow7 * fTemp12 - fSlow6 * F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * self.fRec0[0]))))))));
			self.fRec4[(self.IOTA0 & 2047) as usize] = 256.0 * F64::floor(0.00390625 * F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (fSlow1 * fTemp0 + 3.93216e+06 * fTemp2)))));
			self.fRec5[(self.IOTA0 & 511) as usize] = 256.0 * F64::floor(0.00390625 * F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (self.fRec0[0] + fSlow6 * fTemp12)))));
			let mut fTemp13: F64 = 5400167.0 * self.fRec4[((i32::wrapping_sub(self.IOTA0, 1113)) & 2047) as usize];
			let mut fTemp14: F64 = 6501171.0 * self.fRec3[((i32::wrapping_sub(self.IOTA0, 450)) & 2047) as usize] + 3.4603e+06 * self.fRec1[((i32::wrapping_sub(self.IOTA0, 340)) & 1023) as usize];
			let mut fTemp15: F64 = F64::floor(1.1920928955078125e-07 * (fTemp14 - fTemp13));
			let mut fTemp16: F64 = F64::min(8388607.0, F64::max(-8388608.0, fTemp15));
			self.fVec1[0] = fTemp16;
			let mut fTemp17: F64 = F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (5872025.0 * self.fRec2[((i32::wrapping_sub(self.IOTA0, 50)) & 1023) as usize] + 5158994.0 * self.fRec5[((i32::wrapping_sub(self.IOTA0, 349)) & 511) as usize] - 4697621.0 * self.fRec4[((i32::wrapping_sub(self.IOTA0, 372)) & 2047) as usize]))));
			self.fVec2[0] = fTemp17;
			self.fRec6[0] = F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (fTemp14 + 8388608.0 * (F64::floor(0.5 * fTemp17) - fTemp15) + 4194304.0 * fTemp16 - fTemp13))));
			let mut fTemp18: F64 = F64::floor(0.9998999834060669 * self.fRec17[1]);
			let mut fTemp19: F64 = (*input3) as F64;
			let mut fTemp20: F64 = (*input2) as F64;
			let mut fTemp21: F64 = F64::floor(128.0 * F64::min(32767.0, F64::max(-32768.0, ((if (fTemp20 < 0.0) as i32 != 0 {-0.5} else {0.5}) + 32768.0 * fTemp20) as i32 as F64))) + F64::floor(128.0 * F64::min(32767.0, F64::max(-32768.0, ((if (fTemp19 < 0.0) as i32 != 0 {-0.5} else {0.5}) + 32768.0 * fTemp19) as i32 as F64)));
			self.fVec3[0] = fTemp21;
			self.fRec18[0] = 8379644.0 * (self.fVec3[1] - self.fVec3[2]) + 8.37068e+06 * F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * self.fRec18[1])));
			let mut fTemp22: F64 = F64::min(8388607.0, F64::max(-8388608.0, F64::floor(0.05277848243713379 * F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * self.fRec18[0]))))));
			let mut fTemp23: F64 = F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (8387769.0 * self.fRec17[1] + 8388608.0 * ((if (fTemp22 < 0.0) as i32 != 0 {-1.0 - fTemp22} else {fTemp22}) - fTemp18) + 8106751.0 * self.fRec16[1]))));
			self.fRec16[0] = fTemp23;
			let mut fTemp24: F64 = F64::min(8388607.0, F64::max(-8388608.0, fTemp18));
			let mut fTemp25: F64 = F64::max(fTemp24, fTemp23);
			self.fRec17[0] = fTemp25;
			let mut iTemp26: i32 = (fTemp23 < (if self.iRec15[1] != 0 {16384.0} else {2048.0})) as i32;
			self.iRec15[0] = iTemp26;
			let mut iTemp27: i32 = (F64::min(8388607.0, F64::max(-8388608.0, F64::floor(0.001922607421875 * fTemp24)))) as i32;
			let mut fTemp28: F64 = (iTemp27 & 255) as F64;
			let mut iTemp29: i32 = (iTemp27 & 16777215) >> 8;
			let mut iTemp30: i32 = i32::wrapping_add(self.iVec0[1], iTemp29);
			self.fVec4[0] = (if iTemp26 != 0 {655.0} else {(if (fTemp25 < fSlow9) as i32 != 0 {F64::min(8388607.0, F64::max(-8388608.0, F64::floor(fSlow10 * F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (8388608.0 * (self.itbl0EnvelopeDspSIG0[((if (iTemp30 < 130) as i32 != 0 {iTemp30} else {130})) as usize]) as F64 * fTemp28 + 256.0 * (self.itbl0EnvelopeDspSIG0[((if (iTemp29 < 130) as i32 != 0 {iTemp29} else {130})) as usize]) as F64 * (8388607.0 - 32768.0 * fTemp28))))))))} else {65535.0})});
			let mut fTemp31: F64 = (i32::wrapping_sub(1, self.iVec0[1])) as F64;
			self.fRec14[0] = 41942.0 * (65535.0 * fTemp31 + self.fVec4[1]) + 8346665.0 * F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * self.fRec14[1])));
			let mut fTemp32: F64 = F64::floor(1.1920928955078125e-07 * self.fRec14[0]);
			let mut fTemp33: F64 = F64::min(8388607.0, F64::max(-8388608.0, 8388606.0 * fTemp31 + self.fRec21[1] + F64::min(8388607.0, F64::max(-8388608.0, F64::floor(fSlow11 * self.fRec20[1])))));
			self.fRec20[0] = F64::min(8388607.0, F64::max(-8388608.0, self.fRec20[1] - F64::min(8388607.0, F64::max(-8388608.0, F64::floor(fSlow11 * fTemp33)))));
			self.fRec21[0] = fTemp33;
			self.fVec5[0] = F64::min(8388607.0, F64::max(-8388608.0, 45056.0 + F64::min(8388607.0, F64::max(-8388608.0, F64::floor(0.00537109375 * F64::min(8388607.0, F64::max(-8388608.0, fSlow14 * self.fRec21[0] + fSlow13 * self.fRec20[0])))))));
			let mut iTemp34: i32 = (self.fVec5[1]) as i32;
			let mut fTemp35: F64 = (iTemp34 & 255) as F64;
			let mut fTemp36: F64 = F64::floor(0.00390625 * fTemp21);
			self.fVec6[(self.IOTA0 & 511) as usize] = fTemp36;
			let mut iTemp37: i32 = iTemp34 >> 8;
			self.fVec7[0] = F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (8388608.0 * self.fVec6[((i32::wrapping_sub(self.IOTA0, core::cmp::max(0, core::cmp::min(352, i32::wrapping_add(self.iVec0[2], iTemp37))))) & 511) as usize] * fTemp35 + 256.0 * self.fVec6[((i32::wrapping_sub(self.IOTA0, core::cmp::max(0, core::cmp::min(352, iTemp37)))) & 511) as usize] * (8388607.0 - 32768.0 * fTemp35)))));
			self.fVec8[0] = F64::min(8388607.0, F64::max(-8388608.0, F64::floor(0.5 * (fTemp21 + self.fVec7[1]))));
			let mut fTemp38: F64 = F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (self.fRec14[0] + 8388608.0 * (self.fVec8[1] - fTemp32) + fSlow8 * self.fRec6[1]))));
			self.fVec9[0] = fTemp38;
			self.fRec7[0] = 8379644.0 * (fTemp38 - self.fVec9[1]) + 8.37068e+06 * F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * self.fRec7[1])));
			self.fRec8[(self.IOTA0 & 2047) as usize] = 256.0 * F64::floor(0.00390625 * F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (fSlow7 * fTemp10 - fSlow6 * F64::min(8388607.0, F64::max(-8388608.0, F64::floor(fSlow2 * fTemp11))))))));
			self.fVec10[0] = fSlow15;
			let mut fTemp39: F64 = self.fVec10[1] + 83887.0 * fTemp31;
			let mut fTemp40: F64 = 8388607.0 - (self.fVec10[1] + 83886.0 * fTemp31);
			self.fVec11[0] = F64::min(8388607.0, F64::max(-8388608.0, fTemp32));
			let mut fTemp41: F64 = F64::floor(1.1920928955078125e-07 * self.fRec7[0]);
			self.fVec12[(self.IOTA0 & 511) as usize] = F64::floor(0.00390625 * F64::min(8388607.0, F64::max(-8388608.0, fTemp41)));
			let mut fTemp42: F64 = F64::min(8388607.0, F64::max(-8388608.0, F64::floor(3.0517578125e-05 * self.fVec12[((i32::wrapping_sub(self.IOTA0, 446)) & 511) as usize] * self.fVec11[1])));
			let mut fTemp43: F64 = 2.0 * (16.0 * fTemp42 - 8388608.0 * F64::floor(1.9073486328125e-06 * fTemp42));
			self.fVec13[0] = F64::min(8388607.0, F64::max(-8388608.0, 4.0 * F64::min(8388607.0, F64::max(-8388608.0, 4.0 * F64::min(8388607.0, F64::max(-8388608.0, F64::floor(fSlow16 * F64::min(8388607.0, F64::max(-8388608.0, 4.0 * (fTemp43 - (if (fTemp43 >= 8388608.0) as i32 != 0 {16777216.0} else {0.0})))))))))));
			let mut iTemp44: i32 = (F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (67645734912.0 - 8388608.0 * fTemp41 + self.fRec7[0] + 8064.0 * self.fVec13[1]))))) as i32;
			let mut fTemp45: F64 = (iTemp44 & 255) as F64;
			let mut iTemp46: i32 = (iTemp44 & 16777215) >> 8;
			let mut iTemp47: i32 = i32::wrapping_add(65, iTemp46);
			let mut iTemp48: i32 = i32::wrapping_add(i32::wrapping_add(self.iVec0[1], 65), iTemp46);
			self.fRec9[0] = F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (F64::min(8388607.0, F64::max(-8388608.0, F64::floor(fSlow17 * F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (8388608.0 * (self.itbl0EnvelopeDspSIG0[(core::cmp::max(0, core::cmp::min((if (iTemp48 < 130) as i32 != 0 {iTemp48} else {130}), 130))) as usize]) as F64 * fTemp45 + 256.0 * (self.itbl0EnvelopeDspSIG0[(core::cmp::max(0, core::cmp::min((if (iTemp47 < 130) as i32 != 0 {iTemp47} else {130}), 130))) as usize]) as F64 * (8388607.0 - 32768.0 * fTemp45)))))))) * fTemp40 - self.fRec9[1] * fTemp39))));
			self.fRec10[0] = F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (fTemp40 * self.fRec9[0] - fTemp39 * self.fRec10[1]))));
			self.fVec14[0] = fSlow18;
			let mut fTemp49: F64 = 8388607.0 - (self.fVec14[1] + 6710887.0 * fTemp31);
			self.fRec11[0] = F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (fSlow18 * self.fRec10[0] + self.fRec11[1] * fTemp49))));
			self.fRec12[0] = F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (fSlow18 * self.fRec11[0] + fTemp49 * self.fRec12[1]))));
			self.fRec13[0] = F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (fSlow18 * self.fRec12[0] + self.fRec13[1] * fTemp49))));
			let mut fTemp50: F64 = fSlow19 * self.fRec13[1];
			let mut fTemp51: F64 = (*input0) as F64;
			*output0 = (3.0517578125e-05 * F64::floor(0.00390625 * F64::min(8388607.0, F64::max(-8388608.0, F64::floor(0.999969482421875 * F64::min(8388607.0, F64::max(-8388608.0, F64::min(8388607.0, F64::max(-8388608.0, 256.0 * F64::min(32767.0, F64::max(-32768.0, ((if (fTemp51 < 0.0) as i32 != 0 {-0.5} else {0.5}) + 32768.0 * fTemp51) as i32 as F64)) + self.fVec2[1])) + F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (fSlow20 * fTemp9 + fTemp50))))))))))) as FaustFloat;
			let mut fTemp52: F64 = (*input1) as F64;
			*output1 = (3.0517578125e-05 * F64::floor(0.00390625 * F64::min(8388607.0, F64::max(-8388608.0, F64::floor(0.999969482421875 * F64::min(8388607.0, F64::max(-8388608.0, F64::min(8388607.0, F64::max(-8388608.0, 256.0 * F64::min(32767.0, F64::max(-32768.0, ((if (fTemp52 < 0.0) as i32 != 0 {-0.5} else {0.5}) + 32768.0 * fTemp52) as i32 as F64)) + self.fVec1[1])) + F64::min(8388607.0, F64::max(-8388608.0, F64::floor(1.1920928955078125e-07 * (fSlow20 * fTemp7 + fTemp50))))))))))) as FaustFloat;
			*output2 = (1.1920928955078125e-07 * F64::max(F64::abs(self.fRec13[1]), F64::max(F64::abs(self.fVec2[1]), F64::abs(self.fVec1[1])))) as FaustFloat;
			self.iVec0[2] = self.iVec0[1];
			self.iVec0[1] = self.iVec0[0];
			self.IOTA0 = i32::wrapping_add(self.IOTA0, 1);
			self.fRec0[1] = self.fRec0[0];
			self.fVec1[1] = self.fVec1[0];
			self.fVec2[1] = self.fVec2[0];
			self.fRec6[1] = self.fRec6[0];
			self.fVec3[2] = self.fVec3[1];
			self.fVec3[1] = self.fVec3[0];
			self.fRec18[1] = self.fRec18[0];
			self.fRec16[1] = self.fRec16[0];
			self.fRec17[1] = self.fRec17[0];
			self.iRec15[1] = self.iRec15[0];
			self.fVec4[1] = self.fVec4[0];
			self.fRec14[1] = self.fRec14[0];
			self.fRec20[1] = self.fRec20[0];
			self.fRec21[1] = self.fRec21[0];
			self.fVec5[1] = self.fVec5[0];
			self.fVec7[1] = self.fVec7[0];
			self.fVec8[1] = self.fVec8[0];
			self.fVec9[1] = self.fVec9[0];
			self.fRec7[1] = self.fRec7[0];
			self.fVec10[1] = self.fVec10[0];
			self.fVec11[1] = self.fVec11[0];
			self.fVec13[1] = self.fVec13[0];
			self.fRec9[1] = self.fRec9[0];
			self.fRec10[1] = self.fRec10[0];
			self.fVec14[1] = self.fVec14[0];
			self.fRec11[1] = self.fRec11[0];
			self.fRec12[1] = self.fRec12[0];
			self.fRec13[1] = self.fRec13[0];
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

impl FaustDsp for EnvelopeDsp {
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
