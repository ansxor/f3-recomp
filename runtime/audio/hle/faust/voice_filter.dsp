// ES5505 four-pole voice filter, one voice, one sample per step.
//
// Driver inputs, in order: input, a1, a2, high_pole, gain_left, gain_right. Outputs: left, right.
// The driver writes the mode control between compute() calls.
//
// Stage equations. Each stage keeps one state, its previous output s, and reads its input u and
// u' = its previous input. The output is computed from the current input and the previous state:
//   low-pass:  y = s + a * (u - s)
//   high-pass: y = high_pole * s + (u - u')
// select2 picks one equation rather than blending them, so the unselected one cannot leak a
// non-finite value. Every stage updates its state every sample, whatever the mode, so a mode
// change keeps the state.
//
// Stages 1 and 2: low-pass with a1.
// Stage 3: high-pass when mode == 0; low-pass with a2 when mode == 2; otherwise low-pass with a1.
// Stage 4: high-pass when mode < 2; low-pass with a2 otherwise.

declare name "VoiceFilter";
declare description "ES5505 four-pole voice filter with per-voice gain";

mode = nentry("mode", 0, 0, 3, 1);

// One stage. The ~ feedback is the stage's previous output s, one sample delayed.
stage(hp_on, a, p, u) = (\(s).(select2(hp_on, s + a*(u - s), p*s + (u - u')))) ~ _;

lowpass(a, u) = stage(0, a, 0, u);

process(x, a1, a2, p, gl, gr) = y3*gl, y3*gr
with {
    y1 = lowpass(a1, lowpass(a1, x));
    y2 = stage(mode == 0, select2(mode == 2, a1, a2), p, y1);
    y3 = stage(mode < 2, a2, p, y2);
};
