//! `audio/hle/program_service.hpp` + `program_service.cpp`: the program table.

use crate::bytes::s8;
use crate::hle::patch::{DirectPatch, DirectRegion, Layer, LayerPatch, Patch, Program};
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
        ProgramQuery { program: 0xffff, key: 0, layer_mask: 0, notes_suppressed: false }
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
        PatchList { patches: [core::ptr::null(); Program::layer_count as usize], count: 0 }
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

/// The only place that knows a program's layout: a layered program yields up to three layer patches
/// in descending layer order, a direct program yields the one key region containing the key.
pub struct ProgramService {
    /// The bank that owns the program bytes; it outlives this service (the C++ reference).
    pub bank: *const SoundBank,
    // PatchList hands out pointers into these tables: reserve the exact size once and never grow them.
    // The C++ vectors hold the concrete subclasses and the list stores `const Patch *` base
    // pointers; the Rust Patch enum owns its subclass, so each entry is the variant of its kind.
    pub layers: Vec<Patch>,
    pub regions: Vec<Patch>,
}

impl ProgramService {
    /// # Safety
    /// `bank` must point to a `SoundBank` that outlives the service (the C++ reference rule).
    pub unsafe fn new(bank: *const SoundBank) -> ProgramService {
        let mut service = ProgramService { bank, layers: Vec::new(), regions: Vec::new() };
        service.layers.reserve(1024 * Program::layer_count as usize);
        service.regions.reserve(1024 * Program::region_count as usize);
        for slot in 0..1024u32 {
            let base = unsafe { (*bank).program_data(slot).base };
            let program = Program::new(base);
            if !base.is_null() && program.enabled() != 0 {
                for layer in 0..Program::layer_count {
                    let view = program.layer(layer);
                    service.layers.push(Patch::Layer(LayerPatch::new(
                        view,
                        bank,
                        base,
                        layer as u8,
                        view.delay_ticks(),
                        view.starts_on_release(),
                    )));
                }
                for region in 0..Program::region_count {
                    service.regions.push(Patch::Direct(DirectPatch::new(
                        DirectRegion::default(),
                        bank,
                        base,
                        region as u8,
                    )));
                }
            } else {
                for layer in 0..Program::layer_count {
                    service.layers.push(Patch::Layer(LayerPatch::new(
                        Layer::default(),
                        bank,
                        base,
                        layer as u8,
                        0,
                        false,
                    )));
                }
                for region in 0..Program::region_count {
                    let view = if !base.is_null() {
                        program.region(region)
                    } else {
                        DirectRegion::default()
                    };
                    service
                        .regions
                        .push(Patch::Direct(DirectPatch::new(view, bank, base, region as u8)));
                }
            }
        }
        service
    }

    pub fn resolve(&self, query: &ProgramQuery) -> PatchList {
        let mut list = PatchList::default();
        if query.program == 0xffff {
            return list;
        }
        let slot = program_slot(query.program);
        let base = unsafe { (*self.bank).program_data(slot).base };
        if base.is_null() {
            return list;
        }
        let program = Program::new(base);
        if program.enabled() == 0 {
            let key = i32::from(s8(u32::from(query.key)));
            for region in 0..Program::region_count {
                let patch = &self.regions[(slot * Program::region_count + region) as usize];
                let direct = match patch {
                    Patch::Direct(direct) => direct,
                    // The region table only holds direct patches.
                    Patch::Layer(_) => continue,
                };
                if !direct.contains_key(key) {
                    continue;
                }
                list.patches[0] = patch as *const Patch;
                list.count = 1;
                return list;
            }
            return list;
        }
        if query.notes_suppressed {
            return list;
        }
        let enabled: u32 = if query.layer_mask != 0 {
            u32::from(query.layer_mask)
        } else {
            u32::from(program.enabled())
        };
        // Descending layer order keeps the voice allocation and the driver start order of the layered onset.
        let mut layer = Program::layer_count;
        while layer > 0 {
            layer -= 1;
            if enabled & (1u32 << layer) == 0 {
                continue;
            }
            list.patches[usize::from(list.count)] =
                &self.layers[(slot * Program::layer_count + layer) as usize] as *const Patch;
            list.count += 1;
        }
        list
    }
}
