//! The only crate Corrosion links into the C++ runtime. Every `extern "C"` symbol the C++ shell calls
//! is exported by one of the runtime crates below (declared in `include/f3rt/abi/*.h`); linking them
//! here puts those `no_mangle` symbols into the single staticlib. Each staticlib carries its own copy
//! of `std`, so exactly one is linked: the runtime crates are rlib dependencies of this one.

extern crate f3rt_audio;
extern crate f3rt_eeprom;
