# Stubs

Default stub implementations. Each one is linked only when the selected game provides no implementation of its own.

- `netplay.cpp`: `f3rt::game_netplay()` returns `nullptr`. No versus hook is offered. Selected by `F3RT_GAME_NETPLAY_SOURCES` when `games/<set>/netplay/*.cpp` is empty.

To provide the real version, a game implements it under `games/<set>/`:

- netplay: `games/<set>/netplay/*.cpp`
