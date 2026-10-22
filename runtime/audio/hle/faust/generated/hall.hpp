/* ------------------------------------------------------------
name: "hall"
Code generated with Faust 2.88.0 (https://faust.grame.fr)
Compilation options: -a runtime/audio/hle/faust/architecture.hpp -lang cpp -fpga-mem-th 4 -ct 1 -cn HallDsp -es 1 -mcd 16 -mdd 1024 -mdy 33 -double -ftz 0
------------------------------------------------------------ */

#ifndef  __HallDsp_H__
#define  __HallDsp_H__

// FAUST C++ architecture for f3rt DSP classes. cmake/faust.cmake runs:
//   faust -a runtime/audio/hle/faust/architecture.hpp -cn <Class> [-double] <file>.dsp
// The generated class lives in f3rt::hle::faust and derives from the unmodified FAUST dsp
// interface in runtime/third_party/faust.

// FAUST repeats these standard includes inside the namespace below. Including them first makes
// those repeats no-ops, since a standard header first seen inside a namespace is ill-formed.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include <faust/dsp/dsp.h>
#include <faust/gui/UI.h>
#include <faust/gui/meta.h>

#include "audio/hle/faust/support.hpp"


namespace f3rt::hle::faust {

// FAUST's classInit(int sample_rate) ignores its argument unless the DSP has tables; the warning
// is noise in generated code, so it is suppressed for the class body only.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#ifndef FAUSTFLOAT
#define FAUSTFLOAT float
#endif 

#include <algorithm>
#include <cmath>
#include <cstdint>
#ifndef FAUST_INT_WRAP
#define FAUST_INT_WRAP
inline int faust_wrap_add(int a, int b) { return int((unsigned int)a + (unsigned int)b); }
inline int faust_wrap_sub(int a, int b) { return int((unsigned int)a - (unsigned int)b); }
inline int faust_wrap_mul(int a, int b) { return int((unsigned int)a * (unsigned int)b); }
#endif


#ifndef FAUSTCLASS 
#define FAUSTCLASS HallDsp
#endif

#ifdef __APPLE__ 
#define exp10f __exp10f
#define exp10 __exp10
#endif

#if defined(_WIN32)
#define RESTRICT __restrict
#else
#define RESTRICT __restrict__
#endif


class HallDsp : public dsp {
	
 private:
	
	int iVec0[2];
	FAUSTFLOAT fHslider0;
	FAUSTFLOAT fHslider1;
	double fRec32[2];
	double fRec33[2];
	FAUSTFLOAT fHslider2;
	FAUSTFLOAT fHslider3;
	FAUSTFLOAT fHslider4;
	double fVec1[4];
	int IOTA0;
	double fRec0[2048];
	double fRec1[2048];
	FAUSTFLOAT fHslider5;
	double fRec2[2];
	double fRec3[2];
	double fRec4[512];
	double fRec5[2];
	double fRec6[2];
	double fRec7[1024];
	double fRec8[1024];
	double fRec9[1024];
	FAUSTFLOAT fHslider6;
	FAUSTFLOAT fHslider7;
	FAUSTFLOAT fHslider8;
	FAUSTFLOAT fHslider9;
	FAUSTFLOAT fHslider10;
	double fRec34[2];
	double fRec11[512];
	double fRec12[256];
	double fRec13[512];
	double fRec14[128];
	double fRec35[2];
	double fRec15[128];
	FAUSTFLOAT fHslider11;
	double fRec17[256];
	double fRec18[512];
	FAUSTFLOAT fHslider12;
	double fRec19[2];
	double fRec20[256];
	double fRec21[2];
	double fRec22[2];
	double fRec23[1024];
	double fRec24[1024];
	double fRec25[256];
	double fRec26[256];
	double fRec27[2];
	double fRec28[2];
	double fRec29[2];
	double fRec30[8];
	double fRec31[1024];
	double fVec2[2];
	FAUSTFLOAT fHslider13;
	FAUSTFLOAT fHslider14;
	double fVec3[2];
	int fSampleRate;
	
 public:
	HallDsp() {
	}
	
	HallDsp(const HallDsp&) = default;
	
	virtual ~HallDsp() = default;
	
	HallDsp& operator=(const HallDsp&) = default;
	
	void metadata(Meta* m) { 
		m->declare("compile_options", "-a runtime/audio/hle/faust/architecture.hpp -lang cpp -fpga-mem-th 4 -ct 1 -cn HallDsp -es 1 -mcd 16 -mdd 1024 -mdy 33 -double -ftz 0");
		m->declare("filename", "hall.dsp");
		m->declare("name", "hall");
	}

	virtual int getNumInputs() {
		return 6;
	}
	virtual int getNumOutputs() {
		return 3;
	}
	
	static void classInit(int sample_rate) {
	}
	
	virtual void instanceConstants(int sample_rate) {
		fSampleRate = sample_rate;
	}
	
	virtual void instanceResetUserInterface() {
		fHslider0 = static_cast<FAUSTFLOAT>(0.0);
		fHslider1 = static_cast<FAUSTFLOAT>(0.0);
		fHslider2 = static_cast<FAUSTFLOAT>(0.0);
		fHslider3 = static_cast<FAUSTFLOAT>(0.0);
		fHslider4 = static_cast<FAUSTFLOAT>(8.0);
		fHslider5 = static_cast<FAUSTFLOAT>(0.3125);
		fHslider6 = static_cast<FAUSTFLOAT>(0.65625);
		fHslider7 = static_cast<FAUSTFLOAT>(0.04689);
		fHslider8 = static_cast<FAUSTFLOAT>(0.25);
		fHslider9 = static_cast<FAUSTFLOAT>(0.2448);
		fHslider10 = static_cast<FAUSTFLOAT>(0.2448);
		fHslider11 = static_cast<FAUSTFLOAT>(0.52295);
		fHslider12 = static_cast<FAUSTFLOAT>(0.5);
		fHslider13 = static_cast<FAUSTFLOAT>(0.9695);
		fHslider14 = static_cast<FAUSTFLOAT>(0.9695);
	}
	
	virtual void instanceClear() {
		for (int l0 = 0; l0 < 2; l0 = faust_wrap_add(l0, 1)) {
			iVec0[l0] = 0;
		}
		for (int l1 = 0; l1 < 2; l1 = faust_wrap_add(l1, 1)) {
			fRec32[l1] = 0.0;
		}
		for (int l2 = 0; l2 < 2; l2 = faust_wrap_add(l2, 1)) {
			fRec33[l2] = 0.0;
		}
		for (int l3 = 0; l3 < 4; l3 = faust_wrap_add(l3, 1)) {
			fVec1[l3] = 0.0;
		}
		IOTA0 = 0;
		for (int l4 = 0; l4 < 2048; l4 = faust_wrap_add(l4, 1)) {
			fRec0[l4] = 0.0;
		}
		for (int l5 = 0; l5 < 2048; l5 = faust_wrap_add(l5, 1)) {
			fRec1[l5] = 0.0;
		}
		for (int l6 = 0; l6 < 2; l6 = faust_wrap_add(l6, 1)) {
			fRec2[l6] = 0.0;
		}
		for (int l7 = 0; l7 < 2; l7 = faust_wrap_add(l7, 1)) {
			fRec3[l7] = 0.0;
		}
		for (int l8 = 0; l8 < 512; l8 = faust_wrap_add(l8, 1)) {
			fRec4[l8] = 0.0;
		}
		for (int l9 = 0; l9 < 2; l9 = faust_wrap_add(l9, 1)) {
			fRec5[l9] = 0.0;
		}
		for (int l10 = 0; l10 < 2; l10 = faust_wrap_add(l10, 1)) {
			fRec6[l10] = 0.0;
		}
		for (int l11 = 0; l11 < 1024; l11 = faust_wrap_add(l11, 1)) {
			fRec7[l11] = 0.0;
		}
		for (int l12 = 0; l12 < 1024; l12 = faust_wrap_add(l12, 1)) {
			fRec8[l12] = 0.0;
		}
		for (int l13 = 0; l13 < 1024; l13 = faust_wrap_add(l13, 1)) {
			fRec9[l13] = 0.0;
		}
		for (int l14 = 0; l14 < 2; l14 = faust_wrap_add(l14, 1)) {
			fRec34[l14] = 0.0;
		}
		for (int l15 = 0; l15 < 512; l15 = faust_wrap_add(l15, 1)) {
			fRec11[l15] = 0.0;
		}
		for (int l16 = 0; l16 < 256; l16 = faust_wrap_add(l16, 1)) {
			fRec12[l16] = 0.0;
		}
		for (int l17 = 0; l17 < 512; l17 = faust_wrap_add(l17, 1)) {
			fRec13[l17] = 0.0;
		}
		for (int l18 = 0; l18 < 128; l18 = faust_wrap_add(l18, 1)) {
			fRec14[l18] = 0.0;
		}
		for (int l19 = 0; l19 < 2; l19 = faust_wrap_add(l19, 1)) {
			fRec35[l19] = 0.0;
		}
		for (int l20 = 0; l20 < 128; l20 = faust_wrap_add(l20, 1)) {
			fRec15[l20] = 0.0;
		}
		for (int l21 = 0; l21 < 256; l21 = faust_wrap_add(l21, 1)) {
			fRec17[l21] = 0.0;
		}
		for (int l22 = 0; l22 < 512; l22 = faust_wrap_add(l22, 1)) {
			fRec18[l22] = 0.0;
		}
		for (int l23 = 0; l23 < 2; l23 = faust_wrap_add(l23, 1)) {
			fRec19[l23] = 0.0;
		}
		for (int l24 = 0; l24 < 256; l24 = faust_wrap_add(l24, 1)) {
			fRec20[l24] = 0.0;
		}
		for (int l25 = 0; l25 < 2; l25 = faust_wrap_add(l25, 1)) {
			fRec21[l25] = 0.0;
		}
		for (int l26 = 0; l26 < 2; l26 = faust_wrap_add(l26, 1)) {
			fRec22[l26] = 0.0;
		}
		for (int l27 = 0; l27 < 1024; l27 = faust_wrap_add(l27, 1)) {
			fRec23[l27] = 0.0;
		}
		for (int l28 = 0; l28 < 1024; l28 = faust_wrap_add(l28, 1)) {
			fRec24[l28] = 0.0;
		}
		for (int l29 = 0; l29 < 256; l29 = faust_wrap_add(l29, 1)) {
			fRec25[l29] = 0.0;
		}
		for (int l30 = 0; l30 < 256; l30 = faust_wrap_add(l30, 1)) {
			fRec26[l30] = 0.0;
		}
		for (int l31 = 0; l31 < 2; l31 = faust_wrap_add(l31, 1)) {
			fRec27[l31] = 0.0;
		}
		for (int l32 = 0; l32 < 2; l32 = faust_wrap_add(l32, 1)) {
			fRec28[l32] = 0.0;
		}
		for (int l33 = 0; l33 < 2; l33 = faust_wrap_add(l33, 1)) {
			fRec29[l33] = 0.0;
		}
		for (int l34 = 0; l34 < 8; l34 = faust_wrap_add(l34, 1)) {
			fRec30[l34] = 0.0;
		}
		for (int l35 = 0; l35 < 1024; l35 = faust_wrap_add(l35, 1)) {
			fRec31[l35] = 0.0;
		}
		for (int l36 = 0; l36 < 2; l36 = faust_wrap_add(l36, 1)) {
			fVec2[l36] = 0.0;
		}
		for (int l37 = 0; l37 < 2; l37 = faust_wrap_add(l37, 1)) {
			fVec3[l37] = 0.0;
		}
	}
	
	virtual void init(int sample_rate) {
		classInit(sample_rate);
		instanceInit(sample_rate);
	}
	
	virtual void instanceInit(int sample_rate) {
		instanceConstants(sample_rate);
		instanceResetUserInterface();
		instanceClear();
	}
	
	virtual HallDsp* clone() {
		return new HallDsp(*this);
	}
	
	virtual int getSampleRate() {
		return fSampleRate;
	}
	
	virtual void buildUserInterface(UI* ui_interface) {
		ui_interface->openVerticalBox("hall");
		ui_interface->addHorizontalSlider("chorus_out", &fHslider14, FAUSTFLOAT(0.9695), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("chorus_to_reverb", &fHslider9, FAUSTFLOAT(0.2448), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("damp", &fHslider5, FAUSTFLOAT(0.3125), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("decay", &fHslider8, FAUSTFLOAT(0.25), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("depth", &fHslider4, FAUSTFLOAT(8.0), FAUSTFLOAT(0.0), FAUSTFLOAT(128.0), FAUSTFLOAT(0.001));
		ui_interface->addHorizontalSlider("dry_send", &fHslider13, FAUSTFLOAT(0.9695), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("hall_a", &fHslider12, FAUSTFLOAT(0.5), FAUSTFLOAT(0.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("hall_g", &fHslider6, FAUSTFLOAT(0.65625), FAUSTFLOAT(0.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("hall_g108", &fHslider7, FAUSTFLOAT(0.04689), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(1e-05));
		ui_interface->addHorizontalSlider("hall_g2", &fHslider11, FAUSTFLOAT(0.52295), FAUSTFLOAT(0.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("lfo_c_eps_hi", &fHslider0, FAUSTFLOAT(0.0), FAUSTFLOAT(-1.0), FAUSTFLOAT(1.0), FAUSTFLOAT(1e-09));
		ui_interface->addHorizontalSlider("lfo_c_eps_lo", &fHslider1, FAUSTFLOAT(0.0), FAUSTFLOAT(-1e-06), FAUSTFLOAT(1e-06), FAUSTFLOAT(1e-15));
		ui_interface->addHorizontalSlider("lfo_c_ph_hi", &fHslider2, FAUSTFLOAT(0.0), FAUSTFLOAT(-7.0), FAUSTFLOAT(7.0), FAUSTFLOAT(1e-09));
		ui_interface->addHorizontalSlider("lfo_c_ph_lo", &fHslider3, FAUSTFLOAT(0.0), FAUSTFLOAT(-1e-06), FAUSTFLOAT(1e-06), FAUSTFLOAT(1e-15));
		ui_interface->addHorizontalSlider("send", &fHslider10, FAUSTFLOAT(0.2448), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->closeBox();
	}
	
	virtual void compute(int count, FAUSTFLOAT** RESTRICT inputs, FAUSTFLOAT** RESTRICT outputs) {
		FAUSTFLOAT* input0 = inputs[0];
		FAUSTFLOAT* input1 = inputs[1];
		FAUSTFLOAT* input2 = inputs[2];
		FAUSTFLOAT* input3 = inputs[3];
		FAUSTFLOAT* input4 = inputs[4];
		FAUSTFLOAT* input5 = inputs[5];
		FAUSTFLOAT* output0 = outputs[0];
		FAUSTFLOAT* output1 = outputs[1];
		FAUSTFLOAT* output2 = outputs[2];
		double fSlow0 = static_cast<double>(fHslider1) + static_cast<double>(fHslider0);
		double fSlow1 = std::sin(fSlow0);
		double fSlow2 = std::cos(fSlow0);
		double fSlow3 = static_cast<double>(fHslider3) + static_cast<double>(fHslider2);
		double fSlow4 = std::sin(fSlow3);
		double fSlow5 = std::cos(fSlow3);
		double fSlow6 = static_cast<double>(fHslider4);
		double fSlow7 = static_cast<double>(fHslider5);
		double fSlow8 = 1.0 - fSlow7;
		double fSlow9 = static_cast<double>(fHslider6);
		double fSlow10 = static_cast<double>(fHslider7);
		double fSlow11 = static_cast<double>(fHslider8);
		double fSlow12 = static_cast<double>(fHslider9);
		double fSlow13 = static_cast<double>(fHslider10);
		double fSlow14 = static_cast<double>(fHslider11);
		double fSlow15 = static_cast<double>(fHslider12);
		double fSlow16 = static_cast<double>(fHslider13);
		double fSlow17 = static_cast<double>(fHslider14);
		for (int i0 = 0; i0 < count; i0 = faust_wrap_add(i0, 1)) {
			iVec0[0] = 1;
			double fTemp0 = static_cast<double>(faust_wrap_sub(1, iVec0[1])) + fRec32[1];
			fRec32[0] = fSlow2 * fTemp0 - fSlow1 * fRec33[1];
			fRec33[0] = fSlow1 * fTemp0 + fSlow2 * fRec33[1];
			fVec1[0] = fSlow6 * (fSlow5 * fTemp0 - fSlow4 * fRec33[1]);
			double fTemp1 = 611.0 + fVec1[3];
			double fTemp2 = std::max<double>(0.0, std::min<double>(1.1e+03, fTemp1));
			int iTemp3 = static_cast<int>(fTemp2);
			double fTemp4 = fRec0[(faust_wrap_sub(IOTA0, faust_wrap_add(1, iTemp3))) & 2047];
			double fTemp5 = ((fTemp1 >= 0.0) ? fTemp4 + (fTemp2 - static_cast<double>(iTemp3)) * (fRec0[(faust_wrap_sub(IOTA0, faust_wrap_add(2, iTemp3))) & 2047] - fTemp4) : 0.0);
			double fTemp6 = fRec22[1] + 0.25 * fTemp5;
			fRec0[IOTA0 & 2047] = fTemp6;
			double fTemp7 = 475.0 + fVec1[3];
			double fTemp8 = std::max<double>(0.0, std::min<double>(1.1e+03, fTemp7));
			int iTemp9 = static_cast<int>(fTemp8);
			double fTemp10 = fRec1[(faust_wrap_sub(IOTA0, faust_wrap_add(1, iTemp9))) & 2047];
			double fTemp11 = ((fTemp7 >= 0.0) ? fTemp10 + (fTemp8 - static_cast<double>(iTemp9)) * (fRec1[(faust_wrap_sub(IOTA0, faust_wrap_add(2, iTemp9))) & 2047] - fTemp10) : 0.0);
			double fTemp12 = fRec21[1] + 0.25 * fTemp11;
			fRec1[IOTA0 & 2047] = fTemp12;
			fRec2[0] = fSlow7 * fRec2[1] + fSlow8 * fRec30[7];
			double fTemp13 = fRec31[(faust_wrap_sub(IOTA0, 675)) & 1023];
			fRec3[0] = fSlow7 * fRec3[1] + fSlow8 * fTemp13;
			fRec4[IOTA0 & 511] = fTemp5 - 0.25 * fTemp6;
			fRec5[0] = 0.875 * fRec5[1] + 0.125 * fRec30[7];
			fRec6[0] = 0.875 * fRec6[1] + 0.125 * fTemp13;
			double fTemp14 = fRec4[(faust_wrap_sub(IOTA0, 368)) & 511];
			double fTemp15 = fRec27[1] + 0.25 * fTemp14;
			fRec7[IOTA0 & 1023] = fTemp14 - 0.25 * fTemp15;
			double fTemp16 = fRec8[(faust_wrap_sub(IOTA0, 874)) & 1023];
			double fTemp17 = fRec29[1] + 0.25 * fTemp16;
			fRec8[IOTA0 & 1023] = fTemp17;
			double fTemp18 = fRec9[(faust_wrap_sub(IOTA0, 836)) & 1023];
			double fTemp19 = fRec19[1] + 0.25 * fTemp18;
			fRec9[IOTA0 & 1023] = fTemp19;
			double fTemp20 = fRec14[(faust_wrap_sub(IOTA0, 102)) & 127];
			double fTemp21 = static_cast<double>(input2[i0]);
			double fTemp22 = static_cast<double>(input4[i0]);
			fRec34[0] = 0.9921875 * (fSlow13 * fTemp22 + fSlow12 * fTemp21) + 0.0078125 * fRec34[1];
			double fTemp23 = 0.5 * fRec34[1] + fSlow11 * fRec2[1] + fSlow10 * fRec5[1] + fSlow9 * fTemp20;
			double fRec10 = fTemp20 - fSlow9 * fTemp23;
			fRec11[IOTA0 & 511] = fTemp18 - 0.25 * fTemp19;
			fRec12[IOTA0 & 255] = fTemp16 - 0.25 * fTemp17;
			fRec13[IOTA0 & 511] = fTemp11 - 0.25 * fTemp12;
			fRec14[IOTA0 & 127] = fTemp23;
			double fTemp24 = fRec15[(faust_wrap_sub(IOTA0, 88)) & 127];
			double fTemp25 = static_cast<double>(input3[i0]);
			double fTemp26 = static_cast<double>(input5[i0]);
			fRec35[0] = 0.9921875 * (fSlow13 * fTemp26 + fSlow12 * fTemp25) + 0.0078125 * fRec35[1];
			double fTemp27 = 0.5 * fRec35[1] + fSlow11 * fRec3[1] + fSlow10 * fRec6[1] + fSlow9 * fTemp24;
			fRec15[IOTA0 & 127] = fTemp27;
			double fRec16 = fTemp24 - fSlow9 * fTemp27;
			double fTemp28 = fRec17[(faust_wrap_sub(IOTA0, 141)) & 255];
			double fTemp29 = fRec16 + fSlow14 * fTemp28;
			fRec17[IOTA0 & 255] = fTemp29;
			double fTemp30 = fRec20[(faust_wrap_sub(IOTA0, 168)) & 255];
			double fTemp31 = fRec10 + fSlow14 * fTemp30;
			fRec18[IOTA0 & 511] = fTemp30 - fSlow14 * fTemp31;
			double fTemp32 = fRec11[(faust_wrap_sub(IOTA0, 334)) & 511];
			double fTemp33 = fRec18[(faust_wrap_sub(IOTA0, 449)) & 511] + fSlow15 * fTemp32;
			fRec19[0] = fTemp33;
			fRec20[IOTA0 & 255] = fTemp31;
			double fTemp34 = fRec13[(faust_wrap_sub(IOTA0, 423)) & 511];
			double fTemp35 = fRec28[1] + 0.25 * fTemp34;
			fRec21[0] = fTemp35;
			fRec22[0] = fTemp15;
			fRec23[IOTA0 & 1023] = fTemp34 - 0.25 * fTemp35;
			fRec24[IOTA0 & 1023] = fTemp28 - fSlow14 * fTemp29;
			fRec25[IOTA0 & 255] = fTemp32 - fSlow15 * fTemp33;
			double fTemp36 = fRec12[(faust_wrap_sub(IOTA0, 231)) & 255];
			double fTemp37 = fRec24[(faust_wrap_sub(IOTA0, 628)) & 1023] + fSlow15 * fTemp36;
			fRec26[IOTA0 & 255] = fTemp36 - fSlow15 * fTemp37;
			double fTemp38 = fRec7[(faust_wrap_sub(IOTA0, 649)) & 1023];
			double fTemp39 = fRec26[(faust_wrap_sub(IOTA0, 214)) & 255] + fSlow15 * fTemp38;
			fRec27[0] = fTemp39;
			double fTemp40 = fRec23[(faust_wrap_sub(IOTA0, 616)) & 1023];
			double fTemp41 = fRec25[(faust_wrap_sub(IOTA0, 145)) & 255] + fSlow15 * fTemp40;
			fRec28[0] = fTemp41;
			fRec29[0] = fTemp37;
			fRec30[0] = fTemp38 - fSlow15 * fTemp39;
			fRec31[IOTA0 & 1023] = fTemp40 - fSlow15 * fTemp41;
			double fTemp42 = 0.4375 * (fRec26[(faust_wrap_sub(IOTA0, 163)) & 255] - fRec25[(faust_wrap_sub(IOTA0, 131)) & 255]) + 0.125 * fRec30[3] + 0.9375 * fRec24[(faust_wrap_sub(IOTA0, 101)) & 1023];
			fVec2[0] = fTemp42;
			output0[i0] = static_cast<FAUSTFLOAT>(fSlow17 * fTemp21 + static_cast<double>(input0[i0]) + fSlow16 * fTemp22 + fVec2[1]);
			double fTemp43 = 0.125 * fRec31[(faust_wrap_sub(IOTA0, 225)) & 1023] + 0.5 * fRec18[(faust_wrap_sub(IOTA0, 1)) & 511] - 0.4375 * (fRec26[(faust_wrap_sub(IOTA0, 74)) & 255] - fRec25[(faust_wrap_sub(IOTA0, 10)) & 255]);
			fVec3[0] = fTemp43;
			output1[i0] = static_cast<FAUSTFLOAT>(fSlow17 * fTemp25 + static_cast<double>(input1[i0]) + fSlow16 * fTemp26 + fVec3[1]);
			output2[i0] = static_cast<FAUSTFLOAT>(std::max<double>(std::max<double>(std::fabs(fTemp42), std::max<double>(std::fabs(fTemp43), std::fabs(fRec30[7]))), std::max<double>(std::fabs(fTemp13), std::max<double>(std::fabs(fRec6[0]), std::fabs(fRec5[0])))));
			iVec0[1] = iVec0[0];
			fRec32[1] = fRec32[0];
			fRec33[1] = fRec33[0];
			for (int j0 = 3; j0 > 0; j0 = faust_wrap_sub(j0, 1)) {
				fVec1[j0] = fVec1[faust_wrap_sub(j0, 1)];
			}
			IOTA0 = faust_wrap_add(IOTA0, 1);
			fRec2[1] = fRec2[0];
			fRec3[1] = fRec3[0];
			fRec5[1] = fRec5[0];
			fRec6[1] = fRec6[0];
			fRec34[1] = fRec34[0];
			fRec35[1] = fRec35[0];
			fRec19[1] = fRec19[0];
			fRec21[1] = fRec21[0];
			fRec22[1] = fRec22[0];
			fRec27[1] = fRec27[0];
			fRec28[1] = fRec28[0];
			fRec29[1] = fRec29[0];
			for (int j1 = 7; j1 > 0; j1 = faust_wrap_sub(j1, 1)) {
				fRec30[j1] = fRec30[faust_wrap_sub(j1, 1)];
			}
			fVec2[1] = fVec2[0];
			fVec3[1] = fVec3[0];
		}
	}

};
#pragma GCC diagnostic pop

}  // namespace f3rt::hle::faust

// Macros FAUST defines for the class body (exp10 aliases on Apple included); undefined so they do
// not leak into other code.
#undef FAUSTCLASS
#undef RESTRICT
#undef exp10f
#undef exp10

#endif
