//! Half-open hardware clip-window algebra.

use crate::game::{LAYER_COUNT, LayerId, SceneClip, SceneLayer, SceneRow};

/// The union of clip windows, sorted and merged.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ClipRanges {
    /// Four inside intervals leave at most five gaps. With an outside window,
    /// at most three inside intervals can join the two edge intervals.
    pub ranges: [SceneClip; Self::CAPACITY],
    pub count: usize,
}

impl ClipRanges {
    pub const CAPACITY: usize = 5;

    pub fn as_slice(&self) -> &[SceneClip] {
        &self.ranges[..self.count]
    }

    pub fn iter(&self) -> impl Iterator<Item = &SceneClip> {
        self.as_slice().iter()
    }
}

/// `clip_ranges`: resolve four hardware windows into half-open scanout intervals.
pub fn clip_ranges(
    windows: &[SceneClip; 4],
    enabled: u32,
    inside: u32,
    invert: bool,
    left: i16,
    right: i16,
) -> ClipRanges {
    if left >= right {
        return ClipRanges::default();
    }

    let mut parts = ClipRanges::default();
    if enabled == 0 {
        if invert {
            parts.ranges[0] = SceneClip { left, right };
            parts.count = 1;
        }
        return parts;
    }
    // The union of all selected outsides is the complement of their common inside.
    let mut common_inside = SceneClip { left, right };
    for (n, window) in windows.iter().enumerate() {
        if enabled & (1 << n) == 0 {
            continue;
        }
        let window = SceneClip {
            left: left.max(window.left),
            right: right.min(window.right),
        };
        if inside & (1 << n) != 0 {
            if window.left < window.right {
                parts.ranges[parts.count] = window;
                parts.count += 1;
            }
        } else {
            common_inside.left = common_inside.left.max(window.left);
            common_inside.right = common_inside.right.min(window.right);
        }
    }
    if common_inside.left >= common_inside.right {
        if invert {
            return ClipRanges::default();
        }
        parts.ranges[0] = SceneClip { left, right };
        parts.count = 1;
        return parts;
    }
    if left < common_inside.left {
        parts.ranges[parts.count] = SceneClip {
            left,
            right: common_inside.left,
        };
        parts.count += 1;
    }
    if common_inside.right < right {
        parts.ranges[parts.count] = SceneClip {
            left: common_inside.right,
            right,
        };
        parts.count += 1;
    }

    for i in 1..parts.count {
        let part = parts.ranges[i];
        let mut j = i;
        while j > 0 && parts.ranges[j - 1].left > part.left {
            parts.ranges[j] = parts.ranges[j - 1];
            j -= 1;
        }
        parts.ranges[j] = part;
    }
    let mut visible = ClipRanges::default();
    for part in parts.iter().copied() {
        if visible.count != 0 && part.left <= visible.ranges[visible.count - 1].right {
            let last = &mut visible.ranges[visible.count - 1];
            last.right = last.right.max(part.right);
        } else {
            visible.ranges[visible.count] = part;
            visible.count += 1;
        }
    }
    if !invert {
        return visible;
    }

    let mut gaps = ClipRanges::default();
    let mut left = left;
    for part in visible.iter().copied() {
        if left < part.left {
            gaps.ranges[gaps.count] = SceneClip {
                left,
                right: part.left,
            };
            gaps.count += 1;
        }
        left = part.right;
    }
    if left < right {
        gaps.ranges[gaps.count] = SceneClip { left, right };
        gaps.count += 1;
    }
    gaps
}

/// `clip_ranges`: per-row, per-layer convenience overload.
pub fn clip_ranges_for_row(
    row: &SceneRow,
    layer: &SceneLayer,
    left: i16,
    right: i16,
) -> ClipRanges {
    clip_ranges(
        &row.clips,
        layer.clip_enabled as u32,
        layer.clip_inverted as u32,
        layer.clip_inverse,
        left,
        right,
    )
}

/// Layer draw order: descending priority with a stable tie order.
pub fn scene_order(row: &SceneRow) -> [LayerId; LAYER_COUNT] {
    let mut order = [
        LayerId::Text,
        LayerId::Sp0,
        LayerId::Pf0,
        LayerId::Sp3,
        LayerId::Pf3,
        LayerId::Sp2,
        LayerId::Pf2,
        LayerId::Sp1,
        LayerId::Pf1,
    ];
    for i in 1..order.len() {
        let item = order[i];
        let mut j = i;
        while j > 0 && row.layer(order[j - 1]).priority < row.layer(item).priority {
            order[j] = order[j - 1];
            j -= 1;
        }
        order[j] = item;
    }
    order
}

#[cfg(test)]
mod tests {
    use super::*;

    fn window(left: i16, right: i16) -> SceneClip {
        SceneClip { left, right }
    }

    proptest::proptest! {
        #![proptest_config(proptest::test_runner::Config::with_cases(64))]
        #[test]
        fn intervals_match_pointwise_window_algebra(
            endpoints in proptest::array::uniform4((-80i16..80, -80i16..80)),
            enabled in 0u32..16,
            inside in 0u32..16,
            screen in (-60i16..60, -60i16..60),
        ) {
            use proptest::prelude::*;
            let windows = endpoints.map(|(left, right)| SceneClip { left, right });
            let (left, right) = screen;
            for invert in [false, true] {
                let ranges = clip_ranges(&windows, enabled, inside, invert, left, right);
                prop_assert!(ranges.iter().all(|r| left <= r.left && r.left < r.right && r.right <= right));
                prop_assert!(ranges.as_slice().windows(2).all(|pair| pair[0].right < pair[1].left));
                for x in left..right {
                    let covered = windows.iter().enumerate().any(|(i, w)| {
                        let bit = 1 << i;
                        enabled & bit != 0 && (((w.left..w.right).contains(&x)) == (inside & bit != 0))
                    });
                    prop_assert_eq!(ranges.iter().any(|r| (r.left..r.right).contains(&x)), covered ^ invert);
                }
            }
        }
    }

    #[test]
    fn disabled_windows_are_full_or_empty() {
        let windows = [window(0, 10); 4];
        assert_eq!(clip_ranges(&windows, 0, 0, false, 0, 100).count, 0);
        let inverted = clip_ranges(&windows, 0, 0, true, 0, 100);
        assert_eq!(inverted.count, 1);
        assert_eq!(inverted.ranges[0], window(0, 100));
        assert_eq!(
            clip_ranges(&windows, 0, 0, true, 100, 100).count,
            0,
            "left >= right is empty"
        );
    }

    #[test]
    fn inside_windows_union_and_sort() {
        let windows = [window(50, 60), window(10, 20), window(15, 30), window(0, 0)];
        let parts = clip_ranges(&windows, 0b0111, 0b0111, false, 0, 100);
        assert_eq!(parts.count, 2);
        assert_eq!(parts.ranges[0], window(10, 30));
        assert_eq!(parts.ranges[1], window(50, 60));
    }

    #[test]
    fn outside_windows_leave_edge_intervals() {
        let windows = [window(20, 80), window(40, 100), window(0, 0), window(0, 0)];
        let parts = clip_ranges(&windows, 0b0011, 0, false, 0, 100);
        assert_eq!(parts.count, 2);
        assert_eq!(parts.ranges[0], window(0, 40));
        assert_eq!(parts.ranges[1], window(80, 100));
    }

    #[test]
    fn outside_covering_everything_is_empty_or_full() {
        let windows = [window(-10, 110); 4];
        assert_eq!(clip_ranges(&windows, 0b1111, 0, false, 0, 100).count, 0);
        let inverted = clip_ranges(&windows, 0b1111, 0, true, 0, 100);
        assert_eq!(inverted.count, 1);
        assert_eq!(inverted.ranges[0], window(0, 100));
    }

    #[test]
    fn invert_returns_the_gaps() {
        let windows = [window(20, 40), window(0, 0), window(0, 0), window(0, 0)];
        let parts = clip_ranges(&windows, 0b0001, 0b0001, true, 0, 100);
        assert_eq!(parts.count, 2);
        assert_eq!(parts.ranges[0], window(0, 20));
        assert_eq!(parts.ranges[1], window(40, 100));
    }

    #[test]
    fn scene_order_is_stable_descending_priority() {
        let mut row = SceneRow::default();
        row.layer_mut(LayerId::Pf1).priority = 1;
        row.layer_mut(LayerId::Pf0).priority = 2;
        row.layer_mut(LayerId::Text).priority = 2;
        row.layer_mut(LayerId::Sp3).priority = 3;
        let order = scene_order(&row);
        assert_eq!(
            order,
            [
                LayerId::Sp3,
                LayerId::Text,
                LayerId::Pf0,
                LayerId::Pf1,
                LayerId::Sp0,
                LayerId::Pf3,
                LayerId::Sp2,
                LayerId::Pf2,
                LayerId::Sp1,
            ]
        );
    }

    #[test]
    fn row_helper_uses_layer_fields() {
        let mut row = SceneRow::default();
        row.clips[0] = window(10, 20);
        let layer = SceneLayer {
            clip_enabled: 1,
            clip_inverted: 1,
            clip_inverse: true,
            ..SceneLayer::default()
        };
        let parts = clip_ranges_for_row(&row, &layer, 0, 100);
        assert_eq!(parts.count, 2);
        assert_eq!(parts.ranges[0], window(0, 10));
        assert_eq!(parts.ranges[1], window(20, 100));
    }
}
