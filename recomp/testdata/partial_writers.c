int main(void) {
    f3_cpu cpu = fresh_cpu();
    cpu.pc = 0x400; cpu.a[7] = 0x1000; cpu.dispatch_deadline = UINT64_MAX;
    cpu.d[0] = 0xffff; cpu.d[1] = 1; cpu.d[4] = 0;
    memory[0x1000] = memory[0x1001] = memory[0x1002] = memory[0x1003] = 0;
    cpu.cc_op = F3_CC_OP_LOGIC;                        /* pending state from before */
    assert(dispatch(&cpu));
    assert(cpu.d[4] == 1);                             /* X from the ADD rotated in */
    return 0;
}
