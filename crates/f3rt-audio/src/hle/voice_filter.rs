//! `audio/hle/voice_filter.hpp`: ES5505 four-pole voice filter: one voice, one sample per step.
//!
//! Each stage keeps its previous output and reads its input x (and x', the previous input, which is
//! the previous stage's previous output):
//!   low-pass:   y = previous + coef*(x - previous)
//!   high-pass:  y = highpass_pole*previous + (x - x')   (evaluated as (x + pole*previous) - x')
//! A stage picks one equation, so the unused one cannot leak a non-finite value. Every stage
//! updates every sample whatever the mode, so a mode change keeps its state.
//!
//!   mode   stage 1    stage 2    stage 3    stage 4
//!   0      LP coef1   LP coef1   HP         HP
//!   1      LP coef1   LP coef1   LP coef1   HP
//!   2      LP coef1   LP coef1   LP coef2   LP coef2
//!   3      LP coef1   LP coef1   LP coef1   LP coef2
//!
//! Any mode above 3 behaves as mode 3.

#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct VoiceFilterState {
    pub stage1: f32,
    pub stage2: f32,
    pub stage3: f32,
    pub stage4: f32,
}

impl VoiceFilterState {
    pub fn clear(&mut self) {
        *self = Default::default();
    }
}

pub fn filter_mode(register_mode: u8) -> u32 {
    if register_mode < 3 { u32::from(register_mode) } else { 3 }
}

pub fn filter_step<const MODE: u32>(
    s: &mut VoiceFilterState,
    x: f32,
    coef1: f32,
    coef2: f32,
    highpass_pole: f32,
) -> f32 {
    let previous2 = s.stage2;
    let previous3 = s.stage3;
    let previous4 = s.stage4;
    let stage1 = s.stage1 + coef1 * (x - s.stage1);
    let stage2 = previous2 + coef1 * (stage1 - previous2);
    let stage3 = if MODE == 0 {
        stage2 + highpass_pole * previous3 - previous2
    } else {
        previous3 + (if MODE == 2 { coef2 } else { coef1 }) * (stage2 - previous3)
    };
    let stage4 = if MODE < 2 {
        stage3 + highpass_pole * previous4 - previous3
    } else {
        previous4 + coef2 * (stage3 - previous4)
    };
    *s = VoiceFilterState { stage1, stage2, stage3, stage4 };
    stage4
}
