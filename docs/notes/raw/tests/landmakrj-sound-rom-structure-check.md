---
source_url: python3 script embedded below
ingested: 2026-10-11
sha256: 48f11bd76e44f25235ca5e649cd02944b2f3bb91cc6c83246b9394b1a0d6d72b
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "python3 (ad-hoc, no tools/f3a); run on the supplied roms/landmakrj/ files"
---
<!-- excerpt: command (python3 -c, run from repository root) -->
```python
# Run 2026-10-11 from the repository root; ROM files from roms/landmakrj/ (not committed).
import struct
d='roms/landmakrj/'
s0=open(d+'e61-14.32','rb').read()+b'\xff'*0x20000
s1=open(d+'e61-15.33','rb').read()+b'\xff'*0x20000
snd=bytearray(0x80000); snd[0::2]=s0; snd[1::2]=s1
def sb(a,n): return bytes(snd[a-0xc00000:a-0xc00000+n])
print('accepted-length table at 0xc13260 (17 entries, opcodes 0x80..0x90):', list(sb(0xc13260,17)))
print('handler table at 0xc1323e (17 word entries, low words; high word 0xc1):', [hex(struct.unpack('>H',sb(0xc1323e+2*i,2))[0]) for i in range(17)])
ent=[struct.unpack('>I',sb(0xc20000+8+4*i,4))[0] for i in range(40)]
print('stream directory 0xc20008 (long offsets), nonzero entries:', sum(1 for e in ent if e), 'first/last:', hex(ent[0]), hex(ent[26]))
hdr=[struct.unpack('>H',sb(0xc3879e+8+2*i,2))[0] for i in range(40)]
print('header directory 0xc3879e+8 (word offsets), nonzero entries:', sum(1 for e in hdr if e), 'strides:', sorted({hdr[i+1]-hdr[i] for i in range(26)}))
print('bend range byte at 0xc085f2:', sb(0xc085f2,2).hex())
img=bytearray(0x200000)
for f,o in [('e61-13.20',0),('e61-12.19',1),('e61-11.18',2),('e61-10.17',3)]:
    img[o::4]=open(d+f,'rb').read()
print('main 0x6d5e word (max sound selector):', hex(struct.unpack('>H',img[0x6d5e:0x6d60])[0]), ' selector table 0x6bde first 4 longs:', [hex(struct.unpack('>I',img[0x6bde+4*i:0x6bde+4*i+4])[0]) for i in range(4)])
```

<!-- excerpt: output -->
```
accepted-length table at 0xc13260 (17 entries, opcodes 0x80..0x90): [3, 3, 3, 3, 3, 5, 4, 4, 4, 7, 5, 5, 6, 6, 6, 5, 6]
handler table at 0xc1323e (17 word entries, low words; high word 0xc1): ['0x2f20', '0x2f26', '0x2f1a', '0x2f2c', '0x2f42', '0x301a', '0x2f62', '0x2f96', '0x2f7a', '0x2fcc', '0x30c6', '0x30ba', '0x30d2', '0x30da', '0x30ea', '0x313c', '0x3146']
stream directory 0xc20008 (long offsets), nonzero entries: 27 first/last: 0x198 0x18488
header directory 0xc3879e+8 (word offsets), nonzero entries: 27 strides: [168]
bend range byte at 0xc085f2: 0002
main 0x6d5e word (max sound selector): 0x5f  selector table 0x6bde first 4 longs: ['0x6d60', '0x6d68', '0x6d70', '0x6d78']
```
