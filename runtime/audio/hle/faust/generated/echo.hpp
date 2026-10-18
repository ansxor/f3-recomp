/* ------------------------------------------------------------
name: "echo"
Code generated with Faust 2.88.0 (https://faust.grame.fr)
Compilation options: -a runtime/audio/hle/faust/architecture.hpp -lang cpp -fpga-mem-th 4 -ct 1 -cn EchoDsp -es 1 -mcd 16 -mdd 1024 -mdy 33 -double -ftz 0
------------------------------------------------------------ */

#ifndef  __EchoDsp_H__
#define  __EchoDsp_H__

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
#define FAUSTCLASS EchoDsp
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

static double EchoDsp_faustpower2_f(double value) {
	return value * value;
}

class EchoDsp : public dsp {
	
 private:
	
	int IOTA0;
	int iVec0[2];
	double fRec0[2];
	FAUSTFLOAT fHslider0;
	FAUSTFLOAT fHslider1;
	double fRec15[2];
	double fRec16[2];
	FAUSTFLOAT fHslider2;
	FAUSTFLOAT fHslider3;
	FAUSTFLOAT fHslider4;
	FAUSTFLOAT fHslider5;
	double fVec1[2];
	FAUSTFLOAT fHslider6;
	double fVec2[2];
	FAUSTFLOAT fHslider7;
	double fVec3[2];
	FAUSTFLOAT fHslider8;
	double fRec19[2];
	double fRec17[32768];
	double fVec4[2];
	double fRec18[32768];
	double fVec5[3];
	double fRec1[1024];
	double fRec2[1024];
	double fRec3[2048];
	double fRec4[2];
	FAUSTFLOAT fHslider9;
	FAUSTFLOAT fHslider10;
	FAUSTFLOAT fHslider11;
	FAUSTFLOAT fHslider12;
	FAUSTFLOAT fHslider13;
	FAUSTFLOAT fHslider14;
	FAUSTFLOAT fHslider15;
	double fVec6[2];
	double fVec7[2];
	FAUSTFLOAT fHslider16;
	FAUSTFLOAT fHslider17;
	FAUSTFLOAT fHslider18;
	FAUSTFLOAT fHslider19;
	double fRec20[2];
	double fRec6[256];
	double fRec7[512];
	double fRec8[2];
	double fRec9[1024];
	double fRec10[1024];
	double fRec11[256];
	double fRec12[2048];
	double fRec13[2];
	double fRec14[2];
	FAUSTFLOAT fHslider20;
	FAUSTFLOAT fHslider21;
	FAUSTFLOAT fHslider22;
	double fVec8[2];
	FAUSTFLOAT fHslider23;
	FAUSTFLOAT fHslider24;
	FAUSTFLOAT fHslider25;
	FAUSTFLOAT fHslider26;
	FAUSTFLOAT fHslider27;
	double fVec9[2];
	int fSampleRate;
	
 public:
	EchoDsp() {
	}
	
	EchoDsp(const EchoDsp&) = default;
	
	virtual ~EchoDsp() = default;
	
	EchoDsp& operator=(const EchoDsp&) = default;
	
	void metadata(Meta* m) { 
		m->declare("compile_options", "-a runtime/audio/hle/faust/architecture.hpp -lang cpp -fpga-mem-th 4 -ct 1 -cn EchoDsp -es 1 -mcd 16 -mdd 1024 -mdy 33 -double -ftz 0");
		m->declare("filename", "echo.dsp");
		m->declare("name", "echo");
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
		fHslider5 = static_cast<FAUSTFLOAT>(14848.49);
		fHslider6 = static_cast<FAUSTFLOAT>(7168.0);
		fHslider7 = static_cast<FAUSTFLOAT>(0.0);
		fHslider8 = static_cast<FAUSTFLOAT>(0.00683594);
		fHslider9 = static_cast<FAUSTFLOAT>(0.4);
		fHslider10 = static_cast<FAUSTFLOAT>(0.04922);
		fHslider11 = static_cast<FAUSTFLOAT>(0.00703);
		fHslider12 = static_cast<FAUSTFLOAT>(0.3125);
		fHslider13 = static_cast<FAUSTFLOAT>(0.25);
		fHslider14 = static_cast<FAUSTFLOAT>(0.0);
		fHslider15 = static_cast<FAUSTFLOAT>(1.0);
		fHslider16 = static_cast<FAUSTFLOAT>(0.2448);
		fHslider17 = static_cast<FAUSTFLOAT>(0.2448);
		fHslider18 = static_cast<FAUSTFLOAT>(0.0075);
		fHslider19 = static_cast<FAUSTFLOAT>(0.7071);
		fHslider20 = static_cast<FAUSTFLOAT>(0.46667);
		fHslider21 = static_cast<FAUSTFLOAT>(0.66667);
		fHslider22 = static_cast<FAUSTFLOAT>(0.13333);
		fHslider23 = static_cast<FAUSTFLOAT>(0.9695);
		fHslider24 = static_cast<FAUSTFLOAT>(0.9695);
		fHslider25 = static_cast<FAUSTFLOAT>(0.66667);
		fHslider26 = static_cast<FAUSTFLOAT>(0.66667);
		fHslider27 = static_cast<FAUSTFLOAT>(0.13333);
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
			fRec15[l2] = 0.0;
		}
		for (int l3 = 0; l3 < 2; l3 = faust_wrap_add(l3, 1)) {
			fRec16[l3] = 0.0;
		}
		for (int l4 = 0; l4 < 2; l4 = faust_wrap_add(l4, 1)) {
			fVec1[l4] = 0.0;
		}
		for (int l5 = 0; l5 < 2; l5 = faust_wrap_add(l5, 1)) {
			fVec2[l5] = 0.0;
		}
		for (int l6 = 0; l6 < 2; l6 = faust_wrap_add(l6, 1)) {
			fVec3[l6] = 0.0;
		}
		for (int l7 = 0; l7 < 2; l7 = faust_wrap_add(l7, 1)) {
			fRec19[l7] = 0.0;
		}
		for (int l8 = 0; l8 < 32768; l8 = faust_wrap_add(l8, 1)) {
			fRec17[l8] = 0.0;
		}
		for (int l9 = 0; l9 < 2; l9 = faust_wrap_add(l9, 1)) {
			fVec4[l9] = 0.0;
		}
		for (int l10 = 0; l10 < 32768; l10 = faust_wrap_add(l10, 1)) {
			fRec18[l10] = 0.0;
		}
		for (int l11 = 0; l11 < 3; l11 = faust_wrap_add(l11, 1)) {
			fVec5[l11] = 0.0;
		}
		for (int l12 = 0; l12 < 1024; l12 = faust_wrap_add(l12, 1)) {
			fRec1[l12] = 0.0;
		}
		for (int l13 = 0; l13 < 1024; l13 = faust_wrap_add(l13, 1)) {
			fRec2[l13] = 0.0;
		}
		for (int l14 = 0; l14 < 2048; l14 = faust_wrap_add(l14, 1)) {
			fRec3[l14] = 0.0;
		}
		for (int l15 = 0; l15 < 2; l15 = faust_wrap_add(l15, 1)) {
			fRec4[l15] = 0.0;
		}
		for (int l16 = 0; l16 < 2; l16 = faust_wrap_add(l16, 1)) {
			fVec6[l16] = 0.0;
		}
		for (int l17 = 0; l17 < 2; l17 = faust_wrap_add(l17, 1)) {
			fVec7[l17] = 0.0;
		}
		for (int l18 = 0; l18 < 2; l18 = faust_wrap_add(l18, 1)) {
			fRec20[l18] = 0.0;
		}
		for (int l19 = 0; l19 < 256; l19 = faust_wrap_add(l19, 1)) {
			fRec6[l19] = 0.0;
		}
		for (int l20 = 0; l20 < 512; l20 = faust_wrap_add(l20, 1)) {
			fRec7[l20] = 0.0;
		}
		for (int l21 = 0; l21 < 2; l21 = faust_wrap_add(l21, 1)) {
			fRec8[l21] = 0.0;
		}
		for (int l22 = 0; l22 < 1024; l22 = faust_wrap_add(l22, 1)) {
			fRec9[l22] = 0.0;
		}
		for (int l23 = 0; l23 < 1024; l23 = faust_wrap_add(l23, 1)) {
			fRec10[l23] = 0.0;
		}
		for (int l24 = 0; l24 < 256; l24 = faust_wrap_add(l24, 1)) {
			fRec11[l24] = 0.0;
		}
		for (int l25 = 0; l25 < 2048; l25 = faust_wrap_add(l25, 1)) {
			fRec12[l25] = 0.0;
		}
		for (int l26 = 0; l26 < 2; l26 = faust_wrap_add(l26, 1)) {
			fRec13[l26] = 0.0;
		}
		for (int l27 = 0; l27 < 2; l27 = faust_wrap_add(l27, 1)) {
			fRec14[l27] = 0.0;
		}
		for (int l28 = 0; l28 < 2; l28 = faust_wrap_add(l28, 1)) {
			fVec8[l28] = 0.0;
		}
		for (int l29 = 0; l29 < 2; l29 = faust_wrap_add(l29, 1)) {
			fVec9[l29] = 0.0;
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
	
	virtual EchoDsp* clone() {
		return new EchoDsp(*this);
	}
	
	virtual int getSampleRate() {
		return fSampleRate;
	}
	
	virtual void buildUserInterface(UI* ui_interface) {
		ui_interface->openVerticalBox("echo");
		ui_interface->addHorizontalSlider("c1", &fHslider11, FAUSTFLOAT(0.00703), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(1e-05));
		ui_interface->addHorizontalSlider("c2", &fHslider10, FAUSTFLOAT(0.04922), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(1e-05));
		ui_interface->addHorizontalSlider("chorus_out", &fHslider23, FAUSTFLOAT(0.9695), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("chorus_to_reverb", &fHslider16, FAUSTFLOAT(0.2448), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("cos_mix", &fHslider15, FAUSTFLOAT(1.0), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("damp", &fHslider12, FAUSTFLOAT(0.3125), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("decay", &fHslider13, FAUSTFLOAT(0.25), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("depth", &fHslider4, FAUSTFLOAT(8.0), FAUSTFLOAT(0.0), FAUSTFLOAT(128.0), FAUSTFLOAT(0.001));
		ui_interface->addHorizontalSlider("dry_send", &fHslider24, FAUSTFLOAT(0.9695), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("echo", &fHslider6, FAUSTFLOAT(7168.0), FAUSTFLOAT(0.0), FAUSTFLOAT(2.856e+04), FAUSTFLOAT(1.0));
		ui_interface->addHorizontalSlider("enhance", &fHslider8, FAUSTFLOAT(0.00683594), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(1e-06));
		ui_interface->addHorizontalSlider("feedback", &fHslider7, FAUSTFLOAT(0.0), FAUSTFLOAT(-0.95), FAUSTFLOAT(0.95), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("g", &fHslider9, FAUSTFLOAT(0.4), FAUSTFLOAT(0.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("leak", &fHslider18, FAUSTFLOAT(0.0075), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("lfo_c_eps_hi", &fHslider0, FAUSTFLOAT(0.0), FAUSTFLOAT(-1.0), FAUSTFLOAT(1.0), FAUSTFLOAT(1e-09));
		ui_interface->addHorizontalSlider("lfo_c_eps_lo", &fHslider1, FAUSTFLOAT(0.0), FAUSTFLOAT(-1e-06), FAUSTFLOAT(1e-06), FAUSTFLOAT(1e-15));
		ui_interface->addHorizontalSlider("lfo_c_ph_hi", &fHslider2, FAUSTFLOAT(0.0), FAUSTFLOAT(-7.0), FAUSTFLOAT(7.0), FAUSTFLOAT(1e-09));
		ui_interface->addHorizontalSlider("lfo_c_ph_lo", &fHslider3, FAUSTFLOAT(0.0), FAUSTFLOAT(-1e-06), FAUSTFLOAT(1e-06), FAUSTFLOAT(1e-15));
		ui_interface->addHorizontalSlider("mod_center", &fHslider5, FAUSTFLOAT(14848.49), FAUSTFLOAT(0.0), FAUSTFLOAT(32512.49), FAUSTFLOAT(0.01));
		ui_interface->addHorizontalSlider("scale", &fHslider19, FAUSTFLOAT(0.7071), FAUSTFLOAT(0.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("send", &fHslider17, FAUSTFLOAT(0.2448), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("sin_mix", &fHslider14, FAUSTFLOAT(0.0), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("tap_l0", &fHslider22, FAUSTFLOAT(0.13333), FAUSTFLOAT(-2.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("tap_l1", &fHslider21, FAUSTFLOAT(0.66667), FAUSTFLOAT(-2.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("tap_l2", &fHslider20, FAUSTFLOAT(0.46667), FAUSTFLOAT(-2.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("tap_r0", &fHslider27, FAUSTFLOAT(0.13333), FAUSTFLOAT(-2.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("tap_r1", &fHslider26, FAUSTFLOAT(0.66667), FAUSTFLOAT(-2.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("tap_r2", &fHslider25, FAUSTFLOAT(0.66667), FAUSTFLOAT(-2.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
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
		double fSlow11 = fSlow8 + -0.00390625;
		double fSlow12 = static_cast<double>(fHslider9);
		double fSlow13 = static_cast<double>(fHslider10);
		double fSlow14 = static_cast<double>(fHslider11);
		double fSlow15 = static_cast<double>(fHslider12);
		double fSlow16 = 1.0 - fSlow15;
		double fSlow17 = static_cast<double>(fHslider13);
		double fSlow18 = 0.7071 * static_cast<double>(fHslider14);
		double fSlow19 = static_cast<double>(fHslider15);
		double fSlow20 = static_cast<double>(fHslider16);
		double fSlow21 = static_cast<double>(fHslider17);
		double fSlow22 = static_cast<double>(fHslider18);
		double fSlow23 = static_cast<double>(fHslider19) * (1.0 - fSlow22);
		double fSlow24 = static_cast<double>(fHslider20);
		double fSlow25 = static_cast<double>(fHslider21);
		double fSlow26 = static_cast<double>(fHslider22);
		double fSlow27 = static_cast<double>(fHslider23);
		double fSlow28 = static_cast<double>(fHslider24);
		double fSlow29 = static_cast<double>(fHslider25);
		double fSlow30 = static_cast<double>(fHslider26);
		double fSlow31 = static_cast<double>(fHslider27);
		for (int i0 = 0; i0 < count; i0 = faust_wrap_add(i0, 1)) {
			double fTemp0 = fRec12[(faust_wrap_sub(IOTA0, 1105)) & 2047];
			iVec0[0] = 1;
			fRec0[0] = 0.375 * fRec0[1] + 0.625 * fTemp0;
			double fTemp1 = static_cast<double>(faust_wrap_sub(1, iVec0[1])) + fRec15[1];
			fRec15[0] = fSlow2 * fTemp1 - fSlow1 * fRec16[1];
			fRec16[0] = fSlow1 * fTemp1 + fSlow2 * fRec16[1];
			fVec1[0] = fSlow7 + fSlow6 * (fSlow5 * fTemp1 - fSlow4 * fRec16[1]);
			fVec2[0] = fSlow8;
			double fTemp2 = fRec18[(faust_wrap_sub(IOTA0, faust_wrap_add(std::max<int>(0, std::min<int>(28560, static_cast<int>(fVec2[1]))), 1))) & 32767];
			double fTemp3 = static_cast<double>(input2[i0]);
			double fTemp4 = std::max<double>(-1.0, std::min<double>(1.0, 4.0 * std::max<double>(-1.0, std::min<double>(1.0, 4.0 * fTemp3))));
			fVec3[0] = std::max<double>(-1.0, std::min<double>(1.0, fTemp4 * (2.0 - 0.5 * EchoDsp_faustpower2_f(fTemp4))));
			fRec19[0] = fSlow10 * fVec3[1] + 0.75 * fRec19[1];
			fRec17[IOTA0 & 32767] = fTemp3 + fRec19[0] + fSlow9 * fTemp2;
			fVec4[0] = fSlow11;
			double fTemp5 = std::max<double>(0.0, std::min<double>(32521.0, fVec4[1]));
			int iTemp6 = static_cast<int>(fTemp5);
			double fTemp7 = fRec17[(faust_wrap_sub(IOTA0, faust_wrap_add(iTemp6, 1))) & 32767];
			double fTemp8 = ((fVec4[1] >= 0.0) ? fTemp7 + (fTemp5 - static_cast<double>(iTemp6)) * (fRec17[(faust_wrap_sub(IOTA0, faust_wrap_add(iTemp6, 2))) & 32767] - fTemp7) : 0.0);
			double fTemp9 = static_cast<double>(input3[i0]);
			fRec18[IOTA0 & 32767] = fTemp9 + fSlow9 * fTemp8;
			double fTemp10 = std::max<double>(0.0, std::min<double>(32521.0, fVec1[1]));
			int iTemp11 = static_cast<int>(fTemp10);
			double fTemp12 = fRec17[(faust_wrap_sub(IOTA0, faust_wrap_add(1, iTemp11))) & 32767];
			fVec5[0] = ((fVec1[1] >= 0.0) ? fTemp12 + (fTemp10 - static_cast<double>(iTemp11)) * (fRec17[(faust_wrap_sub(IOTA0, faust_wrap_add(2, iTemp11))) & 32767] - fTemp12) : 0.0);
			fRec1[IOTA0 & 1023] = fVec5[2] - 0.25 * (fRec8[1] + 0.25 * fVec5[2]);
			double fTemp13 = fRec3[(faust_wrap_sub(IOTA0, 1369)) & 2047];
			double fTemp14 = fRec13[1] + 0.25 * fTemp13;
			fRec2[IOTA0 & 1023] = fTemp13 - 0.25 * fTemp14;
			fRec3[IOTA0 & 2047] = fTemp14;
			fRec4[0] = 0.875 * fRec4[1] + 0.125 * fRec0[1];
			double fTemp15 = fRec6[(faust_wrap_sub(IOTA0, 168)) & 255];
			double fTemp16 = fSlow19 * fTemp9 + fSlow18 * fTemp2;
			fVec6[0] = fTemp16;
			double fTemp17 = fSlow18 * fTemp8 + fSlow19 * (fTemp3 + fRec19[0]);
			fVec7[0] = fTemp17;
			double fTemp18 = static_cast<double>(input5[i0]);
			double fTemp19 = static_cast<double>(input4[i0]);
			fRec20[0] = fSlow22 * fRec20[1] + fSlow23 * (fSlow21 * (fTemp19 + fTemp18) + fSlow20 * (fVec7[1] + fVec6[1]));
			double fTemp20 = fRec20[0] + fSlow17 * (fSlow16 * fTemp0 + fSlow15 * fRec0[1]) + fSlow14 * fRec0[1] + fSlow13 * fRec4[1] + fSlow12 * fTemp15;
			double fRec5 = fTemp15 - fSlow12 * fTemp20;
			fRec6[IOTA0 & 255] = fTemp20;
			double fTemp21 = fRec7[(faust_wrap_sub(IOTA0, 276)) & 511];
			double fTemp22 = fRec5 + fSlow12 * fTemp21;
			fRec7[IOTA0 & 511] = fTemp22;
			double fTemp23 = fRec1[(faust_wrap_sub(IOTA0, 695)) & 1023];
			double fTemp24 = fRec14[1] + 0.25 * fTemp23;
			fRec8[0] = fTemp24;
			fRec9[IOTA0 & 1023] = fTemp23 - 0.25 * fTemp24;
			fRec10[IOTA0 & 1023] = fTemp21 - fSlow12 * fTemp22;
			double fTemp25 = fRec2[(faust_wrap_sub(IOTA0, 548)) & 1023];
			double fTemp26 = fRec10[(faust_wrap_sub(IOTA0, 755)) & 1023] + 0.5 * fTemp25;
			fRec11[IOTA0 & 255] = fTemp25 - 0.5 * fTemp26;
			double fTemp27 = fRec9[(faust_wrap_sub(IOTA0, 1011)) & 1023];
			double fTemp28 = fRec11[(faust_wrap_sub(IOTA0, 242)) & 255] + 0.5 * fTemp27;
			fRec12[IOTA0 & 2047] = fTemp27 - 0.5 * fTemp28;
			fRec13[0] = fTemp26;
			fRec14[0] = fTemp28;
			double fTemp29 = fSlow26 * fRec12[(faust_wrap_sub(IOTA0, 384)) & 2047] + fSlow25 * fRec10[(faust_wrap_sub(IOTA0, 4)) & 1023] + fSlow24 * fRec11[(faust_wrap_sub(IOTA0, 26)) & 255];
			fVec8[0] = fTemp29;
			output0[i0] = static_cast<FAUSTFLOAT>(static_cast<double>(input0[i0]) + fSlow28 * fTemp19 + fSlow27 * fVec7[1] + fVec8[1]);
			double fTemp30 = fSlow31 * fTemp0 + fSlow30 * fRec10[(faust_wrap_sub(IOTA0, 242)) & 1023] + fSlow29 * fRec11[(faust_wrap_sub(IOTA0, 180)) & 255];
			fVec9[0] = fTemp30;
			output1[i0] = static_cast<FAUSTFLOAT>(static_cast<double>(input1[i0]) + fSlow28 * fTemp18 + fSlow27 * fVec6[1] + fVec9[1]);
			output2[i0] = static_cast<FAUSTFLOAT>(std::max<double>(std::max<double>(std::max<double>(std::fabs(fTemp29), std::fabs(fTemp30)), std::max<double>(std::fabs(fTemp13), std::max<double>(std::fabs(fTemp0), std::fabs(fRec4[0])))), std::max<double>(std::max<double>(std::fabs(fTemp17), std::max<double>(std::fabs(fTemp16), std::fabs(fRec19[0]))), std::max<double>(std::fabs(fTemp8), std::fabs(fTemp2)))));
			IOTA0 = faust_wrap_add(IOTA0, 1);
			iVec0[1] = iVec0[0];
			fRec0[1] = fRec0[0];
			fRec15[1] = fRec15[0];
			fRec16[1] = fRec16[0];
			fVec1[1] = fVec1[0];
			fVec2[1] = fVec2[0];
			fVec3[1] = fVec3[0];
			fRec19[1] = fRec19[0];
			fVec4[1] = fVec4[0];
			fVec5[2] = fVec5[1];
			fVec5[1] = fVec5[0];
			fRec4[1] = fRec4[0];
			fVec6[1] = fVec6[0];
			fVec7[1] = fVec7[0];
			fRec20[1] = fRec20[0];
			fRec8[1] = fRec8[0];
			fRec13[1] = fRec13[0];
			fRec14[1] = fRec14[0];
			fVec8[1] = fVec8[0];
			fVec9[1] = fVec9[0];
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
