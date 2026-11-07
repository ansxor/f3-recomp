package main

type lowerCtx struct {
	insn    *Insn
	ops     []Operand
	mnem    string
	size    int
	opcode  int
	nextPC  int
	cycles  string
	liveOut int
}

type ea struct {
	isReg         bool
	regType       string
	regNum        int
	isImm         bool
	immVal        int
	isMem         bool
	eaSetup       []string
	eaExpr        string
	postStep      []string
	readStmts     []string
	valExpr       string
	writeLowFirst bool
}
