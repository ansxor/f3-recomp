/* ------------------------------------------------------------
name: "reverb"
Code generated with Faust 2.88.0 (https://faust.grame.fr)
Compilation options: -a runtime/audio/hle/faust/architecture.hpp -lang cpp -fpga-mem-th 4 -ct 1 -cn ReverbDsp -es 1 -mcd 16 -mdd 1024 -mdy 33 -double -ftz 0
------------------------------------------------------------ */

#ifndef  __ReverbDsp_H__
#define  __ReverbDsp_H__

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
#define FAUSTCLASS ReverbDsp
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


class ReverbDsp : public dsp {
	
 private:
	
	int iVec0[2];
	FAUSTFLOAT fHslider0;
	FAUSTFLOAT fHslider1;
	double fRec16[2];
	double fRec17[2];
	FAUSTFLOAT fHslider2;
	FAUSTFLOAT fHslider3;
	FAUSTFLOAT fHslider4;
	double fVec1[4];
	int IOTA0;
	double fRec0[1024];
	double fRec1[2];
	double fRec2[2048];
	double fRec3[1024];
	double fRec4[2];
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
	FAUSTFLOAT fHslider12;
	FAUSTFLOAT fHslider13;
	double fRec18[2];
	double fRec9[256];
	double fRec10[1024];
	double fRec11[256];
	double fRec12[2];
	double fRec13[2];
	double fRec14[512];
	double fRec15[2048];
	FAUSTFLOAT fHslider14;
	FAUSTFLOAT fHslider15;
	FAUSTFLOAT fHslider16;
	double fVec2[2];
	FAUSTFLOAT fHslider17;
	FAUSTFLOAT fHslider18;
	FAUSTFLOAT fHslider19;
	FAUSTFLOAT fHslider20;
	FAUSTFLOAT fHslider21;
	double fVec3[2];
	int fSampleRate;
	
 public:
	ReverbDsp() {
	}
	
	ReverbDsp(const ReverbDsp&) = default;
	
	virtual ~ReverbDsp() = default;
	
	ReverbDsp& operator=(const ReverbDsp&) = default;
	
	void metadata(Meta* m) { 
		m->declare("compile_options", "-a runtime/audio/hle/faust/architecture.hpp -lang cpp -fpga-mem-th 4 -ct 1 -cn ReverbDsp -es 1 -mcd 16 -mdd 1024 -mdy 33 -double -ftz 0");
		m->declare("filename", "reverb.dsp");
		m->declare("name", "reverb");
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
		fHslider10 = static_cast<FAUSTFLOAT>(0.0075);
		fHslider11 = static_cast<FAUSTFLOAT>(0.2448);
		fHslider12 = static_cast<FAUSTFLOAT>(0.2448);
		fHslider13 = static_cast<FAUSTFLOAT>(0.7071);
		fHslider14 = static_cast<FAUSTFLOAT>(0.46667);
		fHslider15 = static_cast<FAUSTFLOAT>(0.66667);
		fHslider16 = static_cast<FAUSTFLOAT>(0.13333);
		fHslider17 = static_cast<FAUSTFLOAT>(0.9695);
		fHslider18 = static_cast<FAUSTFLOAT>(0.9695);
		fHslider19 = static_cast<FAUSTFLOAT>(0.66667);
		fHslider20 = static_cast<FAUSTFLOAT>(0.66667);
		fHslider21 = static_cast<FAUSTFLOAT>(0.13333);
	}
	
	virtual void instanceClear() {
		for (int l0 = 0; l0 < 2; l0 = faust_wrap_add(l0, 1)) {
			iVec0[l0] = 0;
		}
		for (int l1 = 0; l1 < 2; l1 = faust_wrap_add(l1, 1)) {
			fRec16[l1] = 0.0;
		}
		for (int l2 = 0; l2 < 2; l2 = faust_wrap_add(l2, 1)) {
			fRec17[l2] = 0.0;
		}
		for (int l3 = 0; l3 < 4; l3 = faust_wrap_add(l3, 1)) {
			fVec1[l3] = 0.0;
		}
		IOTA0 = 0;
		for (int l4 = 0; l4 < 1024; l4 = faust_wrap_add(l4, 1)) {
			fRec0[l4] = 0.0;
		}
		for (int l5 = 0; l5 < 2; l5 = faust_wrap_add(l5, 1)) {
			fRec1[l5] = 0.0;
		}
		for (int l6 = 0; l6 < 2048; l6 = faust_wrap_add(l6, 1)) {
			fRec2[l6] = 0.0;
		}
		for (int l7 = 0; l7 < 1024; l7 = faust_wrap_add(l7, 1)) {
			fRec3[l7] = 0.0;
		}
		for (int l8 = 0; l8 < 2; l8 = faust_wrap_add(l8, 1)) {
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
			fRec18[l12] = 0.0;
		}
		for (int l13 = 0; l13 < 256; l13 = faust_wrap_add(l13, 1)) {
			fRec9[l13] = 0.0;
		}
		for (int l14 = 0; l14 < 1024; l14 = faust_wrap_add(l14, 1)) {
			fRec10[l14] = 0.0;
		}
		for (int l15 = 0; l15 < 256; l15 = faust_wrap_add(l15, 1)) {
			fRec11[l15] = 0.0;
		}
		for (int l16 = 0; l16 < 2; l16 = faust_wrap_add(l16, 1)) {
			fRec12[l16] = 0.0;
		}
		for (int l17 = 0; l17 < 2; l17 = faust_wrap_add(l17, 1)) {
			fRec13[l17] = 0.0;
		}
		for (int l18 = 0; l18 < 512; l18 = faust_wrap_add(l18, 1)) {
			fRec14[l18] = 0.0;
		}
		for (int l19 = 0; l19 < 2048; l19 = faust_wrap_add(l19, 1)) {
			fRec15[l19] = 0.0;
		}
		for (int l20 = 0; l20 < 2; l20 = faust_wrap_add(l20, 1)) {
			fVec2[l20] = 0.0;
		}
		for (int l21 = 0; l21 < 2; l21 = faust_wrap_add(l21, 1)) {
			fVec3[l21] = 0.0;
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
	
	virtual ReverbDsp* clone() {
		return new ReverbDsp(*this);
	}
	
	virtual int getSampleRate() {
		return fSampleRate;
	}
	
	virtual void buildUserInterface(UI* ui_interface) {
		ui_interface->openVerticalBox("reverb");
		ui_interface->addHorizontalSlider("c1", &fHslider7, FAUSTFLOAT(0.00703), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(1e-05));
		ui_interface->addHorizontalSlider("c2", &fHslider6, FAUSTFLOAT(0.04922), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(1e-05));
		ui_interface->addHorizontalSlider("chorus_out", &fHslider18, FAUSTFLOAT(0.9695), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("chorus_to_reverb", &fHslider11, FAUSTFLOAT(0.2448), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("damp", &fHslider8, FAUSTFLOAT(0.3125), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("decay", &fHslider9, FAUSTFLOAT(0.25), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("depth", &fHslider4, FAUSTFLOAT(8.0), FAUSTFLOAT(0.0), FAUSTFLOAT(128.0), FAUSTFLOAT(0.001));
		ui_interface->addHorizontalSlider("dry_send", &fHslider17, FAUSTFLOAT(0.9695), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("g", &fHslider5, FAUSTFLOAT(0.4), FAUSTFLOAT(0.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("leak", &fHslider10, FAUSTFLOAT(0.0075), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("lfo_c_eps_hi", &fHslider0, FAUSTFLOAT(0.0), FAUSTFLOAT(-1.0), FAUSTFLOAT(1.0), FAUSTFLOAT(1e-09));
		ui_interface->addHorizontalSlider("lfo_c_eps_lo", &fHslider1, FAUSTFLOAT(0.0), FAUSTFLOAT(-1e-06), FAUSTFLOAT(1e-06), FAUSTFLOAT(1e-15));
		ui_interface->addHorizontalSlider("lfo_c_ph_hi", &fHslider2, FAUSTFLOAT(0.0), FAUSTFLOAT(-7.0), FAUSTFLOAT(7.0), FAUSTFLOAT(1e-09));
		ui_interface->addHorizontalSlider("lfo_c_ph_lo", &fHslider3, FAUSTFLOAT(0.0), FAUSTFLOAT(-1e-06), FAUSTFLOAT(1e-06), FAUSTFLOAT(1e-15));
		ui_interface->addHorizontalSlider("scale", &fHslider13, FAUSTFLOAT(0.7071), FAUSTFLOAT(0.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("send", &fHslider12, FAUSTFLOAT(0.2448), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("tap_l0", &fHslider16, FAUSTFLOAT(0.13333), FAUSTFLOAT(-2.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("tap_l1", &fHslider15, FAUSTFLOAT(0.66667), FAUSTFLOAT(-2.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("tap_l2", &fHslider14, FAUSTFLOAT(0.46667), FAUSTFLOAT(-2.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("tap_r0", &fHslider21, FAUSTFLOAT(0.13333), FAUSTFLOAT(-2.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("tap_r1", &fHslider20, FAUSTFLOAT(0.66667), FAUSTFLOAT(-2.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("tap_r2", &fHslider19, FAUSTFLOAT(0.66667), FAUSTFLOAT(-2.0), FAUSTFLOAT(2.0), FAUSTFLOAT(0.0001));
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
		double fSlow1 = std::cos(fSlow0);
		double fSlow2 = std::sin(fSlow0);
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
		double fSlow13 = static_cast<double>(fHslider10);
		double fSlow14 = static_cast<double>(fHslider11);
		double fSlow15 = static_cast<double>(fHslider12);
		double fSlow16 = static_cast<double>(fHslider13) * (1.0 - fSlow13);
		double fSlow17 = static_cast<double>(fHslider14);
		double fSlow18 = static_cast<double>(fHslider15);
		double fSlow19 = static_cast<double>(fHslider16);
		double fSlow20 = static_cast<double>(fHslider17);
		double fSlow21 = static_cast<double>(fHslider18);
		double fSlow22 = static_cast<double>(fHslider19);
		double fSlow23 = static_cast<double>(fHslider20);
		double fSlow24 = static_cast<double>(fHslider21);
		for (int i0 = 0; i0 < count; i0 = faust_wrap_add(i0, 1)) {
			iVec0[0] = 1;
			double fTemp0 = static_cast<double>(faust_wrap_sub(1, iVec0[1])) + fRec17[1];
			fRec16[0] = fSlow2 * fTemp0 + fSlow1 * fRec16[1];
			fRec17[0] = fSlow1 * fTemp0 - fSlow2 * fRec16[1];
			fVec1[0] = fSlow6 * (fSlow5 * fTemp0 - fSlow4 * fRec16[1]);
			double fTemp1 = 921.0 + fVec1[3];
			double fTemp2 = std::max<double>(0.0, std::min<double>(1.1e+03, fTemp1));
			int iTemp3 = static_cast<int>(fTemp2);
			double fTemp4 = fRec2[(faust_wrap_sub(IOTA0, faust_wrap_add(1, iTemp3))) & 2047];
			double fTemp5 = ((fTemp1 >= 0.0) ? fTemp4 + (fTemp2 - static_cast<double>(iTemp3)) * (fRec2[(faust_wrap_sub(IOTA0, faust_wrap_add(2, iTemp3))) & 2047] - fTemp4) : 0.0);
			double fTemp6 = fRec4[1] + 0.25 * fTemp5;
			fRec0[IOTA0 & 1023] = fTemp5 - 0.25 * fTemp6;
			double fTemp7 = fRec15[(faust_wrap_sub(IOTA0, 1105)) & 2047];
			fRec1[0] = 0.375 * fRec1[1] + 0.625 * fTemp7;
			fRec2[IOTA0 & 2047] = fTemp6;
			double fTemp8 = fRec0[(faust_wrap_sub(IOTA0, 695)) & 1023];
			double fTemp9 = fRec12[1] + 0.25 * fTemp8;
			fRec3[IOTA0 & 1023] = fTemp8 - 0.25 * fTemp9;
			fRec4[0] = fTemp9;
			double fTemp10 = fRec6[(faust_wrap_sub(IOTA0, 1369)) & 2047];
			double fTemp11 = fRec13[1] + 0.25 * fTemp10;
			fRec5[IOTA0 & 1023] = fTemp10 - 0.25 * fTemp11;
			fRec6[IOTA0 & 2047] = fTemp11;
			fRec7[0] = 0.875 * fRec7[1] + 0.125 * fRec1[1];
			double fTemp12 = fRec9[(faust_wrap_sub(IOTA0, 168)) & 255];
			double fTemp13 = static_cast<double>(input3[i0]);
			double fTemp14 = static_cast<double>(input2[i0]);
			double fTemp15 = static_cast<double>(input5[i0]);
			double fTemp16 = static_cast<double>(input4[i0]);
			fRec18[0] = fSlow16 * (fSlow15 * (fTemp16 + fTemp15) + fSlow14 * (fTemp14 + fTemp13)) + fSlow13 * fRec18[1];
			double fTemp17 = fRec18[0] + fSlow12 * (fSlow11 * fTemp7 + fSlow10 * fRec1[1]) + fSlow9 * fRec1[1] + fSlow8 * fRec7[1] + fSlow7 * fTemp12;
			double fRec8 = fTemp12 - fSlow7 * fTemp17;
			fRec9[IOTA0 & 255] = fTemp17;
			double fTemp18 = fRec14[(faust_wrap_sub(IOTA0, 276)) & 511];
			double fTemp19 = fRec8 + fSlow7 * fTemp18;
			fRec10[IOTA0 & 1023] = fTemp18 - fSlow7 * fTemp19;
			double fTemp20 = fRec5[(faust_wrap_sub(IOTA0, 548)) & 1023];
			double fTemp21 = fRec10[(faust_wrap_sub(IOTA0, 755)) & 1023] + 0.5 * fTemp20;
			fRec11[IOTA0 & 255] = fTemp20 - 0.5 * fTemp21;
			double fTemp22 = fRec3[(faust_wrap_sub(IOTA0, 1011)) & 1023];
			double fTemp23 = fRec11[(faust_wrap_sub(IOTA0, 242)) & 255] + 0.5 * fTemp22;
			fRec12[0] = fTemp23;
			fRec13[0] = fTemp21;
			fRec14[IOTA0 & 511] = fTemp19;
			fRec15[IOTA0 & 2047] = fTemp22 - 0.5 * fTemp23;
			double fTemp24 = fSlow19 * fRec15[(faust_wrap_sub(IOTA0, 384)) & 2047] + fSlow18 * fRec10[(faust_wrap_sub(IOTA0, 4)) & 1023] + fSlow17 * fRec11[(faust_wrap_sub(IOTA0, 26)) & 255];
			fVec2[0] = fTemp24;
			output0[i0] = static_cast<FAUSTFLOAT>(fSlow21 * fTemp14 + static_cast<double>(input0[i0]) + fSlow20 * fTemp16 + fVec2[1]);
			double fTemp25 = fSlow24 * fTemp7 + fSlow23 * fRec10[(faust_wrap_sub(IOTA0, 242)) & 1023] + fSlow22 * fRec11[(faust_wrap_sub(IOTA0, 180)) & 255];
			fVec3[0] = fTemp25;
			output1[i0] = static_cast<FAUSTFLOAT>(fSlow21 * fTemp13 + static_cast<double>(input1[i0]) + fSlow20 * fTemp15 + fVec3[1]);
			output2[i0] = static_cast<FAUSTFLOAT>(std::max<double>(std::max<double>(std::fabs(fTemp24), std::fabs(fTemp25)), std::max<double>(std::fabs(fTemp10), std::max<double>(std::fabs(fTemp7), std::fabs(fRec7[0])))));
			iVec0[1] = iVec0[0];
			fRec16[1] = fRec16[0];
			fRec17[1] = fRec17[0];
			for (int j0 = 3; j0 > 0; j0 = faust_wrap_sub(j0, 1)) {
				fVec1[j0] = fVec1[faust_wrap_sub(j0, 1)];
			}
			IOTA0 = faust_wrap_add(IOTA0, 1);
			fRec1[1] = fRec1[0];
			fRec4[1] = fRec4[0];
			fRec7[1] = fRec7[0];
			fRec18[1] = fRec18[0];
			fRec12[1] = fRec12[0];
			fRec13[1] = fRec13[0];
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
