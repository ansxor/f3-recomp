static void seed_tables(f3_cpu *cpu) {
    /* Word-offset table read by MOVE.W at 0x3fe, longword table by MOVEA.L. */
    f3_write16(cpu, 0x3f0, 0x0016);
    f3_write16(cpu, 0x3f2, 0x0110);
    f3_write16(cpu, 0x3f4, 0x0212);
    f3_write32(cpu, 0x60a, 0x00000606);
    f3_write32(cpu, 0x60e, 0x00000500);
}
int main(void) {
    /* MOVE.W/JMP $3f0(pc,d0.w): index 0 -> 0x406 (same function), 2 -> 0x500. */
    f3_cpu chained = fresh_cpu(), stepped = fresh_cpu();
    seed_tables(&chained);
    chained.pc = 0x3fe; chained.d[0] = 0; chained.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&chained));                  /* one call runs the whole chain */
    assert(chained.pc == 0x40c);
    f3_cc_flush(&chained);
    assert(chained.d[1] == 0x21 && chained.d[0] == 0x16 && chained.cc_op == 0);
    seed_tables(&stepped);
    stepped.pc = 0x3fe; stepped.d[0] = 0; stepped.dispatch_deadline = 0;
    int calls = 0;
    while (stepped.pc != 0x40c && calls < 32) { assert(dispatch(&stepped)); ++calls; }
    assert(stepped.pc == 0x40c && calls > 1);
    assert(stepped.d[1] == chained.d[1] && stepped.d[0] == chained.d[0]);
    assert(stepped.cycles == chained.cycles);

    f3_cpu crossed = fresh_cpu();
    seed_tables(&crossed);
    crossed.pc = 0x3fe; crossed.d[0] = 2; crossed.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&crossed));
    assert(crossed.pc == 0x502 && crossed.d[2] == 0x27);
    assert(crossed.d[1] == 0);                   /* 0x406 never ran */

    /* MOVEA.L $60a(pc,d0.w),A0; JMP (A0) stages the same two targets. */
    f3_cpu staged = fresh_cpu(), staged_stepped = fresh_cpu();
    seed_tables(&staged);
    staged.pc = 0x600; staged.d[0] = 0; staged.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&staged));
    assert(staged.pc == 0x60a && staged.a[0] == 0x606 && staged.d[1] == 0x21);
    seed_tables(&staged_stepped);
    staged_stepped.pc = 0x600; staged_stepped.d[0] = 0;
    staged_stepped.dispatch_deadline = 0;
    calls = 0;
    while (staged_stepped.pc != 0x60a && calls < 32) {
        assert(dispatch(&staged_stepped)); ++calls;
    }
    assert(staged_stepped.pc == 0x60a && staged_stepped.a[0] == staged.a[0]);
    assert(staged_stepped.d[1] == staged.d[1] && staged_stepped.cycles == staged.cycles);

    f3_cpu staged_cross = fresh_cpu();
    seed_tables(&staged_cross);
    staged_cross.pc = 0x600; staged_cross.d[0] = 4;
    staged_cross.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&staged_cross));
    assert(staged_cross.pc == 0x502 && staged_cross.a[0] == 0x500);
    assert(staged_cross.d[2] == 0x27 && staged_cross.d[1] == 0);
    return 0;
}
