---
source_url: "tools/f3a run --watch (two runs, commands above)"
ingested: 2026-10-09
sha256: b570cf121a413111cace9ede721c64d474db7f6bce72aa84a10e27d69f90789d
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "tools/f3a (git 47cbdc3 + uncommitted), build-landmakrj-instrument"
---
# Round-end gate in sub_08e20c: watch runs

## Run A: 2P versus, no poke (tools/f3a run --game landmakrj --frames 9000 --inputs 2p-versus-rounds.txt --watch sub_08e20c,0x08e62e,sub_08e9c6)

inputs.txt:
```
# Experiment B: 2P VS, P2 idle, P1 mashes b1 period 45 seed 5.
700+20 coin
720+20 p2 coin
800-1300 mash keys=start period=45
1400+5 right
1500+5 start
2000-end mash keys=b1 period=45 seed=5
```

output:
```
$ [landmakrj] /Users/darien/Workspace/f3-stuff/f3-recomp/build-landmakrj-instrument/landmakr --frames 9000 --profile-out /tmp/n1/run.profile --watch-log /tmp/n1/entries.log --watch-entries 8e20c,8e62e,8e9c6 --inputs /private/tmp/n1/inputs.txt
exit=0 pc=0x1136 frame_crc=0xa6746023 cycles=2443003254 native_blocks=33038088 fallback_instructions=0
executions per watched instruction (/tmp/n1/entries.log: one ENTRY line per frame it ran in)
  sub_08e20c             frames 970..970: ran in 1 frame, 1 times
  0x08e62e in sub_08e20c never executed
  sub_08e9c6             never executed
```

## Run B: same script plus `3000-end poke 0x401f54.w=10` (--watch 0x08e62e --until sub_08e9c6)

inputs.txt:
```
# Reaches sub_08e9c6 (charram fill) by poking the VS round counter -$60ac (0x401f54) to 10 from frame 3000 onward; base = 2p-versus-rounds.txt (2P VS, P2 idle, P1 mashes b1 every 45).
# Experiment B: 2P VS, P2 idle, P1 mashes b1 period 45 seed 5.
700+20 coin
720+20 p2 coin
800-1300 mash keys=start period=45
1400+5 right
1500+5 start
2000-end mash keys=b1 period=45 seed=5
3000-end poke 0x401f54.w=10
```

output:
```
$ [landmakrj] /Users/darien/Workspace/f3-stuff/f3-recomp/build-landmakrj-instrument/landmakr --frames 9000 --profile-out /tmp/n2/run.profile --watch-log /tmp/n2/entries.log --watch-entries 8e62e,8e9c6 --inputs /private/tmp/n2/inputs.txt
stopped at frame ~7287: every --until target ran (the run was cut short, so there is no end-of-run summary; dumps and logs are complete up to that frame)
  reached     sub_08e9c6             frame 7287   executed
executions per watched instruction (/tmp/n2/entries.log: one ENTRY line per frame it ran in)
  0x08e62e in sub_08e20c frames 7286..7286: ran in 1 frame, 1 times
  sub_08e9c6             frames 7287..7287: ran in 1 frame, 1 times
```
