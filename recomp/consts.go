package main

/*
#cgo pkg-config: capstone
#if __has_include(<capstone/capstone.h>)
#include <capstone/capstone.h>
#else
#include <capstone.h>
#endif
*/
import "C"

type Mode uint32

const (
	ModeBigEndian Mode = Mode(C.CS_MODE_BIG_ENDIAN)
	Mode68000     Mode = Mode(C.CS_MODE_M68K_000)
	Mode68020     Mode = Mode(C.CS_MODE_M68K_020)
	Mode68040     Mode = Mode(C.CS_MODE_M68K_040)
)

type Reg uint32

const (
	RegD0  Reg = Reg(C.M68K_REG_D0)
	RegD1  Reg = Reg(C.M68K_REG_D1)
	RegD2  Reg = Reg(C.M68K_REG_D2)
	RegD3  Reg = Reg(C.M68K_REG_D3)
	RegD4  Reg = Reg(C.M68K_REG_D4)
	RegD5  Reg = Reg(C.M68K_REG_D5)
	RegD6  Reg = Reg(C.M68K_REG_D6)
	RegD7  Reg = Reg(C.M68K_REG_D7)
	RegA0  Reg = Reg(C.M68K_REG_A0)
	RegA1  Reg = Reg(C.M68K_REG_A1)
	RegA2  Reg = Reg(C.M68K_REG_A2)
	RegA3  Reg = Reg(C.M68K_REG_A3)
	RegA4  Reg = Reg(C.M68K_REG_A4)
	RegA5  Reg = Reg(C.M68K_REG_A5)
	RegA6  Reg = Reg(C.M68K_REG_A6)
	RegA7  Reg = Reg(C.M68K_REG_A7)
	RegPC  Reg = Reg(C.M68K_REG_PC)
	RegSR  Reg = Reg(C.M68K_REG_SR)
	RegCCR Reg = Reg(C.M68K_REG_CCR)
	RegUSP Reg = Reg(C.M68K_REG_USP)
)

type OpType uint32

const (
	OpReg      OpType = OpType(C.M68K_OP_REG)
	OpImm      OpType = OpType(C.M68K_OP_IMM)
	OpMem      OpType = OpType(C.M68K_OP_MEM)
	OpFPSingle OpType = OpType(C.M68K_OP_FP_SINGLE)
	OpFPDouble OpType = OpType(C.M68K_OP_FP_DOUBLE)
	OpRegBits  OpType = OpType(C.M68K_OP_REG_BITS)
	OpRegPair  OpType = OpType(C.M68K_OP_REG_PAIR)
	OpBrDisp   OpType = OpType(C.M68K_OP_BR_DISP)
)

type AddressMode uint32

const (
	AMNone               AddressMode = AddressMode(C.M68K_AM_NONE)
	AMRegDirectData      AddressMode = AddressMode(C.M68K_AM_REG_DIRECT_DATA)
	AMRegDirectAddr      AddressMode = AddressMode(C.M68K_AM_REG_DIRECT_ADDR)
	AMRegIAddr           AddressMode = AddressMode(C.M68K_AM_REGI_ADDR)
	AMRegIAddrPostInc    AddressMode = AddressMode(C.M68K_AM_REGI_ADDR_POST_INC)
	AMRegIAddrPreDec     AddressMode = AddressMode(C.M68K_AM_REGI_ADDR_PRE_DEC)
	AMRegIAddrDisp       AddressMode = AddressMode(C.M68K_AM_REGI_ADDR_DISP)
	AMARegIIndex8BitDisp AddressMode = AddressMode(C.M68K_AM_AREGI_INDEX_8_BIT_DISP)
	AMARegIIndexBaseDisp AddressMode = AddressMode(C.M68K_AM_AREGI_INDEX_BASE_DISP)
	AMMemIPostIndex      AddressMode = AddressMode(C.M68K_AM_MEMI_POST_INDEX)
	AMMemIPreIndex       AddressMode = AddressMode(C.M68K_AM_MEMI_PRE_INDEX)
	AMPCIDisp            AddressMode = AddressMode(C.M68K_AM_PCI_DISP)
	AMPCIIndex8BitDisp   AddressMode = AddressMode(C.M68K_AM_PCI_INDEX_8_BIT_DISP)
	AMPCIIndexBaseDisp   AddressMode = AddressMode(C.M68K_AM_PCI_INDEX_BASE_DISP)
	AMPCMemIPostIndex    AddressMode = AddressMode(C.M68K_AM_PC_MEMI_POST_INDEX)
	AMPCMemIPreIndex     AddressMode = AddressMode(C.M68K_AM_PC_MEMI_PRE_INDEX)
	AMAbsoluteDataShort  AddressMode = AddressMode(C.M68K_AM_ABSOLUTE_DATA_SHORT)
	AMAbsoluteDataLong   AddressMode = AddressMode(C.M68K_AM_ABSOLUTE_DATA_LONG)
	AMImmediate          AddressMode = AddressMode(C.M68K_AM_IMMEDIATE)
	AMBranchDisplacement AddressMode = AddressMode(C.M68K_AM_BRANCH_DISPLACEMENT)
)

type SizeType uint32

const (
	SizeCPU SizeType = SizeType(C.M68K_SIZE_TYPE_CPU)
	SizeFPU SizeType = SizeType(C.M68K_SIZE_TYPE_FPU)
)
