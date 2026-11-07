int main(void) {
    f3_cpu cpu = fresh_cpu();
    cpu.pc = 0x400; cpu.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&cpu));
    assert(cpu.pc == 0x500 && cpu.d[1] == 0);
    return 0;
}
