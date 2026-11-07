package main

/*
#cgo pkg-config: capstone
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#if __has_include(<capstone/capstone.h>)
#include <capstone/capstone.h>
#else
#include <capstone.h>
#endif

// shim_disasm_one decodes one instruction. cs_disasm_iter advances its cursor
// arguments, so they are copied to locals first.
static bool shim_disasm_one(csh handle, const uint8_t *code, size_t size,
	uint64_t addr, cs_insn *insn) {
	const uint8_t *p = code;
	size_t n = size;
	uint64_t a = addr;
	return cs_disasm_iter(handle, &p, &n, &a, insn);
}

static cs_m68k *shim_detail(const cs_insn *insn) {
	return insn->detail ? &insn->detail->m68k : NULL;
}

typedef struct {
	uint64_t imm;
	uint32_t reg;
	uint32_t reg0;
	uint32_t reg1;
} shim_op_value_t;

// shim_op_value_of reads the union behind cs_m68k_op. Go cannot name its
// members.
static shim_op_value_t shim_op_value_of(const cs_m68k_op *op) {
	shim_op_value_t v;
	v.imm = op->imm;
	v.reg = op->reg;
	v.reg0 = op->reg_pair.reg_0;
	v.reg1 = op->reg_pair.reg_1;
	return v;
}

// shim_opsize reads the size union. cpu_size and fpu_size share storage.
static uint32_t shim_opsize(const cs_m68k *m) {
	return m->op_size.cpu_size;
}
*/
import "C"

import (
	"errors"
	"unsafe"
)

// Insn is one decoded instruction. Its slices and strings are Go copies, so it
// stays valid after the Decoder decodes again. ID 0 is the dc.w data
// pseudo-instruction, which has no operands and no operand size.
type Insn struct {
	ID       uint32
	Address  uint64
	Size     uint16
	Bytes    []byte
	Mnemonic string
	OpStr    string
	OpSize   OpSize
	Operands []Operand
}

type OpSize struct {
	Type SizeType
	Size uint32
}

// Operand is one decoded operand (cs_m68k_op). Imm, Reg and RegPair are views
// of one 8-byte union, so only the field that matches Type is meaningful. Imm
// is the raw 64-bit value. FP immediates hold their IEEE bits: single in the
// low 32, double in all 64.
type Operand struct {
	Type         OpType
	AddressMode  AddressMode
	RegisterBits uint32
	Imm          int64
	Reg          Reg
	RegPair      [2]Reg
	Mem          Mem
	BrDisp       BrDisp
}

// Mem is a memory operand (m68k_op_mem). Register fields are zero when unused.
// Names follow Capstone: in_disp is InnerDisp and out_disp is OuterDisp.
type Mem struct {
	BaseReg   Reg
	IndexReg  Reg
	InBaseReg Reg
	InnerDisp uint32
	OuterDisp uint32
	Disp      int16
	Scale     uint8
	Bitfield  uint8
	Width     uint8
	Offset    uint8
	IndexSize uint8
}

type BrDisp struct {
	Disp     int32
	DispSize uint8
}

// Decoder is a detailed Capstone M68K handle with one reusable instruction
// buffer. It is not safe for concurrent use. Close releases the C memory.
type Machine struct {
	handle C.csh
	insn   *C.cs_insn
}

var errClosed = errors.New("capstone: decoder is closed")

func OpenMachine(mode Mode) (*Machine, error) {
	var h C.csh
	if e := C.cs_open(C.CS_ARCH_M68K, C.cs_mode(mode), &h); e != C.CS_ERR_OK {
		return nil, capstoneError(e)
	}
	if e := C.cs_option(h, C.CS_OPT_DETAIL, C.CS_OPT_ON); e != C.CS_ERR_OK {
		C.cs_close(&h)
		return nil, capstoneError(e)
	}
	insn := C.cs_malloc(h)
	if insn == nil {
		C.cs_close(&h)
		return nil, capstoneError(C.CS_ERR_MEM)
	}
	return &Machine{handle: h, insn: insn}, nil
}

// Close frees the instruction buffer and the Capstone handle. Close is
// idempotent.
func (d *Machine) Close() error {
	if d.insn == nil {
		return nil
	}
	C.cs_free(d.insn, 1)
	h := d.handle
	d.insn, d.handle = nil, 0
	if e := C.cs_close(&h); e != C.CS_ERR_OK {
		return capstoneError(e)
	}
	return nil
}

// Decode decodes the single instruction at the start of code, which sits at
// address addr. ok is false when code holds no complete instruction, including
// empty input. err is set only when Capstone reports an error. To walk a
// buffer, advance code by Insn.Size and call Decode again.
func (d *Machine) Decode(code []byte, addr uint64) (Insn, bool, error) {
	if d.insn == nil {
		return Insn{}, false, errClosed
	}
	var p *C.uint8_t
	if len(code) > 0 {
		p = (*C.uint8_t)(unsafe.Pointer(&code[0]))
	}
	if !C.shim_disasm_one(d.handle, p, C.size_t(len(code)), C.uint64_t(addr), d.insn) {
		if e := C.cs_errno(d.handle); e != C.CS_ERR_OK {
			return Insn{}, false, capstoneError(e)
		}
		return Insn{}, false, nil
	}
	ci := d.insn
	out := Insn{
		ID:       uint32(ci.id),
		Address:  uint64(ci.address),
		Size:     uint16(ci.size),
		Bytes:    C.GoBytes(unsafe.Pointer(&ci.bytes[0]), C.int(ci.size)),
		Mnemonic: C.GoString(&ci.mnemonic[0]),
		OpStr:    C.GoString(&ci.op_str[0]),
	}
	if m := C.shim_detail(ci); m != nil && ci.id != 0 {
		out.OpSize = OpSize{Type: SizeType(m.op_size._type), Size: uint32(C.shim_opsize(m))}
		out.Operands = operands(m)
	}
	return out, true, nil
}

func (d *Machine) RegName(r Reg) string {
	if d.insn == nil {
		return ""
	}
	s := C.cs_reg_name(d.handle, C.uint(r))
	if s == nil {
		return ""
	}
	return C.GoString(s)
}

func operands(m *C.cs_m68k) []Operand {
	n := int(m.op_count)
	if n > len(m.operands) {
		n = len(m.operands)
	}
	if n == 0 {
		return nil
	}
	ops := make([]Operand, n)
	for i := range ops {
		src := &m.operands[i]
		v := C.shim_op_value_of(src)
		mem := &src.mem
		ops[i] = Operand{
			Type:         OpType(src._type),
			AddressMode:  AddressMode(src.address_mode),
			RegisterBits: uint32(src.register_bits),
			Imm:          int64(v.imm),
			Reg:          Reg(v.reg),
			RegPair:      [2]Reg{Reg(v.reg0), Reg(v.reg1)},
			Mem: Mem{
				BaseReg:   Reg(mem.base_reg),
				IndexReg:  Reg(mem.index_reg),
				InBaseReg: Reg(mem.in_base_reg),
				InnerDisp: uint32(mem.in_disp),
				OuterDisp: uint32(mem.out_disp),
				Disp:      int16(mem.disp),
				Scale:     uint8(mem.scale),
				Bitfield:  uint8(mem.bitfield),
				Width:     uint8(mem.width),
				Offset:    uint8(mem.offset),
				IndexSize: uint8(mem.index_size),
			},
			BrDisp: BrDisp{Disp: int32(src.br_disp.disp), DispSize: uint8(src.br_disp.disp_size)},
		}
	}
	return ops
}

func capstoneError(e C.cs_err) error {
	return errors.New("capstone: " + C.GoString(C.cs_strerror(e)))
}
