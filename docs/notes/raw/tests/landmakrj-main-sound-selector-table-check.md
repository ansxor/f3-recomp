---
source_url: python3 script embedded below
ingested: 2026-10-11
sha256: 0902b1a8231235d0c872bab1ade3635f2466f88787417896f495e5ab21d219fc
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "python3 (ad-hoc); roms/landmakrj/ files"
---
<!-- excerpt: command -->
```python
import struct
from collections import Counter
d='roms/landmakrj/'
img=bytearray(0x200000)
for f,o in [('e61-13.20',0),('e61-12.19',1),('e61-11.18',2),('e61-10.17',3)]:
    img[o::4]=open(d+f,'rb').read()
ptrs=[struct.unpack('>I',img[0x6bde+4*i:0x6bde+4*i+4])[0] for i in range(0x60)]
print('table end 0x%x; word there: 0x%04x (maximum selector)' % (0x6bde+0x60*4, struct.unpack('>H',img[0x6d5e:0x6d60])[0]))
print('descriptor address range: 0x%x..0x%x' % (min(ptrs), max(ptrs)))
print('descriptor type counts (first byte):', dict(Counter(img[p] for p in ptrs)))
for i in (0,1,2):
    print('selector', i, 'descriptor @0x%x:' % ptrs[i], img[ptrs[i]:ptrs[i]+10].hex())
```

<!-- excerpt: output -->
```
table end 0x6d5e; word there: 0x005f (maximum selector)
descriptor address range: 0x6d60..0x7005
descriptor type counts (first byte): {3: 3, 2: 77, 0: 16}
selector 0 descriptor @0x6d60: 03060380010380020201
selector 1 descriptor @0x6d68: 02016401400027020202
selector 2 descriptor @0x6d70: 02025008400029020201
```
