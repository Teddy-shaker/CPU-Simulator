#include "isa.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace isa {

static const char* const REG_NAMES[32] = {
    "$zero", "$at", "$v0", "$v1", "$a0", "$a1", "$a2", "$a3",
    "$t0", "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7",
    "$s0", "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7",
    "$t8", "$t9", "$k0", "$k1", "$gp", "$sp", "$fp", "$ra"};

Fields split(uint32_t instr) {
    Fields f;
    f.op = (instr >> 26) & 0x3F;
    f.rs = (instr >> 21) & 0x1F;
    f.rt = (instr >> 16) & 0x1F;
    f.rd = (instr >> 11) & 0x1F;
    f.shamt = (instr >> 6) & 0x1F;
    f.funct = instr & 0x3F;
    f.simm = static_cast<int16_t>(instr & 0xFFFF);
    f.zimm = instr & 0xFFFF;
    f.target = instr & 0x03FFFFFF;
    return f;
}

const char* reg_name(int r) { return (r >= 0 && r < 32) ? REG_NAMES[r] : "$??"; }

int parse_reg(const std::string& text) {
    if (text.size() < 2 || text[0] != '$') return -1;
    std::string s;
    for (char c : text) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool numeric = true;
    for (size_t i = 1; i < s.size(); i++)
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) numeric = false;
    if (numeric) {
        if (s.size() > 3) return -1;
        int n = std::atoi(s.c_str() + 1);
        return n < 32 ? n : -1;
    }
    if (s == "$s8") return 30;
    for (int i = 0; i < 32; i++)
        if (s == REG_NAMES[i]) return i;
    return -1;
}

std::string disassemble(uint32_t instr, uint32_t pc) {
    if (instr == 0) return "nop";
    const Fields f = split(instr);
    char buf[80];
    auto raw = [&]() {
        std::snprintf(buf, sizeof buf, ".word 0x%08X", instr);
        return std::string(buf);
    };
    const char* rs = reg_name(f.rs);
    const char* rt = reg_name(f.rt);
    const char* rd = reg_name(f.rd);

    switch (f.op) {
    case OP_RTYPE: {
        const char* name = nullptr;
        bool shift = false;
        switch (f.funct) {
        case F_SLL: name = "sll"; shift = true; break;
        case F_SRL: name = "srl"; shift = true; break;
        case F_ADD: name = "add"; break;
        case F_SUB: name = "sub"; break;
        case F_AND: name = "and"; break;
        case F_OR:  name = "or";  break;
        case F_XOR: name = "xor"; break;
        case F_NOR: name = "nor"; break;
        case F_SLT: name = "slt"; break;
        default: return raw();
        }
        if (shift) {
            if (f.rs != 0) return raw();
            std::snprintf(buf, sizeof buf, "%s %s, %s, %u", name, rd, rt, f.shamt);
        } else {
            if (f.shamt != 0) return raw();
            std::snprintf(buf, sizeof buf, "%s %s, %s, %s", name, rd, rs, rt);
        }
        return buf;
    }
    case OP_ADDI:
    case OP_SLTI:
        std::snprintf(buf, sizeof buf, "%s %s, %s, %d", f.op == OP_ADDI ? "addi" : "slti", rt, rs, f.simm);
        return buf;
    case OP_ANDI:
    case OP_ORI:
    case OP_XORI:
        std::snprintf(buf, sizeof buf, "%s %s, %s, %u",
                      f.op == OP_ANDI ? "andi" : f.op == OP_ORI ? "ori" : "xori", rt, rs, f.zimm);
        return buf;
    case OP_LUI:
        if (f.rs != 0) return raw();
        std::snprintf(buf, sizeof buf, "lui %s, 0x%X", rt, f.zimm);
        return buf;
    case OP_LB:
    case OP_LBU:
    case OP_LW:
    case OP_SB:
    case OP_SW: {
        const char* name = f.op == OP_LB ? "lb" : f.op == OP_LBU ? "lbu" : f.op == OP_LW ? "lw"
                         : f.op == OP_SB ? "sb" : "sw";
        std::snprintf(buf, sizeof buf, "%s %s, %d(%s)", name, rt, f.simm, rs);
        return buf;
    }
    case OP_BEQ:
    case OP_BNE:
        std::snprintf(buf, sizeof buf, "%s %s, %s, 0x%04X", f.op == OP_BEQ ? "beq" : "bne", rs, rt,
                      branch_target(pc, f.simm));
        return buf;
    case OP_J:
        std::snprintf(buf, sizeof buf, "j 0x%04X", jump_target(pc, f.target));
        return buf;
    default:
        return raw();
    }
}

}  // namespace isa
