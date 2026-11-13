// Hall: two reverb chains cross-coupled in a figure of eight. Chain X takes the right input, chain Y the left;
// each chain's output is low-passed and fed into the other chain's input.
//
//   in_r ─►(+)─► x_in ─► [chain X] ─► x_late ─► @7   ─► LP ─► (+) of y_in
//   in_l ─►(+)─► y_in ─► [chain Y] ─► y_late ─► @675 ─► LP ─► (+) of x_in
//
//   LP = decay * one-pole(damping) + cross_lowpass2_gain * one-pole(0.875)
//
// One chain:
//   in ─► allpass(diffuser1) ─► allpass(diffuser2) ─► diffused
//   diffused@early_predelay ─► nested_allpass ─► early
//   early@late_predelay     ─► double_nested_allpass(modulated core) ─► late
//
// Both chains share the tank LFO; their modulated taps sit at 611 (X) and 475 (Y) + lfo_depth*lfo_x frames.
import("es5510.lib");

hall_outer_gain     = hslider("hall_a", 0.5, 0, 2, 0.0001);   // outer gain of both nested blocks
diffuser1_gain      = hslider("hall_g", 0.65625, 0, 2, 0.0001);
diffuser2_gain      = hslider("hall_g2", 0.52295, 0, 2, 0.0001);
cross_lowpass2_gain = hslider("hall_g108", 0.04689, 0, 1, 0.00001);

hall_input_smoothing = 0.0078125;   // 1/128

// One chain: in -> (late, early, diffused).
chain(diffuser1_delay, diffuser2_delay, early_predelay, early_outer_delay, early_inner_delay,
      late_outer_delay, late_predelay, late_middle_delay, core) =
    allpass(diffuser1_gain, diffuser1_delay) : allpass(diffuser2_gain, diffuser2_delay) : \(diffused).(
    diffused@early_predelay : nested_allpass(hall_outer_gain, nested_gain, early_outer_delay, early_inner_delay) : \(early).(
    early@late_predelay : double_nested_allpass(hall_outer_gain, nested_gain, late_outer_delay, late_middle_delay, core) : \(late).(
        late, early, diffused)));

// Both chains with their cross-coupling: (in_l, in_r) -> (x_late, y_late, wet_l, wet_r, peak). The ~ loop carries
// x_late and y_late one frame late, so the 7- and 675-frame reads are @6 and @674.
hall_loop(lfo_x, in_l, in_r) = loop ~ bus(2)
with {
    loop(x_late_prev, y_late_prev) = x_in : chain(88, 141, 628, 232, 874, 650, 214, 369, modulated_core(611, lfo_x)) : \(x_late, x_early, x_diffused).(
        y_in : chain(102, 168, 449, 335, 836, 617, 145, 424, modulated_core(475, lfo_x)) : \(y_late, y_early, y_diffused).(
            x_late, y_late, wet_l, wet_r, peak
            with {
                wet_l = 0.125*x_late@3 + 0.4375*x_early@163 + 0.9375*x_diffused@101 - 0.4375*y_early@131;
                wet_r = -0.4375*x_early@74 + 0.125*y_late@225 + 0.4375*y_early@10 + 0.5*y_diffused@1;
                peak = max(max3(abs(wet_l), abs(wet_r), abs(x_to_y)), max3(abs(y_to_x), abs(y_to_x_lowpass2), abs(x_to_y_lowpass2)));
            }))
    with {
        x_to_y = x_late_prev@6;
        y_to_x = y_late_prev@674;
        // Cross-coupling low-passes, each used one frame late.
        y_to_x_lowpass1 = (1 - damping)*y_to_x : (+ ~ *(damping));
        y_to_x_lowpass2 = 0.125*y_to_x : (+ ~ *(0.875));
        x_to_y_lowpass1 = (1 - damping)*x_to_y : (+ ~ *(damping));
        x_to_y_lowpass2 = 0.125*x_to_y : (+ ~ *(0.875));
        x_in = 0.5*in_r + decay*y_to_x_lowpass1' + cross_lowpass2_gain*y_to_x_lowpass2';
        y_in = 0.5*in_l + decay*x_to_y_lowpass1' + cross_lowpass2_gain*x_to_y_lowpass2';
    };
};

program(dry_l, dry_r, fx_l, fx_r, send_l, send_r) = tank_lfo : \(lfo_x, lfo_y, lfo_next_x, lfo_next_y).(
    hall_loop(lfo_x, smoothed_l', smoothed_r') : \(x_late, y_late, wet_l, wet_r, peak).(out_l, out_r, peak
    with {
        out_l = dry_l + fx_direct*fx_l + send_direct*send_l + wet_l';
        out_r = dry_r + fx_direct*fx_r + send_direct*send_r + wet_r';
    })
    with {
        // Each side's reverb input, one-pole smoothed and used one frame late.
        smoothed_l = (1 - hall_input_smoothing)*(fx_to_reverb*fx_l + send_to_reverb*send_l) : (+ ~ *(hall_input_smoothing));
        smoothed_r = (1 - hall_input_smoothing)*(fx_to_reverb*fx_r + send_to_reverb*send_r) : (+ ~ *(hall_input_smoothing));
    });

process = bus(6) : program;
