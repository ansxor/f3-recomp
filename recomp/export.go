package main

type EA struct {
	IsReg         bool
	RegType       string
	RegNum        int
	IsImm         bool
	ImmVal        int
	IsMem         bool
	EASetup       []string
	EAExpr        string
	PostStep      []string
	ReadStmts     []string
	ValExpr       string
	WriteLowFirst bool
}

func (i *Insn) DecodeEA(op *Operand, size int, prefix string, postIncOnRead bool) *EA {
	e := decodeEA(i, op, size, prefix, postIncOnRead)
	if e == nil {
		return nil
	}
	return &EA{
		IsReg:         e.isReg,
		RegType:       e.regType,
		RegNum:        e.regNum,
		IsImm:         e.isImm,
		ImmVal:        e.immVal,
		IsMem:         e.isMem,
		EASetup:       e.eaSetup,
		EAExpr:        e.eaExpr,
		PostStep:      e.postStep,
		ReadStmts:     e.readStmts,
		ValExpr:       e.valExpr,
		WriteLowFirst: e.writeLowFirst,
	}
}
