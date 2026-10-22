// Reverb: the fx bus and the reverb send feed the mono reverb tank; both also go straight to the DAC.
//
//   dry  ──────────────────────────────────────────────────────────┐
//   fx   ─┬─ fx_direct ────────────────────────────────────────────┤
//         └─ fx_to_reverb ───┐                                     ├─► out
//   send ─┬─ send_direct ────┼─────────────────────────────────────┤
//         └─ send_to_reverb ─┴─► (L+R) ─► tank ─► wet_l, wet_r ' ──┘
//
// The tank's innermost loop taps its own node at 921 + lfo_depth*lfo_x frames.
import("es5510.lib");

program(dry_l, dry_r, fx_l, fx_r, send_l, send_r) = tank_lfo : \(lfo_x, lfo_y, lfo_next_x, lfo_next_y).(
    reverb_in : tank(modulated_core(921, lfo_x)) : \(wet_l, wet_r, peak).(out_l, out_r, peak
    with {
        // The wet signal reaches the DAC one frame late.
        out_l = dry_l + fx_direct*fx_l + send_direct*send_l + wet_l';
        out_r = dry_r + fx_direct*fx_r + send_direct*send_r + wet_r';
    })
    with {
        reverb_in = fx_to_reverb*(fx_l + fx_r) + send_to_reverb*(send_l + send_r);
    });

process = bus(6) : program;
