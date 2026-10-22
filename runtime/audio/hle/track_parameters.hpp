#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace f3rt::hle {

// The 0x38-byte track parameter record. Controllers occupy the even bytes from 0x0a;
// the following odd byte is the low half of that big-endian word. byte() and
// bytes address the rest of the record.
struct TrackParameters {
    struct At {
        static constexpr size_t size = 0x38;
        static constexpr size_t program_high = 0x04;
        static constexpr size_t program_low = 0x05;
        static constexpr size_t controllers = 0x0a;
        static constexpr size_t bend = 0x0a;
        static constexpr size_t mod_0c = 0x0c;
        static constexpr size_t mod_10 = 0x10;
        static constexpr size_t mod_12 = 0x12;
        static constexpr size_t expression = 0x14;
        static constexpr size_t mod_1a = 0x1a;
        static constexpr size_t layer_mask = 0x1c;
        static constexpr size_t volume = 0x20;
        static constexpr size_t previous_key = 0x22;
        static constexpr size_t pan = 0x26;
        static constexpr size_t program_record = 0x2a;
        static constexpr size_t volume_record = 0x2c;
        static constexpr size_t pan_record = 0x2d;
        static constexpr size_t mod_1a_record = 0x2e;
        static constexpr size_t layer_mask_record = 0x2f;
        static constexpr size_t selector_record = 0x30;
        static constexpr size_t pair_select = 0x31;
        static constexpr size_t flags = 0x32;
        static constexpr size_t selector = 0x33;
        static constexpr size_t transpose = 0x35;
    };

    static constexpr size_t size = At::size;
    static constexpr uint8_t controller_center = 0x40;
    static constexpr uint8_t extended_selector = 0x40;
    static constexpr unsigned bend_center = 0x4000;

    std::array<uint8_t, size> bytes{};

    constexpr uint8_t byte(size_t offset) const noexcept { return bytes[offset]; }
    constexpr void set_byte(size_t offset, uint8_t value) noexcept { bytes[offset] = value; }

    constexpr uint16_t word(size_t offset) const noexcept {
        return uint16_t((uint16_t(bytes[offset]) << 8) | bytes[offset + 1]);
    }

    // Controller k writes the even byte at 0x0a + 2k. The layer-mask byte is
    // stored centered (value - 0x40); every other controller stores the value.
    // Returns false when the byte would fall outside the record.
    constexpr bool set_controller(size_t index, uint8_t value) noexcept {
        const size_t offset = At::controllers + index * 2;
        if (offset >= size) return false;
        bytes[offset] = offset == At::layer_mask ? uint8_t(value - controller_center) : value;
        return true;
    }

    constexpr void set_program_image(uint16_t program) noexcept {
        bytes[At::program_high] = uint8_t(program >> 8);
        bytes[At::program_low] = uint8_t(program);
    }

    // Header track-record fields copied into the live controller bytes.
    constexpr void copy_record_to_live() noexcept {
        bytes[At::volume] = bytes[At::volume_record];
        bytes[At::pan] = bytes[At::pan_record];
        bytes[At::mod_1a] = bytes[At::mod_1a_record];
        bytes[At::layer_mask] = bytes[At::layer_mask_record];
        bytes[At::selector] = bytes[At::selector_record];
    }

    constexpr uint16_t bend() const noexcept { return word(At::bend); }
    constexpr uint16_t mod_0c() const noexcept { return word(At::mod_0c); }
    constexpr uint16_t mod_10() const noexcept { return word(At::mod_10); }
    constexpr uint16_t mod_12() const noexcept { return word(At::mod_12); }
    constexpr uint16_t mod_1a() const noexcept { return word(At::mod_1a); }

    constexpr uint8_t expression() const noexcept { return bytes[At::expression]; }
    // Added into the attenuation sum. Zero when the expression byte is zero.
    constexpr uint8_t expression_offset() const noexcept {
        const uint8_t value = expression();
        return value ? uint8_t(value + 127) : 0;
    }

    // Non-zero replaces the program layer-enable mask. A controller write to
    // this byte subtracts 0x40 (firmware PAN_CONTROLLER_OFFSET).
    constexpr uint8_t layer_mask() const noexcept { return bytes[At::layer_mask]; }

    constexpr uint8_t volume() const noexcept { return bytes[At::volume]; }
    constexpr uint8_t volume_index() const noexcept { return uint8_t(volume() & 127); }
    constexpr void set_volume(uint8_t value) noexcept {
        bytes[At::volume] = value;
        bytes[At::volume_record] = value;
    }

    // Previous factory key. A fresh legato voice glides from this byte.
    constexpr uint8_t previous_key() const noexcept { return bytes[At::previous_key]; }
    constexpr void set_previous_key(uint8_t key) noexcept { bytes[At::previous_key] = key; }

    constexpr uint8_t pan() const noexcept { return bytes[At::pan]; }
    constexpr int8_t signed_pan() const noexcept { return int8_t(bytes[At::pan]); }
    constexpr void set_pan(uint8_t value) noexcept {
        bytes[At::pan] = value;
        bytes[At::pan_record] = value;
    }

    constexpr uint8_t pair_select() const noexcept { return bytes[At::pair_select]; }

    constexpr uint8_t flags() const noexcept { return bytes[At::flags]; }
    // Bits 4 and 5 both set: the note-on is dropped.
    constexpr bool notes_suppressed() const noexcept { return (flags() & 0x30) == 0x30; }
    constexpr void set_extended_selector(bool on) noexcept {
        if (on) bytes[At::flags] = uint8_t(bytes[At::flags] | extended_selector);
        else bytes[At::flags] = uint8_t(bytes[At::flags] & uint8_t(~extended_selector));
    }

    constexpr uint8_t selector() const noexcept { return bytes[At::selector]; }
    constexpr void set_selector(uint8_t value) noexcept { bytes[At::selector] = value; }

    constexpr uint8_t transpose() const noexcept { return bytes[At::transpose]; }
    constexpr int8_t signed_transpose() const noexcept { return int8_t(bytes[At::transpose]); }
    constexpr uint8_t transposed_key(uint8_t kernel_key) const noexcept {
        return uint8_t(kernel_key + bytes[At::transpose]);
    }
};

} // namespace f3rt::hle
