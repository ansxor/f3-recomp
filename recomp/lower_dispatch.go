package main

import (
	"fmt"
	"slices"
)

type mainHandler struct {
	match func(c *lowerCtx) bool
	lower func(c *lowerCtx) ([]string, bool)
}

func mnemIn(names ...string) func(c *lowerCtx) bool {
	return func(c *lowerCtx) bool { return slices.Contains(names, c.mnem) }
}

var mainHandlers = []mainHandler{
	{mnemIn("nop"), lowerNop},
	{mnemIn("bra"), lowerBra},
	{mnemIn("bsr"), lowerBsr},
	{matchBcc, lowerBcc},
	{matchDBcc, lowerDBcc},
	{matchScc, lowerScc},
	{mnemIn("jmp"), lowerJmp},
	{mnemIn("jsr"), lowerJsr},
	{mnemIn("rts"), lowerRts},
	{mnemIn("rtd"), lowerRtd},
	{mnemIn("rtr"), lowerRtr},
	{mnemIn("lea"), lowerLea},
	{mnemIn("pea"), lowerPea},
	{mnemIn("link"), lowerLink},
	{mnemIn("unlk"), lowerUnlk},
	{mnemIn("ext", "extb", "swap"), lowerExt},
	{mnemIn("moveq"), lowerMoveq},
	{func(c *lowerCtx) bool { return c.mnem == "exg" && len(c.ops) == 2 }, lowerExg},
	{mnemIn("move", "movea"), lowerMove},
	{mnemIn("movem"), lowerMovem},
	{mnemIn("clr"), lowerClr},
	{mnemIn("tst"), lowerTst},
	{mnemIn("add", "adda", "addi", "addq"), lowerAdd},
	{mnemIn("sub", "suba", "subi", "subq"), lowerSub},
	{mnemIn("cmpm"), lowerCmpm},
	{mnemIn("cmp", "cmpa", "cmpi"), lowerCmp},
	{mnemIn("neg", "negx"), lowerNeg},
	{mnemIn("addx", "subx"), lowerAddxSubx},
	{mnemIn("and", "andi", "or", "ori", "eor", "eori", "not"), lowerLogic},
	{mnemIn("btst", "bset", "bclr", "bchg"), lowerBitOp},
	{mnemIn("asl", "asr", "lsl", "lsr", "rol", "ror", "roxl", "roxr"), lowerShift},
	{mnemIn("mulu", "muls"), lowerMul},
	{mnemIn("divu", "divs"), lowerDiv},
	{mnemIn("trap"), lowerTrap},
	{mnemIn("stop"), lowerStop},
}

func lowerMain(c *lowerCtx) ([]string, bool) {
	for _, h := range mainHandlers {
		if h.match(c) {
			return h.lower(c)
		}
	}
	return nil, false
}

func isDataReg(r Reg) bool { return r >= RegD0 && r <= RegD7 }

func isAddrReg(r Reg) bool { return r >= RegA0 && r <= RegA7 }

func dataRegNum(r Reg) int { return int(r) - int(RegD0) }

func addrRegNum(r Reg) int { return int(r) - int(RegA0) }

func pcCycles(c *lowerCtx) []string {
	return []string{
		fmt.Sprintf("cpu->pc = 0x%08xu;", c.nextPC),
		fmt.Sprintf("cpu->cycles += %s;", c.cycles),
	}
}

func branchTarget(c *lowerCtx, op *Operand) int {
	return (int(c.insn.Address) + 2 + int(op.BrDisp.Disp)) & 0xFFFFFFFF
}

func maskLiteral(size int) string { return fmt.Sprintf("0x%xu", maskMap[size]) }

func signLiteral(size int) string { return fmt.Sprintf("0x%xu", signMap[size]) }

func addrOperandCast(size int) string {
	if size == 2 {
		return "(uint32_t)(int32_t)(int16_t)"
	}
	return "(uint32_t)"
}
