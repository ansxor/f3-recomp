---
source_url: python3 script embedded below
ingested: 2026-10-11
sha256: 95d082c1c479c03d31943d7342c3bbd71e689e1794c0a013e020cffd7326c095
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "python3 (ad-hoc, no tools/f3a); run on the supplied roms/landmakrj/ files"
---
<!-- excerpt: command (python3 -c, run from repository root) -->
```python
# Run 2026-10-11 from the repository root; ROM files from roms/landmakrj/ (not committed).
import hashlib, zlib
d='roms/landmakrj/'
img=bytearray(0x200000)
for f,o in [('e61-13.20',0),('e61-12.19',1),('e61-11.18',2),('e61-10.17',3)]:
    img[o::4]=open(d+f,'rb').read()
print('main sha256', hashlib.sha256(img).hexdigest())
def run(buf,i,step):
    while 0<=i<len(buf) and buf[i]==0xff: i+=step
    return i
print('main FF run containing 0x7030: [%#x, %#x)' % (run(img,0x7030,-1)+1, run(img,0x7030,1)))
print('main FF run containing 0x11b362: [%#x, %#x)' % (run(img,0x11b362,-1)+1, run(img,0x11b362,1)))
print('main word at 0x1ffffe:', img[0x1ffffe:0x200000].hex())
s0=open(d+'e61-14.32','rb').read()+b'\xff'*0x20000
s1=open(d+'e61-15.33','rb').read()+b'\xff'*0x20000
print('sound lane CRCs after FF pad ok:', zlib.crc32(s0)==0x18961bbb, zlib.crc32(s1)==0x2c64557a)
snd=bytearray(0x80000); snd[0::2]=s0; snd[1::2]=s1
print('sound FF run containing 0xc1e45a: [%#x, %#x)' % (run(snd,0xc1e45a-0xc00000,-1)+1+0xc00000, run(snd,0xc1e45a-0xc00000,1)+0xc00000))
print('sound FF run containing 0xc39a30: [%#x, %#x)' % (run(snd,0xc39a30-0xc00000,-1)+1+0xc00000, run(snd,0xc39a30-0xc00000,1)+0xc00000))
```

<!-- excerpt: output -->
```
main sha256 aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
main FF run containing 0x7030: [0x702f, 0x10000)
main FF run containing 0x11b362: [0x11b362, 0x1ffffe)
main word at 0x1ffffe: 0001
sound lane CRCs after FF pad ok: True True
sound FF run containing 0xc1e45a: [0xc1e45a, 0xc20000)
sound FF run containing 0xc39a30: [0xc39a30, 0xc80000)
```
