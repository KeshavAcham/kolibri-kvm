#include "jvm.h"
#include <stdio.h>
#include <stdlib.h>

/* ════════════════════════════════════════════════════════════
 *  Mini test framework
 * ════════════════════════════════════════════════════════════ */

static int tests_run    = 0;
static int tests_passed = 0;

#define RUN_TEST(name, code_arr, expected_ret, expected_val)            \
    do {                                                                 \
        tests_run++;                                                     \
        VM vm; vm_init(&vm, 0);                                         \
        int32_t ret = 0;                                                 \
        JVMResult r = vm_exec(&vm, code_arr, sizeof(code_arr), &ret);   \
        if (r == (expected_ret) && ret == (expected_val)) {             \
            printf("  [PASS] %s\n", name);                              \
            tests_passed++;                                              \
        } else {                                                         \
            printf("  [FAIL] %s  result=%s  got=%d  want=%d\n",         \
                   name, jvm_result_str(r), ret, (int)(expected_val));  \
        }                                                                \
    } while (0)


/* ════════════════════════════════════════════════════════════
 *  Test cases — bytecode hand-assembled from Java snippets
 * ════════════════════════════════════════════════════════════
 *
 * Each comment shows the equivalent Java one-liner and the
 * disassembly produced by `javap -c`.
 * ════════════════════════════════════════════════════════════ */

/*
 * TEST 1: return 42
 * Java:   return 42;
 * javap:  bipush 42 / ireturn
 */
static const uint8_t test_return42[] = {
    OP_BIPUSH, 42,
    OP_IRETURN
};

/*
 * TEST 2: 3 + 4  →  7
 * Java:   return 3 + 4;
 * javap:  iconst_3 / iconst_4 / iadd / ireturn
 */
static const uint8_t test_add[] = {
    OP_ICONST_3,
    OP_ICONST_4,
    OP_IADD,
    OP_IRETURN
};

/*
 * TEST 3: 10 - 3  →  7
 */
static const uint8_t test_sub[] = {
    OP_BIPUSH, 10,
    OP_ICONST_3,
    OP_ISUB,
    OP_IRETURN
};

/*
 * TEST 4: 6 * 7  →  42
 */
static const uint8_t test_mul[] = {
    OP_BIPUSH, 6,
    OP_BIPUSH, 7,
    OP_IMUL,
    OP_IRETURN
};

/*
 * TEST 5: 100 / 4  →  25
 */
static const uint8_t test_div[] = {
    OP_BIPUSH, 100,
    OP_BIPUSH, 4,
    OP_IDIV,
    OP_IRETURN
};

/*
 * TEST 6: 17 % 5  →  2
 */
static const uint8_t test_rem[] = {
    OP_BIPUSH, 17,
    OP_BIPUSH, 5,
    OP_IREM,
    OP_IRETURN
};

/*
 * TEST 7: local variables
 * Java:   int a = 10; int b = 20; return a + b;
 * javap:  bipush 10 / istore_0 / bipush 20 / istore_1
 *         iload_0 / iload_1 / iadd / ireturn
 */
static const uint8_t test_locals[] = {
    OP_BIPUSH,  10,
    OP_ISTORE_0,
    OP_BIPUSH,  20,
    OP_ISTORE_1,
    OP_ILOAD_0,
    OP_ILOAD_1,
    OP_IADD,
    OP_IRETURN
};

/*
 * TEST 8: iinc
 * Java:   int i = 5; i += 3; return i;
 * javap:  bipush 5 / istore_0 / iinc 0 3 / iload_0 / ireturn
 */
static const uint8_t test_iinc[] = {
    OP_BIPUSH,  5,
    OP_ISTORE_0,
    OP_IINC,    0, 3,
    OP_ILOAD_0,
    OP_IRETURN
};

/*
 * TEST 9: if/else branch
 * Java:   int x = 7; return (x > 5) ? 1 : 0;
 *
 * Bytecode layout (branch offsets measured from opcode start):
 *   0: bipush 7
 *   2: istore_0
 *   3: iload_0
 *   4: bipush 5
 *   6: if_icmple  +7  → jump to offset 13  (7 bytes from pc=6 → 6+7=13)
 *   9: iconst_1
 *  10: ireturn
 *  11: (dead) — we need the else target at 11
 *
 * Actually let's lay it out carefully with two-byte branch offsets:
 *
 *  offset | bytes        | mnemonic
 *  -------+--------------+-----------------------------
 *    0    | 10 07        | bipush 7
 *    2    | 3B           | istore_0
 *    3    | 1A           | iload_0
 *    4    | 10 05        | bipush 5
 *    6    | A4 offset16  | if_icmple  → else branch
 *    9    | 04           | iconst_1
 *   10    | AC           | ireturn          ← "then" return 1
 *   11    | 03           | iconst_0
 *   12    | AC           | ireturn          ← "else" return 0
 *
 * if_icmple at offset 6: branch_offset = target - opcode_start
 *   = 11 - 6 = +5
 *   encoded as big-endian i16: 0x00 0x05
 */
static const uint8_t test_branch[] = {
    /* 0 */ OP_BIPUSH, 7,
    /* 2 */ OP_ISTORE_0,
    /* 3 */ OP_ILOAD_0,
    /* 4 */ OP_BIPUSH, 5,
    /* 6 */ OP_IF_ICMPLE, 0x00, 0x05,  /* if (top <= second) jump to offset 11 */
    /* 9 */ OP_ICONST_1,
    /*10 */ OP_IRETURN,
    /*11 */ OP_ICONST_0,
    /*12 */ OP_IRETURN
};

/*
 * TEST 10: while loop — sum 1..5 = 15
 * Java:
 *   int sum = 0, i = 1;
 *   while (i <= 5) { sum += i; i++; }
 *   return sum;
 *
 *  offset | bytes          | mnemonic
 *  -------+----------------+----------------------
 *    0    | 03             | iconst_0
 *    1    | 3B             | istore_0        (sum = 0)
 *    2    | 04             | iconst_1
 *    3    | 3C             | istore_1        (i   = 1)
 *    4    | 1A             | iload_0         ← loop top
 *    5    | 1B             | iload_1
 *    6    | 60             | iadd
 *    7    | 3B             | istore_0        (sum += i)
 *    8    | 84 01 01       | iinc 1, 1       (i++)
 *   11    | 1B             | iload_1
 *   12    | 10 05          | bipush 5
 *   14    | A4 FF F6       | if_icmple -10   → back to offset 4  (14-10=4 ✓)
 *   17    | 1A             | iload_0
 *   18    | AC             | ireturn
 *
 * if_icmple offset = 4 - 14 = -10 = 0xFFF6
 */
static const uint8_t test_loop[] = {
    /* 0 */ OP_ICONST_0,
    /* 1 */ OP_ISTORE_0,
    /* 2 */ OP_ICONST_1,
    /* 3 */ OP_ISTORE_1,
    /* 4 */ OP_ILOAD_0,          /* ← loop top */
    /* 5 */ OP_ILOAD_1,
    /* 6 */ OP_IADD,
    /* 7 */ OP_ISTORE_0,
    /* 8 */ OP_IINC, 1, 1,
    /*11 */ OP_ILOAD_1,
    /*12 */ OP_BIPUSH, 5,
    /*14 */ OP_IF_ICMPLE, 0xFF, 0xF6,   /* if i<=5 goto 4 */
    /*17 */ OP_ILOAD_0,
    /*18 */ OP_IRETURN
};

/*
 * TEST 11: sipush  (value > 127)
 * Java:   return 1000;
 */
static const uint8_t test_sipush[] = {
    OP_SIPUSH, 0x03, 0xE8,   /* 1000 in big-endian */
    OP_IRETURN
};

/*
 * TEST 12: negation
 * Java:   return -42;
 */
static const uint8_t test_neg[] = {
    OP_BIPUSH, 42,
    OP_INEG,
    OP_IRETURN
};

/*
 * TEST 13: bitwise AND
 * Java:   return 0xFF & 0x0F;  → 15
 */
static const uint8_t test_and[] = {
    OP_SIPUSH, 0x00, 0xFF,
    OP_BIPUSH, 0x0F,
    OP_IAND,
    OP_IRETURN
};

/*
 * TEST 14: left shift
 * Java:   return 1 << 3;  → 8
 */
static const uint8_t test_shl[] = {
    OP_ICONST_1,
    OP_ICONST_3,
    OP_ISHL,
    OP_IRETURN
};

/*
 * TEST 15: DUP
 * Java:   // push 5, dup it, add → 10
 */
static const uint8_t test_dup[] = {
    OP_ICONST_5,
    OP_DUP,
    OP_IADD,
    OP_IRETURN
};

/* ════════════════════════════════════════════════════════════
 *  Verbose demo  (prints each opcode as it executes)
 * ════════════════════════════════════════════════════════════ */

static void demo_verbose(void)
{
    printf("\n── Verbose trace: 3 + 4 ──\n");
    VM vm; vm_init(&vm, 1);   /* verbose = 1 */
    int32_t ret = 0;
    vm_exec(&vm, test_add, sizeof(test_add), &ret);
    printf("  result = %d\n\n", ret);
}

/* ════════════════════════════════════════════════════════════
 *  Entry point
 * ════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("╔══════════════════════════════════════╗\n");
    printf("║  KolibriOS J2ME KVM — Bytecode Tests ║\n");
    printf("╚══════════════════════════════════════╝\n\n");

    RUN_TEST("return 42",              test_return42, JVM_RETURN_INT,  42);
    RUN_TEST("3 + 4 = 7",             test_add,      JVM_RETURN_INT,   7);
    RUN_TEST("10 - 3 = 7",            test_sub,      JVM_RETURN_INT,   7);
    RUN_TEST("6 * 7 = 42",            test_mul,      JVM_RETURN_INT,  42);
    RUN_TEST("100 / 4 = 25",          test_div,      JVM_RETURN_INT,  25);
    RUN_TEST("17 %% 5 = 2",           test_rem,      JVM_RETURN_INT,   2);
    RUN_TEST("locals: 10+20 = 30",    test_locals,   JVM_RETURN_INT,  30);
    RUN_TEST("iinc: 5+3 = 8",         test_iinc,     JVM_RETURN_INT,   8);
    RUN_TEST("branch: 7>5 → 1",       test_branch,   JVM_RETURN_INT,   1);
    RUN_TEST("loop: sum(1..5) = 15",  test_loop,     JVM_RETURN_INT,  15);
    RUN_TEST("sipush 1000",           test_sipush,   JVM_RETURN_INT,1000);
    RUN_TEST("neg: -42",              test_neg,      JVM_RETURN_INT, -42);
    RUN_TEST("0xFF & 0x0F = 15",      test_and,      JVM_RETURN_INT,  15);
    RUN_TEST("1 << 3 = 8",            test_shl,      JVM_RETURN_INT,   8);
    RUN_TEST("dup: 5+5 = 10",         test_dup,      JVM_RETURN_INT,  10);

    printf("\n%d / %d tests passed\n", tests_passed, tests_run);

    demo_verbose();

    return (tests_passed == tests_run) ? 0 : 1;
}
