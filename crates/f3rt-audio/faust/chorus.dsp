// Chorus: the fx bus runs through a 4-tap stereo chorus; its output goes to the DAC and, with the reverb send,
// into the reverb tank.
//
// One delay line per side, each read by two interpolated taps swept ±40 frames by two LFOs. Each line also
// takes `feedback` times a tap of the other line.
//
//   fx_l ─►(+)─► left_line ──┬─ @(70  + 40*lfo1_x) ─► left_tap1  ─┐
//           ▲                └─ @(303 + 40*lfo2_x) ─► left_tap2  ─┴─► chorus_l
//           └── feedback * right_tap1
//   fx_r ─►(+)─► right_line ─┬─ @(261 + 40*lfo1_y) ─► right_tap1 ─┐
//           ▲                └─ @(428 + 40*lfo2_y) ─► right_tap2 ─┴─► chorus_r
//           └── feedback * left_tap2
//
//   chorus_l, chorus_r ' ─► DAC (fx_direct) and tank (fx_to_reverb)
import("es5510.lib");

feedback = hslider("feedback", 0, -0.95, 0.95, 0.0001);
wet_mix  = hslider("sin_mix", 0, 0, 1, 0.0001);
dry_mix  = hslider("cos_mix", 1, 0, 1, 0.0001);

// (fx_l, fx_r) -> (left_line, right_line, chorus_l, chorus_r). Inside the ~ loop the lines arrive one frame
// late, which is the frame a tap read takes.
chorus_lines(lfo1_x, lfo1_y, lfo2_x, lfo2_y) = loop ~ bus(2)
with {
    loop(left_line_prev, right_line_prev, in_l, in_r) = left_line, right_line, chorus_l, chorus_r
    with {
        left_tap1  = (left_line_prev, 70 + 40*lfo1_x) : interpolated_tap(470);
        left_tap2  = (left_line_prev, 303 + 40*lfo2_x) : interpolated_tap(470);
        right_tap1 = (right_line_prev, 261 + 40*lfo1_y) : interpolated_tap(470);
        right_tap2 = (right_line_prev, 428 + 40*lfo2_y) : interpolated_tap(470);
        left_line  = in_l + feedback*right_tap1;
        right_line = in_r + feedback*left_tap2;
        chorus_l = wet_mix*0.7071*(left_tap1 + left_tap2) + dry_mix*in_l;
        chorus_r = wet_mix*0.7071*(right_tap1 + right_tap2) + dry_mix*in_r;
    };
};

program(dry_l, dry_r, fx_l, fx_r, send_l, send_r) = (chorus_lfo1, chorus_lfo2, tank_lfo)
  : \(lfo1_x, lfo1_y, lfo1_next_x, lfo1_next_y, lfo2_x, lfo2_y, lfo2_next_x, lfo2_next_y, tank_x, tank_y, tank_next_x, tank_next_y).(
    (fx_l, fx_r) : chorus_lines(lfo1_x, lfo1_y, lfo2_x, lfo2_y) : \(left_line, right_line, chorus_l, chorus_r).(
        reverb_in : tank(modulated_core(921, tank_x)) : \(wet_l, wet_r, tank_peak).(out_l, out_r, peak
        with {
            out_l = dry_l + fx_direct*chorus_out_l + send_direct*send_l + wet_l';
            out_r = dry_r + fx_direct*chorus_out_r + send_direct*send_r + wet_r';
            peak = max3(tank_peak, abs(chorus_l), abs(chorus_r));
        })
        with {
            chorus_out_l = chorus_l';
            chorus_out_r = chorus_r';
            reverb_in = fx_to_reverb*(chorus_out_l + chorus_out_r) + send_to_reverb*(send_l + send_r);
        }));

process = bus(6) : program;
