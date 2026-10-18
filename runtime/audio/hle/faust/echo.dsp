// Echo: the fx bus runs through a ping-pong echo; its output goes to the DAC and, with the reverb send, into
// the reverb tank. The tank's innermost loop is the left echo line here, tapped at
// tank_tap_center + lfo_depth*lfo_x frames.
//
//   fx_l ─► enhancer ─► enhanced_l ─►(+)─► left_line ──┬─ @(echo_delay - 1/256) ─► left_echo ─► echo_l
//                                     ▲                └─ @(tank_tap_center + lfo_depth*lfo_x) ─► tank_return
//                                     └── feedback * right_echo
//   fx_r ───────────────────────────►(+)─► right_line ─── @int(echo_delay) ─► right_echo ─► echo_r
//                                     ▲
//                                     └── feedback * left_echo
//
// The left taps are interpolated. The enhancer adds a smoothed, saturated cubic of 16x the left input back onto
// it. The taps and the enhancer use the previous frame's values, hence the ' on the controls they read.
import("es5510.lib");

feedback        = hslider("feedback", 0, -0.95, 0.95, 0.0001);
wet_mix         = hslider("sin_mix", 0, 0, 1, 0.0001);
dry_mix         = hslider("cos_mix", 1, 0, 1, 0.0001);
echo_delay      = hslider("echo", 7168, 0, 28560, 1);                  // frames
tank_tap_center = hslider("mod_center", 14848.49, 0, 32512.49, 0.01);  // frames
enhance_amount  = hslider("enhance", 0.00683594, 0, 1, 0.000001);

// (fx_l, fx_r) -> (left_line, right_line, echo_l, echo_r, tank_return, peak).
echo_lines(lfo_x) = loop ~ bus(2)
with {
    loop(left_line_prev, right_line_prev, in_l, in_r) = left_line, right_line, echo_l, echo_r, tank_return, peak
    with {
        shaped = saturate(4*saturate(4*in_l));
        enhancer_drive = saturate(2*shaped - 0.5*shaped*shaped*shaped);
        enhancer = (enhance_amount*enhancer_drive') : (+ ~ *(0.75));
        enhanced_l = in_l + enhancer;
        left_echo = (left_line_prev, (echo_delay - 0.00390625)') : interpolated_tap(32521);
        right_echo = right_line_prev@max(0, min(28560, int(echo_delay')));
        left_line = enhanced_l + feedback*right_echo;
        right_line = in_r + feedback*left_echo;
        echo_l = wet_mix*0.7071*left_echo + dry_mix*enhanced_l;
        echo_r = wet_mix*0.7071*right_echo + dry_mix*in_r;
        tank_return = (left_line_prev, (tank_tap_center + lfo_depth*lfo_x)') : interpolated_tap(32521);
        peak = max(max3(abs(echo_l), abs(echo_r), abs(enhancer)), max(abs(left_echo), abs(right_echo)));
    };
};

program(dry_l, dry_r, fx_l, fx_r, send_l, send_r) = tank_lfo : \(lfo_x, lfo_y, lfo_next_x, lfo_next_y).(
    (fx_l, fx_r) : echo_lines(lfo_x) : \(left_line, right_line, echo_l, echo_r, tank_return, echo_peak).(
        reverb_in : tank(external_core(tank_return)) : \(wet_l, wet_r, tank_peak).(out_l, out_r, peak
        with {
            out_l = dry_l + fx_direct*echo_out_l + send_direct*send_l + wet_l';
            out_r = dry_r + fx_direct*echo_out_r + send_direct*send_r + wet_r';
            peak = max(tank_peak, echo_peak);
        })
        with {
            echo_out_l = echo_l';
            echo_out_r = echo_r';
            reverb_in = fx_to_reverb*(echo_out_l + echo_out_r) + send_to_reverb*(send_l + send_r);
        }));

process = bus(6) : program;
