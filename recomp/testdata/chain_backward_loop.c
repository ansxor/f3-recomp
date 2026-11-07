int main(void) {
    f3_cpu chained = fresh_cpu(), stepped = fresh_cpu();
    chained.pc = 0x400; chained.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&chained));                    /* one call runs the whole loop */
    assert(chained.pc == 0x40a);
    f3_cc_flush(&chained);
    assert(chained.d[0] == 0x0000ffffu && chained.d[1] == 7 && chained.cc_op == 0);
    stepped.pc = 0x400; stepped.dispatch_deadline = 0;
    int calls = 0;
    while (stepped.pc != 0x40a) { assert(dispatch(&stepped)); ++calls; }
    assert(calls == 12);                           /* 5 NOP + 5 DBRA + 2 MOVEQ */
    assert(stepped.d[0] == chained.d[0] && stepped.d[1] == chained.d[1]);
    assert(chained.cycles == stepped.cycles);
    return 0;
}
