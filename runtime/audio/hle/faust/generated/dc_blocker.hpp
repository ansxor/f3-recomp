/* ------------------------------------------------------------
name: "dc_blocker"
Code generated with Faust 2.88.0 (https://faust.grame.fr)
Compilation options: -a runtime/audio/hle/faust/architecture.hpp -lang cpp -fpga-mem-th 4 -ct 1 -cn DcBlockerDsp -es 1 -mcd 16 -mdd 1024 -mdy 33 -double -ftz 0
------------------------------------------------------------ */

#ifndef  __DcBlockerDsp_H__
#define  __DcBlockerDsp_H__

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
#define FAUSTCLASS DcBlockerDsp
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


class DcBlockerDsp : public dsp {
	
 private:
	
	FAUSTFLOAT fHslider0;
	FAUSTFLOAT fHslider1;
	double fVec0[2];
	double fRec0[2];
	FAUSTFLOAT fHslider2;
	double fVec1[2];
	double fRec1[2];
	int fSampleRate;
	
 public:
	DcBlockerDsp() {
	}
	
	DcBlockerDsp(const DcBlockerDsp&) = default;
	
	virtual ~DcBlockerDsp() = default;
	
	DcBlockerDsp& operator=(const DcBlockerDsp&) = default;
	
	void metadata(Meta* m) { 
		m->declare("compile_options", "-a runtime/audio/hle/faust/architecture.hpp -lang cpp -fpga-mem-th 4 -ct 1 -cn DcBlockerDsp -es 1 -mcd 16 -mdd 1024 -mdy 33 -double -ftz 0");
		m->declare("filename", "dc_blocker.dsp");
		m->declare("name", "dc_blocker");
	}

	virtual int getNumInputs() {
		return 2;
	}
	virtual int getNumOutputs() {
		return 2;
	}
	
	static void classInit(int sample_rate) {
	}
	
	virtual void instanceConstants(int sample_rate) {
		fSampleRate = sample_rate;
	}
	
	virtual void instanceResetUserInterface() {
		fHslider0 = static_cast<FAUSTFLOAT>(0.99578);
		fHslider1 = static_cast<FAUSTFLOAT>(1.0);
		fHslider2 = static_cast<FAUSTFLOAT>(1.0);
	}
	
	virtual void instanceClear() {
		for (int l0 = 0; l0 < 2; l0 = faust_wrap_add(l0, 1)) {
			fVec0[l0] = 0.0;
		}
		for (int l1 = 0; l1 < 2; l1 = faust_wrap_add(l1, 1)) {
			fRec0[l1] = 0.0;
		}
		for (int l2 = 0; l2 < 2; l2 = faust_wrap_add(l2, 1)) {
			fVec1[l2] = 0.0;
		}
		for (int l3 = 0; l3 < 2; l3 = faust_wrap_add(l3, 1)) {
			fRec1[l3] = 0.0;
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
	
	virtual DcBlockerDsp* clone() {
		return new DcBlockerDsp(*this);
	}
	
	virtual int getSampleRate() {
		return fSampleRate;
	}
	
	virtual void buildUserInterface(UI* ui_interface) {
		ui_interface->openVerticalBox("dc_blocker");
		ui_interface->addHorizontalSlider("gain_l", &fHslider1, FAUSTFLOAT(1.0), FAUSTFLOAT(0.0), FAUSTFLOAT(64.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("gain_r", &fHslider2, FAUSTFLOAT(1.0), FAUSTFLOAT(0.0), FAUSTFLOAT(64.0), FAUSTFLOAT(0.0001));
		ui_interface->addHorizontalSlider("r", &fHslider0, FAUSTFLOAT(0.99578), FAUSTFLOAT(0.0), FAUSTFLOAT(1.0), FAUSTFLOAT(1e-06));
		ui_interface->closeBox();
	}
	
	virtual void compute(int count, FAUSTFLOAT** RESTRICT inputs, FAUSTFLOAT** RESTRICT outputs) {
		FAUSTFLOAT* input0 = inputs[0];
		FAUSTFLOAT* input1 = inputs[1];
		FAUSTFLOAT* output0 = outputs[0];
		FAUSTFLOAT* output1 = outputs[1];
		double fSlow0 = static_cast<double>(fHslider0);
		double fSlow1 = static_cast<double>(fHslider1);
		double fSlow2 = static_cast<double>(fHslider2);
		for (int i0 = 0; i0 < count; i0 = faust_wrap_add(i0, 1)) {
			double fTemp0 = fSlow1 * static_cast<double>(input0[i0]);
			fVec0[0] = fTemp0;
			fRec0[0] = fTemp0 - fVec0[1] + fSlow0 * ((std::fabs(fRec0[1]) < 1e-08) ? 0.0 : fRec0[1]);
			output0[i0] = static_cast<FAUSTFLOAT>(fRec0[0]);
			double fTemp1 = fSlow2 * static_cast<double>(input1[i0]);
			fVec1[0] = fTemp1;
			fRec1[0] = fTemp1 - fVec1[1] + fSlow0 * ((std::fabs(fRec1[1]) < 1e-08) ? 0.0 : fRec1[1]);
			output1[i0] = static_cast<FAUSTFLOAT>(fRec1[0]);
			fVec0[1] = fVec0[0];
			fRec0[1] = fRec0[0];
			fVec1[1] = fVec1[0];
			fRec1[1] = fRec1[0];
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
