// Cycle-accurate 5-stage MIPS pipeline (IF, ID, EX, MEM, WB) with data-hazard
// handling, branches resolved in EX, and a data cache in the MEM stage.
#pragma once
#include <cstdint>
#include <deque>
#include <vector>

#include "cache.h"

enum class HazardMode {
    None,     // no detection at all: dependent instructions read stale values
    Stall,    // stall in ID until the producer has written the register
    Forward   // forward from EX/MEM and MEM/WB; stall one cycle on load-use
};

enum class AluOp : uint8_t { Add, Sub, And, Or, Xor, Nor, Slt, Sll, Srl, Lui };
const char* alu_name(AluOp op);
const char* alu_symbol(AluOp op);

enum class Fwd : uint8_t { None, ExMem, MemWb };

struct Config {
    HazardMode hazard = HazardMode::Forward;
    int miss_penalty = 0;      // extra cycles the pipeline freezes on a cache miss
    bool course_regs = false;  // reset registers to 0x100+i (course convention) instead of 0
};

// Identity of the instruction occupying a stage. `seq` is unique per fetch.
struct StageInfo {
    bool valid = false;
    uint64_t seq = 0;
    uint32_t pc = 0;
    uint32_t instr = 0;
};

struct IF_ID {
    StageInfo info;
};

struct ID_EX {
    StageInfo info;
    int rs = 0, rt = 0, dest = 0, shamt = 0;
    int32_t a = 0, b = 0, imm = 0;
    bool uses_rs = false, uses_rt = false;
    bool reg_write = false, mem_read = false, mem_write = false, mem_to_reg = false, alu_src = false;
    int branch = 0;  // 0 none, 1 beq, 2 bne
    bool jump = false;
    AluOp alu = AluOp::Add;
    int mem_size = 4;
    bool mem_signed = true;
};

struct EX_MEM {
    StageInfo info;
    int32_t alu_result = 0, store_value = 0;
    int dest = 0;
    bool reg_write = false, mem_read = false, mem_write = false, mem_to_reg = false;
    int mem_size = 4;
    bool mem_signed = true;
};

struct MEM_WB {
    StageInfo info;
    int32_t mem_data = 0, alu_result = 0;
    int dest = 0;
    bool reg_write = false, mem_to_reg = false;
};

struct MemAccess {
    bool happened = false, write = false, hit = false, wrote_back = false;
    uint32_t addr = 0;
    int slot = 0, size = 1;
    int32_t value = 0;
};

// Everything that happened during one clock cycle, for the UI.
struct Trace {
    uint64_t cycle = 0;
    StageInfo stage[5];             // instruction in IF, ID, EX, MEM, WB during this cycle
    bool squashed[5] = {};          // flushed at the end of this cycle
    bool stall_id = false;          // data-hazard stall: IF and ID held, bubble into EX
    int stall_reg = 0;
    bool mem_stall = false;         // whole pipeline frozen waiting on a cache miss
    int mem_stall_left = 0;
    bool jump = false;
    bool branch_resolved = false, branch_taken = false;
    uint32_t redirect = 0;          // new PC after a taken branch or jump
    Fwd fwd_a = Fwd::None, fwd_b = Fwd::None;
    bool ex_valid = false;
    AluOp alu = AluOp::Add;
    int32_t alu_a = 0, alu_b = 0, alu_result = 0;
    MemAccess mem;
    bool wb_write = false;
    int wb_reg = 0;
    int32_t wb_value = 0;
};

struct Stats {
    uint64_t cycles = 0, retired = 0;
    uint64_t data_stalls = 0, mem_stall_cycles = 0, flushed = 0, forwards = 0;
    uint64_t branches = 0, taken = 0;
    uint64_t hits = 0, misses = 0;
};

struct State {
    int32_t regs[32] = {};
    uint32_t pc = 0;
    IF_ID if_id;
    ID_EX id_ex;
    EX_MEM ex_mem;
    MEM_WB mem_wb;
    uint8_t mem[MEM_SIZE] = {};
    Cache cache;
    int miss_wait = -1;  // >0: cycles left to wait for the block, 0: ready, -1: idle
    uint64_t fetch_seq = 1, next_seq = 2;
    Stats stats;
    bool halted = false;
};

class Simulator {
public:
    Config cfg;

    Simulator() { reset(); }

    void load(const std::vector<uint32_t>& program);
    void reset();
    bool step();       // advance one clock cycle; false if already halted
    bool step_back();  // undo one cycle; false if there is nothing to undo
    bool preview(Trace& out);  // what the next cycle would do, without changing anything

    const State& state() const { return s_; }
    const std::vector<uint32_t>& program() const { return program_; }
    const std::deque<Trace>& history() const { return history_; }
    bool halted() const { return s_.halted; }
    bool can_step_back() const { return !undo_.empty(); }

    // Memory as the program sees it (cache contents take priority).
    uint8_t peek8(uint32_t addr) const;
    uint32_t peek32(uint32_t addr) const;

private:
    bool pc_valid(uint32_t pc) const { return pc % 4 == 0 && pc / 4 < program_.size(); }
    void update_halted();

    std::vector<uint32_t> program_;
    State s_;
    std::deque<State> undo_;
    std::deque<Trace> history_;
};
