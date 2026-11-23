//! Flicker-hold sprite stage, mechanically ported from
//! `runtime/renderer/game/flicker_hold.{hpp,cpp}`.
//!
//! The C++ class inherits `SpriteStage` and reads `SpriteGroupTracker::current_root`; the Rust
//! port keeps the same shape behind a [`SpriteGroups`] trait so it stays testable without the
//! machine.

use std::collections::{HashMap, HashSet};

use crate::decode::{SpriteGroupSlot, group_key_base, group_key_class, pack_group_key};
use crate::game::{MAX_PRESENTED_SPRITES, SceneSprite};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FlickerFollow {
    SelfFollow,
    Anchor,
}

/// `game_config::FlickerHold`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FlickerHoldConfig {
    pub name: String,
    pub group_class: u8,
    pub anchor_class: u8,
    pub period: u8,
    pub follow: FlickerFollow,
}

/// The group tracker lookup the stage needs (`SpriteGroupTracker::current_root`).
pub trait SpriteGroups {
    fn current_root(&self, key: u64) -> SpriteGroupSlot;
}

/// `f3rt::SpriteFrame`.
pub struct SpriteFrame<'a> {
    pub groups: &'a dyn SpriteGroups,
    pub game_frame: u16,
    pub flipped: bool,
}

#[derive(Clone, Debug, Default)]
struct Stats {
    held_frames: u64,
    drops: u64,
    anchor_missing: u64,
}

#[derive(Clone, Debug, Default)]
struct Held {
    game_frame: u16,
    sprites: Vec<SceneSprite>,
    anchor_x: i32,
    anchor_y: i32,
    has_anchor_root: bool,
}

pub struct FlickerHoldStage {
    config: FlickerHoldConfig,
    groups: HashMap<u64, Held>,
    stats: Stats,
    flipped: bool,
}

impl FlickerHoldStage {
    pub fn new(config: FlickerHoldConfig) -> Self {
        Self {
            config,
            groups: HashMap::new(),
            stats: Stats::default(),
            flipped: false,
        }
    }

    pub fn present(&mut self, sprites: &mut Vec<SceneSprite>, frame: &SpriteFrame<'_>) {
        let game_frame = frame.game_frame;
        let flipped = frame.flipped;
        if flipped != self.flipped {
            self.reset();
        }
        self.flipped = flipped;

        let self_follow = self.config.follow == FlickerFollow::SelfFollow;
        let mut anchors: HashMap<u64, usize> = HashMap::new();
        let mut present: HashSet<u64> = HashSet::new();
        for (index, sprite) in sprites.iter_mut().enumerate() {
            if sprite.group == 0 {
                continue;
            }
            let cls = group_key_class(sprite.group);
            if cls == u32::from(self.config.anchor_class) {
                anchors.entry(sprite.group).or_insert(index);
            }
            if cls != u32::from(self.config.group_class) {
                continue;
            }
            let held = self.groups.entry(sprite.group).or_default();
            if present.insert(sprite.group) {
                held.game_frame = game_frame;
                held.sprites.clear();
            }
            sprite.blend_weight = 1.0 / f32::from(self.config.period);
            held.sprites.push(*sprite);
        }

        let anchor_key_for = |key: u64| {
            pack_group_key(
                group_key_base(key),
                u32::from(self.config.anchor_class),
                (key >> 16) as u16,
                key as u16,
            )
        };
        if !self_follow {
            for &key in &present {
                let anchor = anchors.get(&anchor_key_for(key)).copied();
                let held = self.groups.get_mut(&key).unwrap();
                held.has_anchor_root = anchor.is_some();
                if let Some(index) = anchor {
                    held.anchor_x = sprites[index].group_x;
                    held.anchor_y = sprites[index].group_y;
                }
            }
        }

        let mut insertions: Vec<(usize, u64)> = Vec::new();
        let mut extra = 0usize;
        self.groups.retain(|&key, held| {
            if present.contains(&key) {
                return true;
            }
            let anchor = anchors.get(&anchor_key_for(key)).copied();
            if anchor.is_none() {
                self.stats.anchor_missing += 1;
            }
            if anchor.is_none()
                || (!self_follow && !held.has_anchor_root)
                || game_frame.wrapping_sub(held.game_frame) >= u16::from(self.config.period)
                || sprites.len() + extra + held.sprites.len() > MAX_PRESENTED_SPRITES
            {
                self.stats.drops += 1;
                return false;
            }
            insertions.push((anchor.unwrap(), key));
            extra += held.sprites.len();
            true
        });
        if insertions.is_empty() {
            return;
        }

        insertions.sort();
        let mut output = Vec::with_capacity(sprites.len() + extra);
        let mut previous = 0usize;
        let mut drew_held = false;
        for (index, key) in insertions {
            let anchor = sprites[index];
            let current_root = if self_follow {
                frame.groups.current_root(key)
            } else {
                SpriteGroupSlot {
                    key: anchor.group,
                    x: anchor.group_x,
                    y: anchor.group_y,
                    pc: 0,
                }
            };
            if current_root.key == 0 {
                self.stats.drops += 1;
                self.groups.remove(&key);
                continue;
            }
            let held = self.groups.get(&key).unwrap();
            let origin = held.sprites.first().unwrap();
            let x = if self_follow {
                origin.group_x
            } else {
                held.anchor_x
            };
            let y = if self_follow {
                origin.group_y
            } else {
                held.anchor_y
            };
            let dx = current_root.x - x;
            let dy = current_root.y - y;
            output.extend_from_slice(&sprites[previous..index]);
            previous = index;
            for sprite in &held.sprites {
                let mut sprite = *sprite;
                sprite.x += if flipped { -dx } else { dx };
                sprite.y += if flipped { -dy } else { dy };
                sprite.group_x += dx;
                sprite.group_y += dy;
                output.push(sprite);
            }
            drew_held = true;
        }
        if drew_held {
            output.extend_from_slice(&sprites[previous..]);
            *sprites = output;
            self.stats.held_frames += 1;
        }
    }

    pub fn reset(&mut self) {
        self.groups.clear();
    }

    pub fn report(&self) -> String {
        format!(
            "flicker hold {}: held_frames={} drops={} anchor_missing={}\n",
            self.config.name, self.stats.held_frames, self.stats.drops, self.stats.anchor_missing
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::decode::pack_group_key;

    const BASE: u32 = 0x1234;

    struct StubGroups {
        roots: HashMap<u64, SpriteGroupSlot>,
    }

    impl SpriteGroups for StubGroups {
        fn current_root(&self, key: u64) -> SpriteGroupSlot {
            self.roots.get(&key).copied().unwrap_or_default()
        }
    }

    fn config(
        group_class: u8,
        anchor_class: u8,
        period: u8,
        follow: FlickerFollow,
    ) -> FlickerHoldConfig {
        FlickerHoldConfig {
            name: "test".to_string(),
            group_class,
            anchor_class,
            period,
            follow,
        }
    }

    fn shadow() -> SceneSprite {
        SceneSprite {
            x: 20 << 8,
            group: pack_group_key(BASE, 2, 0, 0),
            group_x: 10 << 8,
            ..SceneSprite::default()
        }
    }

    fn body() -> SceneSprite {
        SceneSprite {
            group: pack_group_key(BASE, 1, 0, 0),
            ..SceneSprite::default()
        }
    }

    #[test]
    fn uses_game_frames_and_wraps_the_counter() {
        let mut roots = HashMap::new();
        roots.insert(
            shadow().group,
            SpriteGroupSlot {
                key: shadow().group,
                x: 12 << 8,
                y: 0,
                pc: 0,
            },
        );
        let groups = StubGroups { roots };
        let mut hold = FlickerHoldStage::new(config(2, 1, 2, FlickerFollow::SelfFollow));
        let mut sprites = vec![shadow(), body()];

        hold.present(
            &mut sprites,
            &SpriteFrame {
                groups: &groups,
                game_frame: 0xffff,
                flipped: false,
            },
        );
        assert_eq!(sprites[0].blend_weight, 0.5);
        assert_eq!(sprites[1].blend_weight, 1.0);

        for _ in 0..2 {
            sprites = vec![body()];
            hold.present(
                &mut sprites,
                &SpriteFrame {
                    groups: &groups,
                    game_frame: 0,
                    flipped: false,
                },
            );
            assert_eq!(sprites.len(), 2);
            assert_eq!(sprites[0].group, shadow().group);
            assert_eq!(sprites[0].x, 22 << 8);
            assert_eq!(sprites[0].blend_weight, 0.5);
        }

        sprites = vec![body()];
        hold.present(
            &mut sprites,
            &SpriteFrame {
                groups: &groups,
                game_frame: 1,
                flipped: false,
            },
        );
        assert_eq!(sprites.len(), 1);
        assert_eq!(
            hold.report(),
            "flicker hold test: held_frames=2 drops=1 anchor_missing=0\n"
        );
    }

    #[test]
    fn follows_anchor_and_drops_on_generation_mismatch() {
        let groups = StubGroups {
            roots: HashMap::new(),
        };
        let mut hold = FlickerHoldStage::new(config(2, 1, 2, FlickerFollow::Anchor));
        let mut shadow = shadow();
        shadow.group_x = 0;
        let mut body = body();
        body.group_x = 70 << 8;
        let mut sprites = vec![shadow, body];

        hold.present(
            &mut sprites,
            &SpriteFrame {
                groups: &groups,
                game_frame: 4,
                flipped: false,
            },
        );
        body.group_x += 5 << 8;
        sprites = vec![body];
        hold.present(
            &mut sprites,
            &SpriteFrame {
                groups: &groups,
                game_frame: 5,
                flipped: false,
            },
        );
        assert_eq!(sprites.len(), 2);
        assert_eq!(sprites[0].x, 25 << 8);

        body.group = pack_group_key(BASE, 1, 1, 0);
        sprites = vec![body];
        hold.present(
            &mut sprites,
            &SpriteFrame {
                groups: &groups,
                game_frame: 5,
                flipped: false,
            },
        );
        assert_eq!(sprites.len(), 1);
        assert_eq!(
            hold.report(),
            "flicker hold test: held_frames=1 drops=1 anchor_missing=1\n"
        );
    }

    #[test]
    fn flip_clears_held_groups_and_keeps_stats() {
        let groups = StubGroups {
            roots: HashMap::new(),
        };
        let mut hold = FlickerHoldStage::new(config(2, 1, 3, FlickerFollow::Anchor));
        let shadow = SceneSprite {
            tile: 1,
            group: pack_group_key(BASE, 2, 0, 0),
            ..SceneSprite::default()
        };
        let body = SceneSprite {
            tile: 2,
            group: pack_group_key(BASE, 1, 0, 0),
            ..SceneSprite::default()
        };
        let mut sprites = vec![shadow, body];
        hold.present(
            &mut sprites,
            &SpriteFrame {
                groups: &groups,
                game_frame: 4,
                flipped: false,
            },
        );
        sprites = vec![body];
        hold.present(
            &mut sprites,
            &SpriteFrame {
                groups: &groups,
                game_frame: 5,
                flipped: false,
            },
        );
        assert_eq!(sprites.len(), 2);

        sprites = vec![body];
        hold.present(
            &mut sprites,
            &SpriteFrame {
                groups: &groups,
                game_frame: 6,
                flipped: true,
            },
        );
        assert_eq!(sprites.len(), 1);
        assert_eq!(sprites[0].tile, body.tile);
        assert_eq!(
            hold.report(),
            "flicker hold test: held_frames=1 drops=0 anchor_missing=0\n"
        );
    }
}
