use std::collections::BTreeMap;

use f3rt_sprites::groups::{CpuRegs, GroupReport, PcCount};
use f3rt_sprites::presentation::{GRAPHICS_BASE, RAM_BASE, SPRITE_RAM_SIZE};
use f3rt_sprites::{
    MachineRam, SpriteGroupReset, SpriteGroupSlot, SpriteGroupTable, SpriteGroupTracker,
    SpriteGroupWrite, SpriteRamWrite,
};
use proptest::prelude::*;

const OBJECTS: [u32; 2] = [RAM_BASE + 0x100, RAM_BASE + 0x180];
const BODY: u32 = 0x100;
const SHADOW: u32 = 0x200;
const UNKNOWN: u32 = 0x300;
const RESET: u32 = 0x400;
const RESET_OTHER: u32 = 0x500;
const SLOTS: [usize; 6] = [0, 1, 2, 0x400, 0x401, 0x7ff];
const WRITES: [SpriteGroupWrite; 2] = [
    SpriteGroupWrite {
        pc: BODY,
        base: 9,
        lane: 3,
        group_class: 1,
        shift: 8,
        x_offset: 0,
        y_offset: 2,
    },
    SpriteGroupWrite {
        pc: SHADOW,
        base: 9,
        lane: 3,
        group_class: 2,
        shift: 4,
        x_offset: 4,
        y_offset: 6,
    },
];
const RESETS: [SpriteGroupReset; 3] = [
    SpriteGroupReset {
        pc: RESET,
        base: 9,
        offset: 0,
    },
    SpriteGroupReset {
        pc: RESET,
        base: 9,
        offset: 2,
    },
    SpriteGroupReset {
        pc: RESET_OTHER,
        base: 9,
        offset: 0,
    },
];

#[derive(Clone, Debug)]
enum Op {
    Ram(usize, usize, i16),
    Sprite(usize, u32, u32, usize, u32),
    ResetWrite(usize, u32, u32, u32, u64),
    Root(usize),
    CountAmbiguity,
    FrameEnd,
    Reset,
}

fn operation() -> impl Strategy<Value = Op> {
    prop_oneof![
        5 => (0usize..2, 0usize..4, any::<i16>()).prop_map(|(o, r, v)| Op::Ram(o, r, v)),
        10 => (0usize..SLOTS.len(), prop_oneof![Just(4), 0u32..16], 1u32..=4, 0usize..3,
               prop_oneof![Just(BODY), Just(SHADOW), Just(UNKNOWN)])
            .prop_map(|(s, b, w, o, pc)| Op::Sprite(s, b, w, o, pc)),
        5 => (0usize..2, 0u32..3, 1u32..=4, prop_oneof![Just(RESET), Just(RESET_OTHER)], 0u64..4)
            .prop_map(|(o, b, w, pc, gap)| Op::ResetWrite(o, b, w, pc, gap)),
        3 => (0usize..32).prop_map(Op::Root),
        2 => Just(Op::CountAmbiguity),
        2 => Just(Op::FrameEnd),
        1 => Just(Op::Reset),
    ]
}

#[derive(Default)]
struct Model {
    roots: [[i16; 4]; 2],
    resets: [[Vec<u64>; 2]; 2],
    slots: BTreeMap<usize, SpriteGroupSlot>,
    bumps_by_pc: BTreeMap<u32, u64>,
}

impl Model {
    fn generation(&self, object: usize, axis: usize) -> u16 {
        if axis >= 2 {
            return 0;
        }
        let frames = &self.resets[object][axis];
        (usize::from(!frames.is_empty())
            + frames
                .windows(2)
                .filter(|pair| pair[1] - pair[0] > 1)
                .count()) as u16
    }

    fn stamp(&self, object: usize, pc: u32) -> SpriteGroupSlot {
        if object >= OBJECTS.len() || pc == UNKNOWN {
            return SpriteGroupSlot::default();
        }
        let (class, x_axis, scale) = if pc == BODY { (1, 0, 256) } else { (2, 2, 16) };
        let key = (u64::from(OBJECTS[object]) << 40)
            | (class << 32)
            | (u64::from(self.generation(object, x_axis)) << 16)
            | u64::from(self.generation(object, x_axis + 1));
        SpriteGroupSlot {
            key,
            x: i32::from(self.roots[object][x_axis]) * scale,
            y: i32::from(self.roots[object][x_axis + 1]) * scale,
            pc,
        }
    }

    fn current_root(&self, saved: SpriteGroupSlot) -> SpriteGroupSlot {
        for object in 0..OBJECTS.len() {
            for pc in [BODY, SHADOW] {
                let current = self.stamp(object, pc);
                if current.key == saved.key {
                    return SpriteGroupSlot { pc: 0, ..current };
                }
            }
        }
        SpriteGroupSlot::default()
    }

    fn count_ambiguity(&self, report: &mut GroupReport) {
        let mut keys = 0;
        let mut slots = 0;
        let mut by_key = BTreeMap::<_, Vec<_>>::new();
        for slot in self.slots.values() {
            by_key.entry(slot.key).or_default().push((slot.x, slot.y));
        }
        for origins in by_key.values() {
            if origins.iter().any(|origin| *origin != origins[0]) {
                keys += 1;
                slots += origins.len() as u64;
            }
        }
        report.ambiguous_keys += keys;
        report.ambiguous_slots += slots;
        report.ambiguous_key_frames += u64::from(keys != 0);
        report.max_ambiguous_keys_frame = report.max_ambiguous_keys_frame.max(keys);
        report.max_ambiguous_slots_frame = report.max_ambiguous_slots_frame.max(slots);
    }
}

fn cpu(pc: u32, object: usize) -> CpuRegs {
    let mut a = [0; 7];
    a[1] = OBJECTS.get(object).copied().unwrap_or(0);
    CpuRegs { pc, a, d: [0; 8] }
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(384))]

    #[test]
    fn tracker_matches_ram_and_slot_history(ops in prop::collection::vec(operation(), 0..100)) {
        let mut tracker = SpriteGroupTracker::new(SpriteGroupTable { writes: &WRITES, resets: &RESETS });
        let mut ram = vec![0; 0x20000];
        let mut model = Model::default();
        let mut report = GroupReport { write_rules: 2, reset_rules: 3, classes: 2, ..GroupReport::default() };
        let mut frame = 0;
        let mut saved = vec![SpriteGroupSlot::default()];
        for (step, op) in ops.iter().enumerate() {
            match *op {
                Op::Ram(object, axis, value) => {
                    model.roots[object][axis] = value;
                    let addr = OBJECTS[object] + axis as u32 * 2;
                    let off = (addr - RAM_BASE) as usize;
                    ram[off..off + 2].copy_from_slice(&value.to_be_bytes());
                    prop_assert_eq!(tracker.note_write(addr, 2, &cpu(UNKNOWN, object), MachineRam::new(&ram), frame), None);
                }
                Op::ResetWrite(object, byte, width, pc, gap) => {
                    frame += gap;
                    for axis in 0..if pc == RESET { 2 } else { 1 } {
                        if !(byte..byte + width).contains(&(axis as u32 * 2)) { continue; }
                        let frames = &mut model.resets[object][axis];
                        if frames.last() == Some(&frame) { continue; }
                        if frames.last().is_some_and(|last| frame - last == 1) {
                            report.periodic_resets += 1;
                        } else {
                            report.generation_bumps += 1;
                            *model.bumps_by_pc.entry(pc).or_default() += 1;
                        }
                        frames.push(frame);
                    }
                    prop_assert_eq!(tracker.note_write(OBJECTS[object] + byte, width, &cpu(pc, object), MachineRam::new(&ram), frame), None);
                }
                Op::Sprite(index, byte, width, object, pc) => {
                    let slot = SLOTS[index];
                    let bank = slot / 0x400;
                    let start = (bank * 0x8000 + (slot % 0x400) * 16) as u32 + byte;
                    let writer = model.stamp(object, pc);
                    saved.push(writer);
                    report.sprite_writes += 1;
                    if writer.key == 0 {
                        if report.unresolved_writes == 0 { report.first_unresolved_pc = pc; }
                        report.unresolved_writes += 1;
                    } else {
                        report.grouped_writes += 1;
                        report.class_writes[if pc == BODY { 1 } else { 2 }] += 1;
                    }
                    for touched in start..start + width {
                        let in_bank = touched % 0x8000;
                        if in_bank >= 0x4000 || touched >= SPRITE_RAM_SIZE { continue; }
                        let slot = (touched / 0x8000 * 0x400 + in_bank / 16) as usize;
                        if writer.key != 0 { model.slots.insert(slot, writer); }
                        else if (4..8).contains(&(in_bank % 16)) { model.slots.remove(&slot); }
                    }
                    prop_assert_eq!(tracker.note_write(GRAPHICS_BASE + start, width, &cpu(pc, object), MachineRam::new(&ram), frame),
                        Some(SpriteRamWrite { start, width }));
                }
                Op::Root(index) => {
                    let stamp = saved[index % saved.len()];
                    prop_assert_eq!(tracker.current_root(stamp.key, MachineRam::new(&ram)), model.current_root(stamp));
                }
                Op::CountAmbiguity => { model.count_ambiguity(&mut report); tracker.count_ambiguity(); }
                Op::FrameEnd => { report.frames += 1; frame += 1; tracker.frame_end(); }
                Op::Reset => { model.slots.clear(); model.resets = Default::default(); tracker.reset(); }
            }
            for (object, base) in OBJECTS.into_iter().enumerate() {
                for axis in 0..4 {
                    prop_assert_eq!(tracker.generation(base + axis as u32 * 2), model.generation(object, axis));
                }
            }
            let actual: BTreeMap<_, _> = tracker.slots().iter().copied().enumerate().filter(|(_, slot)| *slot != SpriteGroupSlot::default()).collect();
            prop_assert_eq!(&actual, &model.slots, "step {}: {:?}", step, op);
            report.top_bump_pcs = model.bumps_by_pc.iter().map(|(&pc, &count)| PcCount { pc, count }).collect();
            report.top_bump_pcs.sort_by_key(|bump| (std::cmp::Reverse(bump.count), bump.pc));
            prop_assert_eq!(tracker.report(), report.clone(), "step {}: {:?}", step, op);
        }
    }
}
