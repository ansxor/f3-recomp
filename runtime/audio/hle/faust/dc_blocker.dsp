// Output stage of the HLE effects: output gain, then a one-pole DC blocker (about 20 Hz). There is no analog
// low-pass: the PCB recording keeps its top end up to 14.7 kHz
// (docs/notes/raw/tests/2026-10-16-kirameki-187-clap-dynamics.md).
//
//   y[n] = x[n] - x[n-1] + r*y[n-1],   r = exp(-2 pi 20 / 29761)   (written by Effects as the control "r")
//
// The recursive state is flushed below 1e-8 (the silence threshold of the effects, under 0.02 int16 LSB at the
// output): a decaying tail then ends in exact zeros instead of walking through denormals. Old C++: Effects::dc_block.

gain_l = hslider("gain_l", 1, 0, 64, 0.0001);
gain_r = hslider("gain_r", 1, 0, 64, 0.0001);
r      = hslider("r", 0.99578, 0, 1, 0.000001);

flush(v) = select2(abs(v) < 1e-8, v, 0.0);
dc_block = (_ <: (_, mem)) : (-) : (+ ~ (flush : *(r)));

process = (*(gain_l), *(gain_r)) : (dc_block, dc_block);
