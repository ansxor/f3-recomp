//! ES5505 four-pole voice filter: one voice, one sample per step.
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

    /// Zeroes every stage that decayed into the subnormal range, so a silent voice's filter tail
    /// never makes the arithmetic of later spans run at subnormal speed.
    pub fn flush_subnormals(&mut self) {
        for stage in [&mut self.stage1, &mut self.stage2, &mut self.stage3, &mut self.stage4] {
            if stage.abs() < f32::MIN_POSITIVE {
                *stage = 0.0;
            }
        }
    }

    pub fn step<const MODE: u32>(
        &mut self,
        x: f32,
        coef1: f32,
        coef2: f32,
        highpass_pole: f32,
    ) -> f32 {
        let previous2 = self.stage2;
        let previous3 = self.stage3;
        let previous4 = self.stage4;
        let stage1 = self.stage1 + coef1 * (x - self.stage1);
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
        *self = VoiceFilterState { stage1, stage2, stage3, stage4 };
        stage4
    }
}

pub fn filter_mode(register_mode: u8) -> u32 {
    u32::from(register_mode.min(3))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn state(stage1: f32, stage2: f32, stage3: f32, stage4: f32) -> VoiceFilterState {
        VoiceFilterState { stage1, stage2, stage3, stage4 }
    }

    #[test]
    fn mode_3_is_four_low_passes() {
        let mut filter = VoiceFilterState::default();
        assert_eq!(filter.step::<3>(1.0, 0.5, 0.5, 0.9), 0.0625);
        assert_eq!(filter, state(0.5, 0.25, 0.125, 0.0625));
    }

    #[test]
    fn mode_2_gives_the_last_two_stages_the_second_coefficient() {
        let mut filter = VoiceFilterState::default();
        assert_eq!(filter.step::<2>(1.0, 0.5, 0.25, 0.9), 0.015625);
        assert_eq!(filter, state(0.5, 0.25, 0.0625, 0.015625));
    }

    #[test]
    fn mode_1_ends_in_a_high_pass_that_keeps_its_memory() {
        let mut filter = VoiceFilterState::default();
        assert_eq!(filter.step::<1>(1.0, 0.5, 0.25, 0.5), 0.125);
        assert_eq!(filter.step::<1>(0.0, 0.5, 0.25, 0.5), 0.125);
        assert_eq!(filter, state(0.25, 0.25, 0.1875, 0.125));
    }

    #[test]
    fn mode_0_ends_in_two_high_passes() {
        let mut filter = VoiceFilterState::default();
        assert_eq!(filter.step::<0>(1.0, 0.5, 0.25, 0.5), 0.25);
        assert_eq!(filter.step::<0>(1.0, 0.5, 0.25, 0.5), 0.25);
        assert_eq!(filter, state(0.75, 0.5, 0.375, 0.25));
    }

    #[test]
    fn unused_equation_cannot_leak_a_non_finite_pole() {
        let mut filter = VoiceFilterState::default();
        assert!(filter.step::<3>(1.0, 0.5, 0.5, f32::NAN).is_finite());
    }

    #[test]
    fn flush_subnormals_zeroes_only_subnormal_stages() {
        let mut filter = state(1e-40, -1e-40, 1e-30, -2.0);
        filter.flush_subnormals();
        assert_eq!(filter, state(0.0, 0.0, 1e-30, -2.0));
    }

    #[test]
    fn clear_drops_every_stage() {
        let mut filter = state(1.0, 2.0, 3.0, 4.0);
        filter.clear();
        assert_eq!(filter, VoiceFilterState::default());
    }

    #[test]
    fn register_modes_above_3_behave_as_3() {
        assert_eq!(filter_mode(2), 2);
        assert_eq!(filter_mode(200), 3);
    }
}
