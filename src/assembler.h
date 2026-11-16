// A small two-pass MIPS assembler for the subset the simulator executes.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct AsmResult {
    bool ok = false;
    std::vector<uint32_t> words;             // machine code, one word per instruction
    std::vector<int> line_of_word;           // 1-based source line of each word
    std::map<uint32_t, std::string> labels;  // address -> label name
    std::string error;
    int error_line = 0;
};

// Supported: add sub and or xor nor slt sll srl addi slti andi ori xori lui
//            lw sw lb lbu sb beq bne j, pseudo: nop li move b, directive: .word
// Comments start with '#', ';' or "//". Labels end with ':'.
AsmResult assemble(const std::string& source);
