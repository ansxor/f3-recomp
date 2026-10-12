---
source_url: python3 script embedded below
ingested: 2026-10-11
sha256: 6a0119eeab5b97857a9c41ee7197bf1a1a513e94d6601e50f01027e5391fd491
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "python3 (ad-hoc); roms/landmakrj/ files padded with FF to 0x40000 each"
---
<!-- excerpt: command -->
```python
import zlib
d='roms/landmakrj/'
s0=open(d+'e61-14.32','rb').read()+b'\xff'*0x20000
s1=open(d+'e61-15.33','rb').read()+b'\xff'*0x20000
snd=bytearray(0x80000); snd[0::2]=s0; snd[1::2]=s1
print('sound image (0xc00000..0xc80000) crc32 %08x' % zlib.crc32(bytes(snd)))
```

<!-- excerpt: output -->
```
sound image (0xc00000..0xc80000) crc32 5a7e9117
```
