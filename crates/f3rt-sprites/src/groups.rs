use std::collections::{HashMap, HashSet};
use std::fmt::Write as _;

use crate::commandw::{FullDetailCache, FullDetailRecord};
use crate::presentation::{
    MachineRam, SPRITE_SLOTS, SpriteGroupSlot, SpriteRamWrite, group_key_base, group_key_class,
    pack_group_key,
};

const NO_OFFSET: u16 = 0xffff;
const NO_ROOT: u32 = 0xffff_ffff;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SpriteGroupWrite {
    pub pc: u32,
    pub base: u8,
    pub lane: u8,
    pub group_class: u8,
    pub shift: u8,
    pub x_offset: u16,
    pub y_offset: u16,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SpriteGroupReset {
    pub pc: u32,
    pub base: u8,
    pub offset: u16,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct SpriteGroupTable<'a> {
    pub writes: &'a [SpriteGroupWrite],
    pub resets: &'a [SpriteGroupReset],
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ClassInfo {
    pub x_offset: u16,
    pub y_offset: u16,
    pub shift: u8,
    pub valid: bool,
}

impl Default for ClassInfo {
    fn default() -> Self {
        Self {
            x_offset: NO_OFFSET,
            y_offset: NO_OFFSET,
            shift: 8,
            valid: false,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PcCount {
    pub pc: u32,
    pub count: u64,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GroupReport {
    pub frames: u64,
    pub write_rules: usize,
    pub reset_rules: usize,
    pub sprite_writes: u64,
    pub grouped_writes: u64,
    pub unresolved_writes: u64,
    pub first_unresolved_pc: u32,
    pub classes: u64,
    pub class_writes: [u64; 256],
    pub generation_bumps: u64,
    pub periodic_resets: u64,
    pub ambiguous_key_frames: u64,
    pub ambiguous_keys: u64,
    pub ambiguous_slots: u64,
    pub max_ambiguous_keys_frame: u64,
    pub max_ambiguous_slots_frame: u64,
    pub top_bump_pcs: Vec<PcCount>,
}

impl Default for GroupReport {
    fn default() -> Self {
        Self {
            frames: 0,
            write_rules: 0,
            reset_rules: 0,
            sprite_writes: 0,
            grouped_writes: 0,
            unresolved_writes: 0,
            first_unresolved_pc: 0,
            classes: 0,
            class_writes: [0; 256],
            generation_bumps: 0,
            periodic_resets: 0,
            ambiguous_key_frames: 0,
            ambiguous_keys: 0,
            ambiguous_slots: 0,
            max_ambiguous_keys_frame: 0,
            max_ambiguous_slots_frame: 0,
            top_bump_pcs: Vec::new(),
        }
    }
}

impl GroupReport {
    pub fn format_summary(&self) -> String {
        format!(
            "sprite_groups: classes={} frames={}\n",
            self.classes, self.frames
        )
    }

    pub fn format_report(&self) -> String {
        let mut out = String::new();
        let _ = writeln!(
            out,
            "sprite groups: write_rules={} reset_rules={} classes={} frames={} sprite_writes={} grouped_writes={} unresolved_writes={} first_unresolved_pc=0x{:x}",
            self.write_rules,
            self.reset_rules,
            self.classes,
            self.frames,
            self.sprite_writes,
            self.grouped_writes,
            self.unresolved_writes,
            self.first_unresolved_pc
        );
        let _ = writeln!(
            out,
            "  ambiguity: key_frames={} keys={} slots={} max_keys_frame={} max_slots_frame={}",
            self.ambiguous_key_frames,
            self.ambiguous_keys,
            self.ambiguous_slots,
            self.max_ambiguous_keys_frame,
            self.max_ambiguous_slots_frame
        );
        let _ = write!(
            out,
            "  generations: bumps={} periodic_suppressed={} top_pcs:",
            self.generation_bumps, self.periodic_resets
        );
        for pc in &self.top_bump_pcs {
            let _ = write!(out, " 0x{:x}={}", pc.pc, pc.count);
        }
        out.push('\n');
        out.push_str("  class writes:");
        for (cls, &count) in self.class_writes.iter().enumerate().skip(1) {
            if count != 0 {
                let _ = write!(out, " class{cls}={count}");
            }
        }
        out.push('\n');
        out
    }
}

#[derive(Clone, Copy, Debug)]
pub struct CpuRegs {
    pub pc: u32,
    pub d: [u32; 8],
    pub a: [u32; 7],
}

impl CpuRegs {
    pub fn register(&self, reg: u8) -> u32 {
        let idx = usize::from(reg);
        if idx < 8 {
            self.d[idx]
        } else if idx < 15 {
            self.a[idx - 8]
        } else {
            0
        }
    }
}

struct GroupRoots {
    x: u32,
    y: u32,
}

impl ClassInfo {
    fn roots(self, base: u32) -> GroupRoots {
        let address = |offset| {
            if offset == NO_OFFSET {
                NO_ROOT
            } else {
                base.wrapping_add(u32::from(offset)) & 0x00ff_ffff
            }
        };
        GroupRoots {
            x: address(self.x_offset),
            y: address(self.y_offset),
        }
    }
}

impl GroupRoots {
    fn position(&self, ram: MachineRam<'_>, shift: u8) -> Option<[i32; 2]> {
        let x = ram.read_ram16(self.x);
        let y = ram.read_ram16(self.y);
        (x.is_some() || y.is_some()).then(|| {
            [
                i32::from(x.unwrap_or(0)) << shift,
                i32::from(y.unwrap_or(0)) << shift,
            ]
        })
    }
}

pub struct SpriteGroupTracker {
    writes: Vec<SpriteGroupWrite>,
    resets: Vec<SpriteGroupReset>,
    classes: [ClassInfo; 256],
    slots: Box<[SpriteGroupSlot; SPRITE_SLOTS]>,
    generation: HashMap<u32, u16>,
    last_reset: HashMap<u32, u64>,
    bump_pc: HashMap<u32, u64>,
    origin_seen: HashMap<u64, (i32, i32)>,
    ambiguous: HashSet<u64>,
    full_detail: Option<FullDetailCache>,
    report: GroupReport,
}

impl SpriteGroupTracker {
    pub fn new(table: SpriteGroupTable<'_>) -> Self {
        let mut writes = Vec::with_capacity(table.writes.len());
        let mut classes = [ClassInfo::default(); 256];
        for &write in table.writes {
            if write.group_class == 0 {
                continue;
            }
            let shift = if write.shift < 24 { write.shift } else { 8 };
            writes.push(SpriteGroupWrite { shift, ..write });
            let info = &mut classes[usize::from(write.group_class)];
            info.shift = shift;
            if write.x_offset != NO_OFFSET {
                info.x_offset = write.x_offset;
            }
            if write.y_offset != NO_OFFSET {
                info.y_offset = write.y_offset;
            }
            info.valid = true;
        }
        writes.sort_by_key(|rule| rule.pc);

        let mut resets = table.resets.to_vec();
        resets.sort_by_key(|rule| rule.pc);

        let class_count = classes.iter().filter(|info| info.valid).count() as u64;
        let report = GroupReport {
            write_rules: writes.len(),
            reset_rules: resets.len(),
            classes: class_count,
            ..GroupReport::default()
        };

        Self {
            writes,
            resets,
            classes,
            slots: Box::new([SpriteGroupSlot::default(); SPRITE_SLOTS]),
            generation: HashMap::new(),
            last_reset: HashMap::new(),
            bump_pc: HashMap::new(),
            origin_seen: HashMap::new(),
            ambiguous: HashSet::new(),
            full_detail: None,
            report,
        }
    }

    pub fn enable_commandw_full_detail(&mut self) {
        self.full_detail = Some(FullDetailCache::default());
    }

    pub fn full_detail_record(
        &self,
        key: u64,
        group_x: i32,
        group_y: i32,
    ) -> Option<FullDetailRecord> {
        self.full_detail.as_ref()?.lookup(key, group_x, group_y)
    }

    pub fn groups_active(&self) -> bool {
        !self.writes.is_empty()
    }

    pub fn write_rules(&self) -> usize {
        self.writes.len()
    }

    pub fn reset_rules(&self) -> usize {
        self.resets.len()
    }

    pub fn slots(&self) -> &[SpriteGroupSlot] {
        self.slots.as_slice()
    }

    pub fn slot(&self, index: usize) -> SpriteGroupSlot {
        self.slots[index]
    }

    pub fn class_info(&self, group_class: u32) -> ClassInfo {
        self.classes[(group_class & 0xff) as usize]
    }

    pub fn generation(&self, address: u32) -> u16 {
        self.generation
            .get(&(address & 0x00ff_ffff))
            .copied()
            .unwrap_or(0)
    }

    pub fn current_root(&self, key: u64, ram: MachineRam<'_>) -> SpriteGroupSlot {
        let class = self.class_info(group_key_class(key));
        let roots = class.roots(group_key_base(key));
        if self.generation(roots.x) != (key >> 16) as u16 || self.generation(roots.y) != key as u16
        {
            return SpriteGroupSlot::default();
        }
        let Some([x, y]) = roots.position(ram, class.shift) else {
            return SpriteGroupSlot::default();
        };
        SpriteGroupSlot { key, x, y, pc: 0 }
    }

    pub fn resolve_write(&self, cpu: &CpuRegs, ram: MachineRam<'_>) -> SpriteGroupSlot {
        let Some(rule) = self.write_rule(cpu.pc) else {
            return SpriteGroupSlot::default();
        };
        let base = cpu.register(rule.base) & 0x00ff_ffff;
        let roots = self.class_info(u32::from(rule.group_class)).roots(base);
        let Some([x, y]) = roots.position(ram, rule.shift) else {
            return SpriteGroupSlot::default();
        };
        let key = pack_group_key(
            base,
            u32::from(rule.group_class),
            self.generation(roots.x),
            self.generation(roots.y),
        );
        SpriteGroupSlot {
            key,
            x,
            y,
            pc: cpu.pc,
        }
    }

    pub fn note_write(
        &mut self,
        address: u32,
        width: u32,
        cpu: &CpuRegs,
        ram: MachineRam<'_>,
        frame: u64,
    ) -> Option<SpriteRamWrite> {
        if let Some(address) = MachineRam::mapped_address(address) {
            self.note_reset_write(address, width, cpu, frame);
            return None;
        }
        let write = SpriteRamWrite::at_address(address, width)?;
        let writer = self.resolve_write(cpu, ram);
        self.record_sprite_write(writer, cpu.pc, ram, frame);
        write.retag(&mut self.slots, &writer);
        Some(write)
    }

    fn record_sprite_write(
        &mut self,
        writer: SpriteGroupSlot,
        pc: u32,
        ram: MachineRam<'_>,
        frame: u64,
    ) {
        self.report.sprite_writes += 1;
        if writer.key == 0 {
            if self.report.unresolved_writes == 0 {
                self.report.first_unresolved_pc = pc;
            }
            self.report.unresolved_writes += 1;
        } else {
            self.report.grouped_writes += 1;
            self.report.class_writes[group_key_class(writer.key) as usize] += 1;
            if let Some(cache) = &mut self.full_detail {
                cache.record_write(writer, ram.bytes(), frame);
            }
        }
    }

    pub fn count_ambiguity(&mut self) {
        if !self.groups_active() {
            return;
        }
        self.origin_seen.clear();
        self.ambiguous.clear();
        for slot in self.slots.iter() {
            if slot.key == 0 {
                continue;
            }
            let origin = (slot.x, slot.y);
            let first = self.origin_seen.entry(slot.key).or_insert(origin);
            if *first != origin {
                self.ambiguous.insert(slot.key);
            }
        }
        let mut slots = 0u64;
        for slot in self.slots.iter() {
            if slot.key != 0 && self.ambiguous.contains(&slot.key) {
                slots += 1;
            }
        }
        let amb_keys = self.ambiguous.len() as u64;
        self.report.ambiguous_keys += amb_keys;
        self.report.ambiguous_slots += slots;
        if !self.ambiguous.is_empty() {
            self.report.ambiguous_key_frames += 1;
        }
        self.report.max_ambiguous_keys_frame = self.report.max_ambiguous_keys_frame.max(amb_keys);
        self.report.max_ambiguous_slots_frame = self.report.max_ambiguous_slots_frame.max(slots);
    }

    pub fn frame_end(&mut self) {
        self.report.frames += 1;
        if let Some(cache) = self.full_detail.as_mut() {
            cache.frame_end(self.report.frames);
        }
    }

    pub fn reset(&mut self) {
        self.generation.clear();
        self.last_reset.clear();
        self.slots.fill(SpriteGroupSlot::default());
        if let Some(cache) = self.full_detail.as_mut() {
            cache.reset();
        }
    }

    pub fn report(&self) -> GroupReport {
        let mut result = self.report.clone();
        let mut top: Vec<PcCount> = self
            .bump_pc
            .iter()
            .map(|(&pc, &count)| PcCount { pc, count })
            .collect();
        top.sort_by(|a, b| b.count.cmp(&a.count).then_with(|| a.pc.cmp(&b.pc)));
        top.truncate(8);
        result.top_bump_pcs = top;
        result
    }

    fn write_rule(&self, pc: u32) -> Option<&SpriteGroupWrite> {
        let idx = self.writes.partition_point(|rule| rule.pc < pc);
        self.writes.get(idx).filter(|rule| rule.pc == pc)
    }

    fn note_reset_write(&mut self, address: u32, width: u32, cpu: &CpuRegs, frame: u64) {
        if self.resets.is_empty() {
            return;
        }
        let pc = cpu.pc;
        let start = self.resets.partition_point(|rule| rule.pc < pc);
        for rule in &self.resets[start..] {
            if rule.pc != pc {
                break;
            }
            let target = cpu.register(rule.base).wrapping_add(u32::from(rule.offset)) & 0x00ff_ffff;
            if target.wrapping_sub(address) >= width {
                continue;
            }
            match self.last_reset.insert(target, frame) {
                Some(last) if last == frame => {}
                Some(last) if last.checked_add(1) == Some(frame) => {
                    self.report.periodic_resets += 1
                }
                _ => {
                    let generation = self.generation.entry(target).or_default();
                    *generation = generation.wrapping_add(1);
                    self.report.generation_bumps += 1;
                    *self.bump_pc.entry(pc).or_default() += 1;
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::presentation::{GRAPHICS_BASE, RAM_BASE, SPRITE_RAM_SIZE};

    const OBJ_A: u32 = RAM_BASE + 0x200;
    const OBJ_B: u32 = RAM_BASE + 0x280;
    const PC_XY: u32 = 0x300;
    const PC_CLASS2: u32 = 0x308;
    const PC_A4: u32 = 0x30c;
    const PC_RESET: u32 = 0x400;
    const PC_RESET2: u32 = 0x404;
    const PC_TILE: u32 = 0x500;
    const BASE_A3: u8 = 8 + 3;
    const BASE_A4: u8 = 8 + 4;

    fn sample_tracker() -> SpriteGroupTracker {
        let writes = [
            SpriteGroupWrite {
                pc: PC_XY,
                base: BASE_A3,
                lane: 3,
                group_class: 1,
                shift: 8,
                x_offset: 0,
                y_offset: 2,
            },
            SpriteGroupWrite {
                pc: PC_CLASS2,
                base: BASE_A3,
                lane: 3,
                group_class: 2,
                shift: 8,
                x_offset: 0x10,
                y_offset: 0x12,
            },
            SpriteGroupWrite {
                pc: PC_A4,
                base: BASE_A4,
                lane: 3,
                group_class: 1,
                shift: 8,
                x_offset: 0,
                y_offset: 2,
            },
        ];
        let resets = [
            SpriteGroupReset {
                pc: PC_RESET,
                base: BASE_A3,
                offset: 0,
            },
            SpriteGroupReset {
                pc: PC_RESET,
                base: BASE_A3,
                offset: 0x10,
            },
            SpriteGroupReset {
                pc: PC_RESET2,
                base: BASE_A3,
                offset: 0,
            },
        ];
        SpriteGroupTracker::new(SpriteGroupTable {
            writes: &writes,
            resets: &resets,
        })
    }

    fn write_be16(ram: &mut [u8], addr: u32, value: u16) {
        let off = (addr & 0x1ffff) as usize;
        ram[off..off + 2].copy_from_slice(&value.to_be_bytes());
    }

    fn cpu_with(pc: u32) -> CpuRegs {
        let mut a = [0u32; 7];
        a[3] = OBJ_A;
        a[4] = OBJ_B;
        CpuRegs { pc, d: [0; 8], a }
    }

    #[test]
    fn resolves_key_origin_and_periodic_resets() {
        let mut tracker = sample_tracker();
        let mut ram = vec![0u8; 0x20000];
        write_be16(&mut ram, OBJ_A, 0x0100);
        write_be16(&mut ram, OBJ_A + 2, 0x00f0);

        let slot_addr = GRAPHICS_BASE + 4 * 16 + 4;
        assert!(
            tracker
                .note_write(slot_addr, 4, &cpu_with(PC_XY), MachineRam::new(&ram), 1)
                .is_some()
        );
        let first = tracker.slot(4);
        assert_ne!(first.key, 0);
        assert_eq!(first.x, 0x0100 << 8);
        assert_eq!(first.y, 0x00f0 << 8);
        assert_eq!(
            tracker.current_root(first.key, MachineRam::new(&ram)),
            SpriteGroupSlot {
                key: first.key,
                x: first.x,
                y: first.y,
                pc: 0
            }
        );

        tracker.note_write(OBJ_A, 2, &cpu_with(PC_RESET), MachineRam::new(&ram), 1);
        assert_eq!(tracker.report().generation_bumps, 1);
        tracker.note_write(OBJ_A, 2, &cpu_with(PC_RESET), MachineRam::new(&ram), 2);
        assert_eq!(tracker.report().generation_bumps, 1);
        assert_eq!(tracker.report().periodic_resets, 1);
        tracker.note_write(OBJ_A, 2, &cpu_with(PC_RESET), MachineRam::new(&ram), 4);
        tracker.note_write(OBJ_A, 2, &cpu_with(PC_RESET2), MachineRam::new(&ram), 4);
        assert_eq!(tracker.report().generation_bumps, 2);

        tracker.note_write(slot_addr, 2, &cpu_with(PC_TILE), MachineRam::new(&ram), 4);
        assert_eq!(tracker.slot(4), SpriteGroupSlot::default());
    }

    #[test]
    fn note_write_returns_sprite_writes_only() {
        let mut tracker = sample_tracker();
        let ram = vec![0u8; 0x20000];
        let cpu = cpu_with(PC_XY);
        assert_eq!(
            tracker.note_write(OBJ_A, 2, &cpu, MachineRam::new(&ram), 0),
            None
        );
        assert_eq!(
            tracker.note_write(GRAPHICS_BASE + 0x30, 4, &cpu, MachineRam::new(&ram), 0),
            Some(SpriteRamWrite {
                start: 0x30,
                width: 4
            })
        );
        assert_eq!(
            tracker.note_write(
                GRAPHICS_BASE + SPRITE_RAM_SIZE,
                1,
                &cpu,
                MachineRam::new(&ram),
                0
            ),
            None
        );
    }

    mod properties {
        use super::*;
        use proptest::prelude::*;

        proptest! {
            #[test]
            fn reset_sequence_bumps_only_on_non_consecutive_frames(
                gaps in prop::collection::vec(0u64..4, 1..25),
            ) {
                let mut tracker = sample_tracker();
                let ram = vec![0u8; 0x20000];
                let cpu = cpu_with(PC_RESET);
                let mut frame = 1u64;
                let mut prev_frame: Option<u64> = None;
                let mut expected_bumps = 0u64;
                let mut expected_periodic = 0u64;

                for gap in gaps {
                    frame += gap;
                    tracker.note_write(OBJ_A, 2, &cpu, MachineRam::new(&ram), frame);
                    match prev_frame {
                        None => {
                            expected_bumps += 1;
                            prev_frame = Some(frame);
                        }
                        Some(p) if p == frame => {}
                        Some(p) if p + 1 == frame => {
                            expected_periodic += 1;
                            prev_frame = Some(frame);
                        }
                        Some(_) => {
                            expected_bumps += 1;
                            prev_frame = Some(frame);
                        }
                    }
                }
                let report = tracker.report();
                prop_assert_eq!(report.generation_bumps, expected_bumps);
                prop_assert_eq!(report.periodic_resets, expected_periodic);
                prop_assert_eq!(tracker.generation(OBJ_A), expected_bumps as u16);
            }

            #[test]
            fn distinct_origins_under_same_key_trigger_ambiguity(
                x1 in any::<i16>(),
                y1 in any::<i16>(),
                x2 in any::<i16>(),
                y2 in any::<i16>(),
            ) {
                let mut tracker = sample_tracker();
                let mut ram = vec![0u8; 0x20000];
                let cpu = cpu_with(PC_XY);

                write_be16(&mut ram, OBJ_A, x1 as u16);
                write_be16(&mut ram, OBJ_A + 2, y1 as u16);
                tracker.note_write(GRAPHICS_BASE + 16 + 4, 4, &cpu, MachineRam::new(&ram), 1);

                write_be16(&mut ram, OBJ_A, x2 as u16);
                write_be16(&mut ram, OBJ_A + 2, y2 as u16);
                tracker.note_write(GRAPHICS_BASE + 32 + 4, 4, &cpu, MachineRam::new(&ram), 1);

                tracker.count_ambiguity();
                let report = tracker.report();
                let differs = (x1, y1) != (x2, y2);
                prop_assert_eq!(report.ambiguous_keys, u64::from(differs));
                prop_assert_eq!(report.ambiguous_slots, if differs { 2 } else { 0 });
            }
        }
    }
}
