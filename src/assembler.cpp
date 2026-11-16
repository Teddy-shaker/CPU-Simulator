#include "assembler.h"

#include <cctype>
#include <cstdlib>
#include <sstream>

#include "isa.h"

namespace {

struct AsmError {
    int line;
    std::string msg;
};

struct Statement {
    int line;
    uint32_t addr;
    std::string mnemonic;
    std::vector<std::string> ops;
};

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string strip_spaces(const std::string& s) {
    std::string out;
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c))) out += c;
    return out;
}

bool is_ident(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_' || s[0] == '.')) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.')) return false;
    return true;
}

// Decimal or 0x-prefixed hex, optional leading minus.
bool parse_int(const std::string& text, long long& out) {
    std::string s = lower(text);
    if (s.empty()) return false;
    bool neg = false;
    size_t i = 0;
    if (s[0] == '-' || s[0] == '+') {
        neg = s[0] == '-';
        i = 1;
    }
    int base = 10;
    if (s.compare(i, 2, "0x") == 0) {
        base = 16;
        i += 2;
    }
    if (i >= s.size() || s.size() - i > 16) return false;
    for (size_t k = i; k < s.size(); k++)
        if (!(base == 16 ? std::isxdigit(static_cast<unsigned char>(s[k])) : std::isdigit(static_cast<unsigned char>(s[k]))))
            return false;
    long long v = std::strtoll(s.c_str() + i, nullptr, base);
    out = neg ? -v : v;
    return true;
}

uint32_t enc_r(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t shamt, uint32_t funct) {
    return (rs << 21) | (rt << 16) | (rd << 11) | (shamt << 6) | funct;
}
uint32_t enc_i(uint32_t op, uint32_t rs, uint32_t rt, long long imm) {
    return (op << 26) | (rs << 21) | (rt << 16) | (static_cast<uint32_t>(imm) & 0xFFFF);
}

struct Encoder {
    const std::map<std::string, uint32_t>& symbols;
    const Statement& st;

    [[noreturn]] void fail(const std::string& msg) const { throw AsmError{st.line, msg}; }

    void want(size_t n) const {
        if (st.ops.size() != n)
            fail("'" + st.mnemonic + "' expects " + std::to_string(n) + " operand" + (n == 1 ? "" : "s") +
                 ", got " + std::to_string(st.ops.size()));
    }
    uint32_t reg(size_t i) const {
        int r = isa::parse_reg(st.ops[i]);
        if (r < 0) fail("expected a register, got '" + st.ops[i] + "'");
        return static_cast<uint32_t>(r);
    }
    long long num(size_t i, long long lo, long long hi) const {
        long long v;
        if (!parse_int(st.ops[i], v)) fail("expected a number, got '" + st.ops[i] + "'");
        if (v < lo || v > hi)
            fail("value " + st.ops[i] + " is out of range (" + std::to_string(lo) + " to " + std::to_string(hi) + ")");
        return v;
    }
    // A label or an absolute address.
    uint32_t address(size_t i) const {
        long long v;
        if (parse_int(st.ops[i], v)) return static_cast<uint32_t>(v);
        auto it = symbols.find(st.ops[i]);
        if (it == symbols.end()) fail("unknown label '" + st.ops[i] + "'");
        return it->second;
    }
    // "offset(base)" -> offset and base register.
    void mem_operand(size_t i, long long& off, uint32_t& base) const {
        const std::string& s = st.ops[i];
        size_t open = s.find('('), close = s.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close < open || close != s.size() - 1)
            fail("expected a memory operand like 8($sp), got '" + s + "'");
        std::string off_text = s.substr(0, open);
        std::string base_text = s.substr(open + 1, close - open - 1);
        off = 0;
        if (!off_text.empty()) {
            if (!parse_int(off_text, off)) fail("bad offset '" + off_text + "'");
            if (off < -32768 || off > 32767) fail("offset " + off_text + " does not fit in 16 bits");
        }
        int r = isa::parse_reg(base_text);
        if (r < 0) fail("expected a base register, got '" + base_text + "'");
        base = static_cast<uint32_t>(r);
    }
    uint32_t branch(uint32_t op, uint32_t rs, uint32_t rt, size_t target_op) const {
        uint32_t target = address(target_op);
        long long delta = static_cast<long long>(target) - static_cast<long long>(st.addr + 4);
        if (delta % 4 != 0) fail("branch target is not word aligned");
        delta /= 4;
        if (delta < -32768 || delta > 32767) fail("branch target is too far away");
        return enc_i(op, rs, rt, delta);
    }

    void emit(std::vector<uint32_t>& out) const {
        const std::string& m = st.mnemonic;
        struct R3 { const char* name; uint32_t funct; };
        static const R3 r3[] = {{"add", isa::F_ADD}, {"sub", isa::F_SUB}, {"and", isa::F_AND}, {"or", isa::F_OR},
                                {"xor", isa::F_XOR}, {"nor", isa::F_NOR}, {"slt", isa::F_SLT}};
        for (const R3& e : r3)
            if (m == e.name) {
                want(3);
                out.push_back(enc_r(reg(1), reg(2), reg(0), 0, e.funct));
                return;
            }
        if (m == "sll" || m == "srl") {
            want(3);
            out.push_back(enc_r(0, reg(1), reg(0), static_cast<uint32_t>(num(2, 0, 31)),
                                m == "sll" ? isa::F_SLL : isa::F_SRL));
        } else if (m == "addi" || m == "slti") {
            want(3);
            out.push_back(enc_i(m == "addi" ? isa::OP_ADDI : isa::OP_SLTI, reg(1), reg(0), num(2, -32768, 32767)));
        } else if (m == "andi" || m == "ori" || m == "xori") {
            want(3);
            uint32_t op = m == "andi" ? isa::OP_ANDI : m == "ori" ? isa::OP_ORI : isa::OP_XORI;
            out.push_back(enc_i(op, reg(1), reg(0), num(2, 0, 65535)));
        } else if (m == "lui") {
            want(2);
            out.push_back(enc_i(isa::OP_LUI, 0, reg(0), num(1, 0, 65535)));
        } else if (m == "lw" || m == "lb" || m == "lbu" || m == "sw" || m == "sb") {
            want(2);
            uint32_t op = m == "lw" ? isa::OP_LW : m == "lb" ? isa::OP_LB : m == "lbu" ? isa::OP_LBU
                        : m == "sw" ? isa::OP_SW : isa::OP_SB;
            long long off;
            uint32_t base;
            mem_operand(1, off, base);
            out.push_back(enc_i(op, base, reg(0), off));
        } else if (m == "beq" || m == "bne") {
            want(3);
            out.push_back(branch(m == "beq" ? isa::OP_BEQ : isa::OP_BNE, reg(0), reg(1), 2));
        } else if (m == "b") {
            want(1);
            out.push_back(branch(isa::OP_BEQ, 0, 0, 0));
        } else if (m == "j") {
            want(1);
            uint32_t target = address(0);
            if (target % 4 != 0) fail("jump target is not word aligned");
            out.push_back((isa::OP_J << 26) | ((target >> 2) & 0x03FFFFFF));
        } else if (m == "nop") {
            want(0);
            out.push_back(0);
        } else if (m == "move") {
            want(2);
            out.push_back(enc_r(reg(1), 0, reg(0), 0, isa::F_ADD));
        } else if (m == "li") {
            want(2);
            uint32_t rt = reg(0);
            long long v = num(1, -2147483648LL, 4294967295LL);
            if (v >= -32768 && v <= 32767) {
                out.push_back(enc_i(isa::OP_ADDI, 0, rt, v));
            } else if (v >= 0 && v <= 65535) {
                out.push_back(enc_i(isa::OP_ORI, 0, rt, v));
            } else {
                uint32_t u = static_cast<uint32_t>(v);
                out.push_back(enc_i(isa::OP_LUI, 0, rt, u >> 16));
                out.push_back(enc_i(isa::OP_ORI, rt, rt, u & 0xFFFF));
            }
        } else if (m == ".word") {
            want(1);
            out.push_back(static_cast<uint32_t>(num(0, -2147483648LL, 4294967295LL)));
        } else {
            fail("unknown instruction '" + m + "'");
        }
    }
};

// Number of machine words a statement expands to (only `li` can be 2).
uint32_t statement_words(const Statement& st) {
    if (st.mnemonic == "li" && st.ops.size() == 2) {
        long long v;
        if (parse_int(st.ops[1], v) && (v < -32768 || v > 65535)) return 2;
    }
    return 1;
}

}  // namespace

AsmResult assemble(const std::string& source) {
    AsmResult result;
    std::vector<Statement> statements;
    std::map<std::string, uint32_t> symbols;

    try {
        // Pass 1: split lines into labels and statements, assign addresses.
        std::istringstream in(source);
        std::string line;
        int line_no = 0;
        uint32_t addr = 0;
        while (std::getline(in, line)) {
            line_no++;
            size_t cut = line.find_first_of("#;");
            size_t slashes = line.find("//");
            if (slashes < cut) cut = slashes;
            if (cut != std::string::npos) line.erase(cut);
            line = trim(line);

            for (;;) {
                size_t colon = line.find(':');
                if (colon == std::string::npos) break;
                std::string name = trim(line.substr(0, colon));
                if (!is_ident(name)) throw AsmError{line_no, "invalid label '" + name + "'"};
                if (symbols.count(name)) throw AsmError{line_no, "label '" + name + "' is defined twice"};
                symbols[name] = addr;
                result.labels[addr] = name;
                line = trim(line.substr(colon + 1));
            }
            if (line.empty()) continue;

            Statement st;
            st.line = line_no;
            st.addr = addr;
            size_t sp = line.find_first_of(" \t");
            st.mnemonic = lower(line.substr(0, sp));
            if (sp != std::string::npos) {
                std::string rest = line.substr(sp);
                size_t start = 0;
                for (;;) {
                    size_t comma = rest.find(',', start);
                    std::string op = strip_spaces(rest.substr(start, comma == std::string::npos ? comma : comma - start));
                    if (op.empty()) throw AsmError{line_no, "missing operand"};
                    st.ops.push_back(op);
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
            }
            addr += 4 * statement_words(st);
            statements.push_back(st);
        }

        // Pass 2: encode.
        for (const Statement& st : statements) {
            size_t before = result.words.size();
            Encoder{symbols, st}.emit(result.words);
            for (size_t i = before; i < result.words.size(); i++) result.line_of_word.push_back(st.line);
        }
        result.ok = true;
    } catch (const AsmError& e) {
        result.ok = false;
        result.words.clear();
        result.line_of_word.clear();
        result.error = e.msg;
        result.error_line = e.line;
    }
    return result;
}
