// ES5510 program 6 "Echo" (ROM pointer 0xc1d4d4; Land Maker preset 11).
//
//   a1 (serio1 L/R) is a ping-pong echo: line EL (left) is a1l + enhancer + feedback * (right line tap), line ER
//                   (right) is a1r + feedback * (left line tap); delay echo = p2 * 112 frames, feedback = p1/128.
//                   The left input passes a small-signal enhancer: y = sat(4 sat(4 a1l)), G112 = sat(2y - y^3/2),
//                   G113 = .75 G113 + enhance * G112 (latched), added to the left line.
//   a2 (serio2 L/R) as in the Reverb program.
//   The tank is the one of reverb.dsp with the Echo constants; its block A has an open innermost all-pass whose
//   return is the left echo line read at mod_center + depth*xc (C++ m_g91 = m_echo_l.tap(t, ...)), not its own node.
//
//   a1l ─► xe = a1l + G113 ─►(+)─► EL ─┬─ @(echo - 1/256), interpolated ─► G105 ──┬─► G115 = sin*.7071*G105 + cos*xe  ─► d_l
//                             ▲          └─ @(mod_center + depth*xc), interp. ─► G91 ─► tank block A (open innermost all-pass)
//                             └─ feedback * G111
//   a1r ──────────────────────────►(+)─► ER ──── @int(echo) ──────────────► G111 ──┬─► G116 = sin*.7071*G111 + cos*a1r ─► d_r
//                                   ▲
//                                   └─ feedback * G105
//
//   G105, G111, G91 and the enhancer state are latched: the taps of frame t - 1 enter the lines at frame t, with the
//   echo time and LFO of frame t - 1 (hence the ' on those controls below).
//
// Old C++: Effects::frame() for Program::Echo; m_echo_l, m_echo_r (lines), m_g105 / m_g111 (taps), m_g112 / m_g113
// (enhancer), m_g115 / m_g116 (outputs).
import("es5510.lib");

feedback   = hslider("feedback", 0, -0.95, 0.95, 0.0001);   // G134 = p1/128
sin_mix    = hslider("sin_mix", 0, 0, 1, 0.0001);           // (G118, G119) as in the Chorus program
cos_mix    = hslider("cos_mix", 1, 0, 1, 0.0001);
echo       = hslider("echo", 7168, 0, 28560, 1);            // p2 * 112 frames
mod_center = hslider("mod_center", 14848.49, 0, 32512.49, 0.01); // 256 * p1 + .49 frames (G0)
enhance    = hslider("enhance", 0.00683594, 0, 1, 0.000001); // G143 / 4

// (a1l, a1r) -> (EL, ER, G115 new, G116 new, G91 as read by block A, enhancer / tap levels).
// fbl / fbr are EL / ER delayed by one sample: the tap read of frame t - 1 that the C++ latched.
echo_lines(xc) = loop ~ bus(2)
with {
    loop(fbl, fbr, a1l, a1r) = el, er, g115n, g116n, m, peak
    with {
        // Enhancer: G112 latched, G113 = .75 G113 + enhance * G112.
        y = sat(4*sat(4*a1l));
        g112n = sat(2*y - 0.5*y*y*y);
        g113 = (enhance*g112n') : (+ ~ *(0.75));
        xe = a1l + g113;
        // The taps were latched: they read the line with the echo time and LFO of the previous frame.
        t105 = (fbl, (echo - 0.00390625)') : itap(32521);                // one fraction step (1/256 frame) short
        t111 = fbr@max(0, min(28560, int(echo')));
        el = xe + feedback*t111;
        er = a1r + feedback*t105;
        g115n = sin_mix*0.7071*t105 + cos_mix*xe;
        g116n = sin_mix*0.7071*t111 + cos_mix*a1r;
        m = (fbl, (mod_center + depth*xc)') : itap(32521);
        peak = max(max3(abs(g115n), abs(g116n), abs(g113)), max(abs(t105), abs(t111)));
    };
};

program(a0l, a0r, a1l, a1r, a2l, a2r) = lfo_c : \(xc, yc, xcn, ycn).(
    (a1l, a1r) : echo_lines(xc) : \(el, er, g115n, g116n, m, echo_peak).(
        mono : tank(ap3_open(m)) : \(wl, wr, tank_peak).(dac_l, dac_r, peak
        with {
            dac_l = a0l + chorus_out*d_l + dry_send*a2l + wl';
            dac_r = a0r + chorus_out*d_r + dry_send*a2r + wr';
            peak = max(tank_peak, echo_peak);
        })
        with {
            d_l = g115n';
            d_r = g116n';
            mono = chorus_to_reverb*(d_l + d_r) + send*(a2l + a2r);
        }));

process = bus(6) : program;
