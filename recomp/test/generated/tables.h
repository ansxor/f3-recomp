static void seed_tables(f3_cpu *cpu) {
    /* Word-offset table read by MOVE.W at 0x3fe, longword table by MOVEA.L. */
    f3_write16(cpu, 0x3f0, 0x0016);
    f3_write16(cpu, 0x3f2, 0x0110);
    f3_write16(cpu, 0x3f4, 0x0212);
    f3_write32(cpu, 0x60a, 0x00000606);
    f3_write32(cpu, 0x60e, 0x00000500);
}
