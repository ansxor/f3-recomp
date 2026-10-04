#ifndef F3_RECOMP_BITFIELD_H
#define F3_RECOMP_BITFIELD_H

/* Called with constant operation/EA kind from generated C. Offsets and widths
 * may be dynamic. All snapshots precede aliased destination-register writes. */
static inline uint32_t f3_bf_rotate(uint32_t value, unsigned count) {
    return (value << (count & 31)) | (value >> ((0u - count) & 31));
}

static inline void f3_bitfield(f3_cpu *cpu, unsigned operation, int memory,
                              uint32_t location, int32_t offset, unsigned width,
                              unsigned result_reg) {
    width = ((width - 1u) & 31u) + 1u;
    const uint32_t mask = UINT32_MAX >> (32u - width);
    const uint32_t insert = cpu->d[result_reg] & mask;
    uint32_t field, original = 0;
    uint64_t window = 0;
    unsigned shift = 0, bytes = 0;
    if (memory) {
        /* Floor division without signed shifts or INT_MIN subtraction. */
        location += (uint32_t)((int64_t)offset / 8 - (offset < 0 && (offset & 7)));
        unsigned bit = (uint32_t)offset & 7u;
        bytes = (bit + width + 7u) / 8u;
        if (bytes == 1) window = f3_read8(cpu, location);
        else if (bytes < 4) {
            window = f3_read16(cpu, location);
            if (bytes == 3) window = (window << 8) | f3_read8(cpu, location + 2);
        } else {
            window = f3_read32(cpu, location);
            if (bytes == 5) window = (window << 8) | f3_read8(cpu, location + 4);
        }
        shift = bytes * 8u - bit - width;
        field = (uint32_t)(window >> shift) & mask;
    } else {
        offset = (uint32_t)offset & 31u;
        original = cpu->d[location];
        field = f3_bf_rotate(original, (unsigned)offset) >> (32u - width);
    }
    const uint32_t flags = operation == 7 ? insert : field;
    f3_cc_flush(cpu);
    cpu->sr = (uint16_t)((cpu->sr & ~15u) | (flags == 0 ? 4u : 0u) |
                        ((flags >> (width - 1u)) & 1u) * 8u);
    if (operation == 1) cpu->d[result_reg] = field;                 /* EXTU */
    else if (operation == 3) {                                    /* EXTS */
        uint32_t sign = UINT32_C(1) << (width - 1u);
        cpu->d[result_reg] = (field ^ sign) - sign;
    } else if (operation == 5) {                                  /* FFO */
        unsigned leading = 0;
        while (leading < width && !(field & (UINT32_C(1) << (width - leading - 1u)))) ++leading;
        cpu->d[result_reg] = (uint32_t)offset + leading;
    } else if (operation == 2 || operation == 4 || operation == 6 || operation == 7) {
        uint32_t replacement = operation == 2 ? field ^ mask :
                               operation == 4 ? 0u : operation == 6 ? mask : insert;
        if (memory) {
            window = (window & ~((uint64_t)mask << shift)) | ((uint64_t)replacement << shift);
            if (bytes == 1) f3_write8(cpu, location, (uint8_t)window);
            else if (bytes < 4) {
                f3_write16(cpu, location, (uint16_t)(window >> (bytes == 3 ? 8 : 0)));
                if (bytes == 3) f3_write8(cpu, location + 2, (uint8_t)window);
            } else {
                f3_write32(cpu, location, (uint32_t)(window >> (bytes == 5 ? 8 : 0)));
                if (bytes == 5) f3_write8(cpu, location + 4, (uint8_t)window);
            }
        } else {
            unsigned rotate = (0u - (unsigned)offset) & 31u;
            uint32_t positioned_mask = f3_bf_rotate(mask << (32u - width), rotate);
            cpu->d[location] = (original & ~positioned_mask) |
                              f3_bf_rotate(replacement << (32u - width), rotate);
        }
    }
}

#endif
