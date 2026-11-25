use f3rt_sprites::commandw::*;
use f3rt_sprites::presentation::{RAM_BASE, RAM_MIRROR_MASK, SpriteGroupSlot};
use proptest::prelude::*;

const OBJECT: u32 = RAM_BASE + 0x100;
const OFFSET: usize = 0x100;

fn put16(bytes: &mut [u8], offset: usize, value: i16) {
    bytes[offset..offset + 2].copy_from_slice(&value.to_be_bytes());
}

fn put_ptr(rom: &mut [u8], offset: usize, target: usize) {
    rom[offset..offset + 4].copy_from_slice(&(target as u32).to_be_bytes());
}

fn zoom_before_lod(zoom: u8, lod: u8) -> u8 {
    match lod {
        0 => zoom,
        1 => 128 + zoom / 2,
        2 => 192 + zoom / 4,
        _ => 224 + zoom / 8,
    }
}

fn screen(x: i16, y: i16, zoom_x: u8, zoom_y: u8) -> Option<ProjectedBody> {
    ((0..=512).contains(&x) && (-128..=384).contains(&y)).then_some(ProjectedBody {
        screen_x: x,
        screen_y: y,
        zoom_x,
        zoom_y,
    })
}

#[derive(Clone, Debug)]
enum CacheOp {
    Write(usize, u8, u8, u16, u8),
    Lookup(usize),
    End(u64),
    Reset,
}

fn cache_op() -> impl Strategy<Value = CacheOp> {
    prop_oneof![
        9 => (0usize..8, 0u8..3, 0u8..4, 0u16..9, any::<u8>())
            .prop_map(|(key, pc, lod, desc, color)| CacheOp::Write(key, pc, lod, desc, color)),
        3 => (0usize..8).prop_map(CacheOp::Lookup),
        3 => prop_oneof![0u64..3, Just(4), Just(5)].prop_map(CacheOp::End),
        1 => Just(CacheOp::Reset),
    ]
}

fn cache_slot(id: usize) -> SpriteGroupSlot {
    SpriteGroupSlot {
        key: (u64::from(OBJECT) << 40) | (1 << 32) | (id / 4) as u64,
        x: (id % 2) as i32,
        y: (id / 2 % 2) as i32,
        pc: 0,
    }
}

fn project_3d_model(
    world: [i16; 3],
    camera: [i16; 3],
    rotation: [i16; 2],
    class: i16,
    unclamped: bool,
) -> Option<ProjectedBody> {
    let [x, y, z] = std::array::from_fn(|axis| {
        let world = i32::from(world[axis]);
        let camera = i32::from(camera[axis]);
        (camera + if axis == 0 { -world } else { world }) as i16 as i64
    });
    let [cos, sin] = rotation.map(i64::from);
    let vertical = (cos * z + sin * y) as i32;
    let depth = (sin * z - cos * y) as i32;
    let divisor = (depth.div_euclid(32)) as i16;
    if (depth.div_euclid(128) as i16) < 256 || divisor == 0 {
        return None;
    }
    let negate_quotient = |num: i64| {
        let num = num as i32;
        let quotient = i64::from(num) / i64::from(divisor);
        -i32::from(i16::try_from(quotient).unwrap_or(num as i16))
    };
    let x = (256 + negate_quotient(x * 1024)) as i16;
    let y = (241 + negate_quotient(i64::from(vertical.div_euclid(16)) * 128)) as i16;
    let zoom = negate_quotient(262_144) as u8;
    let zoom = if unclamped || class <= 1 {
        zoom
    } else {
        zoom.min(224)
    };
    screen(x, y, zoom, zoom)
}

#[derive(Clone, Debug)]
struct Part {
    rows: usize,
    cols: usize,
    color_add: u8,
    flip_xor: u8,
    anchor: [i16; 2],
}

fn part() -> impl Strategy<Value = Part> {
    (
        1usize..=5,
        1usize..=5,
        any::<u8>(),
        0u8..4,
        prop::array::uniform2(prop_oneof![Just(-8), Just(4)]),
    )
        .prop_map(|(rows, cols, color_add, flip_xor, anchor)| Part {
            rows,
            cols,
            color_add,
            flip_xor,
            anchor,
        })
}

fn descriptor_rom(parts: &[Part], zoom: [u8; 2]) -> Vec<u8> {
    let mut rom = vec![0; ANCHOR_OFFSET_TABLE_ADDR + 32];
    for dim in 0..5 {
        let table = 0x2000 + dim * 512;
        put_ptr(&mut rom, SIZE_OFFSET_TABLE_ADDR + dim * 4, table);
        for zoom in zoom {
            put16(
                &mut rom,
                table + usize::from(zoom) * 2,
                dim as i16 * 3 + i16::from(zoom) / 8,
            );
        }
    }
    for magnitude in [4, 8] {
        let table = 0x3000 + magnitude * 256;
        put_ptr(&mut rom, ANCHOR_SCALE_TABLE_ADDR + magnitude, table);
        for zoom in zoom {
            rom[table + usize::from(zoom)] = magnitude as u8 + zoom / 8;
        }
    }
    if parts.len() > 1 {
        put_ptr(&mut rom, DESC_TABLE_ADDR + 4, 0x1000);
        rom[0x1000] = parts.len() as u8;
    }
    for (index, part) in parts.iter().enumerate() {
        let desc = if parts.len() > 1 { index + 2 } else { 1 };
        if parts.len() > 1 {
            let part_entry = 0x1002 + index * 4;
            put16(&mut rom, part_entry, desc as i16);
            rom[part_entry + 2] = part.color_add;
            rom[part_entry + 3] = part.flip_xor;
        }
        let grid = 0x4000 + index * 128;
        put_ptr(&mut rom, DESC_TABLE_ADDR + desc * 4, grid);
        put16(&mut rom, grid, part.rows as i16 - 1);
        put16(&mut rom, grid + 2, part.cols as i16 - 1);
        put16(
            &mut rom,
            grid + 4,
            (6 + (part.cols - 1) * part.rows * 2) as i16,
        );
        put16(&mut rom, grid + 6, (6 + part.rows * 2) as i16);
        put16(&mut rom, grid + 8, (6 + part.rows * part.cols * 2) as i16);
        for cell in 0..part.rows * part.cols {
            put16(
                &mut rom,
                grid + 10 + cell * 2,
                (100 * index + cell + 1) as i16,
            );
        }
        put16(
            &mut rom,
            ANCHOR_OFFSET_TABLE_ADDR + desc * 4,
            part.anchor[0],
        );
        put16(
            &mut rom,
            ANCHOR_OFFSET_TABLE_ADDR + desc * 4 + 2,
            part.anchor[1],
        );
    }
    rom
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(512))]

    #[test]
    fn cache_matches_write_lookup_expiry_and_reset_history(ops in prop::collection::vec(cache_op(), 0..100)) {
        let mut cache = FullDetailCache::default();
        let mut ram = vec![0; OFFSET + OBJ_RECORD_SIZE];
        let mut model = [None; 8];
        let mut frame = 0u64;
        for op in ops {
            match op {
                CacheOp::Write(id, pc, lod, desc, color) => {
                    let mut slot = cache_slot(id);
                    slot.pc = [BODY_SINGLE_PC, BODY_MULTI_PC, 0x500][usize::from(pc)];
                    ram[OFFSET + OBJ_FLAGS0_OFF] = 0x80;
                    ram[OFFSET + OBJ_FLAGS1_OFF] = lod << 2;
                    ram[OFFSET + OBJ_COLOR_OFF] = color;
                    ram[OFFSET + OBJ_ZOOM_X_OFF] = color;
                    ram[OFFSET + OBJ_ZOOM_Y_OFF] = !color;
                    put16(&mut ram, OFFSET + OBJ_SCREEN_X_OFF, 120);
                    put16(&mut ram, OFFSET + OBJ_SCREEN_Y_OFF, 96);
                    put16(&mut ram, OFFSET + OBJ_FULL_DETAIL_DESC_OFF, desc as i16);
                    cache.record_write(slot, &ram, frame);
                    if pc != 2 {
                        model[id] = (lod != 0 && desc != 0).then_some((frame, FullDetailRecord {
                            flags0: 0x80, color, desc, projected: screen(120, 96, zoom_before_lod(color, lod), zoom_before_lod(!color, lod)).unwrap(),
                        }));
                    }
                }
                CacheOp::Lookup(id) => {
                    let slot = cache_slot(id);
                    prop_assert_eq!(cache.lookup(slot.key, slot.x, slot.y), model[id].map(|(_, record)| record));
                }
                CacheOp::End(gap) => {
                    frame += gap;
                    cache.frame_end(frame);
                    for entry in &mut model { *entry = entry.filter(|(at, _)| frame - at <= 4); }
                }
                CacheOp::Reset => { cache.reset(); model.fill(None); }
            }
            for (id, expected) in model.iter().enumerate() {
                let slot = cache_slot(id);
                prop_assert_eq!(cache.lookup(slot.key, slot.x, slot.y), expected.map(|(_, record)| record), "after {:?}, key {}", op, id);
            }
        }
    }

    #[test]
    fn projection_matches_2d_unrescale_and_screen_cull(
        xy in (-200i16..=600, -200i16..=410), zoom in any::<[u8; 2]>(), lod in 0u8..4, active in any::<bool>(),
    ) {
        let mut ram = [0; OFFSET + OBJ_RECORD_SIZE];
        ram[OFFSET + OBJ_FLAGS0_OFF] = if active { 0x80 } else { 0 };
        ram[OFFSET + OBJ_FLAGS1_OFF] = lod << 2;
        ram[OFFSET + OBJ_ZOOM_X_OFF] = zoom[0];
        ram[OFFSET + OBJ_ZOOM_Y_OFF] = zoom[1];
        put16(&mut ram, OFFSET + OBJ_SCREEN_X_OFF, xy.0);
        put16(&mut ram, OFFSET + OBJ_SCREEN_Y_OFF, xy.1);
        let expected = active.then(|| screen(xy.0, xy.1, zoom_before_lod(zoom[0], lod), zoom_before_lod(zoom[1], lod))).flatten();
        prop_assert_eq!(project_body(&ram, OBJECT), expected);
    }

    #[test]
    fn projection_matches_3d_rotation_perspective_clamp_and_cull(
        (world, camera, rotation) in prop_oneof![
            4 => ((-160i16..=160, -400i16..=40, -90i16..=90), (-40i16..=40, -20i16..=20, -28i16..=28),
                   prop_oneof![Just([256, 0]), Just([192, 64]), Just([192, -64])])
                .prop_map(|((x,y,z), (cx,cy,cz), rot)| ([x,y,z], [cx,cy,cz], rot)),
            1 => (any::<[i16; 3]>(), any::<[i16; 3]>(), any::<[i16; 2]>()),
        ],
        class in -2i16..=5, unclamped in any::<bool>(),
    ) {
        let mut ram = vec![0; 0xa000];
        ram[OFFSET + OBJ_FLAGS0_OFF] = 0xc0;
        put16(&mut ram, OFFSET + OBJ_CLASS_OFF, class);
        for (off, value) in [OBJ_WORLD_X_OFF, OBJ_WORLD_Y_OFF, OBJ_WORLD_Z_OFF].into_iter().zip(world) {
            put16(&mut ram, OFFSET + off, value);
        }
        for (address, value) in [CAMERA_X_ADDR, CAMERA_Y_ADDR, CAMERA_Z_ADDR, CAMERA_COS_ADDR, CAMERA_SIN_ADDR]
            .into_iter().zip(camera.into_iter().chain(rotation)) {
            put16(&mut ram, (address & RAM_MIRROR_MASK) as usize, value);
        }
        ram[(ZOOM_CLAMP_DISABLE_ADDR & RAM_MIRROR_MASK) as usize] = u8::from(unclamped);
        prop_assert_eq!(project_body(&ram, OBJECT), project_3d_model(world, camera, rotation, class, unclamped));
    }

    #[test]
    fn emitter_matches_grid_model_for_single_and_composite_descriptors(
        parts in prop::collection::vec(part(), 1..5),
        zoom in prop::array::uniform2(prop_oneof![Just(0), any::<u8>()]),
        screen in prop::array::uniform2(-130i16..=450), color in any::<u8>(),
    ) {
        let rom = descriptor_rom(&parts, zoom);
        let key = 0x4100000100010002;
        let mut out = Vec::new();
        for flip in 0..4 {
            for anchor in [false, true] {
                out.clear();
                let flags0 = 0x80 | flip | if anchor { 8 } else { 0 };
                let projected = ProjectedBody { screen_x: screen[0], screen_y: screen[1], zoom_x: zoom[0], zoom_y: zoom[1] };
                emit_full_detail_record(&rom, FullDetailRecord { flags0, color, desc: 1, projected }, key, &mut out).unwrap();
                prop_assert_eq!(out.len(), parts.iter().map(|part| part.rows * part.cols).sum::<usize>());
                let mut entries = out.iter();
                for (index, part) in parts.iter().enumerate() {
                    let single = parts.len() == 1;
                    let final_flip = flip ^ if single { 0 } else { part.flip_xor };
                    let head_group = SpriteGroupSlot { key, x: i32::from(screen[0]) * 256, y: i32::from(screen[1]) * 256,
                        pc: if single { BODY_SINGLE_PC } else { BODY_MULTI_PC } };
                    let dims = [part.cols, part.rows];
                    let pos: [u16; 2] = std::array::from_fn(|axis| {
                        let size_offset = (dims[axis] - 1) as i16 * 3 + i16::from(zoom[axis]) / 8;
                        let sign = if final_flip & (1 << axis) == 0 { -1 } else { 1 };
                        let mut offset = part.anchor[axis];
                        if zoom[axis] != 0 { offset += offset.signum() * i16::from(zoom[axis] / 8); }
                        (screen[axis] - size_offset + if anchor { sign * offset } else { 0 }).rem_euclid(4096) as u16
                    });
                    for col in 0..part.cols {
                        for row in 0..part.rows {
                            let source_col = if final_flip & 1 == 0 { col } else { part.cols - 1 - col };
                            let source_row = if final_flip & 2 == 0 { row } else { part.rows - 1 - row };
                            let tile = (index * 100 + source_col * part.rows + source_row + 1) as u16;
                            let first = col == 0 && row == 0;
                            let last = col + 1 == part.cols && row + 1 == part.rows;
                            let chain = if last { 0 } else { 0x0800 };
                            let control = (u16::from(final_flip) << 8) | chain;
                            let (expected, expected_group) = if first {
                                let color = if single { color } else { color.wrapping_add(part.color_add) };
                                ([tile, u16::from_be_bytes([zoom[1], zoom[0]]), pos[0], pos[1], control | u16::from(color), 0, 0, 0], head_group)
                            } else {
                                let advance = if row == 0 { 0xc400 } else { 0x7400 };
                                ([tile, 0, 0, 0, control | advance, 0, 0, 0], SpriteGroupSlot::default())
                            };
                            let entry = entries.next().unwrap();
                            let actual = std::array::from_fn(|word| u16::from_be_bytes([entry.bytes[word * 2], entry.bytes[word * 2 + 1]]));
                            prop_assert_eq!(actual, expected);
                            prop_assert_eq!(entry.group, expected_group);
                        }
                    }
                }
            }
        }
    }
}
