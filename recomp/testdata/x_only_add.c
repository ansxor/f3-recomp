int main(void) {
    f3_cpu cpu = fresh_cpu();
    cpu.pc = 0x400; cpu.dispatch_deadline = UINT64_MAX; cpu.sr = 0x10;
    cpu.cc_op = F3_CC_OP_SUB; cpu.cc_src = 2; cpu.cc_dst = 1;
    cpu.cc_result = 0xffffffffu; cpu.cc_width = 4;     /* flushes to borrow: X=1 */
    cpu.d[0] = 1; cpu.d[1] = 2; cpu.d[3] = 5; cpu.d[4] = 10;
    while (dispatch(&cpu)) {}
    assert(cpu.d[4] == 15);                            /* 1+2 does not carry */
    cpu.pc = 0x400; cpu.sr = 0; cpu.cc_op = 0;
    cpu.d[0] = 0xffffffffu; cpu.d[1] = 1; cpu.d[4] = 10;
    while (dispatch(&cpu)) {}
    assert(cpu.d[4] == 16);                            /* carry reaches ADDX */
    return 0;
}
