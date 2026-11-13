//! `runtime/audio/hle/`: one module per C++ file of the HLE sound engine.

pub mod block;
pub mod command;
pub mod core;
pub mod effects;
pub mod events;
pub mod faust;
pub mod mailbox;
pub mod note_pool;
pub mod note_timing;
pub mod patch;
pub mod program_service;
pub mod pulse_timer;
pub mod rom_view;
pub mod sequencer;
pub mod sound_bank;
pub mod sound_rom_5a7e9117;
pub mod synthesis;
pub mod track_parameters;
pub mod track_service;
pub mod voice;
pub mod voice_filter;
pub mod voice_render;
pub mod voice_service;
