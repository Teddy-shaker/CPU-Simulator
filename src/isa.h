// MIPS instruction-set definitions: field extraction, register names and the
// disassembler (this grew out of the course "decoder" project).
#pragma once
#include <cstdint>
#include <string>

namespace isa {

enum Opcode : uint32_t {
    OP_RTYPE = 0, OP_J = 2, OP_BEQ = 4, OP_BNE = 5,
    OP_ADDI = 8, OP_SLTI = 10, OP_ANDI = 12, OP_ORI = 13, OP_XORI = 14, OP_LUI = 15,
    OP_LB = 32, OP_LW = 35, OP_LBU = 36, OP_SB = 40, OP_SW = 43
};

enum Funct : uint32_t {
    F_SLL = 0, F_SRL = 2,
    F_ADD = 32, F_SUB = 34, F_AND = 36, F_OR = 37, F_XOR = 38, F_NOR = 39, F_SLT = 42
};

struct Fields {
    uint32_t op, rs, rt, rd, shamt, funct;
    int32_t simm;     // sign-extended 16-bit immediate
    uint32_t zimm;    // zero-extended 16-bit immediate
    uint32_t target;  // 26-bit jump target
};

Fields split(uint32_t instr);

inline uint32_t branch_target(uint32_t pc, int32_t simm) { return pc + 4 + (static_cast<uint32_t>(simm) << 2); }
inline uint32_t jump_target(uint32_t pc, uint32_t target) { return ((pc + 4) & 0xF0000000u) | (target << 2); }

// "$t0" style name for register 0..31.
const char* reg_name(int r);
// Accepts "$8", "$t0", "$zero"... Returns -1 if it is not a register.
int parse_reg(const std::string& text);

// Text form of an instruction located at address `pc`.
std::string disassemble(uint32_t instr, uint32_t pc);

}  // namespace isa
