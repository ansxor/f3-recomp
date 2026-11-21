//! Table locations selected by the game manifest and validated before starting the synth.

pub struct DriverLayout {
    pub bend_range_ptr: u32,
    pub rate_table: u32,
    pub phase_rate_table: u32,
    pub pan_table: u32,
    pub attenuation_table: u32,
    pub velocity_curve_table: u32,
    pub filter_table: u32,
    pub pitch_table: u32,
    pub pitch_mod_table: u32,
    pub split_mode_table: u32,
    pub lfo_wave_table: u32,
    pub controller_defaults_table: u32,
    pub heap_base_ptr: u32,
    pub heap_limit_ptr_a: u32,
    pub heap_limit_ptr_b: u32,
    pub heap_reserve_ptr: u32,
    pub heap_next_ptr: u32,
    pub noise_seed_ptr: u32,
    pub noise_history_ptr: u32,
    pub reset_start_ptr: u32,
    pub reset_end_ptr: u32,
    pub budget_table: u32,
    pub split_table_base: u32,
    pub special_split_table: u32,
    pub split_directory: u32,
    pub volume_pan_table_offset: u32,
    pub preset_table_offset: u32,
    pub esp_program_table_offset: u32,
    pub esp_pointer_delta: u32,
}

pub static STANDARD: DriverLayout = DriverLayout {
    bend_range_ptr: 0xc085f2,
    rate_table: 0xc089dc,
    phase_rate_table: 0xc08aa4,
    pan_table: 0xc08b6c,
    attenuation_table: 0xc08bec,
    velocity_curve_table: 0xc08c6c,
    filter_table: 0xc08e6c,
    pitch_table: 0xc09e6c,
    pitch_mod_table: 0xc0a46c,
    split_mode_table: 0xc0a734,
    lfo_wave_table: 0xc0a73c,
    controller_defaults_table: 0xc13556,
    heap_base_ptr: 0xc15fb6,
    heap_limit_ptr_a: 0xc15fba,
    heap_limit_ptr_b: 0xc15fc0,
    heap_reserve_ptr: 0xc15fcc,
    heap_next_ptr: 0xc15fd8,
    noise_seed_ptr: 0xc16f04,
    noise_history_ptr: 0xc16f0a,
    reset_start_ptr: 0xc17b60,
    reset_end_ptr: 0xc17b64,
    budget_table: 0xc1a56e,
    split_table_base: 0xc1ae76,
    special_split_table: 0xc1ae84,
    split_directory: 0xc1c1e0,
    volume_pan_table_offset: 0xa634,
    preset_table_offset: 0x1c2d2,
    esp_program_table_offset: 0x1c3a2,
    esp_pointer_delta: 0x0,
};

// The early reset vector is $c10824. Its note handlers reference the split directory
// and base at $c17c78/$c17c82, and the controller defaults at $c13c6c.
// The effect commands read preset parameters at $c16030 and program pointers at $c19908.
pub static EARLY: DriverLayout = DriverLayout {
    bend_range_ptr: 0xc085a4,
    rate_table: 0xc08966,
    phase_rate_table: 0xc08a2e,
    pan_table: 0xc08af6,
    attenuation_table: 0xc08b76,
    velocity_curve_table: 0xc08bf6,
    filter_table: 0xc08df6,
    pitch_table: 0xc09df6,
    pitch_mod_table: 0xc0a3f6,
    split_mode_table: 0xc0a6be,
    lfo_wave_table: 0xc0a6c6,
    controller_defaults_table: 0xc134ac,
    heap_base_ptr: 0xc15f0c,
    heap_limit_ptr_a: 0xc15f10,
    heap_limit_ptr_b: 0xc15f16,
    heap_reserve_ptr: 0xc15f22,
    heap_next_ptr: 0xc15f2e,
    noise_seed_ptr: 0xc16e9c,
    noise_history_ptr: 0xc16ea2,
    reset_start_ptr: 0xc17af2,
    reset_end_ptr: 0xc17af6,
    budget_table: 0xc1a4ee,
    split_table_base: 0xc1adf6,
    special_split_table: 0xc1ae04,
    split_directory: 0xc1c160,
    volume_pan_table_offset: 0xa5be,
    preset_table_offset: 0x1c252,
    esp_program_table_offset: 0x1c322,
    esp_pointer_delta: 0x80,
};

// Arabian Magic shares the tables before $c10824 and has a six-byte difference
// in the code before the controller defaults. Later references move with those tables.
pub static EARLY_ARABIAN: DriverLayout = DriverLayout {
    controller_defaults_table: 0xc134a6,
    heap_base_ptr: 0xc15f06,
    heap_limit_ptr_a: 0xc15f0a,
    heap_limit_ptr_b: 0xc15f10,
    heap_reserve_ptr: 0xc15f1c,
    heap_next_ptr: 0xc15f28,
    noise_seed_ptr: 0xc16e96,
    noise_history_ptr: 0xc16e9c,
    reset_start_ptr: 0xc17aec,
    reset_end_ptr: 0xc17af0,
    budget_table: 0xc1a4e8,
    split_table_base: 0xc1adf0,
    special_split_table: 0xc1adfe,
    split_directory: 0xc1c15a,
    preset_table_offset: 0x1c24c,
    esp_program_table_offset: 0x1c31c,
    esp_pointer_delta: 0x86,
    ..EARLY
};

impl DriverLayout {
    pub fn select(id: u8, rom: &[u8]) -> Option<&'static Self> {
        // Check the reset vector and the first three ESP pointers at the selected layout's
        // address. The latter distinguishes the early drivers even with the same reset vector.
        let (layout, reset_vector, pointers) = match id {
            0 => (&STANDARD, 0x00c1089eu32, [0xc1c836u32, 0xc1c4e2, 0xc1c4e2]),
            1 => (&EARLY, 0x00c10824, [0xc1c7b6, 0xc1c462, 0xc1c462]),
            2 => (&EARLY_ARABIAN, 0x00c10824, [0xc1c7b0, 0xc1c45c, 0xc1c45c]),
            _ => return None,
        };
        let offset = layout.esp_program_table_offset as usize;
        let signature = pointers.map(u32::to_be_bytes).concat();
        (rom.get(4..8) == Some(&reset_vector.to_be_bytes())
            && rom.get(offset..offset + signature.len()) == Some(signature.as_slice()))
        .then_some(layout)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn selected_layout_requires_its_own_rom_signature() {
        let mut rom = vec![0; 0x80000];
        for (id, vector, offset, pointers) in [
            (0, 0x00c1089eu32, 0x1c3a2, [0xc1c836u32, 0xc1c4e2, 0xc1c4e2]),
            (1, 0x00c10824, 0x1c322, [0xc1c7b6, 0xc1c462, 0xc1c462]),
            (2, 0x00c10824, 0x1c31c, [0xc1c7b0, 0xc1c45c, 0xc1c45c]),
        ] {
            rom.fill(0);
            rom[4..8].copy_from_slice(&vector.to_be_bytes());
            for (slot, pointer) in
                rom[offset..offset + 12].as_chunks_mut::<4>().0.iter_mut().zip(pointers)
            {
                *slot = pointer.to_be_bytes();
            }
            for selected in 0..=3 {
                assert_eq!(DriverLayout::select(selected, &rom).is_some(), selected == id);
            }
            assert!(DriverLayout::select(id, &rom[..offset]).is_none());
        }
    }
}
