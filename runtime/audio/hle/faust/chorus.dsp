// ES5510 program 5 "Chorus" (ROM pointer 0xc1d171; Kirameki preset 5).
//
//   a1 (serio1 L/R) is a 4-tap stereo chorus: two lines, W29 (left in) and W34 (right in), cross-fed back
//                   (W29 gets G130 = a tap of W34, W34 gets G129 = a tap of W29) with gain feedback = p1/128.
//                   Two LFOs (A, B) modulate the taps by +-40 frames.
//   a2 (serio2 L/R) as in the Reverb program.
//   The chorus output (latched twice: taps -> G128..G131 -> G115 / G116 -> DAC) feeds the reverb tank, which is
//   the one of reverb.dsp with the Chorus constants (scale .7071, leak .0075, g = .4, taps .1333/.6667/.4667 L,
//   .1333/.6667/.6667 R).
//
//    a1l ─►(+)─ W29 ──┬─ @70 + 40*xA ──► T128 ─┐  (taps are latched: T128 .. T131 = C++ m_g128 .. m_g131)
//           ▲         ├─ @303 + 40*xB ─► T129 ─┼─► sin*.7071*(T128 + T129) + cos*a1l ─► G115 ─┐
//           │         │                         │                                           │
//    a1r ─►(+)─ W34 ──┼─ @261 + 40*yA ─► T130 ─┤                                            ├─► d_l, d_r (latched)
//           ▲  ▲      └─ @428 + 40*yB ─► T131 ─┘  sin*.7071*(T130 + T131) + cos*a1r ─► G116 ─┘
//           │  └── feedback * T129 (W34)       feedback * T130 (W29): the lines are cross-fed
//
//   d_l, d_r ─► DAC (chorus_out) and, summed with a2, into the reverb tank.
//
// Old C++: Effects::frame() for Program::Chorus; m_w29, m_w34 (lines), m_g128..m_g131 (taps), m_g115 / m_g116 (outputs).
import("es5510.lib");

feedback = hslider("feedback", 0, -0.95, 0.95, 0.0001);   // G134 = p1/128
sin_mix  = hslider("sin_mix", 0, 0, 1, 0.0001);           // G118 = table(8n + 4), n = p6 & 15
cos_mix  = hslider("cos_mix", 1, 0, 1, 0.0001);           // G119 = table(127 - (8n + 4))

// (a1l, a1r) -> (W29, W34, G115 new, G116 new): the two lines and the chorus outputs before their latch.
// Inside the ~ loop the lines arrive delayed by one sample, which is the one frame of latency of a tap read.
chorus_lines(xa, ya, xb, yb) = loop ~ bus(2)
with {
    loop(fb29, fb34, a1l, a1r) = w29, w34, g115n, g116n
    with {
        t128 = (fb29, 70 + 40*xa) : itap(470);     // m_g128 = m_w29.tap(t, 70 + 40 xa)
        t129 = (fb29, 303 + 40*xb) : itap(470);    // m_g129 = m_w29.tap(t, 303 + 40 xb)
        t130 = (fb34, 261 + 40*ya) : itap(470);    // m_g130 = m_w34.tap(t, 261 + 40 ya)
        t131 = (fb34, 428 + 40*yb) : itap(470);    // m_g131 = m_w34.tap(t, 428 + 40 yb)
        w29 = a1l + feedback*t130;
        w34 = a1r + feedback*t129;
        g115n = sin_mix*0.7071*(t128 + t129) + cos_mix*a1l;
        g116n = sin_mix*0.7071*(t130 + t131) + cos_mix*a1r;
    };
};

program(a0l, a0r, a1l, a1r, a2l, a2r) = (lfo_a, lfo_b, lfo_c) : \(xa, ya, xan, yan, xb, yb, xbn, ybn, xc, yc, xcn, ycn).(
    (a1l, a1r) : chorus_lines(xa, ya, xb, yb) : \(w29, w34, g115n, g116n).(
        m : tank(ap3_tap(921, xc)) : \(wl, wr, tank_peak).(dac_l, dac_r, peak
        with {
            dac_l = a0l + chorus_out*d_l + dry_send*a2l + wl';
            dac_r = a0r + chorus_out*d_r + dry_send*a2r + wr';
            peak = max3(tank_peak, abs(g115n), abs(g116n));
        })
        with {
            d_l = g115n';
            d_r = g116n';
            m = chorus_to_reverb*(d_l + d_r) + send*(a2l + a2r);
        }));

process = bus(6) : program;
