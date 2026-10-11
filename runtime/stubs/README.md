# Stubs

Default stub implementations. Each one is linked only when the selected game provides no implementation of its own.

- `boot.cpp`: `f3rt::game_boot()` returns `nullptr`. The frontend never turbo-boots the game. Selected by `F3RT_GAME_BOOT_SOURCES` when `games/<set>/boot/*.cpp` is empty.
- `netplay.cpp`: `f3rt::game_netplay()` returns `nullptr`. No versus hook is offered. Selected by `F3RT_GAME_NETPLAY_SOURCES` when `games/<set>/netplay/*.cpp` is empty.

To provide the real version, a game implements it under `games/<set>/`:

- boot: `games/<set>/boot/*.cpp`
- netplay: `games/<set>/netplay/*.cpp`
