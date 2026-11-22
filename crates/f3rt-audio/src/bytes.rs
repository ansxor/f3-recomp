//! Unaligned big-endian integer access and the signed narrowing helpers.
#![allow(clippy::missing_safety_doc)]

// Kept until `hle/sound_bank.rs` and `hle/patch.rs` move to the slice forms.
pub unsafe fn read_be16(p: *const u8) -> u16 {
    unsafe { u16::from_be_bytes([*p, *p.add(1)]) }
}

pub unsafe fn read_be32(p: *const u8) -> u32 {
    unsafe { u32::from_be_bytes([*p, *p.add(1), *p.add(2), *p.add(3)]) }
}

fn array_at<const N: usize>(bytes: &[u8], offset: usize) -> [u8; N] {
    bytes[offset..offset + N]
        .try_into()
        .expect("slice length is N")
}

pub fn read_be16_at(bytes: &[u8], offset: usize) -> u16 {
    u16::from_be_bytes(array_at(bytes, offset))
}

pub fn read_be32_at(bytes: &[u8], offset: usize) -> u32 {
    u32::from_be_bytes(array_at(bytes, offset))
}

pub fn read_be64_at(bytes: &[u8], offset: usize) -> u64 {
    u64::from_be_bytes(array_at(bytes, offset))
}

pub fn write_be16_at(bytes: &mut [u8], offset: usize, val: u16) {
    bytes[offset..offset + 2].copy_from_slice(&val.to_be_bytes());
}

pub fn write_be32_at(bytes: &mut [u8], offset: usize, val: u32) {
    bytes[offset..offset + 4].copy_from_slice(&val.to_be_bytes());
}

pub fn write_be64_at(bytes: &mut [u8], offset: usize, val: u64) {
    bytes[offset..offset + 8].copy_from_slice(&val.to_be_bytes());
}

pub fn s16(x: u32) -> i16 {
    x as u16 as i16
}

pub fn s8(x: u32) -> i8 {
    x as u8 as i8
}
