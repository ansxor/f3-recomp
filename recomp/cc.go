package main

import "fmt"

const (
	CCRC    = 0x01
	CCRV    = 0x02
	CCRZ    = 0x04
	CCRN    = 0x08
	CCRX    = 0x10
	CCRNZVC = CCRN | CCRZ | CCRV | CCRC
	CCRAll  = CCRX | CCRNZVC
)

var CondMap = map[string]int{
	"t":  0,
	"f":  1,
	"ra": 1,
	"hi": 2,
	"ls": 3,
	"cc": 4,
	"hs": 4,
	"cs": 5,
	"lo": 5,
	"ne": 6,
	"eq": 7,
	"vc": 8,
	"vs": 9,
	"pl": 10,
	"mi": 11,
	"ge": 12,
	"lt": 13,
	"gt": 14,
	"le": 15,
}

var maskMap = map[int]int{1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}
var signMap = map[int]int{1: 0x80, 2: 0x8000, 4: 0x80000000}

func ccLogic(liveOut int, result string, width int) []string {
	if liveOut&CCRNZVC == 0 {
		return []string{}
	}
	return []string{fmt.Sprintf("cpu->cc_op = F3_CC_OP_LOGIC; cpu->cc_result = %s; cpu->cc_width = %d;", result, width)}
}

func ccCmp(liveOut int, src, dst, result string, width int) []string {
	if liveOut&CCRNZVC == 0 {
		return []string{}
	}
	return []string{fmt.Sprintf(
		"cpu->cc_op = F3_CC_OP_CMP; cpu->cc_src = %s; cpu->cc_dst = %s; cpu->cc_result = %s; cpu->cc_width = %d;",
		src, dst, result, width)}
}

func ccArith(liveOut int, op, src, dst, result string, width int, carry string) []string {
	stmts := []string{}
	if liveOut&CCRX != 0 {
		stmts = append(stmts, fmt.Sprintf("cpu->sr = (cpu->sr & ~0x10u) | ((%s) ? 0x10u : 0u);", carry))
	}
	if liveOut&CCRNZVC != 0 {
		stmts = append(stmts, fmt.Sprintf(
			"cpu->cc_op = F3_CC_OP_%s; cpu->cc_src = %s; cpu->cc_dst = %s; cpu->cc_result = %s; cpu->cc_width = %d;",
			op, src, dst, result, width))
	} else if liveOut&CCRX != 0 {
		stmts = append(stmts, "cpu->cc_op = F3_CC_OP_NONE;")
	}
	return stmts
}

func discardValue(e *ea) []string {
	if e.isMem {
		return []string{fmt.Sprintf("(void)%s;", e.valExpr)}
	}
	return []string{}
}
