//! Per-pixel layer visitation masks, mechanically ported from
//! `runtime/renderer/game/layer_visits.hpp`.

use crate::game::LAYER_COUNT;

/// One layer's first/second visit bit pair.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LayerPass {
    pub first_bit: u32,
    pub repeat_bit: u32,
}

/// Bits recording which layers a scanout pixel already saw, and whether they repeat.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LayerVisits {
    bits: u32,
}

impl LayerVisits {
    const REPEAT_SHIFT: u32 = 2 * LAYER_COUNT as u32;

    pub const fn pass(layer: u32, select: bool) -> LayerPass {
        LayerPass {
            first_bit: 1u32 << (layer * 2 + select as u32),
            repeat_bit: 1u32 << (Self::REPEAT_SHIFT + layer),
        }
    }

    pub const fn new(first: LayerPass) -> Self {
        Self {
            bits: first.first_bit,
        }
    }

    pub fn add(&mut self, next: LayerPass) {
        self.bits |= if self.bits & next.first_bit != 0 {
            next.repeat_bit
        } else {
            next.first_bit
        };
    }

    pub const fn saw(&self, layer: u32, select: bool) -> bool {
        (self.bits >> (layer * 2 + select as u32)) & 1 != 0
    }

    pub const fn passes(&self, layer: u32) -> u32 {
        1 + ((self.bits >> (Self::REPEAT_SHIFT + layer)) & 1)
    }

    pub const fn empty(&self) -> bool {
        self.bits == 0
    }

    pub const fn hash(&self) -> u32 {
        self.bits.wrapping_mul(0x9e37_79b1)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn first_visit_sets_bits_and_repeat_sets_repeat() {
        let mut visits = LayerVisits::new(LayerVisits::pass(0, false));
        assert!(visits.saw(0, false));
        assert!(!visits.saw(0, true));
        assert_eq!(visits.passes(0), 1);
        assert!(!visits.empty());

        // Second visit with the same select bit records the repeat instead.
        visits.add(LayerVisits::pass(0, false));
        assert_eq!(visits.passes(0), 2);
        assert!(visits.saw(0, false));

        // The other select bit is a fresh visit.
        visits.add(LayerVisits::pass(0, true));
        assert!(visits.saw(0, true));
        assert_eq!(visits.passes(0), 2);
    }

    #[test]
    fn each_layer_has_its_own_bits() {
        let mut visits = LayerVisits::default();
        assert!(visits.empty());
        for layer in 0..LAYER_COUNT as u32 {
            visits.add(LayerVisits::pass(layer, false));
            assert!(visits.saw(layer, false));
        }
        for layer in 0..LAYER_COUNT as u32 {
            assert_eq!(visits.passes(layer), 1);
            visits.add(LayerVisits::pass(layer, true));
            assert_eq!(visits.passes(layer), 1);
            assert!(visits.saw(layer, true));
        }
    }

    #[test]
    fn hash_is_a_scalar_multiple() {
        let visits = LayerVisits::new(LayerVisits::pass(3, true));
        assert_eq!(visits.hash(), visits.bits.wrapping_mul(0x9e37_79b1));
        assert_eq!(LayerVisits::default().hash(), 0, "empty hashes to zero");
    }
}
