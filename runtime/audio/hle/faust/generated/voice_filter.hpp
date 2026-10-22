/* ------------------------------------------------------------
name: "VoiceFilter"
Code generated with Faust 2.88.0 (https://faust.grame.fr)
Compilation options: -a runtime/audio/hle/faust/architecture.hpp -lang cpp -fpga-mem-th 4 -ct 1 -cn VoiceFilterDsp -es 1 -mcd 16 -mdd 1024 -mdy 33 -single -ftz 0
------------------------------------------------------------ */

#ifndef  __VoiceFilterDsp_H__
#define  __VoiceFilterDsp_H__

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
#define FAUSTCLASS VoiceFilterDsp
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


class VoiceFilterDsp : public dsp {
	
 private:
	
	FAUSTFLOAT fEntry0;
	float fRec3[2];
	float fRec2[2];
	float fRec1[2];
	float fRec0[2];
	int fSampleRate;
	
 public:
	VoiceFilterDsp() {
	}
	
	VoiceFilterDsp(const VoiceFilterDsp&) = default;
	
	virtual ~VoiceFilterDsp() = default;
	
	VoiceFilterDsp& operator=(const VoiceFilterDsp&) = default;
	
	void metadata(Meta* m) { 
		m->declare("compile_options", "-a runtime/audio/hle/faust/architecture.hpp -lang cpp -fpga-mem-th 4 -ct 1 -cn VoiceFilterDsp -es 1 -mcd 16 -mdd 1024 -mdy 33 -single -ftz 0");
		m->declare("description", "ES5505 four-pole voice filter with per-voice gain");
		m->declare("filename", "voice_filter.dsp");
		m->declare("name", "VoiceFilter");
	}

	virtual int getNumInputs() {
		return 6;
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
		fEntry0 = static_cast<FAUSTFLOAT>(0.0f);
	}
	
	virtual void instanceClear() {
		for (int l0 = 0; l0 < 2; l0 = faust_wrap_add(l0, 1)) {
			fRec3[l0] = 0.0f;
		}
		for (int l1 = 0; l1 < 2; l1 = faust_wrap_add(l1, 1)) {
			fRec2[l1] = 0.0f;
		}
		for (int l2 = 0; l2 < 2; l2 = faust_wrap_add(l2, 1)) {
			fRec1[l2] = 0.0f;
		}
		for (int l3 = 0; l3 < 2; l3 = faust_wrap_add(l3, 1)) {
			fRec0[l3] = 0.0f;
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
	
	virtual VoiceFilterDsp* clone() {
		return new VoiceFilterDsp(*this);
	}
	
	virtual int getSampleRate() {
		return fSampleRate;
	}
	
	virtual void buildUserInterface(UI* ui_interface) {
		ui_interface->openVerticalBox("VoiceFilter");
		ui_interface->addNumEntry("mode", &fEntry0, FAUSTFLOAT(0.0f), FAUSTFLOAT(0.0f), FAUSTFLOAT(3.0f), FAUSTFLOAT(1.0f));
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
		float fSlow0 = static_cast<float>(fEntry0);
		int iSlow1 = fSlow0 < 2.0f;
		int iSlow2 = fSlow0 == 0.0f;
		int iSlow3 = fSlow0 == 2.0f;
		for (int i0 = 0; i0 < count; i0 = faust_wrap_add(i0, 1)) {
			float fTemp0 = static_cast<float>(input1[i0]);
			fRec3[0] = fRec3[1] + fTemp0 * (static_cast<float>(input0[i0]) - fRec3[1]);
			fRec2[0] = fRec2[1] + fTemp0 * (fRec3[0] - fRec2[1]);
			float fTemp1 = static_cast<float>(input2[i0]);
			float fTemp2 = static_cast<float>(input3[i0]);
			fRec1[0] = ((iSlow2) ? fRec2[0] + fTemp2 * fRec1[1] - fRec2[1] : fRec1[1] + ((iSlow3) ? fTemp1 : fTemp0) * (fRec2[0] - fRec1[1]));
			fRec0[0] = ((iSlow1) ? fRec1[0] + fTemp2 * fRec0[1] - fRec1[1] : fRec0[1] + fTemp1 * (fRec1[0] - fRec0[1]));
			output0[i0] = static_cast<FAUSTFLOAT>(static_cast<float>(input4[i0]) * fRec0[0]);
			output1[i0] = static_cast<FAUSTFLOAT>(static_cast<float>(input5[i0]) * fRec0[0]);
			fRec3[1] = fRec3[0];
			fRec2[1] = fRec2[0];
			fRec1[1] = fRec1[0];
			fRec0[1] = fRec0[0];
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
