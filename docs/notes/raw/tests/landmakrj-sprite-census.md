---
source_url: docs/developer/GPU-VIDEO.md
ingested: 2026-10-11
sha256: 5b3fba8d7061b07486b5d18b8b2dba25620892a612d9f35decdf0dc836447281
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "repository record of runs (secondhand: command output and artifacts under ignored build/ were not retained); source doc deleted after ingest; 172,000-frame survey, seeds 5/6/7/41 and no-input attract"
---
<!-- excerpt: docs/developer/GPU-VIDEO.md lines 854-870 -->
### Sprite precision and honest baseline gain

Across the 172,000-frame census, **32,571,398 sprite records**, **259,072 zoomed
records (0.795%)**, and **zero fractional X/Y origins**. Counts include
offscreen/occluded records, not just visible pixels. ROM `$4688` single tiles
retain 8-bit X/Y zoom; `$480c..$4a36` and `$a913c..$a93a2` grid widths mask the
low four X-zoom bits while placement uses all bits. Intermediate half-pixel
carry is quantized to integer tile origins before descriptor submission.
Preserving discarded carry previously caused the frame-1080 native mismatch;
it is not honest subpixel positioning data.

Descriptors support axis-aligned scale and flips, **no angle/rotation matrix**.
Retain the one-frame rendered sprite lag, reverse descriptor/texel precedence,
nominal culling before `+255` Y rounding and the existing off raster.

Existing off 4x already gains source sampling on zoomed sprites: frame 1080
SP2 differs from nearest 1x by **2560 pixels**, SP3 by **8832**, composite by
