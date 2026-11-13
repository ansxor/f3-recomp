//! `f3rt/bytes.hpp`: unaligned big-endian integer access and the signed narrowing helpers.
//!
//! The pointer forms are `unsafe` because they dereference the caller's pointer, as the C++ memcpy
//! versions dereference theirs; their safety requirement is the C++ one (a live object of that size).
#![allow(clippy::missing_safety_doc)]

/// Read an unaligned big-endian integer of type `T` from a raw byte pointer.
pub unsafe fn read_be16(p: *const u8) -> u16 {
    unsafe { u16::from_be_bytes([*p, *p.add(1)]) }
}

pub unsafe fn read_be32(p: *const u8) -> u32 {
    unsafe { u32::from_be_bytes([*p, *p.add(1), *p.add(2), *p.add(3)]) }
}

pub unsafe fn read_be64(p: *const u8) -> u64 {
    let mut bytes = [0u8; 8];
    unsafe { std::ptr::copy_nonoverlapping(p, bytes.as_mut_ptr(), 8) };
    u64::from_be_bytes(bytes)
}

/// Byte-span forms: read at a byte offset.
pub fn read_be16_at(bytes: &[u8], offset: usize) -> u16 {
    unsafe { read_be16(bytes.as_ptr().add(offset)) }
}

pub fn read_be32_at(bytes: &[u8], offset: usize) -> u32 {
    unsafe { read_be32(bytes.as_ptr().add(offset)) }
}

pub fn read_be64_at(bytes: &[u8], offset: usize) -> u64 {
    unsafe { read_be64(bytes.as_ptr().add(offset)) }
}

/// Write an unaligned big-endian integer of type `T` to a raw byte pointer.
pub unsafe fn write_be16(p: *mut u8, val: u16) {
    let bytes = val.to_be_bytes();
    unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), p, 2) };
}

pub unsafe fn write_be32(p: *mut u8, val: u32) {
    let bytes = val.to_be_bytes();
    unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), p, 4) };
}

pub unsafe fn write_be64(p: *mut u8, val: u64) {
    let bytes = val.to_be_bytes();
    unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), p, 8) };
}

/// Byte-span forms: write at a byte offset.
pub fn write_be16_at(bytes: &mut [u8], offset: usize, val: u16) {
    unsafe { write_be16(bytes.as_mut_ptr().add(offset), val) };
}

pub fn write_be32_at(bytes: &mut [u8], offset: usize, val: u32) {
    unsafe { write_be32(bytes.as_mut_ptr().add(offset), val) };
}

pub fn write_be64_at(bytes: &mut [u8], offset: usize, val: u64) {
    unsafe { write_be64(bytes.as_mut_ptr().add(offset), val) };
}

pub fn s16(x: u32) -> i16 {
    x as u16 as i16
}

pub fn s8(x: u32) -> i8 {
    x as u8 as i8
}
