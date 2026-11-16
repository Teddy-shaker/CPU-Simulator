#include "cpu.h"

#include "isa.h"

namespace {

constexpr size_t MAX_HISTORY = 4096;

ID_EX decode(const StageInfo& in, const int32_t* regs) {
    ID_EX d;
    d.info = in;
    const isa::Fields f = isa::split(in.instr);
    d.rs = static_cast<int>(f.rs);
    d.rt = static_cast<int>(f.rt);
    d.shamt = static_cast<int>(f.shamt);

    auto rtype = [&](AluOp op) {
        d.alu = op;
        d.dest = static_cast<int>(f.rd);
        d.reg_write = d.uses_rs = d.uses_rt = true;
    };
    auto shift = [&](AluOp op) {
        d.alu = op;
        d.dest = static_cast<int>(f.rd);
        d.reg_write = d.uses_rt = true;
    };
    auto itype = [&](AluOp op, int32_t imm) {
        d.alu = op;
        d.dest = static_cast<int>(f.rt);
        d.reg_write = d.uses_rs = d.alu_src = true;
        d.imm = imm;
    };
    auto load = [&](int size, bool is_signed) {
        itype(AluOp::Add, f.simm);
        d.mem_read = d.mem_to_reg = true;
        d.mem_size = size;
        d.mem_signed = is_signed;
    };
    auto store = [&](int size) {
        d.alu = AluOp::Add;
        d.uses_rs = d.uses_rt = d.alu_src = d.mem_write = true;
        d.imm = f.simm;
        d.mem_size = size;
    };
    auto branch = [&](int kind) {
        d.alu = AluOp::Sub;
        d.uses_rs = d.uses_rt = true;
        d.imm = f.simm;
        d.branch = kind;
    };

    switch (f.op) {
    case isa::OP_RTYPE:
        switch (f.funct) {
        case isa::F_SLL: shift(AluOp::Sll); break;
        case isa::F_SRL: shift(AluOp::Srl); break;
        case isa::F_ADD: rtype(AluOp::Add); break;
        case isa::F_SUB: rtype(AluOp::Sub); break;
        case isa::F_AND: rtype(AluOp::And); break;
        case isa::F_OR:  rtype(AluOp::Or);  break;
        case isa::F_XOR: rtype(AluOp::Xor); break;
        case isa::F_NOR: rtype(AluOp::Nor); break;
        case isa::F_SLT: rtype(AluOp::Slt); break;
        default: break;  // unknown: flows through as a no-op
        }
        break;
    case isa::OP_ADDI: itype(AluOp::Add, f.simm); break;
    case isa::OP_SLTI: itype(AluOp::Slt, f.simm); break;
    case isa::OP_ANDI: itype(AluOp::And, static_cast<int32_t>(f.zimm)); break;
    case isa::OP_ORI:  itype(AluOp::Or,  static_cast<int32_t>(f.zimm)); break;
    case isa::OP_XORI: itype(AluOp::Xor, static_cast<int32_t>(f.zimm)); break;
    case isa::OP_LUI:
        itype(AluOp::Lui, static_cast<int32_t>(f.zimm));
        d.uses_rs = false;
        break;
    case isa::OP_LW:  load(4, true);  break;
    case isa::OP_LB:  load(1, true);  break;
    case isa::OP_LBU: load(1, false); break;
    case isa::OP_SW:  store(4); break;
    case isa::OP_SB:  store(1); break;
    case isa::OP_BEQ: branch(1); break;
    case isa::OP_BNE: branch(2); break;
    case isa::OP_J:   d.jump = true; break;
    default: break;
    }

    if (d.dest == 0) d.reg_write = false;  // $zero is never written
    d.a = regs[d.rs];
    d.b = regs[d.rt];
    return d;
}

int32_t run_alu(AluOp op, int32_t a, int32_t b, int shamt) {
    const uint32_t ua = static_cast<uint32_t>(a), ub = static_cast<uint32_t>(b);
    switch (op) {
    case AluOp::Add: return static_cast<int32_t>(ua + ub);
    case AluOp::Sub: return static_cast<int32_t>(ua - ub);
    case AluOp::And: return static_cast<int32_t>(ua & ub);
    case AluOp::Or:  return static_cast<int32_t>(ua | ub);
    case AluOp::Xor: return static_cast<int32_t>(ua ^ ub);
    case AluOp::Nor: return static_cast<int32_t>(~(ua | ub));
    case AluOp::Slt: return a < b ? 1 : 0;
    case AluOp::Sll: return static_cast<int32_t>(ub << shamt);
    case AluOp::Srl: return static_cast<int32_t>(ub >> shamt);
    case AluOp::Lui: return static_cast<int32_t>(ub << 16);
    }
    return 0;
}

int32_t wb_value(const MEM_WB& w) { return w.mem_to_reg ? w.mem_data : w.alu_result; }

}  // namespace

const char* alu_name(AluOp op) {
    static const char* const names[] = {"ADD", "SUB", "AND", "OR", "XOR", "NOR", "SLT", "SLL", "SRL", "LUI"};
    return names[static_cast<int>(op)];
}

const char* alu_symbol(AluOp op) {
    static const char* const symbols[] = {"+", "-", "&", "|", "^", "nor", "<", "<<", ">>", "<<"};
    return symbols[static_cast<int>(op)];
}

void Simulator::load(const std::vector<uint32_t>& program) {
    program_ = program;
    reset();
}

void Simulator::reset() {
    s_ = State{};
    for (int i = 0; i < MEM_SIZE; i++) s_.mem[i] = static_cast<uint8_t>(i % 256);
    if (cfg.course_regs)
        for (int i = 1; i < 32; i++) s_.regs[i] = 0x100 + i;
    undo_.clear();
    history_.clear();
    update_halted();
}

void Simulator::update_halted() {
    s_.halted = !pc_valid(s_.pc) && !s_.if_id.info.valid && !s_.id_ex.info.valid && !s_.ex_mem.info.valid &&
                !s_.mem_wb.info.valid;
}

uint8_t Simulator::peek8(uint32_t addr) const {
    addr &= ADDR_MASK;
    if (s_.cache.probe(addr)) return s_.cache.line[Cache::slot_of(addr)].data[Cache::offset_of(addr)];
    return s_.mem[addr];
}

uint32_t Simulator::peek32(uint32_t addr) const {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(peek8(addr + i)) << (8 * i);
    return v;
}

bool Simulator::step_back() {
    if (undo_.empty()) return false;
    s_ = undo_.back();
    undo_.pop_back();
    if (!history_.empty()) history_.pop_back();
    return true;
}

bool Simulator::preview(Trace& out) {
    if (!step()) return false;
    out = history_.back();
    step_back();
    return true;
}

bool Simulator::step() {
    if (s_.halted) return false;

    undo_.push_back(s_);
    if (undo_.size() > MAX_HISTORY) undo_.pop_front();

    // Latch values as they were at the start of the cycle.
    const IF_ID fd = s_.if_id;
    const ID_EX ie = s_.id_ex;
    const EX_MEM em = s_.ex_mem;
    const MEM_WB mw = s_.mem_wb;

    Trace t;
    t.cycle = s_.stats.cycles + 1;
    StageInfo fetch;
    if (pc_valid(s_.pc)) fetch = StageInfo{true, s_.fetch_seq, s_.pc, program_[s_.pc / 4]};
    t.stage[0] = fetch;
    t.stage[1] = fd.info;
    t.stage[2] = ie.info;
    t.stage[3] = em.info;
    t.stage[4] = mw.info;

    auto finish = [&]() {
        s_.stats.cycles++;
        update_halted();
        history_.push_back(t);
        if (history_.size() > MAX_HISTORY) history_.pop_front();
        return true;
    };

    // ---- Cache miss penalty: the whole pipeline freezes until the block arrives.
    const bool mem_op = em.info.valid && (em.mem_read || em.mem_write);
    if (mem_op) {
        if (s_.miss_wait < 0 && cfg.miss_penalty > 0) {
            bool all_cached = true;
            for (int i = 0; i < em.mem_size; i++)
                all_cached = all_cached && s_.cache.probe(static_cast<uint32_t>(em.alu_result) + i);
            if (!all_cached) s_.miss_wait = cfg.miss_penalty;
        }
        if (s_.miss_wait > 0) {
            s_.miss_wait--;
            t.mem_stall = true;
            t.mem_stall_left = s_.miss_wait;
            t.mem.addr = static_cast<uint32_t>(em.alu_result) & ADDR_MASK;
            t.mem.slot = static_cast<int>(Cache::slot_of(t.mem.addr));
            s_.stats.mem_stall_cycles++;
            return finish();
        }
    }

    // ---- WB: runs first so ID reads the freshly written value in the same cycle.
    if (mw.info.valid) {
        s_.stats.retired++;
        if (mw.reg_write && mw.dest != 0) {
            s_.regs[mw.dest] = wb_value(mw);
            t.wb_write = true;
            t.wb_reg = mw.dest;
            t.wb_value = wb_value(mw);
        }
    }

    // ---- MEM
    MEM_WB n_mw;
    if (em.info.valid) {
        n_mw.info = em.info;
        n_mw.alu_result = em.alu_result;
        n_mw.dest = em.dest;
        n_mw.reg_write = em.reg_write;
        n_mw.mem_to_reg = em.mem_to_reg;
        if (mem_op) {
            const uint32_t addr = static_cast<uint32_t>(em.alu_result);
            bool hit = true;
            uint32_t value = 0;
            for (int i = 0; i < em.mem_size; i++) {  // little-endian
                if (em.mem_read) {
                    uint8_t byte = 0;
                    hit = s_.cache.read(addr + i, s_.mem, byte, &t.mem.wrote_back) && hit;
                    value |= static_cast<uint32_t>(byte) << (8 * i);
                } else {
                    uint8_t byte = static_cast<uint8_t>(static_cast<uint32_t>(em.store_value) >> (8 * i));
                    hit = s_.cache.write(addr + i, byte, s_.mem, &t.mem.wrote_back) && hit;
                    value |= static_cast<uint32_t>(byte) << (8 * i);
                }
            }
            if (em.mem_size == 1 && em.mem_signed) value = static_cast<uint32_t>(static_cast<int8_t>(value));
            if (em.mem_read) n_mw.mem_data = static_cast<int32_t>(value);
            t.mem.happened = true;
            t.mem.write = em.mem_write;
            t.mem.hit = hit;
            t.mem.addr = addr & ADDR_MASK;
            t.mem.slot = static_cast<int>(Cache::slot_of(addr));
            t.mem.size = em.mem_size;
            t.mem.value = static_cast<int32_t>(value);
            (hit ? s_.stats.hits : s_.stats.misses)++;
            s_.miss_wait = -1;
        }
    }

    // ---- EX
    EX_MEM n_em;
    bool flush = false;
    if (ie.info.valid) {
        int32_t a = ie.a, b = ie.b;
        if (cfg.hazard == HazardMode::Forward) {
            auto forward = [&](int reg, int32_t& value) {
                if (reg == 0) return Fwd::None;
                if (em.info.valid && em.reg_write && em.dest == reg && !em.mem_to_reg) {
                    value = em.alu_result;
                    return Fwd::ExMem;
                }
                if (mw.info.valid && mw.reg_write && mw.dest == reg) {
                    value = wb_value(mw);
                    return Fwd::MemWb;
                }
                return Fwd::None;
            };
            if (ie.uses_rs) t.fwd_a = forward(ie.rs, a);
            if (ie.uses_rt) t.fwd_b = forward(ie.rt, b);
            s_.stats.forwards += (t.fwd_a != Fwd::None) + (t.fwd_b != Fwd::None);
        }
        const int32_t op2 = ie.alu_src ? ie.imm : b;
        const int32_t result = run_alu(ie.alu, a, op2, ie.shamt);

        n_em.info = ie.info;
        n_em.alu_result = result;
        n_em.store_value = b;
        n_em.dest = ie.dest;
        n_em.reg_write = ie.reg_write;
        n_em.mem_read = ie.mem_read;
        n_em.mem_write = ie.mem_write;
        n_em.mem_to_reg = ie.mem_to_reg;
        n_em.mem_size = ie.mem_size;
        n_em.mem_signed = ie.mem_signed;

        t.ex_valid = true;
        t.alu = ie.alu;
        t.alu_result = result;
        if (ie.alu == AluOp::Sll || ie.alu == AluOp::Srl) {
            t.alu_a = op2;
            t.alu_b = ie.shamt;
        } else if (ie.alu == AluOp::Lui) {
            t.alu_a = op2;
            t.alu_b = 16;
        } else {
            t.alu_a = a;
            t.alu_b = op2;
        }

        if (ie.branch) {
            const bool taken = ie.branch == 1 ? a == b : a != b;
            t.branch_resolved = true;
            t.branch_taken = taken;
            s_.stats.branches++;
            if (taken) {
                s_.stats.taken++;
                flush = true;
                t.redirect = isa::branch_target(ie.info.pc, ie.imm);
            }
        }
    }

    // ---- ID and IF
    IF_ID n_fd;
    ID_EX n_ie;
    uint32_t n_pc = s_.pc;
    auto new_fetch_seq = [&]() { s_.fetch_seq = s_.next_seq++; };

    if (flush) {
        // Branch taken: the two younger instructions were fetched down the wrong path.
        t.squashed[0] = fetch.valid;
        t.squashed[1] = fd.info.valid;
        s_.stats.flushed += fetch.valid + fd.info.valid;
        n_pc = t.redirect;
        new_fetch_seq();
    } else {
        bool stall = false;
        if (fd.info.valid) {
            const ID_EX d = decode(fd.info, s_.regs);
            auto reads = [&](int reg) { return reg != 0 && ((d.uses_rs && d.rs == reg) || (d.uses_rt && d.rt == reg)); };
            if (cfg.hazard == HazardMode::Forward) {
                // Load-use: the loaded value only exists after MEM, one cycle too late to forward.
                if (ie.info.valid && ie.mem_read && ie.reg_write && reads(ie.dest)) {
                    stall = true;
                    t.stall_reg = ie.dest;
                }
            } else if (cfg.hazard == HazardMode::Stall) {
                if (ie.info.valid && ie.reg_write && reads(ie.dest)) {
                    stall = true;
                    t.stall_reg = ie.dest;
                } else if (em.info.valid && em.reg_write && reads(em.dest)) {
                    stall = true;
                    t.stall_reg = em.dest;
                }
            }
            if (stall) {
                t.stall_id = true;
                s_.stats.data_stalls++;
                n_fd = fd;  // hold; a bubble goes into EX
            } else {
                n_ie = d;
                if (d.jump) {
                    t.jump = true;
                    t.redirect = isa::jump_target(fd.info.pc, isa::split(fd.info.instr).target);
                }
            }
        }
        if (stall) {
            // PC and IF/ID hold.
        } else if (t.jump) {
            t.squashed[0] = fetch.valid;
            s_.stats.flushed += fetch.valid;
            n_pc = t.redirect;
            new_fetch_seq();
        } else if (fetch.valid) {
            n_fd.info = fetch;
            n_pc = s_.pc + 4;
            new_fetch_seq();
        }
    }

    // ---- Clock edge
    s_.pc = n_pc;
    s_.if_id = n_fd;
    s_.id_ex = n_ie;
    s_.ex_mem = n_em;
    s_.mem_wb = n_mw;
    return finish();
}
