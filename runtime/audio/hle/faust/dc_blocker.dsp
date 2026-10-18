// Output stage: per-channel gain, then a one-pole DC blocker (about 20 Hz):
//   y[n] = x[n] - x[n-1] + pole*y[n-1],   pole = exp(-2*pi*20 / 29762)
// The feedback is flushed to exact zero below 1e-8 (under 0.02 int16 LSB at the output), so a decaying tail
// ends in zeros instead of denormals.

gain_l = hslider("gain_l", 1, 0, 64, 0.0001);
gain_r = hslider("gain_r", 1, 0, 64, 0.0001);
pole   = hslider("r", 0.99578, 0, 1, 0.000001);

flush_tiny(v) = select2(abs(v) < 1e-8, v, 0.0);
dc_block = (_ <: (_, mem)) : (-) : (+ ~ (flush_tiny : *(pole)));

process = (*(gain_l), *(gain_r)) : (dc_block, dc_block);
