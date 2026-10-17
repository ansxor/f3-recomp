#pragma once
#include <cstddef>
#include <cstdint>

namespace f3rt {

// ============================================================================
// Main CPU (68EC020) 24-bit Bus Map (Taito F3 Motherboard)
// *_END bounds are exclusive unless otherwise noted.
// ============================================================================

constexpr uint32_t ROM_BASE = 0x000000;
constexpr uint32_t ROM_END = 0x200000; // Program ROM window, 2 MiB (1048576 words)
constexpr size_t MAIN_ROM_MAX_SIZE = 0x200000;
constexpr size_t CPU_VECTOR_TABLE_SIZE =
    0x400; // 68EC020 exception vector table (256 vectors * 4 bytes)

constexpr uint32_t RAM_BASE = 0x400000;         // Main work RAM base
constexpr uint32_t RAM_END = 0x440000;          // 128 KiB RAM mirrored across 256 KiB
constexpr size_t RAM_SIZE = 0x20000;            // Physical RAM size (128 KiB)
constexpr uint32_t RAM_PHYSICAL_END = 0x420000; // Physical RAM boundary before mirrors
constexpr uint32_t RAM_MIRROR_MASK = 0x1ffff;

constexpr uint32_t PALETTE_BASE = 0x440000; // Palette RAM base
constexpr uint32_t PALETTE_END = 0x448000;  // Palette RAM end (8 KiB)
constexpr size_t PALETTE_RAM_SIZE = 0x8000;

constexpr uint32_t IO_BASE = 0x4a0000; // TC0640FIO base: inputs on read, registers on write
constexpr uint32_t IO_END = 0x4a0020;
constexpr uint32_t WATCHDOG = 0x4a0000;   // Any write kicks the watchdog
constexpr uint32_t COIN_BANK0 = 0x4a0004; // Lockout/counter byte; +1 is the word's low byte
constexpr uint32_t COIN_BANK1 = 0x4a0014;
constexpr uint32_t EEPROM_PINS = 0x4a0013;   // Serial EEPROM clock/data/select
constexpr uint32_t TIMER_CONTROL = 0x4c0000; // 16-bit timer interrupt control, big-endian

// Graphics RAM (0x600000 - 0x640000, 256 KiB)
constexpr uint32_t GRAPHICS_BASE = 0x600000;
constexpr uint32_t GRAPHICS_END = 0x640000;
constexpr size_t GRAPHICS_RAM_SIZE = 0x40000;

// Sub-regions within Graphics RAM (offsets relative to GRAPHICS_BASE)
constexpr uint32_t OFFS_SPRITERAM = 0x00000; // Sprite display list (64 KiB, 0x600000)
constexpr size_t SPRITE_RAM_SIZE = 0x10000;  // 64 KiB sprite RAM
constexpr size_t SPRITE_BANK_STRIDE =
    0x8000; // 32 KiB per sprite bank (Bank 0: 0x0000, Bank 1: 0x8000)
constexpr uint32_t OFFS_PLAYFIELDS = 0x10000; // Playfield tilemaps (0x610000)
constexpr size_t PLAYFIELD_LAYER_SIZE =
    0x2000; // 8 KiB per playfield layer (extended mode, 64 columns x 32 rows)
constexpr size_t PLAYFIELD_STANDARD_LAYER_SIZE =
    0x1000; // 4 KiB per playfield layer (standard mode, 32 columns x 32 rows)
constexpr uint32_t OFFS_TEXTRAM_MAP = 0x1c000;    // Text tilemap (0x61c000)
constexpr uint32_t OFFS_TEXTRAM_GLYPHS = 0x1e000; // Text glyph/font patterns (0x61e000)
constexpr size_t TEXTRAM_GLYPH_SIZE = 0x2000;     // 8 KiB font pattern RAM
constexpr int TEXTRAM_TILES = 0x1000;             // 64x64 = 4096 text tiles
constexpr uint32_t OFFS_LINERAM = 0x20000;        // Line RAM (0x620000)
constexpr size_t LINERAM_SIZE = 0x10000;
constexpr uint32_t OFFS_PIVOT = 0x30000; // Pivot RAM (0x630000)

// Line RAM sub-offsets (relative to lineram pointer / OFFS_LINERAM)
constexpr uint32_t LINERAM_SECTIONS_OFFSET = 0x4000; // Base of section line tables (0x624000)
constexpr size_t LINERAM_SECTION_STRIDE = 0x1000;    // 4 KiB per section
constexpr size_t LINERAM_SUBSECTION_STRIDE = 0x200; // 512 bytes per subsection (256 scanline words)
constexpr size_t LINERAM_LATCH_STRIDE = 0x200;      // 512 bytes per section latch table
constexpr size_t LINERAM_BANK_OFFSET = 0x800;       // Bank offset for alternate line table

// Control Registers (0x660000 - 0x660020 / 0x660040)
constexpr uint32_t CONTROL_BASE = 0x660000; // PF0-3 X/Y scroll, pivot/text regs
constexpr uint32_t CONTROL_END = 0x660020;
constexpr uint32_t CONTROL_EXTENDED_END = 0x660040;

// Shared RAM (Main CPU side: 0xc00000 - 0xc00800, 2 KiB)
constexpr uint32_t SHARED_BASE = 0xc00000; // Shared dual-port RAM with sound CPU
constexpr uint32_t SHARED_END = 0xc00800;
constexpr size_t SHARED_RAM_SIZE = 0x800; // 2048 bytes

// Sound CPU Reset Controls (Main CPU bus)
constexpr uint32_t SOUND_RESET_RELEASE = 0xc80000; // 4 bytes; write releases sound CPU reset
constexpr uint32_t SOUND_RESET_ASSERT = 0xc80100;  // 4 bytes; write holds sound CPU in reset
constexpr uint32_t SOUND_RESET_WINDOW_SIZE = 4;

// ============================================================================
// Sound CPU (68000) 24-bit Bus Map
// ============================================================================

constexpr uint32_t SOUND_RAM_BASE = 0x000000; // Sound work RAM (64 KiB, mirrored to 0x040000)
constexpr uint32_t SOUND_RAM_END = 0x040000;
constexpr uint32_t SOUND_RAM_MASK = 0xffff;

constexpr uint32_t SOUND_SHARED_BASE =
    0x140000; // Shared DPRAM (8-bit even byte lane: (addr - 0x140000) >> 1)
constexpr uint32_t SOUND_SHARED_END = 0x141000;

constexpr uint32_t SOUND_ES5505_BASE = 0x200000; // ES5505 (OTIS) sound chip
constexpr uint32_t SOUND_ES5505_END = 0x200020;
constexpr uint32_t OTIS_PAGE = 0x20001e; // OTIS register page select

constexpr uint32_t SOUND_ES5510_BASE = 0x260000; // ES5510 (ESP) DSP chip
constexpr uint32_t SOUND_ES5510_END = 0x260200;

constexpr uint32_t SOUND_DUART_BASE = 0x280000; // MC68681 DUART (8-bit odd byte lane)
constexpr uint32_t SOUND_DUART_END = 0x280020;

constexpr uint32_t SOUND_BANK_TABLE_BASE = 0x300000; // ES5505 voice sample bank mapping table
constexpr uint32_t SOUND_BANK_TABLE_END = 0x300040;

// Main CPU sound program bankswitch window (0x300000 - 0x30007f)
constexpr uint32_t MAIN_SOUND_BANKSWITCH_BASE = 0x300000;
constexpr uint32_t MAIN_SOUND_BANKSWITCH_END = 0x300080;

constexpr uint32_t SOUND_VOLUME_BASE = 0x340000; // MB87078 electronic volume (8-bit even byte lane)
constexpr uint32_t SOUND_VOLUME_END = 0x340004;

constexpr uint32_t SOUND_ROM_BASE = 0xc00000; // Sound Program ROM (512 KiB)
constexpr uint32_t SOUND_ROM_END = 0xc80000;
constexpr size_t SOUND_ROM_SIZE = 0x80000;

constexpr uint32_t SOUND_RAM_HIGH_BASE = 0xff0000; // Sound work RAM high mirror
constexpr uint32_t SOUND_RAM_HIGH_END = 0x1000000;

// Sound trace monitoring bus extent
constexpr uint32_t SOUND_TRACE_BUS_BEGIN = SOUND_SHARED_BASE; // 0x140000
constexpr uint32_t SOUND_TRACE_BUS_END = SOUND_VOLUME_END;    // 0x340004

// ============================================================================
// Shared DPRAM Mailbox Protocol Offsets (within the 2 KiB shared RAM)
// ============================================================================

constexpr uint32_t MAILBOX_RING_BASE = 0x000; // Ring buffer base (1024 bytes)
constexpr size_t MAILBOX_RING_SIZE = 1024;
constexpr uint32_t MAILBOX_RING_MASK = 0x3ff;

constexpr uint32_t MAILBOX_PRODUCER_H = 0x480; // Doubled ring producer index (high byte)
constexpr uint32_t MAILBOX_PRODUCER_L = 0x481; // Doubled ring producer index (low byte)

constexpr uint32_t MAILBOX_CONSUMER_H = 0x482; // Doubled ring consumer index (high byte)
constexpr uint32_t MAILBOX_CONSUMER_L = 0x483; // Doubled ring consumer index (low byte)

constexpr uint32_t MAILBOX_GAIN_CH0 = 0x7fa; // Left channel output gain / volume
constexpr uint32_t MAILBOX_GAIN_CH1 = 0x7fb; // Right channel output gain / volume

// Main CPU addresses for mailbox registers (SHARED_BASE + offset)
constexpr uint32_t MAIN_MAILBOX_PRODUCER_H = SHARED_BASE + MAILBOX_PRODUCER_H; // 0xc00480
constexpr uint32_t MAIN_MAILBOX_PRODUCER_L = SHARED_BASE + MAILBOX_PRODUCER_L; // 0xc00481
constexpr uint32_t MAIN_MAILBOX_CONSUMER_H = SHARED_BASE + MAILBOX_CONSUMER_H; // 0xc00482
constexpr uint32_t MAIN_MAILBOX_CONSUMER_L = SHARED_BASE + MAILBOX_CONSUMER_L; // 0xc00483
constexpr uint32_t MAIN_MAILBOX_GAIN_CH0 = SHARED_BASE + MAILBOX_GAIN_CH0;     // 0xc007fa
constexpr uint32_t MAIN_MAILBOX_GAIN_CH1 = SHARED_BASE + MAILBOX_GAIN_CH1;     // 0xc007fb

} // namespace f3rt
