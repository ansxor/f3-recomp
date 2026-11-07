#define PROGRAMS @PROGRAMS@
#define STATES @STATES@
static const uint32_t starts[PROGRAMS][@MAXLEN@] = {
@STARTS@
};
static const unsigned lengths[PROGRAMS] = {@LENGTHS@};
static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint32_t next(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}
static uint32_t value(void) {
    static const uint32_t edge[] = {0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff,
                                    0x7fffffffu, 0x80000000u, 0xffffffffu};
    return next() % 3 ? edge[next() % 11] : next();
}
static int same(const f3_cpu *a, const f3_cpu *b) {
    return a->pc == b->pc && a->sr == b->sr && a->usp == b->usp && a->cycles == b->cycles &&
        !memcmp(a->d, b->d, sizeof a->d) && !memcmp(a->a, b->a, sizeof a->a);
}
int main(void) {
    for (unsigned p = 0; p < PROGRAMS; ++p) for (unsigned k = 0; k < STATES; ++k) {
        f3_cpu a = {0};
        for (int i = 0; i < 8; ++i) { a.d[i] = value(); a.a[i] = value(); }
        a.sr = 0x2000u | (next() & 0x1fu);
        a.cc_op = (uint8_t)(next() % 5); a.cc_width = (uint8_t)(1u << (next() % 3));
        a.cc_src = value(); a.cc_dst = value();
        uint32_t mask = a.cc_width == 1 ? 0xffu : a.cc_width == 2 ? 0xffffu : 0xffffffffu;
        a.cc_result = a.cc_op == F3_CC_OP_ADD ? (a.cc_src + a.cc_dst) & mask :
                      a.cc_op == F3_CC_OP_LOGIC ? value() : (a.cc_dst - a.cc_src) & mask;
        if (a.cc_op == F3_CC_OP_ADD || a.cc_op == F3_CC_OP_SUB) {
            f3_cpu probe = a; f3_cc_flush(&probe);      /* eager X agrees with pending ADD/SUB */
            a.sr = (uint16_t)((a.sr & ~F3_CCR_X) | (probe.sr & F3_CCR_X));
        }
        a.pc = starts[p][next() % lengths[p]]; uint32_t entry = a.pc;
        f3_cpu b = a; uint64_t sum_a, sum_b;
        wsum = 0; run_exact(&a); sum_a = wsum; f3_cc_flush(&a);
        wsum = 0; run_elided(&b); sum_b = wsum; f3_cc_flush(&b);
        if (!same(&a, &b) || sum_a != sum_b) {
            printf("program %u entry %x\n", p, entry);
            printf("exact  pc=%x sr=%x d0=%x\n", a.pc, a.sr, a.d[0]);
            printf("elided pc=%x sr=%x d0=%x\n", b.pc, b.sr, b.d[0]);
            return 1;
        }
    }
    return 0;
}
