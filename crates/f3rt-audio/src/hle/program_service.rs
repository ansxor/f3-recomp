//! `audio/hle/program_service.hpp` + `program_service.cpp`: the program table.

use std::sync::Arc;

use crate::bytes::s8;
use crate::hle::patch::{DirectPatch, LayerPatch, Patch, Program};
use crate::hle::sound_bank::SoundBank;
use crate::hle::sound_rom_5a7e9117::program_slot;

/// What a note asks the program table for. `key` is the kernel key after the track transpose;
/// `layer_mask` is the track's controller layer mask, zero when the program's own enable mask
/// applies.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ProgramQuery {
    pub program: u16,
    pub key: u8,
    pub layer_mask: u8,
    pub notes_suppressed: bool,
}

impl Default for ProgramQuery {
    fn default() -> ProgramQuery {
        ProgramQuery {
            program: 0xffff,
            key: 0,
            layer_mask: 0,
            notes_suppressed: false,
        }
    }
}

/// The patches one note starts, in voice-allocation order. The pointers are into ProgramService's
/// table and stay valid for the service's lifetime.
#[derive(Clone, Copy)]
pub struct PatchList {
    pub patches: [*const Patch; Program::layer_count as usize],
    pub count: u8,
}

impl Default for PatchList {
    fn default() -> PatchList {
        PatchList {
            patches: [core::ptr::null(); Program::layer_count as usize],
            count: 0,
        }
    }
}

impl PatchList {
    pub fn begin(&self) -> *const *const Patch {
        self.patches.as_ptr()
    }

    pub fn end(&self) -> *const *const Patch {
        unsafe { self.patches.as_ptr().add(usize::from(self.count)) }
    }
}

/// What one program slot holds. Only real records get patches, so every view is backed by ROM bytes.
pub enum SlotPatches {
    Empty,
    Layered {
        program: Program,
        layers: [Patch; Program::layer_count as usize],
    },
    Direct(Box<[Patch; Program::region_count as usize]>),
}

/// The only place that knows a program's layout: a layered program yields up to three layer patches
/// in descending layer order, a direct program yields the one key region containing the key.
pub struct ProgramService {
    // PatchList hands out pointers into this table: it is built once at its final size and never
    // grows.
    pub slots: Vec<SlotPatches>,
}

impl ProgramService {
    pub fn new(bank: Arc<SoundBank>) -> ProgramService {
        let mut slots = Vec::with_capacity(1024);
        for slot in 0..1024u32 {
            let Some(record) = bank.program_data(slot).base else {
                slots.push(SlotPatches::Empty);
                continue;
            };
            let program = Program::new(record);
            slots.push(if program.enabled() != 0 {
                SlotPatches::Layered {
                    program,
                    layers: std::array::from_fn(|layer| {
                        let view = program.layer(layer as u32);
                        Patch::Layer(LayerPatch::new(
                            view,
                            Arc::clone(&bank),
                            program,
                            layer as u8,
                            view.delay_ticks(),
                            view.starts_on_release(),
                        ))
                    }),
                }
            } else {
                SlotPatches::Direct(Box::new(std::array::from_fn(|region| {
                    Patch::Direct(DirectPatch::new(
                        program.region(region as u32),
                        Arc::clone(&bank),
                        program,
                        region as u8,
                    ))
                })))
            });
        }
        ProgramService { slots }
    }

    pub fn resolve(&self, query: &ProgramQuery) -> PatchList {
        let mut list = PatchList::default();
        if query.program == 0xffff {
            return list;
        }
        match &self.slots[program_slot(query.program) as usize] {
            SlotPatches::Empty => {}
            SlotPatches::Direct(regions) => {
                let key = i32::from(s8(u32::from(query.key)));
                let found = regions
                    .iter()
                    .find(|patch| matches!(patch, Patch::Direct(direct) if direct.contains_key(key)));
                if let Some(patch) = found {
                    list.patches[0] = patch as *const Patch;
                    list.count = 1;
                }
            }
            SlotPatches::Layered { program, layers } => {
                if query.notes_suppressed {
                    return list;
                }
                let enabled: u32 = if query.layer_mask != 0 {
                    u32::from(query.layer_mask)
                } else {
                    u32::from(program.enabled())
                };
                // Descending layer order keeps the voice allocation and the driver start order of the
                // layered onset.
                for (layer, patch) in layers.iter().enumerate().rev() {
                    if enabled & (1u32 << layer) == 0 {
                        continue;
                    }
                    list.patches[usize::from(list.count)] = patch as *const Patch;
                    list.count += 1;
                }
            }
        }
        list
    }
}
