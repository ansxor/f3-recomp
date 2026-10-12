---
source_url: bash command embedded below
ingested: 2026-10-11
sha256: ef75618405794263b755f5b90b18cb9ac85b06100d8d4a29c7696bad4dc9a38f
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "diff (GNU/BSD) on games/landmakr/config.toml and games/landmakrj/config.toml"
---
<!-- excerpt: command (bash, repository root, 2026-10-11) -->
```sh
diff <(sed -n '/^\[sprites\]/,$p' games/landmakr/config.toml) <(sed -n '/^\[sprites\]/,$p' games/landmakrj/config.toml) && echo IDENTICAL
```

<!-- excerpt: output (exit code 1; the only difference is one trailing blank line in landmakrj) -->
```
129a130
> 
```
