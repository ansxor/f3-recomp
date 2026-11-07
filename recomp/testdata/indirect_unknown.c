static void seed_tables(f3_cpu *cpu) {
    f3_write16(cpu, 0x3f0, 0x0016);
    f3_write16(cpu, 0x3f2, 0x0110);
    f3_write16(cpu, 0x3f4, 0x0000);
    f3_write32(cpu, 0x60a, 0x00000606);
    f3_write32(cpu, 0x60e, 0x00000500);
}
int main(void) {
    /* Index 4 loads offset 0: pc 0x3f0 is not a resolved case, so the transfer
       returns to the dispatcher with the MOVE.W flags materialised. */
    f3_cpu fallback = fresh_cpu();
    seed_tables(&fallback);
    fallback.pc = 0x3fe; fallback.d[0] = 4; fallback.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&fallback));
    assert(fallback.pc == 0x3f0 && fallback.d[0] == 0);
    f3_cc_flush(&fallback);
    assert(fallback.cc_op == 0 && (fallback.sr & 0xfu) == 4);   /* Z, no lazy flags */

    f3_cpu traced = fresh_cpu();
    traced.pc = 0x402; traced.d[0] = 0x16; traced.sr = 0x8000;
    traced.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&traced));
    f3_cc_flush(&traced);
    assert(traced.pc == 0x406 && traced.cc_op == 0);
    return 0;
}
