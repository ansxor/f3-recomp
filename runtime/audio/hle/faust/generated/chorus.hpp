/* ------------------------------------------------------------
name: "chorus"
Code generated with Faust 2.88.0 (https://faust.grame.fr)
Compilation options: -a runtime/audio/hle/faust/architecture.hpp -lang cpp -fpga-mem-th 4 -ct 1 -cn ChorusDsp -es 1 -mcd 16 -mdd 1024 -mdy 33 -double -ftz 0
------------------------------------------------------------ */

#ifndef  __ChorusDsp_H__
#define  __ChorusDsp_H__

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
#define FAUSTCLASS ChorusDsp
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


class ChorusDsp : public dsp {
	
 private:
	
	int IOTA0;
	int iVec0[2];
	double fRec0[2];
	FAUSTFLOAT fHslider0;
	FAUSTFLOAT fHslider1;
	double fRec16[2];
	double fRec17[2];
	FAUSTFLOAT fHslider2;
	FAUSTFLOAT fHslider3;
	FAUSTFLOAT fHslider4;
	double fVec1[4];
	double fRec1[1024];
	double fRec2[2];
	double fRec3[2048];
	double fRec4[1024];
	double fRec5[1024];
	double fRec6[2048];
	double fRec7[2];
	FAUSTFLOAT fHslider5;
	FAUSTFLOAT fHslider6;
	FAUSTFLOAT fHslider7;
	FAUSTFLOAT fHslider8;
	FAUSTFLOAT fHslider9;
	FAUSTFLOAT fHslider10;
	FAUSTFLOAT fHslider11;
	double fRec19[2];
	double fRec20[2];
	FAUSTFLOAT fHslider12;
	FAUSTFLOAT fHslider13;
	FAUSTFLOAT fHslider14;
	FAUSTFLOAT fHslider15;
	double fRec23[2];
	double fRec24[2];
	FAUSTFLOAT fHslider16;
	FAUSTFLOAT fHslider17;
	FAUSTFLOAT fHslider18;
	double fRec21[512];
	double fRec22[512];
	FAUSTFLOAT fHslider19;
	FAUSTFLOAT fHslider20;
	double fVec2[2];
	double fVec3[2];
	FAUSTFLOAT fHslider21;
	FAUSTFLOAT fHslider22;
	FAUSTFLOAT fHslider23;
	FAUSTFLOAT fHslider24;
	double fRec18[2];
	double fRec9[512];
	double fRec10[256];
	double fRec11[1024];
	double fRec12[256];
	double fRec13[2];
	double fRec14[2];
	double fRec15[2048];
	FAUSTFLOAT fHslider25;
	FAUSTFLOAT fHslider26;
	FAUSTFLOAT fHslider27;
	double fVec4[2];
	FAUSTFLOAT fHslider28;
	FAUSTFLOAT fHslider29;
	FAUSTFLOAT fHslider30;
	FAUSTFLOAT fHslider31;
	FAUSTFLOAT fHslider32;
	double fVec5[2];
	int fSampleRate;
	
 public:
	ChorusDsp() {
	}
	
	ChorusDsp(const ChorusDsp&) = default;
	
	virtual ~ChorusDsp() = default;
	
	ChorusDsp& operator=(const ChorusDsp&) = default;
	
	void metadata(Meta* m) { 
		m->declare("compile_options", "-a runtime/audio/hle/faust/architecture.hpp -lang cpp -fpga-mem-th 4 -ct 1 -cn ChorusDsp -es 1 -mcd 16 -mdd 1024 -mdy 33 -double -ftz 0");
		m->declare("filename", "chorus.dsp");
		m->declare("name", "chorus");
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
		fHslider5 = static_cast<FAUSTFLOAT>(0.4);
		fHslider6 = static_cast<FAUSTFLOAT>(0.04922);
		fHslider7 = static_cast<FAUSTFLOAT>(0.00703);
		fHslider8 = static_cast<FAUSTFLOAT>(0.3125);
		fHslider9 = static_cast<FAUSTFLOAT>(0.25);
		fHslider10 = static_cast<FAUSTFLOAT>(0.0);
		fHslider11 = static_cast<FAUSTFLOAT>(0.0);
		fHslider12 = static_cast<FAUSTFLOAT>(0.0);
		fHslider13 = static_cast<FAUSTFLOAT>(0.0);
		fHslider14 = static_cast<FAUSTFLOAT>(0.0);
		fHslider15 = static_cast<FAUSTFLOAT>(0.0);
		fHslider16 = static_cast<FAUSTFLOAT>(0.0);
		fHslider17 = static_cast<FAUSTFLOAT>(0.0);
		fHslider18 = static_cast<FAUSTFLOAT>(0.0);
		fHslider19 = static_cast<FAUSTFLOAT>(0.0);
		fHslider20 = static_cast<FAUSTFLOAT>(1.0);
		fHslider21 = static_cast<FAUSTFLOAT>(0.2448);
		fHslider22 = static_cast<FAUSTFLOAT>(0.2448);
		fHslider23 = static_cast<FAUSTFLOAT>(0.0075);
		fHslider24 = static_cast<FAUSTFLOAT>(0.7071);
		fHslider25 = static_cast<FAUSTFLOAT>(0.46667);
		fHslider26 = static_cast<FAUSTFLOAT>(0.66667);
		fHslider27 = static_cast<FAUSTFLOAT>(0.13333);
		fHslider28 = static_cast<FAUSTFLOAT>(0.9695);
		fHslider29 = static_cast<FAUSTFLOAT>(0.9695);
		fHslider30 = static_cast<FAUSTFLOAT>(0.66667);
		fHslider31 = static_cast<FAUSTFLOAT>(0.66667);
		fHslider32 = static_cast<FAUSTFLOAT>(0.13333);
	}
	
	virtual void instanceClear() {
		IOTA0 = 0;
		for (int l0 = 0; l0 < 2; l0 = faust_wrap_add(l0, 1)) {
			iVec0[l0] = 0;
		}
		for (int l1 = 0; l1 < 2; l1 = faust_wrap_add(l1, 1)) {
			fRec0[l1] = 0.0;
		}
		for (int l2 = 0; l2 < 2; l2 = faust_wrap_add(l2, 1)) {
			fRec16[l2] = 0.0;
		}
		for (int l3 = 0; l3 < 2; l3 = faust_wrap_add(l3, 1)) {
			fRec17[l3] = 0.0;
		}
		for (int l4 = 0; l4 < 4; l4 = faust_wrap_add(l4, 1)) {
			fVec1[l4] = 0.0;
		}
		for (int l5 = 0; l5 < 1024; l5 = faust_wrap_add(l5, 1)) {
			fRec1[l5] = 0.0;
		}
		for (int l6 = 0; l6 < 2; l6 = faust_wrap_add(l6, 1)) {
			fRec2[l6] = 0.0;
		}
		for (int l7 = 0; l7 < 2048; l7 = faust_wrap_add(l7, 1)) {
			fRec3[l7] = 0.0;
		}
		for (int l8 = 0; l8 < 1024; l8 = faust_wrap_add(l8, 1)) {
			fRec4[l8] = 0.0;
		}
		for (int l9 = 0; l9 < 1024; l9 = faust_wrap_add(l9, 1)) {
			fRec5[l9] = 0.0;
		}
		for (int l10 = 0; l10 < 2048; l10 = faust_wrap_add(l10, 1)) {
			fRec6[l10] = 0.0;
		}
		for (int l11 = 0; l11 < 2; l11 = faust_wrap_add(l11, 1)) {
			fRec7[l11] = 0.0;
		}
		for (int l12 = 0; l12 < 2; l12 = faust_wrap_add(l12, 1)) {
			fRec19[l12] = 0.0;
		}
		for (int l13 = 0; l13 < 2; l13 = faust_wrap_add(l13, 1)) {
			fRec20[l13] = 0.0;
		}
		for (int l14 = 0; l14 < 2; l14 = faust_wrap_add(l14, 1)) {
			fRec23[l14] = 0.0;
		}
		for (int l15 = 0; l15 < 2; l15 = faust_wrap_add(l15, 1)) {
			fRec24[l15] = 0.0;
		}
		for (int l16 = 0; l16 < 512; l16 = faust_wrap_add(l16, 1)) {
			fRec21[l16] = 0.0;
		}
		for (int l17 = 0; l17 < 512; l17 = faust_wrap_add(l17, 1)) {
			fRec22[l17] = 0.0;
		}
		for (int l18 = 0; l18 < 2; l18 = faust_wrap_add(l18, 1)) {
			fVec2[l18] = 0.0;
		}
		for (int l19 = 0; l19 < 2; l19 = faust_wrap_add(l19, 1)) {
			fVec3[l19] = 0.0;
		}
		for (int l20 = 0; l20 < 2; l20 = faust_wrap_add(l20, 1)) {
			fRec18[l20] = 0.0;
		}
		for (int l21 = 0; l21 < 512; l21 = faust_wrap_add(l21, 1)) {
			fRec9[l21] = 0.0;
		}
		for (int l22 = 0; l22 < 256; l22 = faust_wrap_add(l22, 1)) {
			fRec10[l22] = 0.0;
		}
		for (int l23 = 0; l23 < 1024; l23 = faust_wrap_add(l23, 1)) {
			fRec11[l23] = 0.0;
		}
		for (int l24 = 0; l24 < 256; l24 = faust_wrap_add(l24, 1)) {
			fRec12[l24] = 0.0;
		}
		for (int l25 = 0; l25 < 2; l25 = faust_wrap_add(l25, 1)) {
			fRec13[l25] = 0.0;
		}
		for (int l26 = 0; l26 < 2; l26 = faust_wrap_add(l26, 1)) {
			fRec14[l26] = 0.0;
		}
		for (int l27 = 0; l27 < 2048; l27 = faust_wrap_add(l27, 1)) {
			fRec15[l27] = 0.0;
		}
		for (int l28 = 0; l28 < 2; l28 = faust_wrap_add(l28, 1)) {
			fVec4[l28] = 0.0;
		}
		for (int l29 = 0; l29 < 2; l29 = faust_wrap_add(l29, 1)) {
			fVec5[l29] = 0.0;
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
	
	virtual ChorusDsp* clone() {
		return new ChorusDsp(*this);
	}
	
	virtual int getSampleRate() {
		return fSampleRate;
	}
	
	virtual void buildUserInterface(UI* ui_interface) {
		ui_interface->openVerticalBox("chorus");
		ui_interface->addHorizontalSlider("c1", &fHslider7, FAUSTFLOAT(0.00703), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(1e-05));
		ui_interface->addHorizontalSlider("c2", &fHslider6, FAUSTFLOAT(0.04922), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(1e-05));
		ui_interface->addHorizontalSlider("chorus_out", &fHslider28, FAUSTFLOAT(0.9695), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("chorus_to_reverb", &fHslider21, FAUSTFLOAT(0.2448), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("cos_mix", &fHslider20, FAUSTFLOAT(1.0), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("damp", &fHslider8, FAUSTFLOAT(0.3125), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("decay", &fHslider9, FAUSTFLOAT(0.25), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("depth", &fHslider4, FAUSTFLOAT(8.0), FAUSTFLOAT(0.0), FAUSTFLOAT(128.0), FAUSTFLOAT(0.001));
		ui_interface->addHorizontalSlider("dry_send", &fHslider29, FAUSTFLOAT(0.9695), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("feedback", &fHslider18, FAUSTFLOAT(0.0), FAUSTFLOAT(-0.95), FAUSTFLOAT(0.95), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("g", &fHslider5, FAUSTFLOAT(0.4), FAUSTFLOAT(0.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("leak", &fHslider23, FAUSTFLOAT(0.0075), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("lfo_a_eps_hi", &fHslider14, FAUSTFLOAT(0.0), FAUSTFLOAT(-1.0), FAUSTFLOAT(1.0), FAUSTFLOAT(1e-09));
		ui_interface->addHorizontalSlider("lfo_a_eps_lo", &fHslider15, FAUSTFLOAT(0.0), FAUSTFLOAT(-1e-06), FAUSTFLOAT(1e-06), FAUSTFLOAT(1e-15));
		ui_interface->addHorizontalSlider("lfo_a_ph_hi", &fHslider16, FAUSTFLOAT(0.0), FAUSTFLOAT(-7.0), FAUSTFLOAT(7.0), FAUSTFLOAT(1e-09));
		ui_interface->addHorizontalSlider("lfo_a_ph_lo", &fHslider17, FAUSTFLOAT(0.0), FAUSTFLOAT(-1e-06), FAUSTFLOAT(1e-06), FAUSTFLOAT(1e-15));
		ui_interface->addHorizontalSlider("lfo_b_eps_hi", &fHslider10, FAUSTFLOAT(0.0), FAUSTFLOAT(-1.0), FAUSTFLOAT(1.0), FAUSTFLOAT(1e-09));
		ui_interface->addHorizontalSlider("lfo_b_eps_lo", &fHslider11, FAUSTFLOAT(0.0), FAUSTFLOAT(-1e-06), FAUSTFLOAT(1e-06), FAUSTFLOAT(1e-15));
		ui_interface->addHorizontalSlider("lfo_b_ph_hi", &fHslider12, FAUSTFLOAT(0.0), FAUSTFLOAT(-7.0), FAUSTFLOAT(7.0), FAUSTFLOAT(1e-09));
		ui_interface->addHorizontalSlider("lfo_b_ph_lo", &fHslider13, FAUSTFLOAT(0.0), FAUSTFLOAT(-1e-06), FAUSTFLOAT(1e-06), FAUSTFLOAT(1e-15));
		ui_interface->addHorizontalSlider("lfo_c_eps_hi", &fHslider0, FAUSTFLOAT(0.0), FAUSTFLOAT(-1.0), FAUSTFLOAT(1.0), FAUSTFLOAT(1e-09));
		ui_interface->addHorizontalSlider("lfo_c_eps_lo", &fHslider1, FAUSTFLOAT(0.0), FAUSTFLOAT(-1e-06), FAUSTFLOAT(1e-06), FAUSTFLOAT(1e-15));
		ui_interface->addHorizontalSlider("lfo_c_ph_hi", &fHslider2, FAUSTFLOAT(0.0), FAUSTFLOAT(-7.0), FAUSTFLOAT(7.0), FAUSTFLOAT(1e-09));
		ui_interface->addHorizontalSlider("lfo_c_ph_lo", &fHslider3, FAUSTFLOAT(0.0), FAUSTFLOAT(-1e-06), FAUSTFLOAT(1e-06), FAUSTFLOAT(1e-15));
		ui_interface->addHorizontalSlider("scale", &fHslider24, FAUSTFLOAT(0.7071), FAUSTFLOAT(0.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("send", &fHslider22, FAUSTFLOAT(0.2448), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("sin_mix", &fHslider19, FAUSTFLOAT(0.0), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("tap_l0", &fHslider27, FAUSTFLOAT(0.13333), FAUSTFLOAT(-2.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("tap_l1", &fHslider26, FAUSTFLOAT(0.66667), FAUSTFLOAT(-2.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("tap_l2", &fHslider25, FAUSTFLOAT(0.46667), FAUSTFLOAT(-2.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("tap_r0", &fHslider32, FAUSTFLOAT(0.13333), FAUSTFLOAT(-2.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("tap_r1", &fHslider31, FAUSTFLOAT(0.66667), FAUSTFLOAT(-2.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("tap_r2", &fHslider30, FAUSTFLOAT(0.66667), FAUSTFLOAT(-2.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
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
		double fSlow8 = static_cast<double>(fHslider6);
		double fSlow9 = static_cast<double>(fHslider7);
		double fSlow10 = static_cast<double>(fHslider8);
		double fSlow11 = 1.0 - fSlow10;
		double fSlow12 = static_cast<double>(fHslider9);
		double fSlow13 = static_cast<double>(fHslider11) + static_cast<double>(fHslider10);
		double fSlow14 = std::cos(fSlow13);
		double fSlow15 = std::sin(fSlow13);
		double fSlow16 = static_cast<double>(fHslider13) + static_cast<double>(fHslider12);
		double fSlow17 = std::cos(fSlow16);
		double fSlow18 = std::sin(fSlow16);
		double fSlow19 = static_cast<double>(fHslider15) + static_cast<double>(fHslider14);
		double fSlow20 = std::sin(fSlow19);
		double fSlow21 = std::cos(fSlow19);
		double fSlow22 = static_cast<double>(fHslider17) + static_cast<double>(fHslider16);
		double fSlow23 = std::cos(fSlow22);
		double fSlow24 = std::sin(fSlow22);
		double fSlow25 = static_cast<double>(fHslider18);
		double fSlow26 = 0.7071 * static_cast<double>(fHslider19);
		double fSlow27 = static_cast<double>(fHslider20);
		double fSlow28 = static_cast<double>(fHslider21);
		double fSlow29 = static_cast<double>(fHslider22);
		double fSlow30 = static_cast<double>(fHslider23);
		double fSlow31 = static_cast<double>(fHslider24) * (1.0 - fSlow30);
		double fSlow32 = static_cast<double>(fHslider25);
		double fSlow33 = static_cast<double>(fHslider26);
		double fSlow34 = static_cast<double>(fHslider27);
		double fSlow35 = static_cast<double>(fHslider28);
		double fSlow36 = static_cast<double>(fHslider29);
		double fSlow37 = static_cast<double>(fHslider30);
		double fSlow38 = static_cast<double>(fHslider31);
		double fSlow39 = static_cast<double>(fHslider32);
		for (int i0 = 0; i0 < count; i0 = faust_wrap_add(i0, 1)) {
			double fTemp0 = fRec15[(faust_wrap_sub(IOTA0, 1105)) & 2047];
			iVec0[0] = 1;
			fRec0[0] = 0.375 * fRec0[1] + 0.625 * fTemp0;
			double fTemp1 = static_cast<double>(faust_wrap_sub(1, iVec0[1]));
			double fTemp2 = fTemp1 + fRec16[1];
			fRec16[0] = fSlow2 * fTemp2 - fSlow1 * fRec17[1];
			fRec17[0] = fSlow1 * fTemp2 + fSlow2 * fRec17[1];
			fVec1[0] = fSlow6 * (fSlow5 * fTemp2 - fSlow4 * fRec17[1]);
			double fTemp3 = 921.0 + fVec1[3];
			double fTemp4 = std::max<double>(0.0, std::min<double>(1.1e+03, fTemp3));
			int iTemp5 = static_cast<int>(fTemp4);
			double fTemp6 = fRec3[(faust_wrap_sub(IOTA0, faust_wrap_add(1, iTemp5))) & 2047];
			double fTemp7 = ((fTemp3 >= 0.0) ? fTemp6 + (fTemp4 - static_cast<double>(iTemp5)) * (fRec3[(faust_wrap_sub(IOTA0, faust_wrap_add(2, iTemp5))) & 2047] - fTemp6) : 0.0);
			double fTemp8 = fRec2[1] + 0.25 * fTemp7;
			fRec1[IOTA0 & 1023] = fTemp7 - 0.25 * fTemp8;
			double fTemp9 = fRec1[(faust_wrap_sub(IOTA0, 695)) & 1023];
			double fTemp10 = fRec13[1] + 0.25 * fTemp9;
			fRec2[0] = fTemp10;
			fRec3[IOTA0 & 2047] = fTemp8;
			fRec4[IOTA0 & 1023] = fTemp9 - 0.25 * fTemp10;
			double fTemp11 = fRec6[(faust_wrap_sub(IOTA0, 1369)) & 2047];
			double fTemp12 = fRec14[1] + 0.25 * fTemp11;
			fRec5[IOTA0 & 1023] = fTemp11 - 0.25 * fTemp12;
			fRec6[IOTA0 & 2047] = fTemp12;
			fRec7[0] = 0.875 * fRec7[1] + 0.125 * fRec0[1];
			double fTemp13 = fRec10[(faust_wrap_sub(IOTA0, 168)) & 255];
			double fTemp14 = fTemp1 + fRec20[1];
			fRec19[0] = fSlow15 * fTemp14 + fSlow14 * fRec19[1];
			fRec20[0] = fSlow14 * fTemp14 - fSlow15 * fRec19[1];
			double fTemp15 = 428.0 + 4e+01 * (fSlow18 * fTemp14 + fSlow17 * fRec19[1]);
			double fTemp16 = fTemp1 + fRec23[1];
			fRec23[0] = fSlow21 * fTemp16 - fSlow20 * fRec24[1];
			fRec24[0] = fSlow20 * fTemp16 + fSlow21 * fRec24[1];
			double fTemp17 = 261.0 + 4e+01 * (fSlow24 * fTemp16 + fSlow23 * fRec24[1]);
			double fTemp18 = std::max<double>(0.0, std::min<double>(4.7e+02, fTemp17));
			int iTemp19 = static_cast<int>(fTemp18);
			double fTemp20 = fRec22[(faust_wrap_sub(IOTA0, faust_wrap_add(1, iTemp19))) & 511];
			double fTemp21 = ((fTemp17 >= 0.0) ? fTemp20 + (fTemp18 - static_cast<double>(iTemp19)) * (fRec22[(faust_wrap_sub(IOTA0, faust_wrap_add(2, iTemp19))) & 511] - fTemp20) : 0.0);
			double fTemp22 = static_cast<double>(input2[i0]);
			fRec21[IOTA0 & 511] = fTemp22 + fSlow25 * fTemp21;
			double fTemp23 = 303.0 + 4e+01 * (fSlow17 * fTemp14 - fSlow18 * fRec19[1]);
			double fTemp24 = std::max<double>(0.0, std::min<double>(4.7e+02, fTemp23));
			int iTemp25 = static_cast<int>(fTemp24);
			double fTemp26 = fRec21[(faust_wrap_sub(IOTA0, faust_wrap_add(1, iTemp25))) & 511];
			double fTemp27 = ((fTemp23 >= 0.0) ? fTemp26 + (fTemp24 - static_cast<double>(iTemp25)) * (fRec21[(faust_wrap_sub(IOTA0, faust_wrap_add(2, iTemp25))) & 511] - fTemp26) : 0.0);
			double fTemp28 = static_cast<double>(input3[i0]);
			fRec22[IOTA0 & 511] = fTemp28 + fSlow25 * fTemp27;
			double fTemp29 = std::max<double>(0.0, std::min<double>(4.7e+02, fTemp15));
			int iTemp30 = static_cast<int>(fTemp29);
			double fTemp31 = fRec22[(faust_wrap_sub(IOTA0, faust_wrap_add(1, iTemp30))) & 511];
			double fTemp32 = fSlow27 * fTemp28 + fSlow26 * (fTemp21 + ((fTemp15 >= 0.0) ? fTemp31 + (fTemp29 - static_cast<double>(iTemp30)) * (fRec22[(faust_wrap_sub(IOTA0, faust_wrap_add(2, iTemp30))) & 511] - fTemp31) : 0.0));
			fVec2[0] = fTemp32;
			double fTemp33 = 7e+01 + 4e+01 * (fSlow23 * fTemp16 - fSlow24 * fRec24[1]);
			double fTemp34 = std::max<double>(0.0, std::min<double>(4.7e+02, fTemp33));
			int iTemp35 = static_cast<int>(fTemp34);
			double fTemp36 = fRec21[(faust_wrap_sub(IOTA0, faust_wrap_add(1, iTemp35))) & 511];
			double fTemp37 = fSlow27 * fTemp22 + fSlow26 * (fTemp27 + ((fTemp33 >= 0.0) ? fTemp36 + (fTemp34 - static_cast<double>(iTemp35)) * (fRec21[(faust_wrap_sub(IOTA0, faust_wrap_add(2, iTemp35))) & 511] - fTemp36) : 0.0));
			fVec3[0] = fTemp37;
			double fTemp38 = static_cast<double>(input5[i0]);
			double fTemp39 = static_cast<double>(input4[i0]);
			fRec18[0] = fSlow30 * fRec18[1] + fSlow31 * (fSlow29 * (fTemp39 + fTemp38) + fSlow28 * (fVec3[1] + fVec2[1]));
			double fTemp40 = fRec18[0] + fSlow12 * (fSlow11 * fTemp0 + fSlow10 * fRec0[1]) + fSlow9 * fRec0[1] + fSlow8 * fRec7[1] + fSlow7 * fTemp13;
			double fRec8 = fTemp13 - fSlow7 * fTemp40;
			double fTemp41 = fRec9[(faust_wrap_sub(IOTA0, 276)) & 511];
			double fTemp42 = fRec8 + fSlow7 * fTemp41;
			fRec9[IOTA0 & 511] = fTemp42;
			fRec10[IOTA0 & 255] = fTemp40;
			fRec11[IOTA0 & 1023] = fTemp41 - fSlow7 * fTemp42;
			double fTemp43 = fRec5[(faust_wrap_sub(IOTA0, 548)) & 1023];
			double fTemp44 = fRec11[(faust_wrap_sub(IOTA0, 755)) & 1023] + 0.5 * fTemp43;
			fRec12[IOTA0 & 255] = fTemp43 - 0.5 * fTemp44;
			double fTemp45 = fRec4[(faust_wrap_sub(IOTA0, 1011)) & 1023];
			double fTemp46 = fRec12[(faust_wrap_sub(IOTA0, 242)) & 255] + 0.5 * fTemp45;
			fRec13[0] = fTemp46;
			fRec14[0] = fTemp44;
			fRec15[IOTA0 & 2047] = fTemp45 - 0.5 * fTemp46;
			double fTemp47 = fSlow34 * fRec15[(faust_wrap_sub(IOTA0, 384)) & 2047] + fSlow33 * fRec11[(faust_wrap_sub(IOTA0, 4)) & 1023] + fSlow32 * fRec12[(faust_wrap_sub(IOTA0, 26)) & 255];
			fVec4[0] = fTemp47;
			output0[i0] = static_cast<FAUSTFLOAT>(static_cast<double>(input0[i0]) + fSlow36 * fTemp39 + fSlow35 * fVec3[1] + fVec4[1]);
			double fTemp48 = fSlow39 * fTemp0 + fSlow38 * fRec11[(faust_wrap_sub(IOTA0, 242)) & 1023] + fSlow37 * fRec12[(faust_wrap_sub(IOTA0, 180)) & 255];
			fVec5[0] = fTemp48;
			output1[i0] = static_cast<FAUSTFLOAT>(static_cast<double>(input1[i0]) + fSlow36 * fTemp38 + fSlow35 * fVec2[1] + fVec5[1]);
			output2[i0] = static_cast<FAUSTFLOAT>(std::max<double>(std::max<double>(std::max<double>(std::fabs(fTemp47), std::fabs(fTemp48)), std::max<double>(std::fabs(fTemp11), std::max<double>(std::fabs(fTemp0), std::fabs(fRec7[0])))), std::max<double>(std::fabs(fTemp37), std::fabs(fTemp32))));
			IOTA0 = faust_wrap_add(IOTA0, 1);
			iVec0[1] = iVec0[0];
			fRec0[1] = fRec0[0];
			fRec16[1] = fRec16[0];
			fRec17[1] = fRec17[0];
			for (int j0 = 3; j0 > 0; j0 = faust_wrap_sub(j0, 1)) {
				fVec1[j0] = fVec1[faust_wrap_sub(j0, 1)];
			}
			fRec2[1] = fRec2[0];
			fRec7[1] = fRec7[0];
			fRec19[1] = fRec19[0];
			fRec20[1] = fRec20[0];
			fRec23[1] = fRec23[0];
			fRec24[1] = fRec24[0];
			fVec2[1] = fVec2[0];
			fVec3[1] = fVec3[0];
			fRec18[1] = fRec18[0];
			fRec13[1] = fRec13[0];
			fRec14[1] = fRec14[0];
			fVec4[1] = fVec4[0];
			fVec5[1] = fVec5[0];
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
