// ES5505 four-pole voice filter: one voice, one sample per step.
//
// Each stage keeps its previous output and reads its input x (and x', the previous input):
//   low-pass:   y = previous + coef*(x - previous)
//   high-pass:  y = highpass_pole*previous + (x - x')
// select2 picks one equation instead of blending them, so the unused one cannot leak a non-finite value. Every
// stage updates every sample whatever the mode, so a mode change keeps its state.
//
//   mode   stage 1    stage 2    stage 3    stage 4
//   0      LP coef1   LP coef1   HP         HP
//   1      LP coef1   LP coef1   LP coef1   HP
//   2      LP coef1   LP coef1   LP coef2   LP coef2
//   3      LP coef1   LP coef1   LP coef1   LP coef2
//
// The host writes `mode` between compute() calls.

declare name "VoiceFilter";
declare description "ES5505 four-pole voice filter with per-voice gain";

mode = nentry("mode", 0, 0, 3, 1);

// One stage; the ~ feedback is the stage's previous output.
stage(highpass, coef, highpass_pole, x) =
    (\(previous).(select2(highpass, previous + coef*(x - previous), highpass_pole*previous + (x - x')))) ~ _;

lowpass(coef, x) = stage(0, coef, 0, x);

process(x, lowpass_coef1, lowpass_coef2, highpass_pole, gain_l, gain_r) = stage4*gain_l, stage4*gain_r
with {
    stage2 = lowpass(lowpass_coef1, lowpass(lowpass_coef1, x));
    stage3 = stage(mode == 0, select2(mode == 2, lowpass_coef1, lowpass_coef2), highpass_pole, stage2);
    stage4 = stage(mode < 2, lowpass_coef2, highpass_pole, stage3);
};
