// Built-in example programs.
#pragma once

struct Sample {
    const char* name;
    bool course_regs;  // start with registers = 0x100 + i
    const char* source;
};

inline const Sample SAMPLES[] = {
    {"Sum 1..10 (loop + forwarding)", false,
     R"(# Sum the integers 1..10 with a loop.
# Watch the forwarding arcs under the EX stage, and the
# two instructions that get flushed each time bne is taken.

        addi $t0, $zero, 10      # counter
        addi $t1, $zero, 0       # sum
loop:   add  $t1, $t1, $t0
        addi $t0, $t0, -1
        bne  $t0, $zero, loop
        sw   $t1, 0x100($zero)   # store 55
        lw   $v0, 0x100($zero)   # and read it back
)"},

    {"Fibonacci into memory", false,
     R"(# Write the first 12 Fibonacci numbers to memory at 0x200.
# Each word store goes through the data cache.

        li   $s0, 0x200          # array pointer
        li   $t0, 0              # fib(n)
        li   $t1, 1              # fib(n+1)
        li   $t3, 12             # how many
loop:   sw   $t0, 0($s0)
        add  $t2, $t0, $t1
        move $t0, $t1
        move $t1, $t2
        addi $s0, $s0, 4
        addi $t3, $t3, -1
        bne  $t3, $zero, loop
)"},

    {"Cache conflict (ping-pong)", false,
     R"(# 0x040 and 0x140 map to the same cache slot (4) with
# different tags, so they keep evicting each other.
# Try a miss penalty of 3+ cycles and watch the pipeline freeze.

        li   $s0, 0x040          # block A: tag 0, slot 4
        li   $s1, 0x140          # block B: tag 1, slot 4
        li   $t3, 4
loop:   lb   $t0, 0($s0)         # A: miss first time, then hit
        lb   $t1, 0($s1)         # miss: evicts A (write-back), loads B
        add  $t2, $t0, $t1
        sb   $t2, 1($s0)         # miss: evicts B, A becomes dirty
        addi $t3, $t3, -1
        bne  $t3, $zero, loop
        lb   $t4, 1($s0)         # hit
)"},

    {"Load-use hazard", false,
     R"(# A value loaded from memory is used by the very next
# instruction. Even with forwarding this costs one stall:
# the data is not available until the end of MEM.
# Switch Hazards to "None" to see the wrong answer.

        li   $t0, 7
        sw   $t0, 0x80($zero)
        lw   $t1, 0x80($zero)
        add  $t2, $t1, $t1       # load-use: needs $t1 right away
        sub  $t3, $t2, $t0       # forwarded from EX/MEM
        or   $t4, $t3, $t1       # forwarded from MEM/WB
)"},

    {"Course project 3 (raw machine code)", true,
     R"(# The hex words from the pipeline assignment, loaded with
# .word. The Program panel disassembles them.
# Registers start at 0x100 + i, memory at address % 256.

        .word 0xa1020000
        .word 0x810AFFFC
        .word 0x00831820
        .word 0x01263820
        .word 0x01224820
        .word 0x81180000
        .word 0x81510010
        .word 0x00624022
)"},
};

inline constexpr int SAMPLE_COUNT = sizeof(SAMPLES) / sizeof(SAMPLES[0]);
