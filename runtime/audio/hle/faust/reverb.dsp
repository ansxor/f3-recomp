// ES5510 program 0 "Reverb" (ROM pointer 0xc1c836; Command War preset 1, Kirameki preset 1).
//
//   a0 (serio0 L/R)  dry in every program.
//   a1 (serio1 L/R)  reaches the DAC on the same frame with table(127 - p8) and the reverb with table(p8);
//                    this program has no chorus or echo, so the serio1 path has no latency.
//   a2 (serio2 L/R)  table(127 - p9) direct, table(p9) into the reverb.
//
//    a0 ─────────────────────────────────────────────────────────────────────────────┐
//    a1 ─┬─ chorus_out ──────────────────────────────────────────────────────────────┤
//        └─ chorus_to_reverb ─┐                                                      ├─► DAC
//    a2 ─┬─ dry_send ─────────┼──────────────────────────────────────────────────────┤
//        └─ send ─────────────┴─(L+R)─► m ─► tank(N3) ─► wet_l, wet_r ─ latch ───────┘
//
//   N3 = modulated tap of the innermost node of block A at 921 + depth*xc frames, xc the LFO C value
//   (C++ m_g91 = m_w54.tap(t, 921 + m_lfo_depth * xc), latched; ap3_tap in es5510.lib aligns it with the nesting).
//
// Inputs: a0l a0r a1l a1r a2l a2r (already scaled by the OTIS input gain). Outputs: DAC L, DAC R and the
// per-frame peak of the internal levels the C++ uses to detect silence.
import("es5510.lib");

// Old C++: Effects::frame() for Program::Reverb.
program(a0l, a0r, a1l, a1r, a2l, a2r) = lfo_c : \(xc, yc, xcn, ycn).(
    m : tank(ap3_tap(921, xc)) : \(wl, wr, pk).(dac_l, dac_r, pk
    with {
        // m_g89 / m_g90 are latched: the wet taps of frame t reach the DAC on frame t + 1.
        dac_l = a0l + chorus_out*a1l + dry_send*a2l + wl';
        dac_r = a0r + chorus_out*a1r + dry_send*a2r + wr';
    })
    with {
        m = chorus_to_reverb*(a1l + a1r) + send*(a2l + a2r);
    });

process = bus(6) : program;
