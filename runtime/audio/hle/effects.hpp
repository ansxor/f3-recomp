#pragma once
#include "audio/hle/events.hpp"
#include "audio/hle/sound_rom_5a7e9117.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace f3rt::hle {
static_assert(SAMPLE_RATE == 29761, "effect delay lengths are in ES5510 frames (the OTIS/ESP sample rate)");

// Substitute for the ES5510 effects programs. The sound ROM holds a table of program pointers (0xc1c3a2) and a
// preset table (0xc1c2d2, 16 bytes per preset); byte 5 of a preset row indexes the pointer table and bytes 6..15
// are the ten parameters p0..p9 in the order the sound commands set them. A preset loads the program its pointer
// names; programs are identified here by that pointer, so the selection does not depend on the game. Five
// programs are decoded (every program the three profiled games Kirameki, Land Maker and Command War select) and
// each has its own graph below; any other program throws instead of being approximated.
// Delay lengths, gains and topology are the ones of the loaded program as it runs in the reference backend:
// every delay is a (read offset - write offset) / 256 distance inside the DSP's single 64K-word delay RAM,
// every gain is a GPR of the program, and each graph reproduces the emulated ES5510 impulse response to about
// -70 dB re peak (the 16-bit truncation floor), checked by tools/compare/esp_graph.py verify.
// docs/notes/raw/tests/2026-10-16-kirameki-187-pcb-stereo-reverb.md sections 8 and 9 list the graphs and the
// parameter mappings, which were read by patching preset rows in a ROM copy and diffing the GPRs.
// No DSP instructions or sampled audio are stored.
//
//   serio0 (OTIS pair 1, buses 2/3): dry in every program.
//   serio1 (pair 2, buses 4/5) and serio2 (pair 3, buses 6/7) feed a reverb.
//   Reverb (0xc1c836, Command War preset 1): serio1 and serio2 go to the reverb and to the output on the same
//     frame; the reverb tank is mono: a 1105 frame loop return with decay/damping -> all-pass pair (168, 276)
//     -> nested all-pass block B (755/549/1369) -> nested all-pass block A (1012/696/242 plus an LFO-modulated
//     921 frame tap); left and right wet outputs are different taps of the loop nodes.
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
class Effects {
public:
    enum class Program : uint8_t { None, Reverb, Hall, Chorus, Echo, Mute };
    // ROM pointers of the decoded programs (the same pointer table in all three sound ROMs).
    static constexpr uint32_t K_PROGRAM_TABLE_OFFSET = 0x1c3a2;
    static constexpr uint32_t K_PTR_REVERB = 0xc1c836, K_PTR_HALL = 0xc1c4e2, K_PTR_MUTE = 0xc1ce1b,
                              K_PTR_CHORUS = 0xc1d171, K_PTR_ECHO = 0xc1d4d4;

private:
    // Delay RAM segment: power-of-two ring addressed by the frame counter. get(t, d) is the value written
    // d frames before frame t (d = 0 is the value written this frame).
    struct Ring {
        std::vector<float> v;
        uint32_t mask;
        explicit Ring(uint32_t n) : v(n, 0.f), mask(n - 1) {}
        float get(uint32_t t, uint32_t d) const { return v[(t - d) & mask]; }
        void put(uint32_t t, float x) { v[t & mask] = x; }
        // ES5510 interpolated tap: two adjacent frames weighted by the fractional delay. Delays below zero
        // read the far end of the ring in the hardware (no signal), here silence.
        float tap(uint32_t t, float d) const {
            if (d < 0.f) return 0.f;
            const uint32_t i = uint32_t(d);
            const float f = d - float(i);
            const float a = get(t, i), b = get(t, i + 1);
            return a + f * (b - a);
        }
        void clear() { std::fill(v.begin(), v.end(), 0.f); }
    };
    // Ring sizes are the next power of two above the longest read distance of each segment.
    Ring m_w48{2048}, m_w50{1024}, m_w53{1024}, m_w54{1024}; // nested all-pass block A (1105/1012/696/~921)
    Ring m_w64{256}, m_w69{512}, m_w68{1024};                // diffuser pair (168, 276) and its output
    Ring m_w85{256}, m_w87{1024}, m_w88{2048};               // nested all-pass block B (755, 549, 1369)
    Ring m_w29{512}, m_w34{512};                              // chorus lines, left / right (taps <= 469)
    Ring m_echo_l{32768}, m_echo_r{32768};                    // echo lines (p2 * 112 <= 28560, 256 * p1 + 9 <= 32521)

    // One side of the Hall program: diffuser pair, nested all-pass block B, nested all-pass block A.
    struct HallChain {
        Ring wd{256}, w1{256}, wo{1024};          // diffuser pair and its output
        Ring bq{512}, br{1024}, bout{256};        // block B
        Ring an{1024}, cn{512}, o2{1024}, o4{1024}; // block A, output node, modulated-tap node
        uint16_t dd1, dd2, dp, dq, dr, da, db, dc;  // delays
        HallChain(uint16_t d1, uint16_t d2, uint16_t p, uint16_t q, uint16_t r, uint16_t a, uint16_t b, uint16_t c)
            : dd1(d1), dd2(d2), dp(p), dq(q), dr(r), da(a), db(b), dc(c) {}
        void step(uint32_t t, float v, float a, float g, float g2, float mod) {
            const float d1 = wd.get(t, dd1), d2 = w1.get(t, dd2);
            const float o1 = -g * v + (1.f - g * g) * d1;
            wd.put(t, v + g * d1);
            w1.put(t, o1 + g2 * d2);
            wo.put(t, (1.f - g2 * g2) * d2 - g2 * o1);
            const float p = wo.get(t, dp), q = bq.get(t, dq), r = br.get(t, dr);
            bout.put(t, -a * p + (1.f - a * a) * q);
            bq.put(t, -.25f * p - .25f * a * q + .9375f * r);
            br.put(t, .25f * r + p + a * q);
            const float x = an.get(t, da), y = bout.get(t, db), z = cn.get(t, dc);
            o2.put(t, (1.f - a * a) * x - a * y);
            an.put(t, -.25f * a * x + .9375f * z - .25f * y);
            cn.put(t, -.25f * a * x - .0625f * z - .25f * y + .9375f * mod);
            o4.put(t, a * x + .25f * z + y + .25f * mod);
        }
        void clear() {
            for (Ring *ring : {&wd, &w1, &wo, &bq, &br, &bout, &an, &cn, &o2, &o4}) ring->clear();
        }
    };
    HallChain m_hall_x{88, 141, 628, 232, 874, 650, 214, 369}; // right input chain
    HallChain m_hall_y{102, 168, 449, 335, 836, 617, 145, 424}; // left input chain

    // Registers carried to the next frame (ES5510 GPRs, one frame of latency).
    float m_g115 = 0, m_g116 = 0;                          // serio1 path outputs (programs Chorus and Echo)
    float m_g128 = 0, m_g129 = 0, m_g130 = 0, m_g131 = 0;  // chorus taps
    float m_g105 = 0, m_g111 = 0, m_g112 = 0, m_g113 = 0;  // echo taps and enhancer state
    float m_g67 = 0, m_g70 = 0, m_g71 = 0;                 // reverb input leak and the two in-loop damping poles
    float m_g89 = 0, m_g90 = 0, m_g91 = 0;                 // reverb output taps L/R, modulated tap of block A
    float m_hu_l = 0, m_hu_r = 0, m_hlp_y1 = 0, m_hlp_y2 = 0, m_hlp_x1 = 0, m_hlp_x2 = 0; // Hall input and low-pass states
    float m_hmod_x = 0, m_hmod_y = 0;                      // Hall modulated taps
    uint32_t m_t = 0;

    // Sine LFOs (GPR pairs): (x, y) rotated by eps each frame.
    struct Lfo {
        double x = 1.0, y = 0.0, c = 1.0, s = 0.0;
        void set_rate(double eps) { c = std::cos(eps); s = std::sin(eps); }
        void step() {
            const double nx = x * c - y * s;
            y = x * s + y * c;
            x = nx;
        }
    };
    Lfo m_lfo_a, m_lfo_b, m_lfo_c;

    // Reverb tank constants of the Reverb/Chorus/Echo programs (GPR values of the decoded programs).
    struct Tank {
        float scale = .7071f;   // input scale: G59 * G4 (Chorus/Echo) or ASR * G4 (Reverb)
        float leak = .0075f;    // G3: one-pole leak of the mono input, gain (1 - leak) * scale
        float c1 = .00703f, c2 = .04922f; // G108 / 8 and G108 * 0.875: recirculation through the damping poles
        float g = .4f;          // all-pass gain of the 168/276 pair (G8), (1 - g^2) = G10
        float tap_l[3] = {.13333f, .66667f, .46667f}; // wet weights of W48 / W68 / W85 at 384/4/26 (L)
        float tap_r[3] = {.13333f, .66667f, .66667f}; // ... and 1105/242/180 (R)
    };
    Tank m_tank;

    // Parameter-dependent coefficients (update()).
    Program m_program = Program::None;
    float m_decay = .25f, m_damp = .3125f, m_cos_mix = 1.f, m_sin_mix = 0.f, m_feedback = 0.f;
    float m_send = .2448f, m_dry_send = .9695f, m_chorus_to_reverb = .2448f, m_chorus_out = .9695f;
    float m_echo = 7168.f, m_mod_center = .49f, m_enhance = .00683594f;
    float m_hall_a = .5f, m_hall_g = .65625f, m_hall_g2 = .52295f, m_hall_g108 = .04689f, m_lfo_depth = 8.f;

    std::span<const uint8_t> m_rom;
    std::array<uint8_t, 10> m_parameters{};
    uint8_t m_preset = 0;
    static constexpr size_t K_DELAY_BUFFER_SIZE = 96000ull * SAMPLE_RATE / 48000;
    static constexpr size_t K_BUCKET_COUNT = 16;
    static constexpr size_t K_BUCKET_SIZE = 6000ull * SAMPLE_RATE / 48000; // 16 * K_BUCKET_SIZE ~ K_DELAY_BUFFER_SIZE

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
        m_lfo_c.set_rate(4864.0 / Q23);
        m_lfo_depth = 8.f;
        switch (m_program) {
        case Program::Reverb: {
            m_tank = Tank{};
            m_tank.scale = .5f;
            m_tank.leak = .0703125f;
            const float g108 = float(m_parameters[1]) / 128.f;
            m_tank.c1 = g108 / 8.f;
            m_tank.c2 = g108 * .875f;
            m_tank.g = float(m_parameters[7]) / 128.f;
            m_tank.tap_l[0] = .125f; m_tank.tap_l[1] = .625f; m_tank.tap_l[2] = .4375f;
            m_tank.tap_r[0] = .125f; m_tank.tap_r[1] = .625f; m_tank.tap_r[2] = .625f;
            m_decay = std::min(float(m_parameters[0]) / 128.f, .9f);
            m_lfo_c.set_rate(lfo_rate(196.608));
            m_lfo_depth = float(m_parameters[3]) / 2.f;
            break;
        }
        case Program::Chorus:
            m_tank = Tank{};
            m_decay = std::min(float(m_parameters[0]) / 128.f, .9f);
            m_lfo_a.set_rate(lfo_rate(196.608));
            m_lfo_b.set_rate(lfo_rate(296.4));
            break;
        case Program::Echo:
            m_tank = Tank{};
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
            m_lfo_c.set_rate(lfo_rate(196.608));
            m_lfo_depth = float(m_parameters[3]) / 2.f;
            break;
        case Program::Mute:
        case Program::None:
            break;
        }
    }

    // One ES5510 frame of the Reverb, Chorus and Echo programs. a0/a1/a2 = serio0/1/2 (L, R) inputs; returns the
    // DAC pair in out[0..1] and the largest internal level through peak.
    void frame(const float *a0, const float *a1, const float *a2, float *out, float &peak) {
        const uint32_t t = m_t++;
        const Tank &tk = m_tank;
        // serio1 path output (the program's own latency: none in program 0, one frame in programs 5 and 6).
        const float d_l = m_program == Program::Reverb ? a1[0] : m_g115;
        const float d_r = m_program == Program::Reverb ? a1[1] : m_g116;
        out[0] = a0[0] + m_chorus_out * d_l + m_dry_send * a2[0] + m_g89;
        out[1] = a0[1] + m_chorus_out * d_r + m_dry_send * a2[1] + m_g90;

        // Front end lines written this frame.
        float xe = a1[0], y = 0.f;
        if (m_program == Program::Chorus) {
            m_w29.put(t, a1[0] + m_feedback * m_g130);
            m_w34.put(t, a1[1] + m_feedback * m_g129);
        } else if (m_program == Program::Echo) {
            m_g113 = .75f * m_g113 + m_enhance * m_g112;
            xe = a1[0] + m_g113;
            y = std::clamp(4.f * std::clamp(4.f * a1[0], -1.f, 1.f), -1.f, 1.f);
            m_echo_l.put(t, xe + m_feedback * m_g111);
            m_echo_r.put(t, a1[1] + m_feedback * m_g105);
        }

        // Reverb input: mono sum of the send and the serio1 path, with the one-pole leak of the last input.
        const float u = tk.scale * (1.f - tk.leak) * (m_chorus_to_reverb * (d_l + d_r) + m_send * (a2[0] + a2[1])) +
                        tk.leak * m_g67;
        // Loop return through the decay gain and the two damping poles.
        const float ret = m_w48.get(t, 1105);
        const float v = u + m_decay * ((1.f - m_damp) * ret + m_damp * m_g70) + tk.c1 * m_g70 + tk.c2 * m_g71;
        // Diffuser pair: all-pass 168 and 276 frames, gain g, (1 - g^2) on the delayed branch.
        const float g = tk.g, h = 1.f - tk.g * tk.g;
        const float d1 = m_w64.get(t, 168), d2 = m_w69.get(t, 276);
        m_w64.put(t, v + g * d1);
        m_w69.put(t, -g * v + h * d1 + g * d2);
        m_w68.put(t, g * g * v - g * h * d1 + h * d2);
        // Nested all-pass block B (755 / 549 / 1369).
        const float p = m_w68.get(t, 755), q = m_w87.get(t, 549), r = m_w88.get(t, 1369);
        m_w85.put(t, -.5f * p + .75f * q);
        m_w87.put(t, -.25f * p - .125f * q + .9375f * r);
        m_w88.put(t, .25f * r + p + .5f * q);
        // Nested all-pass block A (1012 / 696 / 242 and the modulated tap latched last frame).
        const float a = m_w50.get(t, 1012), b = m_w85.get(t, 242), c = m_w53.get(t, 696);
        m_w48.put(t, .75f * a - .5f * b);
        m_w50.put(t, -.125f * a + .9375f * c - .25f * b);
        m_w53.put(t, -.125f * a - .0625f * c - .25f * b + .9375f * m_g91);
        m_w54.put(t, .5f * a + .25f * c + b + .25f * m_g91);

        // LFOs: depth uses C before its update (program order); A and B (Chorus) after theirs.
        const float xc = float(m_lfo_c.x);
        m_lfo_c.step();
        if (m_program == Program::Chorus) {
            m_lfo_a.step();
            m_lfo_b.step();
            const float xa = float(m_lfo_a.x), ya = float(m_lfo_a.y), xb = float(m_lfo_b.x), yb = float(m_lfo_b.y);
            const float n128 = m_w29.tap(t, 70.f + 40.f * xa), n129 = m_w29.tap(t, 303.f + 40.f * xb);
            const float n130 = m_w34.tap(t, 261.f + 40.f * ya), n131 = m_w34.tap(t, 428.f + 40.f * yb);
            m_g115 = m_sin_mix * .7071f * (m_g128 + m_g129) + m_cos_mix * a1[0];
            m_g116 = m_sin_mix * .7071f * (m_g130 + m_g131) + m_cos_mix * a1[1];
            m_g128 = n128;
            m_g129 = n129;
            m_g130 = n130;
            m_g131 = n131;
            m_g91 = m_w54.tap(t, 921.f + m_lfo_depth * xc);
        } else if (m_program == Program::Echo) {
            m_g115 = m_sin_mix * .7071f * m_g105 + m_cos_mix * xe;
            m_g116 = m_sin_mix * .7071f * m_g111 + m_cos_mix * a1[1];
            m_g105 = m_echo_l.tap(t, m_echo - .00390625f); // the hardware tap sits one fraction step (1/256 frame) short
            m_g111 = m_echo_r.get(t, uint32_t(m_echo));
            m_g112 = std::clamp(2.f * y - .5f * y * y * y, -1.f, 1.f);
            m_g91 = m_echo_l.tap(t, m_mod_center + m_lfo_depth * xc);
        } else {
            m_g91 = m_w54.tap(t, 921.f + m_lfo_depth * xc);
        }
        // Wet taps and damping poles latched for the next frame.
        m_g89 = tk.tap_l[0] * m_w48.get(t, 384) + tk.tap_l[1] * m_w68.get(t, 4) + tk.tap_l[2] * m_w85.get(t, 26);
        m_g90 = tk.tap_r[0] * m_w48.get(t, 1105) + tk.tap_r[1] * m_w68.get(t, 242) + tk.tap_r[2] * m_w85.get(t, 180);
        const float n70 = .625f * ret + .375f * m_g70;
        const float n71 = .125f * m_g70 + .875f * m_g71;
        m_g67 = u;
        m_g70 = n70;
        m_g71 = n71;
        peak = std::max({peak, std::abs(m_g89), std::abs(m_g90), std::abs(m_g115), std::abs(m_g116), std::abs(r),
                         std::abs(ret), std::abs(m_g71), std::abs(m_g113), std::abs(m_g105), std::abs(m_g111)});
    }

    // One ES5510 frame of the Hall program (two cross-coupled chains, the serio1 and serio2 sums enter per side).
    void frame_hall(const float *a0, const float *a1, const float *a2, float *out, float &peak) {
        const uint32_t t = m_t++;
        out[0] = a0[0] + m_chorus_out * a1[0] + m_dry_send * a2[0] + m_g89;
        out[1] = a0[1] + m_chorus_out * a1[1] + m_dry_send * a2[1] + m_g90;
        const float damp = m_damp;
        const float vx = .5f * m_hu_r + m_decay * m_hlp_x1 + m_hall_g108 * m_hlp_x2;
        const float vy = .5f * m_hu_l + m_decay * m_hlp_y1 + m_hall_g108 * m_hlp_y2;
        m_hall_x.step(t, vx, m_hall_a, m_hall_g, m_hall_g2, m_hmod_x);
        m_hall_y.step(t, vy, m_hall_a, m_hall_g, m_hall_g2, m_hmod_y);
        const float leak = .0078125f;
        const float ul = leak * m_hu_l + (1.f - leak) * (m_chorus_to_reverb * a1[0] + m_send * a2[0]);
        const float ur = leak * m_hu_r + (1.f - leak) * (m_chorus_to_reverb * a1[1] + m_send * a2[1]);
        const float w13 = m_hall_x.o2.get(t, 7), w44 = m_hall_y.o2.get(t, 675);
        m_hlp_y1 = (1.f - damp) * w13 + damp * m_hlp_y1;
        m_hlp_y2 = .125f * w13 + .875f * m_hlp_y2;
        m_hlp_x1 = (1.f - damp) * w44 + damp * m_hlp_x1;
        m_hlp_x2 = .125f * w44 + .875f * m_hlp_x2;
        const float xc = float(m_lfo_c.x);
        m_lfo_c.step();
        const float d90 = 611.f + m_lfo_depth * xc;
        m_hmod_x = m_hall_x.o4.tap(t, d90);
        m_hmod_y = m_hall_y.o4.tap(t, d90 - 136.f);
        m_g89 = .125f * m_hall_x.o2.get(t, 3) + .4375f * m_hall_x.bout.get(t, 163) +
                .9375f * m_hall_x.wo.get(t, 101) - .4375f * m_hall_y.bout.get(t, 131);
        m_g90 = -.4375f * m_hall_x.bout.get(t, 74) + .125f * m_hall_y.o2.get(t, 225) +
                .4375f * m_hall_y.bout.get(t, 10) + .5f * m_hall_y.wo.get(t, 1);
        m_hu_l = ul;
        m_hu_r = ur;
        peak = std::max({peak, std::abs(m_g89), std::abs(m_g90), std::abs(w13), std::abs(w44), std::abs(m_hlp_x2),
                         std::abs(m_hlp_y2)});
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

    // DC blocker state
    std::array<float, 2> m_dc_x1{0.f, 0.f};
    std::array<float, 2> m_dc_y1{0.f, 0.f};
    // Output DC blocker (one pole, 20 Hz). There is no analog low-pass: the PCB recording keeps its
    // top end up to 14.7 kHz (docs/notes/raw/tests/2026-10-16-kirameki-187-clap-dynamics.md).
    float m_dc_r = 0.f;
    void init_dc_blocker() { m_dc_r = std::exp(-2.0f * 3.14159265358979323846f * 20.0f / float(SAMPLE_RATE)); }

    void dc_block(float *stereo, size_t frames) {
        if (frames == 0) return;
        const float R = m_dc_r;
        for (size_t f = 0; f < frames; ++f) {
            for (size_t ch = 0; ch < 2; ++ch) {
                const float x = stereo[f * 2 + ch];
                const float y = x - m_dc_x1[ch] + R * m_dc_y1[ch];
                m_dc_x1[ch] = x;
                m_dc_y1[ch] = y;
                stereo[f * 2 + ch] = y;
            }
        }
        for (size_t ch = 0; ch < 2; ++ch) {
            if (std::abs(m_dc_x1[ch]) < K_THRESHOLD) m_dc_x1[ch] = 0.f;
            if (std::abs(m_dc_y1[ch]) < K_THRESHOLD) m_dc_y1[ch] = 0.f;
        }
    }

    void silence_state() {
        for (Ring *ring : {&m_w29, &m_w34, &m_w48, &m_w50, &m_w53, &m_w54, &m_w64, &m_w69, &m_w68, &m_w85,
                           &m_w87, &m_w88, &m_echo_l, &m_echo_r})
            ring->clear();
        m_hall_x.clear();
        m_hall_y.clear();
        m_g115 = m_g116 = m_g128 = m_g129 = m_g130 = m_g131 = 0.f;
        m_g105 = m_g111 = m_g112 = m_g113 = 0.f;
        m_g67 = m_g70 = m_g71 = m_g89 = m_g90 = m_g91 = 0.f;
        m_hu_l = m_hu_r = m_hlp_y1 = m_hlp_y2 = m_hlp_x1 = m_hlp_x2 = m_hmod_x = m_hmod_y = 0.f;
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
        if (program != m_program) {
            silence_state();
            m_lfo_a = m_lfo_b = m_lfo_c = Lfo{};
        }
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
    void reset() {
        silence_state();
        m_lfo_a = m_lfo_b = m_lfo_c = Lfo{};
        m_t = 0;
        m_bucket_peaks.fill(0.f);
        m_current_bucket_peak = 0.f;
        m_bucket_frames = 0;
        m_silent_frames = K_DELAY_BUFFER_SIZE;
        m_dc_x1.fill(0.f);
        m_dc_y1.fill(0.f);
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
            m_t += uint32_t(frames);
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

        if (m_bypassed || m_program == Program::None) {
            if (silent_sends || m_program == Program::None) {
                // All state is zero (or no program is loaded): only the dry path is audible. Time still
                // advances so the LFO phase and ring addressing stay consistent with a running DSP.
                m_t += uint32_t(frames);
                for (size_t f = 0; f < frames; ++f) {
                    const float dry0 = buses[f * 8 + K_BUS_DRY_L] * input_gain[0];
                    const float dry1 = buses[f * 8 + K_BUS_DRY_R] * input_gain[1];
                    stereo[f * 2 + 0] = dry0 * output_gain[0];
                    stereo[f * 2 + 1] = dry1 * output_gain[1];
                }
                dc_block(stereo, frames);
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

        const bool hall = m_program == Program::Hall;
        for (size_t f = 0; f < frames; ++f) {
            const float *bus = buses + f * 8;
            const float a0[2] = {bus[K_BUS_DRY_L] * input_gain[0], bus[K_BUS_DRY_R] * input_gain[1]};
            const float a1[2] = {bus[K_BUS_DELAY_L] * input_gain[0], bus[K_BUS_DELAY_R] * input_gain[1]};
            const float a2[2] = {bus[K_BUS_REVERB_L] * input_gain[0], bus[K_BUS_REVERB_R] * input_gain[1]};
            float out[2];
            if (hall) {
                frame_hall(a0, a1, a2, out, m_current_bucket_peak);
            } else {
                frame(a0, a1, a2, out, m_current_bucket_peak);
            }
            stereo[f * 2 + 0] = out[0] * output_gain[0];
            stereo[f * 2 + 1] = out[1] * output_gain[1];
            if (++m_bucket_frames >= K_BUCKET_SIZE) {
                m_bucket_peaks[m_bucket_idx] = m_current_bucket_peak;
                m_bucket_idx = (m_bucket_idx + 1) % K_BUCKET_COUNT;
                m_current_bucket_peak = 0.f;
                m_bucket_frames = 0;
            }
        }

        dc_block(stereo, frames);

        if (m_silent_frames >= K_DELAY_BUFFER_SIZE) {
            float peak = m_current_bucket_peak;
            for (float bp : m_bucket_peaks) peak = std::max(peak, bp);
            if (peak < K_THRESHOLD) {
                silence_state();
                m_bucket_peaks.fill(0.f);
                m_current_bucket_peak = 0.f;
                m_bucket_frames = 0;
                m_bypassed = true;
            }
        }
    }
};
} // namespace f3rt::hle
