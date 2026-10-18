#pragma once
#include "audio/hle/events.hpp"
#include "audio/hle/observe.hpp"
#include "audio/hle/faust/generated/chorus.hpp"
#include "audio/hle/faust/generated/dc_blocker.hpp"
#include "audio/hle/faust/generated/echo.hpp"
#include "audio/hle/faust/generated/hall.hpp"
#include "audio/hle/faust/generated/reverb.hpp"
#include "audio/hle/faust/support.hpp"
#include "audio/hle/sound_rom_5a7e9117.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace f3rt::hle {
static_assert(SAMPLE_RATE == 29762, "effect delay lengths are in ES5510 frames (the OTIS/ESP sample rate)");

// Substitute for the ES5510 effects programs. The sound ROM holds a table of program pointers (0xc1c3a2) and a
// preset table (0xc1c2d2, 16 bytes per preset); byte 5 of a preset row indexes the pointer table and bytes 6..15
// are the ten parameters p0..p9 in the order the sound commands set them. A preset loads the program its pointer
// names; programs are identified here by that pointer, so the selection does not depend on the game. Five
// programs are decoded (every program the three profiled games Kirameki, Land Maker and Command War select) and
// each has its own graph; any other program throws instead of being approximated.
// Delay lengths, gains and topology are the ones of the loaded program as it runs in the reference backend:
// every delay is a (read offset - write offset) / 256 distance inside the DSP's single 64K-word delay RAM,
// every gain is a GPR of the program, and each graph reproduces the emulated ES5510 impulse response to about
// -70 dB re peak (the 16-bit truncation floor), checked by tools/compare/esp_graph.py verify.
// docs/notes/raw/tests/2026-10-16-kirameki-187-pcb-stereo-reverb.md sections 8 and 9 list the graphs and the
// parameter mappings, which were read by patching preset rows in a ROM copy and diffing the GPRs.
// No DSP instructions or sampled audio are stored.
//
// The signal processing lives in FAUST (runtime/audio/hle/faust/): es5510.lib holds the all-pass networks, the
// interpolated tap, the LFO and the shared reverb tank, and reverb.dsp / chorus.dsp / echo.dsp / hall.dsp /
// dc_blocker.dsp are the programs, each with a signal-flow diagram and descriptive signal-flow names rather
// than decoded register names. The headers under faust/generated/ are committed; the faust-regen target
// rebuilds them (normal builds never run faust). This class keeps what is not signal processing: reading the
// preset table and the program pointer, mapping p0..p9 to the programs' controls (update()), routing the OTIS
// buses, the auto-bypass and the LFO phase of bypassed frames.
//
//   serio0 (OTIS pair 1, buses 2/3): dry in every program.
//   serio1 (pair 2, buses 4/5) and serio2 (pair 3, buses 6/7) feed a reverb.
//   Reverb (0xc1c836, Command War preset 1): serio1 and serio2 go to the reverb and to the output on the same
//     frame; the reverb tank is mono (nested all-pass blocks around a 1105 frame loop, see es5510.lib).
//   Chorus (0xc1d171, Kirameki preset 5): the same tank; serio1 is a 4-tap interpolated stereo chorus
//     (taps 70/303 L and 261/428 R frames, +-40 frames, two LFOs) with cross-fed feedback, its output feeds
//     the reverb.
//   Echo (0xc1d4d4, Land Maker preset 11): the same tank; serio1 is a ping-pong echo (p2 * 112 frames, feedback
//     p1/128) whose left input passes a small-signal enhancer (a saturating cubic of 16 x the input, smoothed,
//     added back); the block A tap reads the left echo line at 256 * p1 frames (G0 holds p1 << 16 | 0x7e after
//     the driver's write).
//   Hall (0xc1c4e2, Kirameki presets 0 and 2): two cross-coupled stereo-in/stereo-out reverb chains (a figure
//     of eight): per side a diffuser pair, two nested all-pass blocks and a shared LFO-modulated tap; each side's
//     output is low-passed and injected into the other side's input with gain p0/128.
//   Mute (0xc1ce1b, Kirameki preset 4): the program is a single END, the DAC gets nothing.
//   DAC L/R = dry + table(127-p8) * serio1 path + table(127-p9) * serio2 + reverb taps.
//
// LFOs: the ES5510 LFOs are GPR pairs the microcode rotates every frame, and the DSP executes its whole program
// every frame whatever the input is (ES5510::run_once() once per frame, runtime/audio/reference/engine.cpp:131),
// so an LFO phase keeps advancing through silence. Here too: every LFO advances by its rate on every frame,
// including the frames the auto-bypass does not compute; the phase restarts at (1, 0) only when a program is
// loaded (select() of a different program) and on reset().
class Effects {
public:
    enum class Program : uint8_t { None, Reverb, Hall, Chorus, Echo, Mute };
    // ROM pointers of the decoded programs (the same pointer table in all three sound ROMs).
    static constexpr uint32_t K_PROGRAM_TABLE_OFFSET = 0x1c3a2;
    static constexpr uint32_t K_PTR_REVERB = 0xc1c836, K_PTR_HALL = 0xc1c4e2, K_PTR_MUTE = 0xc1ce1b,
                              K_PTR_CHORUS = 0xc1d171, K_PTR_ECHO = 0xc1d4d4;

private:
    // A FAUST control zone, assigned from update().
    class Zone {
    public:
        Zone(const faust::Controls &controls, std::string_view label) : m_zone(controls.zone(label)) {}
        Zone(const Zone &) = delete;
        Zone &operator=(const Zone &) = delete;
        void operator=(float value) const { *m_zone = value; }

    private:
        FAUSTFLOAT *m_zone;
    };

    // Sine LFO of a program. FAUST rotates the GPR pair by eps every frame it computes; the frames it does not
    // compute (auto-bypass) are carried as a phase offset (double, mod 2 pi) that rotates the LFO output. The
    // rate and the offset are doubles (a float rate shifts the phase by up to 8e-5 rad in 30 s, 0.01 frame of
    // delay at the deepest modulation), passed through the float controls as the sum of two floats.
    class Lfo {
    public:
        Lfo(const faust::Controls &controls, std::string_view name)
            : m_eps_hi(controls, "lfo_" + std::string(name) + "_eps_hi"),
              m_eps_lo(controls, "lfo_" + std::string(name) + "_eps_lo"),
              m_ph_hi(controls, "lfo_" + std::string(name) + "_ph_hi"),
              m_ph_lo(controls, "lfo_" + std::string(name) + "_ph_lo") {}
        void set_rate(double eps) {
            m_eps = eps;
            write(eps, m_eps_hi, m_eps_lo);
        }
        // Frames the DSP did not compute: the LFO rotates by frames * eps.
        void skip(uint64_t frames) {
            m_phase = std::fmod(m_phase + double(frames) * m_eps, 2.0 * 3.14159265358979323846);
            write(m_phase, m_ph_hi, m_ph_lo);
        }
        void restart() {
            m_phase = 0.0;
            write(0.0, m_ph_hi, m_ph_lo);
        }

    private:
        static void write(double value, const Zone &hi, const Zone &lo) {
            const float high = float(value);
            hi = high;
            lo = float(value - double(high));
        }
        Zone m_eps_hi, m_eps_lo, m_ph_hi, m_ph_lo;
        double m_eps = 0.0, m_phase = 0.0;
    };

    // A FAUST program instance, its control zones and the LFOs it has.
    template <class Dsp> struct Instance {
        Dsp dsp;
        faust::Controls controls{dsp};
        Instance() { dsp.init(SAMPLE_RATE); }
        Instance(const Instance &) = delete;
        Instance &operator=(const Instance &) = delete;
    };
    // Controls every ES5510 program has (p8 / p9 gains, loop decay and damping, LFO C depth).
    template <class Dsp> struct Common : Instance<Dsp> {
        Zone chorus_out{this->controls, "chorus_out"}, dry_send{this->controls, "dry_send"},
            chorus_to_reverb{this->controls, "chorus_to_reverb"}, send{this->controls, "send"},
            decay{this->controls, "decay"}, damp{this->controls, "damp"}, depth{this->controls, "depth"};
        Lfo lfo_c{this->controls, "c"};
    };
    // The Reverb, Chorus and Echo programs share the reverb tank and its constants.
    template <class Dsp> struct Tank : Common<Dsp> {
        Zone scale{this->controls, "scale"}, leak{this->controls, "leak"}, c1{this->controls, "c1"},
            c2{this->controls, "c2"}, g{this->controls, "g"}, tap_l0{this->controls, "tap_l0"},
            tap_l1{this->controls, "tap_l1"}, tap_l2{this->controls, "tap_l2"}, tap_r0{this->controls, "tap_r0"},
            tap_r1{this->controls, "tap_r1"}, tap_r2{this->controls, "tap_r2"};
    };
    struct ReverbProgram : Tank<faust::ReverbDsp> {};
    template <class Dsp> struct FeedbackProgram : Tank<Dsp> {
        Zone feedback{this->controls, "feedback"}, sin_mix{this->controls, "sin_mix"},
            cos_mix{this->controls, "cos_mix"};
    };
    struct ChorusProgram : FeedbackProgram<faust::ChorusDsp> {
        Lfo lfo_a{controls, "a"}, lfo_b{controls, "b"};
    };
    struct EchoProgram : FeedbackProgram<faust::EchoDsp> {
        Zone echo{controls, "echo"}, mod_center{controls, "mod_center"}, enhance{controls, "enhance"};
    };
    struct HallProgram : Common<faust::HallDsp> {
        Zone a{controls, "hall_a"}, g{controls, "hall_g"}, g2{controls, "hall_g2"}, g108{controls, "hall_g108"};
    };
    struct DcBlocker : Instance<faust::DcBlockerDsp> {
        Zone gain_l{controls, "gain_l"}, gain_r{controls, "gain_r"}, r{controls, "r"};
    };
    // The generated classes hold their delay memory inline (about 900 KB for all four), so each instance is
    // allocated once, when the Effects is constructed, and never moves: the control zones point into it.
    std::unique_ptr<ReverbProgram> m_reverb = std::make_unique<ReverbProgram>();
    std::unique_ptr<ChorusProgram> m_chorus = std::make_unique<ChorusProgram>();
    std::unique_ptr<EchoProgram> m_echo_program = std::make_unique<EchoProgram>();
    std::unique_ptr<HallProgram> m_hall = std::make_unique<HallProgram>();
    std::unique_ptr<DcBlocker> m_dc = std::make_unique<DcBlocker>();

    // Reverb tank constants of the Reverb/Chorus/Echo programs (GPR values of the decoded programs).
    struct TankConstants {
        float scale = .7071f;   // input scale: G59 * G4 (Chorus/Echo) or ASR * G4 (Reverb)
        float leak = .0075f;    // G3: one-pole leak of the mono input, gain (1 - leak) * scale
        float c1 = .00703f, c2 = .04922f; // G108 / 8 and G108 * 0.875: recirculation through the damping poles
        float g = .4f;          // all-pass gain of the 168/276 pair (G8), (1 - g^2) = G10
        float tap_l[3] = {.13333f, .66667f, .46667f}; // wet weights of W48 / W68 / W85 at 384/4/26 (L)
        float tap_r[3] = {.13333f, .66667f, .66667f}; // ... and 1105/242/180 (R)
    };
    TankConstants m_tank;

    // Parameter-dependent coefficients (update()).
    Program m_program = Program::None;
    float m_decay = .25f, m_damp = .3125f, m_feedback = 0.f, m_sin_mix = 0.f, m_cos_mix = 1.f;
    float m_send = .2448f, m_dry_send = .9695f, m_chorus_to_reverb = .2448f, m_chorus_out = .9695f;
    float m_echo = 7168.f, m_mod_center = .49f, m_enhance = .00683594f;
    float m_hall_a = .5f, m_hall_g = .65625f, m_hall_g2 = .52295f, m_hall_g108 = .04689f, m_lfo_depth = 8.f;
    double m_eps_a = 0.0, m_eps_b = 0.0, m_eps_c = 0.0; // LFO rates, rad per frame

    std::span<const uint8_t> m_rom;
    std::array<uint8_t, 10> m_parameters{};
    uint8_t m_preset = 0;
    static constexpr size_t K_DELAY_BUFFER_SIZE = 96000ull * SAMPLE_RATE / 48000;
    static constexpr size_t K_BUCKET_COUNT = 16;
    static constexpr size_t K_BUCKET_SIZE = 6000ull * SAMPLE_RATE / 48000; // 16 * K_BUCKET_SIZE ~ K_DELAY_BUFFER_SIZE
    // Frames per FAUST compute() call (the caller renders at most 256 at a time).
    static constexpr size_t K_CHUNK = 256;

    // Silence threshold: maximum output gain is Audio::output_boost (12.0) * route (3.2) = 38.4 and the wet
    // path gain is bounded by ~3.0, so a line peak below 1e-8 is under 0.02 int16 LSB at the output.
    static constexpr float K_THRESHOLD = 1e-8f;

    std::array<float, K_BUCKET_COUNT> m_bucket_peaks{};
    size_t m_bucket_frames = 0;
    size_t m_bucket_idx = 0;
    float m_current_bucket_peak = 0.f;
    uint64_t m_silent_frames = K_DELAY_BUFFER_SIZE;
    bool m_bypassed = true;

    float table(unsigned index) const {
        const size_t p = sound_5a7e9117::VOLUME_PAN_TABLE_OFFSET + std::min(index, 127u) * 2;
        return float(uint16_t(uint16_t(m_rom[p]) << 8 | m_rom[p + 1])) / 32768.f;
    }
    // Parameter mapping per program. Every entry was read by patching the preset row in a ROM copy and diffing
    // the ES5510 GPRs the driver writes (section 9 of the note); GPR formulas are exact unless marked.
    //   all programs: p8 -> G124/G125 = table(p8)/table(127-p8), p9 -> G122/G123 = table(p9)/table(127-p9).
    //   LFO rate G = 196.608 * (p2 - 15) / 2^23 rad per frame (Reverb/Hall G106, Chorus G133; Chorus G107 = 296.4 * ...).
    //   Reverb: p0 -> G110 = p0/128, p1 -> G108 = p1/128, p2 -> LFO C rate, p3 -> LFO depth p3/2 frames,
    //     p6 high nibble -> G1 = n/16, p7 -> g = p7/128 (all-pass gain G8).
    //   Hall: p0 -> G110 = p0/128 and a = G5 = (p0 + 7)/128, p1 -> G108 = p1/128, p2 -> LFO rate, p3 -> depth,
    //     p6 high nibble -> G1, p7 -> g = p7/128 (g2 = G17 = 0.796875 g).
    //   Chorus: p0 -> G110 = p0/128, p1 -> G134 = p1/128, p2 -> LFO A/B rates, p6 high nibble -> G1, p6 low nibble n ->
    //     (G118, G119) = (table(8n + 4), table(127 - (8n + 4))).
    //   Echo (as observed in Land Maker, whose driver never writes G110/G108/G1 after the program load): p1 -> G134
    //     and G0 (256 p1 + 0.49 frames), p2 -> echo p2 * 112 frames, p6 low nibble -> (G118, G119) as above,
    //     p6 high nibble -> G143 = (n^2 + 3) / 256 (the enhancer state decay; the enhancer gain is G143 / 4).
    //   Leak G3 depends on p6's low nibble in a way not decoded; the presets of the profiled games fix it (Reverb
    //     0.0703 for p6 = 94, Hall 0.0078 for p6 = 143, Chorus/Echo 0.0075).
    void update() {
        constexpr double Q23 = 8388608.0;
        const auto lfo_rate = [&](double slope) { return std::max(0.0, slope * (double(m_parameters[2]) - 15.0)) / Q23; };
        m_chorus_to_reverb = table(m_parameters[8]);
        m_chorus_out = table(127 - m_parameters[8]);
        m_send = table(m_parameters[9]);
        m_dry_send = table(127 - m_parameters[9]);
        const unsigned angle = std::min(8u * (m_parameters[6] & 15u) + 4u, 127u);
        m_sin_mix = table(angle);
        m_cos_mix = table(127 - angle);
        m_feedback = std::clamp(float(int8_t(m_parameters[1])) / 128.f, -.95f, .95f);
        m_damp = float(m_parameters[6] >> 4) / 16.f;
        m_eps_c = 4864.0 / Q23;
        m_lfo_depth = 8.f;
        switch (m_program) {
        case Program::Reverb: {
            m_tank = TankConstants{};
            m_tank.scale = .5f;
            m_tank.leak = .0703125f;
            const float g108 = float(m_parameters[1]) / 128.f;
            m_tank.c1 = g108 / 8.f;
            m_tank.c2 = g108 * .875f;
            m_tank.g = float(m_parameters[7]) / 128.f;
            m_tank.tap_l[0] = .125f; m_tank.tap_l[1] = .625f; m_tank.tap_l[2] = .4375f;
            m_tank.tap_r[0] = .125f; m_tank.tap_r[1] = .625f; m_tank.tap_r[2] = .625f;
            m_decay = std::min(float(m_parameters[0]) / 128.f, .9f);
            m_eps_c = lfo_rate(196.608);
            m_lfo_depth = float(m_parameters[3]) / 2.f;
            break;
        }
        case Program::Chorus:
            m_tank = TankConstants{};
            m_decay = std::min(float(m_parameters[0]) / 128.f, .9f);
            m_eps_a = lfo_rate(196.608);
            m_eps_b = lfo_rate(296.4);
            break;
        case Program::Echo:
            m_tank = TankConstants{};
            m_decay = .25f;
            m_damp = .3125f;
            m_echo = float(m_parameters[2]) * 112.f;
            m_mod_center = std::clamp(float(int8_t(m_parameters[1])), 0.f, 127.f) * 256.f + .49f;
            m_enhance = float((m_parameters[6] >> 4) * (m_parameters[6] >> 4) + 3) / 1024.f;
            break;
        case Program::Hall:
            m_decay = std::min(float(m_parameters[0]) / 128.f, .9f);
            m_hall_a = float(m_parameters[0] + 7) / 128.f;
            m_hall_g108 = float(m_parameters[1]) / 128.f;
            m_hall_g = float(m_parameters[7]) / 128.f;
            m_hall_g2 = .796875f * m_hall_g;
            m_eps_c = lfo_rate(196.608);
            m_lfo_depth = float(m_parameters[3]) / 2.f;
            break;
        case Program::Mute:
        case Program::None:
            break;
        }
        write_controls();
    }

    template <class P> void write_common(P &p) {
        p.chorus_out = m_chorus_out;
        p.dry_send = m_dry_send;
        p.chorus_to_reverb = m_chorus_to_reverb;
        p.send = m_send;
        p.decay = m_decay;
        p.damp = m_damp;
        p.depth = m_lfo_depth;
        p.lfo_c.set_rate(m_eps_c);
    }
    template <class P> void write_tank(P &p) {
        write_common(p);
        p.scale = m_tank.scale;
        p.leak = m_tank.leak;
        p.c1 = m_tank.c1;
        p.c2 = m_tank.c2;
        p.g = m_tank.g;
        p.tap_l0 = m_tank.tap_l[0];
        p.tap_l1 = m_tank.tap_l[1];
        p.tap_l2 = m_tank.tap_l[2];
        p.tap_r0 = m_tank.tap_r[0];
        p.tap_r1 = m_tank.tap_r[1];
        p.tap_r2 = m_tank.tap_r[2];
    }
    template <class P> void write_feedback(P &p) {
        write_tank(p);
        p.feedback = m_feedback;
        p.sin_mix = m_sin_mix;
        p.cos_mix = m_cos_mix;
    }
    // Writes the coefficients update() computed into the zones of the loaded program.
    void write_controls() {
        switch (m_program) {
        case Program::Reverb: write_tank(*m_reverb); break;
        case Program::Chorus:
            write_feedback(*m_chorus);
            m_chorus->lfo_a.set_rate(m_eps_a);
            m_chorus->lfo_b.set_rate(m_eps_b);
            break;
        case Program::Echo:
            write_feedback(*m_echo_program);
            m_echo_program->echo = m_echo;
            m_echo_program->mod_center = m_mod_center;
            m_echo_program->enhance = m_enhance;
            break;
        case Program::Hall:
            write_common(*m_hall);
            m_hall->a = m_hall_a;
            m_hall->g = m_hall_g;
            m_hall->g2 = m_hall_g2;
            m_hall->g108 = m_hall_g108;
            break;
        case Program::Mute:
        case Program::None:
            break;
        }
    }

    // Physical PCB wiring from OTIS (ES5505) to ESP (ES5510), 12's oscilloscope trace through TC0660FCM
    // (docs/notes/raw/docs/taito-f3/5505-5510.txt): OTIS ser0 -> NC; ser1 -> ESP serio0;
    // ser2 -> serio1; ser3 -> serio2; ESP serio3 -> DAC. This is MAME esqpump's order.
    // - OTIS pair 0 (ch 0/1, buses 0/1): unconnected (nc) -> silent, reaches nothing.
    // - OTIS pair 1 (ch 2/3, buses 2/3): feeds ESP serio0 -> dry output.
    // - OTIS pair 2 (ch 4/5, buses 4/5): feeds ESP serio1 -> chorus / echo / direct (and the reverb).
    // - OTIS pair 3 (ch 6/7, buses 6/7): feeds ESP serio2 -> reverb send.
    static constexpr size_t K_BUS_DRY_L = 2;
    static constexpr size_t K_BUS_DRY_R = 3;
    static constexpr size_t K_BUS_DELAY_L = 4;
    static constexpr size_t K_BUS_DELAY_R = 5;
    static constexpr size_t K_BUS_REVERB_L = 6;
    static constexpr size_t K_BUS_REVERB_R = 7;

    // Output DC blocker (one pole, 20 Hz) written to the dc_blocker.dsp control "r".
    void init_dc_blocker() {
        m_dc->r = std::exp(-2.0f * 3.14159265358979323846f * 20.0f / float(SAMPLE_RATE));
    }

    // Runs the loaded program over n frames: planar inputs a0l a0r a1l a1r a2l a2r, outputs DAC L, DAC R, peak.
    void compute_program(int n, FAUSTFLOAT **in, FAUSTFLOAT **out) {
        switch (m_program) {
        case Program::Reverb: m_reverb->dsp.compute(n, in, out); break;
        case Program::Chorus: m_chorus->dsp.compute(n, in, out); break;
        case Program::Echo: m_echo_program->dsp.compute(n, in, out); break;
        case Program::Hall: m_hall->dsp.compute(n, in, out); break;
        case Program::Mute:
        case Program::None: break;
        }
    }

    // Program load and reset: clear the delay memory and state of the DSPs and restart the LFOs at (1, 0).
    void clear_program(Program program) {
        switch (program) {
        case Program::Reverb:
            m_reverb->dsp.instanceClear();
            m_reverb->lfo_c.restart();
            break;
        case Program::Chorus:
            m_chorus->dsp.instanceClear();
            m_chorus->lfo_a.restart();
            m_chorus->lfo_b.restart();
            m_chorus->lfo_c.restart();
            break;
        case Program::Echo:
            m_echo_program->dsp.instanceClear();
            m_echo_program->lfo_c.restart();
            break;
        case Program::Hall:
            m_hall->dsp.instanceClear();
            m_hall->lfo_c.restart();
            break;
        case Program::Mute:
        case Program::None: break;
        }
    }
    // Frames the loaded program was not computed (auto-bypass): every LFO keeps rotating.
    void skip_lfos(uint64_t frames) {
        switch (m_program) {
        case Program::Reverb: m_reverb->lfo_c.skip(frames); break;
        case Program::Chorus:
            m_chorus->lfo_a.skip(frames);
            m_chorus->lfo_b.skip(frames);
            m_chorus->lfo_c.skip(frames);
            break;
        case Program::Echo: m_echo_program->lfo_c.skip(frames); break;
        case Program::Hall: m_hall->lfo_c.skip(frames); break;
        case Program::Mute:
        case Program::None: break;
        }
    }

    uint32_t rom_u32(size_t offset) const {
        return uint32_t(m_rom[offset]) << 24 | uint32_t(m_rom[offset + 1]) << 16 | uint32_t(m_rom[offset + 2]) << 8 |
               m_rom[offset + 3];
    }

public:
    explicit Effects(std::span<const uint8_t> rom) : m_rom(rom) {
        init_dc_blocker();
        reset();
    }
    // The FAUST instances keep pointers to their own members (control zones).
    Effects(const Effects &) = delete;
    Effects &operator=(const Effects &) = delete;

    // Loads a preset: reads its parameters and the program it names. Throws std::runtime_error for a program
    // that has not been decoded (the DSP would run something this model does not reproduce).
    void select(uint8_t preset) {
        if (preset >= 13) return;
        const size_t row = sound_5a7e9117::PRESET_TABLE_OFFSET + size_t(preset) * 16;
        const uint32_t pointer = rom_u32(K_PROGRAM_TABLE_OFFSET + size_t(m_rom[row + 5]) * 4);
        Program program;
        switch (pointer) {
        case K_PTR_REVERB: program = Program::Reverb; break;
        case K_PTR_HALL: program = Program::Hall; break;
        case K_PTR_CHORUS: program = Program::Chorus; break;
        case K_PTR_ECHO: program = Program::Echo; break;
        case K_PTR_MUTE: program = Program::Mute; break;
        default:
            throw std::runtime_error("ES5510 program at " + std::to_string(pointer) + " (preset " +
                                     std::to_string(preset) + ") is not decoded");
        }
        // Row bytes 6..15 are p0..p9 in the order the sound commands set them (confirmed by diffing the DSP
        // registers after patching row bytes).
        for (unsigned n = 0; n < 10; ++n) m_parameters[n] = m_rom[row + 6 + n];
        if (program != m_program) clear_program(program);
        m_program = program;
        m_preset = preset;
        update();
    }
    void parameter(uint8_t index, uint8_t value) {
        if (index < m_parameters.size()) {
            m_parameters[index] = value;
            update();
        }
    }
    Program program() const { return m_program; }
    // True while the auto-bypass skips the loaded program (silent sends and nothing audible left in it).
    bool bypassed() const { return m_bypassed; }
    EffectsInfo info() const {
        EffectsInfo out{};
        switch (m_program) {
        case Program::None: out.program = EffectsProgram::None; break;
        case Program::Reverb: out.program = EffectsProgram::Reverb; break;
        case Program::Hall: out.program = EffectsProgram::Hall; break;
        case Program::Chorus: out.program = EffectsProgram::Chorus; break;
        case Program::Echo: out.program = EffectsProgram::Echo; break;
        case Program::Mute: out.program = EffectsProgram::Mute; break;
        }
        out.preset = m_preset;
        out.p = m_parameters;
        out.decay = m_decay;
        out.damp = m_damp;
        out.feedback = m_feedback;
        out.send = m_send;
        out.dry_send = m_dry_send;
        out.chorus_to_reverb = m_chorus_to_reverb;
        out.chorus_out = m_chorus_out;
        out.lfo_a = m_eps_a;
        out.lfo_b = m_eps_b;
        out.lfo_c = m_eps_c;
        out.lfo_depth_frames = m_lfo_depth;
        out.echo_frames = m_echo;
        out.mod_center = m_mod_center;
        out.enhance = m_enhance;
        out.hall_a = m_hall_a;
        out.hall_g = m_hall_g;
        out.hall_g2 = m_hall_g2;
        out.hall_g108 = m_hall_g108;
        out.bypassed = m_bypassed;
        return out;
    }
    void reset() {
        for (Program program : {Program::Reverb, Program::Chorus, Program::Echo, Program::Hall}) clear_program(program);
        m_bucket_peaks.fill(0.f);
        m_current_bucket_peak = 0.f;
        m_bucket_frames = 0;
        m_silent_frames = K_DELAY_BUFFER_SIZE;
        m_dc->dsp.instanceClear();
        m_bypassed = true;
        m_program = Program::None;
        m_parameters.fill(0);
        update();
    }
    void mix(const float *buses, float *stereo, size_t frames,
             const std::array<float, 2> &input_gain, const std::array<float, 2> &output_gain) {
        if (frames == 0) return;
        if (m_program == Program::Mute) {
            // A single END: the DSP never writes serio3, the DAC input stays at its last (constant) value.
            std::fill(stereo, stereo + frames * 2, 0.f);
            return;
        }

        bool silent_sends = true;
        for (size_t f = 0; f < frames; ++f) {
            if (buses[f * 8 + K_BUS_DELAY_L] != 0.f || buses[f * 8 + K_BUS_DELAY_R] != 0.f ||
                buses[f * 8 + K_BUS_REVERB_L] != 0.f || buses[f * 8 + K_BUS_REVERB_R] != 0.f) {
                silent_sends = false;
                break;
            }
        }

        m_dc->gain_l = output_gain[0];
        m_dc->gain_r = output_gain[1];
        std::array<std::array<FAUSTFLOAT, K_CHUNK>, 6> input;
        std::array<std::array<FAUSTFLOAT, K_CHUNK>, 3> output;
        std::array<std::array<FAUSTFLOAT, K_CHUNK>, 2> blocked;
        FAUSTFLOAT *in[6], *out[3], *dc_out[2];
        for (size_t n = 0; n < 6; ++n) in[n] = input[n].data();
        for (size_t n = 0; n < 3; ++n) out[n] = output[n].data();
        for (size_t n = 0; n < 2; ++n) dc_out[n] = blocked[n].data();
        const auto interleave = [&](size_t offset, size_t n) {
            for (size_t f = 0; f < n; ++f) {
                stereo[(offset + f) * 2 + 0] = dc_out[0][f];
                stereo[(offset + f) * 2 + 1] = dc_out[1][f];
            }
        };

        if (m_bypassed || m_program == Program::None) {
            if (silent_sends || m_program == Program::None) {
                // Nothing audible is left in the program (or none is loaded): only the dry path is heard. The DSP
                // is not computed, but its LFOs keep rotating (see the class comment).
                skip_lfos(frames);
                for (size_t offset = 0; offset < frames; offset += K_CHUNK) {
                    const size_t n = std::min(K_CHUNK, frames - offset);
                    for (size_t f = 0; f < n; ++f) {
                        const float *bus = buses + (offset + f) * 8;
                        in[0][f] = bus[K_BUS_DRY_L] * input_gain[0];
                        in[1][f] = bus[K_BUS_DRY_R] * input_gain[1];
                    }
                    m_dc->dsp.compute(int(n), in, dc_out);
                    interleave(offset, n);
                }
                return;
            }
            m_bypassed = false;
            m_silent_frames = 0;
        }

        if (!silent_sends) {
            m_silent_frames = 0;
        } else {
            m_silent_frames += frames;
        }

        for (size_t offset = 0; offset < frames; offset += K_CHUNK) {
            const size_t n = std::min(K_CHUNK, frames - offset);
            for (size_t f = 0; f < n; ++f) {
                const float *bus = buses + (offset + f) * 8;
                in[0][f] = bus[K_BUS_DRY_L] * input_gain[0];
                in[1][f] = bus[K_BUS_DRY_R] * input_gain[1];
                in[2][f] = bus[K_BUS_DELAY_L] * input_gain[0];
                in[3][f] = bus[K_BUS_DELAY_R] * input_gain[1];
                in[4][f] = bus[K_BUS_REVERB_L] * input_gain[0];
                in[5][f] = bus[K_BUS_REVERB_R] * input_gain[1];
            }
            compute_program(int(n), in, out);
            for (size_t f = 0; f < n; ++f) {
                m_current_bucket_peak = std::max(m_current_bucket_peak, out[2][f]);
                if (++m_bucket_frames >= K_BUCKET_SIZE) {
                    m_bucket_peaks[m_bucket_idx] = m_current_bucket_peak;
                    m_bucket_idx = (m_bucket_idx + 1) % K_BUCKET_COUNT;
                    m_current_bucket_peak = 0.f;
                    m_bucket_frames = 0;
                }
            }
            m_dc->dsp.compute(int(n), out, dc_out);
            interleave(offset, n);
        }

        if (m_silent_frames >= K_DELAY_BUFFER_SIZE) {
            float peak = m_current_bucket_peak;
            for (float bp : m_bucket_peaks) peak = std::max(peak, bp);
            if (peak < K_THRESHOLD) {
                // Everything the detector watches is under 0.02 LSB: the state is not cleared (it would only
                // drop that residue), the program is just not computed until a send arrives.
                m_bucket_peaks.fill(0.f);
                m_current_bucket_peak = 0.f;
                m_bucket_frames = 0;
                m_bypassed = true;
            }
        }
    }
};
} // namespace f3rt::hle
