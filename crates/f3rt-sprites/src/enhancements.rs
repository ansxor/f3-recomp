use std::fmt::Write as _;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VideoBackend {
    Cpu,
    Gpu,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EnhancementKind {
    FullDetail,
    FlickerHold,
}

impl EnhancementKind {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::FullDetail => "full_detail",
            Self::FlickerHold => "flicker_hold",
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EnhancementEntry {
    pub name: String,
    pub kind: EnhancementKind,
    pub backends: Vec<VideoBackend>,
    pub enabled: bool,
}

#[derive(Clone, Debug, Default)]
pub struct EnhancementSet {
    entries: Vec<EnhancementEntry>,
}

impl EnhancementSet {
    pub fn new(entries: Vec<EnhancementEntry>) -> Self {
        Self { entries }
    }

    pub fn push_entry(&mut self, entry: EnhancementEntry) {
        self.entries.push(entry);
    }

    pub fn entries(&self) -> &[EnhancementEntry] {
        &self.entries
    }

    pub fn override_setting(&mut self, setting: &str) -> Result<(), String> {
        let Some((name, value)) = setting.split_once('=') else {
            return Err("--enhance expects NAME=on|off".to_string());
        };
        let enabled = match value {
            "on" => true,
            "off" => false,
            _ => return Err("--enhance expects NAME=on|off".to_string()),
        };
        self.set(name, enabled)
    }

    pub fn set(&mut self, name: &str, enabled: bool) -> Result<(), String> {
        let Some(entry) = self.entries.iter_mut().find(|entry| entry.name == name) else {
            return Err(format!("Unknown enhancement '{name}'"));
        };
        entry.enabled = enabled;
        Ok(())
    }

    pub fn is_enabled(&self, name: &str, backend: VideoBackend) -> bool {
        self.entries
            .iter()
            .find(|entry| entry.name == name)
            .is_some_and(|entry| entry.enabled && entry.backends.contains(&backend))
    }

    pub fn list(&self) -> String {
        let mut out = String::new();
        for entry in &self.entries {
            let _ = writeln!(out, "{} {}", entry.kind.as_str(), entry.name);
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample_set() -> EnhancementSet {
        EnhancementSet::new(vec![
            EnhancementEntry {
                name: "full-detail".to_string(),
                kind: EnhancementKind::FullDetail,
                backends: vec![VideoBackend::Cpu, VideoBackend::Gpu],
                enabled: true,
            },
            EnhancementEntry {
                name: "object-shadow".to_string(),
                kind: EnhancementKind::FlickerHold,
                backends: vec![VideoBackend::Gpu],
                enabled: true,
            },
        ])
    }

    #[test]
    fn overrides_toggle_entries_and_reject_invalid_syntax() {
        let mut set = sample_set();
        assert!(set.is_enabled("full-detail", VideoBackend::Cpu));
        assert!(set.is_enabled("full-detail", VideoBackend::Gpu));
        assert!(!set.is_enabled("object-shadow", VideoBackend::Cpu));
        assert!(set.is_enabled("object-shadow", VideoBackend::Gpu));

        set.override_setting("full-detail=off").unwrap();
        assert!(!set.is_enabled("full-detail", VideoBackend::Gpu));
        set.override_setting("full-detail=on").unwrap();
        assert!(set.is_enabled("full-detail", VideoBackend::Gpu));

        assert_eq!(
            set.override_setting("invalid"),
            Err("--enhance expects NAME=on|off".to_string())
        );
        assert_eq!(
            set.override_setting("=on"),
            Err("Unknown enhancement ''".to_string())
        );
    }

    #[test]
    fn list_prints_kind_and_name() {
        let set = sample_set();
        assert_eq!(
            set.list(),
            "full_detail full-detail\nflicker_hold object-shadow\n"
        );
    }

    mod properties {
        use super::*;
        use proptest::prelude::*;

        #[derive(Clone, Debug)]
        enum Op {
            Set(usize, bool),
            Override(usize, bool),
            Malformed(usize, &'static str),
            Query(usize, bool),
            List,
        }

        const NAMES: [&str; 6] = ["detail", "shadow", "", " Detail", "shadow ", "absent"];

        fn operation() -> impl Strategy<Value = Op> {
            prop_oneof![
                (0usize..6, any::<bool>()).prop_map(|(name, on)| Op::Set(name, on)),
                (0usize..6, any::<bool>()).prop_map(|(name, on)| Op::Override(name, on)),
                (
                    0usize..6,
                    prop::sample::select(&["", "=ON", "=on=off", "= on", "="][..])
                )
                    .prop_map(|(name, suffix)| Op::Malformed(name, suffix)),
                (0usize..6, any::<bool>()).prop_map(|(name, gpu)| Op::Query(name, gpu)),
                Just(Op::List),
            ]
        }

        proptest! {
            #![proptest_config(ProptestConfig::with_cases(384))]

            #[test]
            fn enhancement_table_matches_first_entry_and_override_history(
                table in prop::collection::vec((0usize..5, any::<bool>(), 0u8..4, any::<bool>()), 0..16),
                ops in prop::collection::vec(operation(), 0..70),
            ) {
                use std::collections::BTreeMap;

                let mut model = BTreeMap::new();
                let mut listing = String::new();
                let entries = table.into_iter().map(|(id, full, mask, enabled)| {
                    model.entry(id).or_insert((mask, enabled));
                    listing.push_str(&format!("{} {}\n", if full { "full_detail" } else { "flicker_hold" }, NAMES[id]));
                    EnhancementEntry {
                        name: NAMES[id].into(),
                        kind: if full { EnhancementKind::FullDetail } else { EnhancementKind::FlickerHold },
                        backends: [VideoBackend::Cpu, VideoBackend::Gpu].into_iter().enumerate()
                            .filter_map(|(bit, backend)| (mask & (1 << bit) != 0).then_some(backend)).collect(),
                        enabled,
                    }
                }).collect();
                let mut set = EnhancementSet::new(entries);
                for op in ops {
                    match op {
                        Op::Set(id, on) | Op::Override(id, on) => {
                            let name = NAMES[id];
                            let result = if matches!(op, Op::Set(..)) {
                                set.set(name, on)
                            } else {
                                set.override_setting(&format!("{name}={}", if on { "on" } else { "off" }))
                            };
                            let expected = model.get_mut(&id).map(|(_, enabled)| *enabled = on)
                                .ok_or_else(|| format!("Unknown enhancement '{name}'"));
                            prop_assert_eq!(result, expected);
                        }
                        Op::Malformed(id, suffix) => {
                            prop_assert_eq!(set.override_setting(&format!("{}{suffix}", NAMES[id])),
                                Err("--enhance expects NAME=on|off".into()));
                        }
                        Op::Query(id, gpu) => {
                            let expected = model.get(&id).is_some_and(|&(mask, on)| on && mask & (1 << u8::from(gpu)) != 0);
                            let backend = if gpu { VideoBackend::Gpu } else { VideoBackend::Cpu };
                            prop_assert_eq!(set.is_enabled(NAMES[id], backend), expected);
                        }
                        Op::List => prop_assert_eq!(set.list(), listing.as_str()),
                    }
                    for (id, name) in NAMES.into_iter().enumerate() {
                        let (mask, enabled) = model.get(&id).copied().unwrap_or((0, false));
                        prop_assert_eq!(set.is_enabled(name, VideoBackend::Cpu), enabled && (mask & 1 != 0));
                        prop_assert_eq!(set.is_enabled(name, VideoBackend::Gpu), enabled && (mask & 2 != 0));
                    }
                }
                prop_assert_eq!(set.list(), listing);
            }
        }
    }
}
