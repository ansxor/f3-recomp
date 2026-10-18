// ES5510 programs 1 and 2 "Hall" (ROM pointer 0xc1c4e2; Kirameki presets 0 and 2).
//
// Two cross-coupled stereo-in / stereo-out reverb chains, a figure of eight: chain X takes the right input,
// chain Y the left input; each chain's output node (o2) is low-passed and injected into the other chain's input.
//
//   per chain:   v ─► AP(g, dd1) ─► AP(g2, dd2) ─► wo ─ @dp ─► block B ─► bout ─ @db ─► block A ─► o2
//                                                                  AP(a)[AP(.25)[dr] dq]       AP(a)[AP(.25)[AP(.25)[mod] dc] da]
//                (C++ HallChain: wd, w1, wo | bq, br, bout | an, cn, o2, o4; the innermost node of block A is o4)
//
//   v_x = .5*hu_r + decay*hlp_x1 + hall_g108*hlp_x2        hlp_x1 = LP(1 - damp, damp) of w44 = Y.o2@675
//   v_y = .5*hu_l + decay*hlp_y1 + hall_g108*hlp_y2        hlp_y1 = LP(1 - damp, damp) of w13 = X.o2@7
//                                                          hlp_x2 / hlp_y2 = LP(.125, .875) of w44 / w13
//   hu_l, hu_r = leak*hu + (1 - leak)*(chorus_to_reverb*a1 + send*a2), leak = 1/128  (latched)
//
//         ┌────────────── X.o2@7 ─► LP ─► LP2 ───────────────────────────┐
//         ▼                                                              │ (a figure of eight)
//   hu_r ─► v_x ─► [chain X: 88/141 | 628 | 232/874 | 650/369/214] ─► o2_x ─┘
//   hu_l ─► v_y ─► [chain Y: 102/168 | 449 | 335/836 | 617/424/145] ─► o2_y ──► Y.o2@675 ─► LP ─► LP2 ─► v_x
//
//   wet_l (G89) = .125*X.o2@3 + .4375*X.bout@163 + .9375*X.wo@101 - .4375*Y.bout@131
//   wet_r (G90) = -.4375*X.bout@74 + .125*Y.o2@225 + .4375*Y.bout@10 + .5*Y.wo@1          both latched
//
// The modulated tap of block A reads o4 at 611 + depth*xc (X) and 475 + depth*xc (Y) frames, xc the LFO C value of
// the previous frame (C++ m_hmod_x / m_hmod_y, latched).
//
// Old C++: Effects::frame_hall(); m_hall_x / m_hall_y (HallChain), m_hu_l / m_hu_r, m_hlp_*, m_hmod_*, m_g89 / m_g90.
import("es5510.lib");

hall_a    = hslider("hall_a", 0.5, 0, 2, 0.0001);        // a = G5 = (p0 + 7)/128, the outer all-pass of blocks A and B
hall_g    = hslider("hall_g", 0.65625, 0, 2, 0.0001);    // g = G8 = p7/128, first diffuser
hall_g2   = hslider("hall_g2", 0.52295, 0, 2, 0.0001);   // g2 = G17 = .796875 g, second diffuser
hall_g108 = hslider("hall_g108", 0.04689, 0, 1, 0.00001); // G108 = p1/128

// One chain: v -> (o2, bout, wo). mod: the modulated tap delay of block A (frames).
chain(dd1, dd2, dp, dq, dr, da, db, dc, N3) = ap1(hall_g, dd1) : ap1(hall_g2, dd2) : \(wo).(
    wo@dp : ap2(hall_a, inner_gain, dq, dr) : \(bout).(
        bout@db : ap3(hall_a, inner_gain, da, dc, N3) : \(o2).(o2, bout, wo)));

// Both chains and their cross-coupling: (hu_l, hu_r) -> (o2_x, o2_y, wet_l, wet_r, peak), no inputs. The loop
// carries o2_x and o2_y one frame late, which is part of the @7 and @675 reads (@6 and @674 behind the ~ delay).
// xc is the LFO C value of the current frame (see ap3_tap).
hall_loop(xc, hu_l, hu_r) = loop ~ bus(2)
with {
    loop(fx, fy) = vx : chain(88, 141, 628, 232, 874, 650, 214, 369, ap3_tap(611, xc)) : \(o2x, boutx, wox).(
        vy : chain(102, 168, 449, 335, 836, 617, 145, 424, ap3_tap(475, xc)) : \(o2y, bouty, woy).(
            o2x, o2y, wet_l, wet_r, peak
            with {
                // C++ m_g89 / m_g90 before their latch.
                wet_l = 0.125*o2x@3 + 0.4375*boutx@163 + 0.9375*wox@101 - 0.4375*bouty@131;
                wet_r = -0.4375*boutx@74 + 0.125*o2y@225 + 0.4375*bouty@10 + 0.5*woy@1;
                peak = max(max3(abs(wet_l), abs(wet_r), abs(w13)), max3(abs(w44), abs(lpx2), abs(lpy2)));
            }))
    with {
        w13 = fx@6;                                   // X.o2@7
        w44 = fy@674;                                 // Y.o2@675
        // Low-passes of the cross-coupling, latched for the next frame (C++ m_hlp_y1/2 of w13, m_hlp_x1/2 of w44).
        lpx1 = (1 - damp)*w44 : (+ ~ *(damp));
        lpx2 = 0.125*w44 : (+ ~ *(0.875));
        lpy1 = (1 - damp)*w13 : (+ ~ *(damp));
        lpy2 = 0.125*w13 : (+ ~ *(0.875));
        vx = 0.5*hu_r + decay*lpx1' + hall_g108*lpx2';
        vy = 0.5*hu_l + decay*lpy1' + hall_g108*lpy2';
    };
};

hall_leak = 0.0078125;   // 1/128

program(a0l, a0r, a1l, a1r, a2l, a2r) = lfo_c : \(xc, yc, xcn, ycn).(
    hall_loop(xc, ul', ur') : \(o2x, o2y, wl, wr, pk).(dac_l, dac_r, pk
    with {
        dac_l = a0l + chorus_out*a1l + dry_send*a2l + wl';
        dac_r = a0r + chorus_out*a1r + dry_send*a2r + wr';
    })
    with {
        // Inputs per side: hu = leak*hu' + (1 - leak)*(chorus_to_reverb*a1 + send*a2), latched (m_hu_l / m_hu_r).
        ul = (1 - hall_leak)*(chorus_to_reverb*a1l + send*a2l) : (+ ~ *(hall_leak));
        ur = (1 - hall_leak)*(chorus_to_reverb*a1r + send*a2r) : (+ ~ *(hall_leak));
    });

process = bus(6) : program;
