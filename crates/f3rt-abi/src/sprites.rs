use f3rt_sprites::enhancements::{
    EnhancementEntry as REnhancementEntry, EnhancementKind as REnhancementKind,
    EnhancementSet as REnhancementSet, VideoBackend as RVideoBackend,
};
use f3rt_sprites::groups::{
    CpuRegs, SpriteGroupReset as RReset, SpriteGroupTable as RTable,
    SpriteGroupTracker as RTracker, SpriteGroupWrite as RWrite,
};
use f3rt_sprites::presentation::{MachineRam, SpriteGroupSlot as RSlot};

#[cxx::bridge(namespace = "f3rt::rust_sprites")]
mod ffi {
    struct SpriteGroupSlot {
        key: u64,
        x: i32,
        y: i32,
        pc: u32,
    }

    struct SpriteGroupWrite {
        pc: u32,
        base: u8,
        lane: u8,
        group_class: u8,
        shift: u8,
        x_offset: u16,
        y_offset: u16,
    }

    struct SpriteGroupReset {
        pc: u32,
        base: u8,
        offset: u16,
    }

    struct WriteEvent {
        address: u32,
        width: u32,
        pc: u32,
        d: [u32; 8],
        a: [u32; 7],
        frame: u64,
    }

    struct FullDetailRecord {
        valid: bool,
        flags0: u8,
        color: u8,
        desc: u16,
        screen_x: i16,
        screen_y: i16,
        zoom_x: u8,
        zoom_y: u8,
    }

    extern "Rust" {
        type SpriteGroupTracker;
        fn new_sprite_group_tracker(
            writes: &[SpriteGroupWrite],
            resets: &[SpriteGroupReset],
        ) -> Box<SpriteGroupTracker>;
        fn groups_active(&self) -> bool;
        fn copy_slots(&self, dst: &mut [SpriteGroupSlot]);
        fn current_root(&self, key: u64, ram: &[u8]) -> SpriteGroupSlot;
        fn note_write(&mut self, event: &WriteEvent, ram: &[u8]);
        fn count_ambiguity(&mut self);
        fn frame_end(&mut self);
        fn reset(&mut self);
        fn format_summary(&self) -> String;
        fn format_report(&self) -> String;
        fn enable_commandw_full_detail(&mut self);
        fn full_detail_record(&self, key: u64, group_x: i32, group_y: i32) -> FullDetailRecord;
    }

    enum EnhancementKind {
        FullDetail,
        FlickerHold,
    }

    extern "Rust" {
        type EnhancementSet;
        fn new_enhancement_set() -> Box<EnhancementSet>;
        fn push_entry(&mut self, name: &str, kind: EnhancementKind);
        fn is_enabled(&self, name: &str, gpu: bool) -> bool;
        fn override_setting(&mut self, setting: &str) -> Result<()>;
        fn set(&mut self, name: &str, enabled: bool) -> Result<()>;
        fn list(&self) -> String;
    }
}

impl From<RSlot> for ffi::SpriteGroupSlot {
    fn from(slot: RSlot) -> Self {
        Self {
            key: slot.key,
            x: slot.x,
            y: slot.y,
            pc: slot.pc,
        }
    }
}

pub struct SpriteGroupTracker(RTracker);

fn new_sprite_group_tracker(
    writes: &[ffi::SpriteGroupWrite],
    resets: &[ffi::SpriteGroupReset],
) -> Box<SpriteGroupTracker> {
    let writes: Vec<_> = writes
        .iter()
        .map(|rule| RWrite {
            pc: rule.pc,
            base: rule.base,
            lane: rule.lane,
            group_class: rule.group_class,
            shift: rule.shift,
            x_offset: rule.x_offset,
            y_offset: rule.y_offset,
        })
        .collect();
    let resets: Vec<_> = resets
        .iter()
        .map(|rule| RReset {
            pc: rule.pc,
            base: rule.base,
            offset: rule.offset,
        })
        .collect();
    Box::new(SpriteGroupTracker(RTracker::new(RTable {
        writes: &writes,
        resets: &resets,
    })))
}

impl SpriteGroupTracker {
    fn groups_active(&self) -> bool {
        self.0.groups_active()
    }

    fn copy_slots(&self, dst: &mut [ffi::SpriteGroupSlot]) {
        for (target, &slot) in dst.iter_mut().zip(self.0.slots()) {
            *target = slot.into();
        }
    }

    fn current_root(&self, key: u64, ram: &[u8]) -> ffi::SpriteGroupSlot {
        self.0.current_root(key, MachineRam::new(ram)).into()
    }

    fn note_write(&mut self, event: &ffi::WriteEvent, ram: &[u8]) {
        let cpu = CpuRegs {
            pc: event.pc,
            d: event.d,
            a: event.a,
        };
        self.0.note_write(
            event.address,
            event.width,
            &cpu,
            MachineRam::new(ram),
            event.frame,
        );
    }

    fn count_ambiguity(&mut self) {
        self.0.count_ambiguity();
    }

    fn frame_end(&mut self) {
        self.0.frame_end();
    }

    fn reset(&mut self) {
        self.0.reset();
    }

    fn format_summary(&self) -> String {
        self.0.report().format_summary()
    }

    fn format_report(&self) -> String {
        self.0.report().format_report()
    }

    fn enable_commandw_full_detail(&mut self) {
        self.0.enable_commandw_full_detail();
    }

    fn full_detail_record(&self, key: u64, group_x: i32, group_y: i32) -> ffi::FullDetailRecord {
        if let Some(record) = self.0.full_detail_record(key, group_x, group_y) {
            ffi::FullDetailRecord {
                valid: true,
                flags0: record.flags0,
                color: record.color,
                desc: record.desc,
                screen_x: record.projected.screen_x,
                screen_y: record.projected.screen_y,
                zoom_x: record.projected.zoom_x,
                zoom_y: record.projected.zoom_y,
            }
        } else {
            ffi::FullDetailRecord {
                valid: false,
                flags0: 0,
                color: 0,
                desc: 0,
                screen_x: 0,
                screen_y: 0,
                zoom_x: 0,
                zoom_y: 0,
            }
        }
    }
}

pub struct EnhancementSet {
    inner: REnhancementSet,
}

fn new_enhancement_set() -> Box<EnhancementSet> {
    Box::new(EnhancementSet {
        inner: REnhancementSet::default(),
    })
}

impl EnhancementSet {
    fn push_entry(&mut self, name: &str, kind: ffi::EnhancementKind) {
        use RVideoBackend::{Cpu, Gpu};
        let (kind, backends) = match kind {
            ffi::EnhancementKind::FullDetail => (REnhancementKind::FullDetail, vec![Cpu, Gpu]),
            _ => (REnhancementKind::FlickerHold, vec![Gpu]),
        };
        self.inner.push_entry(REnhancementEntry {
            name: name.into(),
            kind,
            backends,
            enabled: true,
        });
    }

    fn is_enabled(&self, name: &str, gpu: bool) -> bool {
        let backend = if gpu {
            RVideoBackend::Gpu
        } else {
            RVideoBackend::Cpu
        };
        self.inner.is_enabled(name, backend)
    }

    fn override_setting(&mut self, setting: &str) -> Result<(), String> {
        self.inner.override_setting(setting)
    }

    fn set(&mut self, name: &str, enabled: bool) -> Result<(), String> {
        self.inner.set(name, enabled)
    }

    fn list(&self) -> String {
        self.inner.list()
    }
}
