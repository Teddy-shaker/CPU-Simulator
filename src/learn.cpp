#include "learn.h"

#include <cstdarg>
#include <cstdio>

#include "isa.h"

namespace {

std::string fmt(const char* f, ...) {
    char buf[640];
    va_list args;
    va_start(args, f);
    std::vsnprintf(buf, sizeof buf, f, args);
    va_end(args);
    return buf;
}

std::string dis(const StageInfo& in) { return isa::disassemble(in.instr, in.pc); }

const Trace* last_trace(const Simulator& sim) { return sim.history().empty() ? nullptr : &sim.history().back(); }

// "opcode 0x08 (addi), rs = $t0, rt = $t0, immediate = -1"
std::string fields_text(uint32_t instr) {
    const isa::Fields f = isa::split(instr);
    if (instr == 0) return "all 32 bits are zero, which MIPS treats as a do-nothing instruction (nop)";
    if (f.op == isa::OP_RTYPE)
        return fmt("opcode 0 means R-type, so the operation is in the funct field (0x%02X). rs = %s, rt = %s, rd = %s, shamt = %u",
                   f.funct, isa::reg_name(f.rs), isa::reg_name(f.rt), isa::reg_name(f.rd), f.shamt);
    if (f.op == isa::OP_J) return fmt("opcode 0x%02X is a jump; the other 26 bits are the target (word %u)", f.op, f.target);
    return fmt("opcode 0x%02X is I-type: rs = %s, rt = %s, 16-bit immediate = %d", f.op, isa::reg_name(f.rs),
               isa::reg_name(f.rt), f.simm);
}

const char* fwd_name(Fwd f) { return f == Fwd::ExMem ? "EX/MEM" : "MEM/WB"; }

std::string addr_split(uint32_t addr) {
    return fmt("0x%03X = tag %u, slot %X, offset %u", addr, Cache::tag_of(addr), Cache::slot_of(addr), Cache::offset_of(addr));
}

const char* const STAGE_TITLE[5] = {"IF: Instruction Fetch", "ID: Instruction Decode", "EX: Execute", "MEM: Memory Access",
                                    "WB: Write Back"};

const char* const STAGE_WHAT[5] = {
    "Fetch reads the instruction that the Program Counter (PC) points at out of instruction memory, and works out "
    "PC + 4 as the default next address. It has no idea yet what the instruction means: it just grabs 32 bits and "
    "hands them on. Because it always assumes the program continues straight ahead, anything it fetches right "
    "behind a taken branch is a wrong guess.",

    "Decode splits the 32-bit word into fields (opcode, rs, rt, rd, immediate), reads the two source registers from "
    "the register file, and produces the control signals that tell the later stages what to do (RegWrite, MemRead, "
    "ALUSrc ...). Hazard detection also lives here: if a value this instruction needs is not ready yet, Decode holds "
    "the instruction in place and sends a do-nothing bubble onward instead.",

    "Execute is where the ALU does the arithmetic. R-type instructions combine two registers; I-type ones combine a "
    "register with the immediate; loads and stores add base + offset to get an address; branches subtract their "
    "operands to compare them. Two forwarding multiplexers sit in front of the ALU and can swap in a newer value "
    "from EX/MEM or MEM/WB when the register value read in Decode is out of date.",

    "Only loads and stores do work here: they read or write data memory, and every access goes through the cache. "
    "The address is split into tag | slot | offset. If the slot is valid and its tag matches, it is a hit and the "
    "byte comes straight from the cache. Otherwise it is a miss: the whole 16-byte block is fetched from main memory "
    "(after writing the old block back if it was dirty). Every other instruction just passes through.",

    "Write Back stores the result into the destination register: either the ALU result or the data loaded from "
    "memory (the MemToReg control bit picks which). The write happens in the first half of the cycle, so an "
    "instruction reading that register in Decode during the same cycle already sees the new value. After this the "
    "instruction is finished, or 'retired'."};

const char* const LATCH_NAME[4] = {"IF/ID", "ID/EX", "EX/MEM", "MEM/WB"};
const char* const LATCH_HOLDS[4] = {
    "IF/ID carries the raw instruction word and PC + 4.",
    "ID/EX carries the two register values read in Decode, the sign-extended immediate, the destination register "
    "number and every control signal.",
    "EX/MEM carries the ALU result (a value, or a memory address), the value to store for sb/sw, the destination "
    "register and the control signals still needed. It is the first place a freshly computed result can be "
    "forwarded from.",
    "MEM/WB carries the data read from memory, the ALU result, and the destination register. It is the second "
    "forwarding source."};

const char* reg_role(int r) {
    if (r == 0) return "$zero is hard-wired to 0. Writes to it are ignored, which makes it handy for copying (add) and comparing against zero.";
    if (r == 1) return "$at is reserved for the assembler, which uses it as scratch space when it expands pseudo-instructions.";
    if (r <= 3) return "$v0 and $v1 hold function return values by convention.";
    if (r <= 7) return "$a0 to $a3 carry the first four arguments of a function call by convention.";
    if (r <= 15 || r == 24 || r == 25) return "$t registers are temporaries: a function may overwrite them freely, so the caller cannot rely on them surviving a call.";
    if (r <= 23) return "$s registers are 'saved': a function that uses one must restore its old value before returning.";
    if (r <= 27) return "$k0 and $k1 are reserved for the operating system kernel.";
    if (r == 28) return "$gp is the global pointer, pointing at the program's static data.";
    if (r == 29) return "$sp is the stack pointer: the address of the top of the stack.";
    if (r == 30) return "$fp is the frame pointer, marking the start of the current function's stack frame.";
    return "$ra holds the return address written by a call instruction.";
}

Explanation explain_stage(const Simulator& sim, int i) {
    Explanation e;
    e.title = STAGE_TITLE[i];
    e.color_stage = i;
    e.what = STAGE_WHAT[i];
    const Trace* t = last_trace(sim);
    if (!t) {
        e.now.push_back("Nothing has been clocked yet. Press Step and watch the first instruction arrive.");
        return e;
    }
    const StageInfo& in = t->stage[i];
    if (!in.valid) {
        e.now.push_back(i == 0 ? "The PC has run past the last instruction, so there is nothing left to fetch."
                               : "It holds a bubble this cycle: an empty slot that does nothing, left behind by a stall, "
                                 "a flush, or simply the start or end of the program.");
        return e;
    }
    const isa::Fields f = isa::split(in.instr);
    if (t->mem_stall && i != 3) {
        e.now.push_back(fmt("It is holding '%s' but frozen: the Memory stage is waiting for a cache miss, and nothing "
                            "may move until the block arrives.", dis(in).c_str()));
        return e;
    }
    switch (i) {
    case 0:
        e.now.push_back(fmt("It fetched '%s' from address 0x%04X (machine code 0x%08X).", dis(in).c_str(), in.pc, in.instr));
        if (t->squashed[0])
            e.now.push_back(fmt("But the PC was just redirected to 0x%04X, so this instruction is flushed: it was a wrong "
                                "guess and will never execute.", t->redirect));
        else if (t->stall_id)
            e.now.push_back("Decode is stalled, so Fetch is held too. The PC does not move and the same instruction is "
                            "fetched again next cycle.");
        else
            e.now.push_back(fmt("The PC moves on to 0x%04X.", in.pc + 4));
        break;
    case 1:
        e.now.push_back(fmt("It is decoding '%s': %s.", dis(in).c_str(), fields_text(in.instr).c_str()));
        if (t->squashed[1])
            e.now.push_back("The branch ahead of it was taken, so this instruction is flushed and turns into a bubble.");
        else if (t->stall_id)
            e.now.push_back(fmt("STALL: it needs %s, but the instruction that produces it is still in the pipeline and "
                                "the value cannot reach it in time. It stays here one more cycle and a bubble goes "
                                "into Execute.", isa::reg_name(t->stall_reg)));
        else if (sim.state().id_ex.info.valid && sim.state().id_ex.info.seq == in.seq) {
            const ID_EX& d = sim.state().id_ex;
            e.now.push_back(fmt("It read %s = %d and %s = %d from the register file and latched them into ID/EX.",
                                isa::reg_name(d.rs), d.a, isa::reg_name(d.rt), d.b));
            if (t->jump) e.now.push_back(fmt("It is a jump, so the PC is redirected to 0x%04X right away.", t->redirect));
        }
        break;
    case 2:
        if (!t->ex_valid) break;
        e.now.push_back(fmt("Executing '%s': the ALU computed %d %s %d = %d.", dis(in).c_str(), t->alu_a, alu_symbol(t->alu),
                            t->alu_b, t->alu_result));
        if (t->fwd_a != Fwd::None)
            e.now.push_back(fmt("Operand A (%s) was forwarded from %s. The copy read back in Decode was stale because the "
                                "instruction producing it had not reached Write Back yet.", isa::reg_name(f.rs), fwd_name(t->fwd_a)));
        if (t->fwd_b != Fwd::None)
            e.now.push_back(fmt("Operand B (%s) was forwarded from %s for the same reason.", isa::reg_name(f.rt), fwd_name(t->fwd_b)));
        if (t->branch_resolved)
            e.now.push_back(t->branch_taken
                                ? fmt("The comparison says the branch IS taken: the PC jumps to 0x%04X and the two "
                                      "instructions fetched behind it are flushed.", t->redirect)
                                : std::string("The comparison says the branch is NOT taken, so the instructions already "
                                              "fetched behind it were the right ones. No time lost."));
        break;
    case 3:
        if (t->mem_stall) {
            e.now.push_back(fmt("'%s' wants address %s.", dis(in).c_str(), addr_split(t->mem.addr).c_str()));
            e.now.push_back(fmt("That block is not in the cache: a MISS. The whole pipeline is frozen for %d more "
                                "cycle%s while it is fetched from main memory.", t->mem_stall_left + 1,
                                t->mem_stall_left == 0 ? "" : "s"));
        } else if (t->mem.happened) {
            e.now.push_back(fmt("'%s' %s address %s.", dis(in).c_str(), t->mem.write ? "wrote to" : "read from",
                                addr_split(t->mem.addr).c_str()));
            e.now.push_back(t->mem.hit ? "Cache HIT: the slot was valid and its tag matched."
                                       : "Cache MISS: the slot was empty or held a different tag, so the block was loaded from main memory.");
            if (t->mem.wrote_back)
                e.now.push_back("The block that used to be in this slot was dirty, so it was written back to main memory first.");
            e.now.push_back(t->mem.write ? fmt("Value stored: %d. Only the cache was updated; the slot is now dirty.", t->mem.value)
                                         : fmt("Value read: %d.", t->mem.value));
        } else {
            e.now.push_back(fmt("'%s' does not touch memory, so it simply passes through to Write Back.", dis(in).c_str()));
        }
        break;
    case 4:
        if (t->wb_write)
            e.now.push_back(fmt("'%s' wrote %d (0x%08X) into %s. It is now retired.", dis(in).c_str(), t->wb_value,
                                static_cast<uint32_t>(t->wb_value), isa::reg_name(t->wb_reg)));
        else
            e.now.push_back(fmt("'%s' has no register to write (stores, branches and nops do not), so nothing changes. "
                                "It is now retired.", dis(in).c_str()));
        break;
    }
    return e;
}

Explanation explain_latch(const Simulator& sim, int i) {
    Explanation e;
    e.title = std::string(LATCH_NAME[i]) + " pipeline register";
    e.color_stage = i + 1;
    e.what = std::string(
                 "Pipeline registers are the walls between stages. On every clock edge each one captures what the stage "
                 "on its left just produced and holds it steady for the stage on its right during the whole next cycle. "
                 "That is what lets five instructions be in flight at once without trampling each other. ") +
             LATCH_HOLDS[i];
    const State& s = sim.state();
    const StageInfo* in[4] = {&s.if_id.info, &s.id_ex.info, &s.ex_mem.info, &s.mem_wb.info};
    if (in[i]->valid)
        e.now.push_back(fmt("It currently holds '%s', which the stage on its right will work on next cycle.", dis(*in[i]).c_str()));
    else
        e.now.push_back("It currently holds a bubble (no instruction).");
    e.now.push_back("Open the 'Pipeline registers' tab to see every field of all four.");
    return e;
}

Explanation explain_reg(const Simulator& sim, int r) {
    Explanation e;
    e.title = fmt("Register %d: %s", r, isa::reg_name(r));
    e.what = std::string("The CPU has 32 general-purpose registers of 32 bits each. They are the only things the ALU can "
                         "work on directly; everything else has to be loaded from memory first. ") + reg_role(r);
    const State& s = sim.state();
    e.now.push_back(fmt("Its value is %d (0x%08X).", s.regs[r], static_cast<uint32_t>(s.regs[r])));
    if (r != 0) {
        bool pending = false;
        auto note = [&](bool valid, bool writes, int dest, const StageInfo& in, const char* where, int cycles) {
            if (valid && writes && dest == r) {
                pending = true;
                e.now.push_back(fmt("'%s' (now heading into %s) will overwrite it in %d cycle%s.", dis(in).c_str(), where,
                                    cycles, cycles == 1 ? "" : "s"));
            }
        };
        note(s.mem_wb.info.valid, s.mem_wb.reg_write, s.mem_wb.dest, s.mem_wb.info, "WB", 1);
        note(s.ex_mem.info.valid, s.ex_mem.reg_write, s.ex_mem.dest, s.ex_mem.info, "MEM", 2);
        note(s.id_ex.info.valid, s.id_ex.reg_write, s.id_ex.dest, s.id_ex.info, "EX", 3);
        if (pending)
            e.now.push_back("Until then this value is out of date. Any instruction reading it in the meantime has a data "
                            "hazard, which is exactly what stalling and forwarding exist to fix.");
        else
            e.now.push_back("No instruction in the pipeline is about to change it.");
    }
    return e;
}

Explanation explain_slot(const Simulator& sim, int i) {
    Explanation e;
    e.title = fmt("Cache slot %X", i);
    e.color_stage = 3;
    e.what = fmt("Each slot stores one 16-byte block of memory. This cache is direct-mapped: an address can only ever "
                 "live in the slot named by its middle four bits, so every address of the form 0x?%X? competes for this "
                 "one slot. V (valid) says the slot holds real data. Tag records which of the 8 possible blocks is here. "
                 "D (dirty) means a store changed the cached copy and main memory has not been updated yet.", i);
    const CacheLine& l = sim.state().cache.line[i];
    if (!l.valid) {
        e.now.push_back("It is empty (V = 0). The first access that maps here is guaranteed to miss.");
    } else {
        const uint32_t base = sim.state().cache.block_base(i);
        e.now.push_back(fmt("It holds tag %u, which is the block at addresses 0x%03X to 0x%03X.", l.tag, base, base + 15));
        e.now.push_back(l.dirty ? "It is dirty: the cache has newer data than main memory. The block will be written back "
                                  "when another block evicts it."
                                : "It is clean: identical to main memory, so it can be replaced without writing anything back.");
    }
    return e;
}

}  // namespace

Explanation explain(const Simulator& sim, Selection sel) {
    switch (sel.kind) {
    case Selection::Stage: return explain_stage(sim, sel.index);
    case Selection::Latch: return explain_latch(sim, sel.index);
    case Selection::Reg:   return explain_reg(sim, sel.index);
    case Selection::Slot:  return explain_slot(sim, sel.index);
    default: return Explanation{};
    }
}

// ================================================================ challenge

bool make_quiz(Simulator& sim, Quiz& q) {
    const bool prev_stalled = !sim.history().empty() && sim.history().back().stall_id;
    Trace nt;
    if (!sim.preview(nt)) return false;
    const State& s = sim.state();  // state before the previewed cycle
    q = Quiz{};

    // 1. A branch is about to be resolved.
    if (nt.branch_resolved) {
        const StageInfo& in = nt.stage[2];
        const isa::Fields f = isa::split(in.instr);
        const bool bne = f.op == isa::OP_BNE;
        q.question = fmt("'%s' moves into Execute and compares its two registers. Is the branch taken?", dis(in).c_str());
        q.options = {"Taken: jump to the target, flush the instructions behind it", "Not taken: carry straight on"};
        q.correct = nt.branch_taken ? 0 : 1;
        q.why = fmt("%s = %d and %s = %d. %s branches when they are %s, so it is %s.", isa::reg_name(f.rs), nt.alu_a,
                    isa::reg_name(f.rt), nt.alu_b, bne ? "bne" : "beq", bne ? "different" : "equal",
                    nt.branch_taken ? "taken" : "not taken");
        return true;
    }
    // 2. Decode is about to stall.
    if (nt.stall_id && !prev_stalled) {
        const StageInfo& in = nt.stage[1];
        q.question = fmt("'%s' is in Decode and needs %s, which '%s' just ahead of it produces. Can it move on to "
                         "Execute this cycle?", dis(in).c_str(), isa::reg_name(nt.stall_reg), dis(nt.stage[2]).c_str());
        q.options = {"Yes, the register file already has the right value", "Yes, forwarding delivers the value in time",
                     "No, it has to stall and a bubble goes into Execute"};
        q.correct = 2;
        q.why = sim.cfg.hazard == HazardMode::Forward
                    ? fmt("The instruction ahead is a load. Its data only comes out of memory at the END of the Memory "
                          "stage, but Execute would need it at the START of that same cycle. Forwarding cannot send "
                          "data back in time, so one stall is unavoidable (the load-use hazard).")
                    : fmt("Forwarding is switched off, so the only way to get the new %s is to wait in Decode until the "
                          "instruction ahead has written it back.", isa::reg_name(nt.stall_reg));
        return true;
    }
    // 3. A load or store is about to reach the cache.
    const bool first_miss_cycle = nt.mem_stall && s.miss_wait < 0;
    const bool plain_access = nt.mem.happened && s.miss_wait < 0;
    if (first_miss_cycle || plain_access) {
        const StageInfo& in = nt.stage[3];
        const uint32_t addr = nt.mem.addr;
        const CacheLine& l = s.cache.line[Cache::slot_of(addr)];
        const bool hit = plain_access && nt.mem.hit;
        q.question = fmt("'%s' reaches the Memory stage and accesses address %s. Look at that slot in the cache table: "
                         "hit or miss?", dis(in).c_str(), addr_split(addr).c_str());
        q.options = {"Hit", "Miss"};
        q.correct = hit ? 0 : 1;
        if (!l.valid) q.why = fmt("Slot %X is empty (V = 0), so it has to be a miss.", Cache::slot_of(addr));
        else if (l.tag != Cache::tag_of(addr))
            q.why = fmt("Slot %X is valid but holds tag %u, and this address has tag %u. Same slot, different block: a "
                        "conflict miss%s.", Cache::slot_of(addr), l.tag, Cache::tag_of(addr),
                        l.dirty ? ", and the dirty block must be written back first" : "");
        else q.why = fmt("Slot %X is valid and its tag (%u) matches, so the block is already there: a hit.",
                         Cache::slot_of(addr), l.tag);
        return true;
    }
    if (nt.mem_stall) return false;
    // 4. An operand is about to be forwarded.
    if (nt.ex_valid && (nt.fwd_a != Fwd::None || nt.fwd_b != Fwd::None)) {
        const StageInfo& in = nt.stage[2];
        const isa::Fields f = isa::split(in.instr);
        const bool ask_a = nt.fwd_a != Fwd::None;
        const Fwd src = ask_a ? nt.fwd_a : nt.fwd_b;
        const char* reg = isa::reg_name(ask_a ? f.rs : f.rt);
        q.question = fmt("'%s' moves into Execute. Where does the ALU get the up-to-date value of %s from?",
                         dis(in).c_str(), reg);
        q.options = {"The register file (the value read in Decode)", "Forwarded from EX/MEM", "Forwarded from MEM/WB"};
        q.correct = src == Fwd::ExMem ? 1 : 2;
        const StageInfo& producer = src == Fwd::ExMem ? nt.stage[3] : nt.stage[4];
        q.why = fmt("%s is written by '%s', which is %s ahead and had not been written back yet when '%s' was decoded. "
                    "Its result is sitting in %s, so it is forwarded from there.", reg, dis(producer).c_str(),
                    src == Fwd::ExMem ? "one instruction" : "two instructions", dis(in).c_str(), fwd_name(src));
        return true;
    }
    // 5. Now and then: what value is written back?
    if (nt.wb_write && nt.cycle % 3 == 0) {
        const StageInfo& in = nt.stage[4];
        const int32_t v = nt.wb_value;
        std::vector<int32_t> values = {v};
        const int32_t candidates[] = {s.regs[nt.wb_reg], v + 1, v - 1, v * 2, 0, v + 4, -v};
        for (int32_t c : candidates) {
            bool dup = false;
            for (int32_t x : values) dup = dup || x == c;
            if (!dup && values.size() < 4) values.push_back(c);
        }
        const size_t n = values.size(), shift = static_cast<size_t>(nt.cycle) % n;
        q.options.resize(n);
        for (size_t i = 0; i < n; i++) q.options[(i + shift) % n] = fmt("%d", values[i]);
        q.correct = static_cast<int>(shift % n);
        q.question = fmt("'%s' reaches Write Back. What value does it write into %s?", dis(in).c_str(), isa::reg_name(nt.wb_reg));
        q.why = fmt("The value travelled down the pipeline in the %s field of MEM/WB: %d.",
                    s.mem_wb.mem_to_reg ? "MemData" : "ALUResult", v);
        return true;
    }
    return false;
}

// ================================================================ lessons

bool until_met(Until until, const Trace& t) {
    switch (until) {
    case Until::Start:     return true;
    case Until::Cycle:     return true;
    case Until::Forward:   return t.fwd_a != Fwd::None || t.fwd_b != Fwd::None;
    case Until::Stall:     return t.stall_id;
    case Until::Flush:     return t.branch_resolved && t.branch_taken;
    case Until::MemStall:  return t.mem_stall;
    case Until::MemAccess: return t.mem.happened;
    case Until::Hit:       return t.mem.happened && t.mem.hit;
    case Until::WriteBack: return t.mem.happened && t.mem.wrote_back;
    case Until::RegWrite:  return t.wb_write;
    case Until::Halt:      return false;
    }
    return false;
}

const std::vector<Lesson>& lessons() {
    static const char* const HAZARD_SRC =
        "        li   $t0, 5\n"
        "        add  $t1, $t0, $t0     # should be 10\n"
        "        add  $t2, $t1, $t0     # should be 15\n";

    static const std::vector<Lesson> all = {
        {"1. The five stages", "Follow one instruction down the assembly line.", 2, 0,
         "        li   $t0, 5\n        li   $t1, 7\n        li   $t2, 100\n        li   $t3, 1\n        li   $t4, 2\n",
         {
             {Until::Start, -1, false,
              "A pipelined CPU works like an assembly line. Every instruction passes through the same five stations "
              "(IF, ID, EX, MEM, WB) and spends exactly one clock cycle in each. This program just loads five "
              "constants, so nothing can go wrong. Press Continue to tick the clock once."},
             {Until::Cycle, -1, false,
              "Cycle 1, Fetch. The first instruction was read out of instruction memory. 'li $t0, 5' shows up as "
              "'addi $t0, $zero, 5' because li is only a shorthand the assembler expands. The PC has already moved "
              "to the next instruction. Click the IF box above to see exactly what it did."},
             {Until::Cycle, -1, false,
              "Cycle 2, Decode. The first instruction is being taken apart into its fields. Notice that Fetch is "
              "NOT idle: it is already bringing in the second instruction. This overlap is the entire point of a "
              "pipeline."},
             {Until::Cycle, -1, false,
              "Cycle 3, Execute. The ALU adds $zero + 5. Three instructions are now in flight at once, each in a "
              "different stage, kept apart by the grey pipeline registers between the boxes. Click one of those "
              "grey bars."},
             {Until::Cycle, -1, false,
              "Cycle 4, Memory. An addi has nothing to do here, so it passes through. Every instruction visits "
              "every stage anyway: keeping them all in step is what makes the pipeline simple."},
             {Until::RegWrite, -1, false,
              "Cycle 5, Write Back. $t0 just became 5 in the Registers panel on the right (it flashed). One "
              "instruction took five cycles from start to finish. That sounds slow, but keep going..."},
             {Until::Halt, -1, false,
              "Look at the pipeline diagram at the bottom: after the first instruction, another one finishes EVERY "
              "cycle. Five instructions took 9 cycles instead of 25. The CPI (cycles per instruction) in the toolbar "
              "heads toward 1 as programs get longer. That is the speed-up pipelining buys."},
         }},

        {"2. Data hazards", "What goes wrong when instructions overlap, and the simple fix.", 0, 0, HAZARD_SRC,
         {
             {Until::Start, -1, false,
              "Overlap has a catch. Here 'add' needs $t0, but the 'li' that sets $t0 is only one step ahead of it "
              "and will not write the register until its fifth cycle. For this lesson hazard handling is switched "
              "OFF (see 'Hazards' in the toolbar). Guess what $t1 and $t2 will end up as, then press Continue."},
             {Until::Halt, -1, false,
              "$t1 and $t2 are both 0. They should be 10 and 15. Each add read its registers in Decode before the "
              "instruction ahead of it had reached Write Back, so it got the old value. This is a data hazard, and "
              "a real CPU must never let it happen. Press Continue to try the simplest fix."},
             {Until::Stall, 1, true,
              "Fix number one: stalling. Hazards is now 'Stall only' and the program restarted. Decode noticed that "
              "$t0 is still in flight, so it is holding the add where it is (orange STALL badge) and sending an "
              "empty bubble into Execute instead. Click the ID box for the details."},
             {Until::Halt, -1, false,
              "Now $t1 = 10 and $t2 = 15: correct. But look at the Stalls counter and the pipeline diagram: the "
              "faded cells are cycles spent waiting. We paid for correctness with time. The next lesson shows a "
              "smarter fix."},
         }},

        {"3. Forwarding", "Get the right value without waiting for it.", 2, 0, HAZARD_SRC,
         {
             {Until::Start, -1, false,
              "Same program as before. Think about where the result of 'li' actually is: as soon as Execute "
              "finishes, the value 5 exists, sitting in the EX/MEM pipeline register. Why wait two more cycles for "
              "it to be written back? Forwarding adds wires that carry it straight to the ALU inputs."},
             {Until::Forward, -1, false,
              "See the arcs under the EX box: both ALU inputs (A and B) were taken from EX/MEM instead of the stale "
              "values read in Decode. No stall was needed. Click the EX box to read what happened."},
             {Until::Cycle, -1, false,
              "The second add needs two different things. $t1 was computed one cycle ago, so it comes from EX/MEM. "
              "$t0 was computed two cycles ago, so by now it has moved along to MEM/WB and is forwarded from there. "
              "Those are the two forwarding paths."},
             {Until::Halt, -1, false,
              "Same correct answers as stalling, but with zero stalls and fewer cycles. This is why every real "
              "pipelined CPU has forwarding. There is one case it cannot solve, though: that is the next lesson."},
         }},

        {"4. The load-use hazard", "The one case where forwarding is not enough.", 2, 0,
         "        li   $t0, 7\n        sw   $t0, 0x80($zero)\n        lw   $t1, 0x80($zero)\n"
         "        add  $t2, $t1, $t1     # uses $t1 right after the load\n        sub  $t3, $t2, $t0\n",
         {
             {Until::Start, -1, false,
              "A load (lw) gets its data out of memory at the END of the Memory stage. The add right behind it "
              "needs that data at the START of Execute, which is the very same cycle. Forwarding can move data "
              "sideways, but not backwards in time."},
             {Until::Stall, -1, false,
              "So one stall is unavoidable, even with forwarding on. The add waits in Decode for a single cycle "
              "while the lw goes through Memory."},
             {Until::Forward, -1, false,
              "Now the loaded value is in MEM/WB, and it is forwarded to the add from there. One cycle lost instead "
              "of two."},
             {Until::Halt, -1, false,
              "This is called the load-use hazard. Compilers know about it and try to put some unrelated "
              "instruction between a load and the first use of its result, so the slot is not wasted."},
         }},

        {"5. Branches and flushing", "The CPU guesses, and sometimes guesses wrong.", 2, 0,
         "        li   $t0, 3\nloop:   addi $t0, $t0, -1\n        bne  $t0, $zero, loop\n"
         "        li   $t1, 99\n        li   $t2, 42\n",
         {
             {Until::Start, -1, false,
              "A branch is not decided until Execute compares its registers. But Fetch cannot sit idle for two "
              "cycles waiting, so it guesses that the branch is NOT taken and keeps fetching the instructions that "
              "follow it."},
             {Until::Flush, -1, false,
              "bne compared 2 with 0: not equal, so the branch IS taken. The two 'li' instructions fetched behind "
              "it were wrong guesses. They are flushed (red, crossed out) and never execute. Two cycles wasted: "
              "this is the branch penalty."},
             {Until::Cycle, -1, false,
              "Fetch restarts at the 'loop' label, and the two flushed slots travel down the pipeline as bubbles."},
             {Until::Halt, -1, false,
              "On the final pass $t0 reached 0, the guess 'not taken' was right, and nothing was flushed. Check "
              "Flushed in the toolbar: 4 instructions thrown away over two taken branches. Real CPUs use branch "
              "predictors that learn each branch's habits to make far better guesses."},
         }},

        {"6. The cache", "Why memory is slow, and how a cache hides it.", 2, 3,
         "        li   $s0, 0x040\n        li   $s1, 0x140\n        lb   $t0, 0($s0)\n        lb   $t1, 1($s0)\n"
         "        sb   $t1, 2($s0)\n        lb   $t2, 0($s1)\n",
         {
             {Until::Start, -1, false,
              "Main memory is far slower than the CPU, so a small fast cache sits in front of it. This one has 16 "
              "slots of 16 bytes. An 11-bit address is split into tag | slot | offset: the slot picks a row in the "
              "cache table on the right, and the tag says which block is in that row. Miss penalty is set to 3 "
              "cycles for this lesson."},
             {Until::MemStall, -1, false,
              "The first lb wants address 0x040, which belongs in slot 4. That slot is empty (V = 0), so this is a "
              "miss. The whole pipeline freezes while the 16-byte block is fetched from main memory. Click row 4 of "
              "the cache table."},
             {Until::MemAccess, -1, false,
              "The block has arrived. Slot 4 is now valid with tag 0 and holds addresses 0x040 to 0x04F, all "
              "sixteen bytes, not just the one that was asked for."},
             {Until::Hit, -1, false,
              "The second lb reads 0x041. That is in the block we just loaded, so it is a hit and costs no extra "
              "time. Programs tend to touch nearby addresses (spatial locality), which is why loading whole blocks "
              "pays off."},
             {Until::MemAccess, -1, false,
              "The sb wrote into the cache only, and the slot's D (dirty) bit turned on. Main memory is now out of "
              "date. Open the 'Main memory' tab: the byte at 0x042 is shown in orange."},
             {Until::WriteBack, -1, false,
              "Address 0x140 also maps to slot 4, but with tag 1: a conflict. The dirty block had to be written "
              "back to main memory before the new block replaced it. A direct-mapped cache can only keep one of "
              "the two."},
             {Until::Halt, -1, false,
              "Check the Cache numbers in the toolbar: two hits, two misses, and 'Miss wait' shows the cycles lost "
              "to them. Try the 'Cache conflict' sample from the editor's list to watch two blocks fight over one "
              "slot."},
         }},
    };
    return all;
}
