// license:BSD-3-Clause
// copyright-holders:Christian Brunschen
/**********************************************************************************************
 *
 *   es5510.hpp - Ensoniq ES5510 (ESP) Digital Signal Processor
 *   Standalone implementation for f3rt
 *
 **********************************************************************************************/

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace f3rt {
class StateWriter;
class StateReader;

class ES5510 {
public:
    static constexpr uint32_t DRAM_SIZE = (1 << 20); // 1M 16-bit words
    static constexpr uint32_t DRAM_MASK = (DRAM_SIZE - 1);

    enum State {
        STATE_RUNNING = 0,
        STATE_HALTED = 1
    };

    enum OpSrcDst {
        SRC_DST_REG =   1 << 0,
        SRC_DST_DELAY = 1 << 1,
        SRC_DST_BOTH =  (1 << 0) | (1 << 1)
    };

    enum RamControlAccess {
        RAM_CONTROL_DELAY = 0,
        RAM_CONTROL_TABLE_A,
        RAM_CONTROL_TABLE_B,
        RAM_CONTROL_IO
    };

    enum RamCycle {
        RAM_CYCLE_READ = 0,
        RAM_CYCLE_WRITE = 1,
        RAM_CYCLE_DUMP_FIFO = 2
    };

    struct AluOp {
        int operands;
        const char *opcode;
    };

    struct OpSelect {
        OpSrcDst alu_src;
        OpSrcDst alu_dst;
        OpSrcDst mac_src;
        OpSrcDst mac_dst;
    };

    struct RamControl {
        RamCycle cycle;
        RamControlAccess access;
        const char *description;
    };

    struct AluState {
        uint8_t aReg;
        uint8_t bReg;
        OpSrcDst src;
        OpSrcDst dst;
        uint8_t op;
        int32_t aValue;
        int32_t bValue;
        int32_t result;
        bool update_ccr;
        bool write_result;
    };

    struct MulAccState {
        uint8_t cReg;
        uint8_t dReg;
        OpSrcDst src;
        OpSrcDst dst;
        bool accumulate;
        int32_t cValue;
        int32_t dValue;
        int64_t product;
        int64_t result;
        bool write_result;
    };

    struct RamState {
        int32_t address;
        bool io;
        RamCycle cycle;
    };

    ES5510();
    ~ES5510();

    void reset();

    // Host interface (sound CPU bus)
    uint8_t host_r(uint32_t offset);
    void host_w(uint32_t offset, uint8_t data);

    // Serial audio samples I/O
    int16_t ser_r(int offset) const;
    void ser_w(int offset, int16_t data);

    // Halt control
    void set_HALT(bool halt) { halt_asserted = halt; }
    bool get_HALT() const { return halt_asserted; }
    State get_state() const { return state; }

    // Run one sample frame of ESP processing
    void run_once();

    // Public register access
    int32_t read_reg(uint8_t reg);
    void write_reg(uint8_t reg, int32_t value);
    void write_to_dol(int32_t value);

    uint64_t &instr_at(int pc) { return m_instr[pc % 160]; }
    int16_t &dram_at(int addr) { return m_dram[addr & DRAM_MASK]; }
    size_t state_size() const;
    void save_state(StateWriter &writer) const;
    void load_state(StateReader &reader);

private:
    int32_t alu_operation(uint8_t op, int32_t aValue, int32_t bValue, uint8_t &flags);
    void alu_operation_end();
    void execute_run(int cycles);

    int16_t dram_r(int addr) const { return m_dram[addr & DRAM_MASK]; }
    void dram_w(int addr, int16_t data) { m_dram[addr & DRAM_MASK] = data; }

    bool halt_asserted;
    uint8_t pc;
    State state;

    std::unique_ptr<int32_t[]> m_gpr;     // 192 (0xc0) 24-bit registers
    std::unique_ptr<uint64_t[]> m_instr;  // 160 48-bit instructions
    std::unique_ptr<int16_t[]> m_dram;    // DRAM delay memory

    int16_t ser0r, ser0l;
    int16_t ser1r, ser1l;
    int16_t ser2r, ser2l;
    int16_t ser3r, ser3l;

    int64_t machl;
    bool mac_overflow;
    int16_t dil;
    int32_t memsiz;
    int32_t memmask;
    int32_t memincrement;
    int8_t memshift;
    int32_t dlength;
    int32_t abase;
    int32_t bbase;
    int32_t dbase;
    int32_t sigreg;
    int mulshift;
    int8_t ccr;
    int8_t cmr;
    int16_t dol[2];
    int dol_count;

    // Host latches
    int32_t  dol_latch;
    int32_t  dil_latch;
    uint32_t dadr_latch;
    int32_t  gpr_latch;
    uint64_t instr_latch;
    uint8_t  ram_sel;
    uint8_t  host_control;
    uint8_t  host_serial;

    AluState alu;
    MulAccState mulacc;
    RamState ram, ram_p, ram_pp;
};

} // namespace f3rt
