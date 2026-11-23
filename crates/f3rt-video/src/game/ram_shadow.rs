//! RAM change tracking, mechanically ported from `runtime/renderer/game/ram_shadow.hpp`.

use std::sync::atomic::{AtomicU64, Ordering};

static NEXT_CONTENT_VERSION: AtomicU64 = AtomicU64::new(1);

/// A fresh content version, shared by every shadowed RAM buffer.
pub fn fresh_content_version() -> u64 {
    NEXT_CONTENT_VERSION.fetch_add(1, Ordering::Relaxed)
}

/// Reports whether the guarded RAM differs from the last observed contents.
///
/// Like the C++ template, copies start empty (and assignments invalidate); only `changed`
/// stores a snapshot.
#[derive(Debug, Default)]
pub struct RamShadow<const N: usize> {
    bytes: Option<Box<[u8; N]>>,
}

impl<const N: usize> Clone for RamShadow<N> {
    fn clone(&self) -> Self {
        Self::default()
    }
}

impl<const N: usize> RamShadow<N> {
    pub fn new() -> Self {
        Self::default()
    }

    /// Returns true when `source` differs from the stored snapshot (and stores it).
    pub fn changed(&mut self, source: &[u8; N]) -> bool {
        if let Some(bytes) = &self.bytes {
            if bytes[..] == source[..] {
                return false;
            }
        } else {
            self.bytes = Some(Box::new([0u8; N]));
        }
        self.bytes.as_mut().unwrap()[..].copy_from_slice(source);
        true
    }

    pub fn invalidate(&mut self) {
        self.bytes = None;
    }
}

/// [`RamShadow`] for buffers whose size is only known at runtime (e.g. a playfield layer whose
/// stride comes from [`VideoConfig`](crate::config::VideoConfig)). Allocates on the first check and
/// when the guarded size changes, never on a steady-state comparison.
#[derive(Debug, Default)]
pub struct RamShadowSlice {
    bytes: Option<Box<[u8]>>,
}

impl Clone for RamShadowSlice {
    /// Copies start empty, mirroring `RamShadowSlice`'s C++ copy constructor.
    fn clone(&self) -> Self {
        Self::default()
    }
}

impl RamShadowSlice {
    pub fn new() -> Self {
        Self::default()
    }

    /// Returns true when `source` differs from the stored snapshot (and stores it).
    pub fn changed(&mut self, source: &[u8]) -> bool {
        match &mut self.bytes {
            Some(bytes) if bytes.len() == source.len() => {
                if &bytes[..] == source {
                    return false;
                }
                bytes.copy_from_slice(source);
            }
            _ => self.bytes = Some(source.to_vec().into_boxed_slice()),
        }
        true
    }

    pub fn invalidate(&mut self) {
        self.bytes = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn first_check_reports_change_and_identical_contents_do_not() {
        let mut shadow = RamShadow::<4>::new();
        let source = [1u8, 2, 3, 4];
        assert!(shadow.changed(&source));
        assert!(!shadow.changed(&source));
        assert!(shadow.changed(&[1, 2, 3, 5]));
        assert!(!shadow.changed(&[1, 2, 3, 5]));
    }

    #[test]
    fn invalidate_forces_the_next_check_to_report_change() {
        let mut shadow = RamShadow::<2>::new();
        let source = [9u8, 9];
        assert!(shadow.changed(&source));
        shadow.invalidate();
        assert!(shadow.changed(&source));
    }

    #[test]
    fn copies_start_empty() {
        let mut shadow = RamShadow::<2>::new();
        assert!(shadow.changed(&[7, 7]));
        let mut copy = shadow.clone();
        assert!(copy.changed(&[7, 7]), "a copy has no snapshot");
        shadow.invalidate();
        assert!(shadow.changed(&[7, 7]));
    }

    #[test]
    fn slice_shadow_tracks_runtime_sizes() {
        let mut shadow = RamShadowSlice::new();
        assert!(shadow.changed(&[1, 2, 3, 4]));
        assert!(!shadow.changed(&[1, 2, 3, 4]));
        assert!(shadow.changed(&[1, 2, 3, 5]));
        // A different guarded size re-primes instead of comparing across lengths.
        assert!(shadow.changed(&[1, 2]));
        assert!(!shadow.changed(&[1, 2]));
        shadow.invalidate();
        assert!(shadow.changed(&[1, 2]));
    }

    #[test]
    fn versions_are_unique_and_increasing() {
        let a = fresh_content_version();
        let b = fresh_content_version();
        assert!(b > a);
        assert_ne!(a, b);
    }
}
