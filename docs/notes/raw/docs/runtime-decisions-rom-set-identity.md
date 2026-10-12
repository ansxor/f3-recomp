---
source_url: docs/developer/DECISIONS.md:12-13
ingested: 2026-10-11
sha256: 837b7442d4ba598865832e75ebdce6f573d4d36f60b03aedc6af75129878106a
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "repository documentation text (runtime-derived, not hardware); source file deleted after ingest"
---
<!-- excerpt: docs/developer/DECISIONS.md lines 12-13 -->
- 2026-10-03: MAME `taito_f3.cpp` lines 4166–4171 defines World `landmakr` as four 0x80000-byte lanes e61-19.20, e61-18.19, e61-17.18, e61-16.17, interleaved at byte offsets 0,1,2,3. Supplied `roms/landmakr` instead contains e61-13.20, e61-12.19, e61-11.18, e61-10.17: MAME `landmakrj`. Keep distinct validated game configurations; never silently identify Japan as World.
- Japan `landmakrj` (2.01J) became the execution target. World remained configuration-only and untested.
