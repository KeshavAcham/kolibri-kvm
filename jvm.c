#include "jvm.h"
#include <stdio.h>
#include <stdlib.h>

/* ════════════════════════════════════════════════════════════
 * Mini test framework
 * ════════════════════════════════════════════════════════════ */

static int tests_run    = 0;
static int tests_passed = 0;

#define RUN_TEST(name, code_arr, expected_ret, expected_val) \
    do { \
        tests_run++; \
        VM vm; vm_init(&vm, 0); \
        int32_t ret = 0; \
        JVMResult r = vm_exec(&vm, code_arr, sizeof(code_arr), &ret); \
        if (r == (expected_ret) && ret == (expected_val)) { \
            printf("  [PASS] %s\n", name); \
            tests_passed++; \
        } else { \
            printf("  [FAIL] %s  result=%s got=%d want=%d\n", \
                   name, jvm_result_str(r), ret, (int)(expected_val)); \
        } \
    } while (0)

/* Error-result test: we only care about the JVMResult, not the return value */
#define RUN_ERR_TEST(name, code_arr, expected_ret) \
    do { \
        tests_run++; \
        VM vm; vm_init(&vm, 0); \
        int32_t ret = 0; \
        JVMResult r = vm_exec(&vm, code_arr, sizeof(code_arr), &ret); \
        if (r == (expected_ret)) { \
            printf("  [PASS] %s\n", name); \
            tests_passed++; \
        } else { \
            printf("  [FAIL] %s  result=%s (want %s)\n", \
                   name, jvm_result_str(r), jvm_result_str(expected_ret)); \
        } \
    } while (0)

/* ════════════════════════════════════════════════════════════
 * Original 15 tests
 * ════════════════════════════════════════════════════════════ */

static const uint8_t test_return42[] = { OP_BIPUSH, 42, OP_IRETURN };

static const uint8_t test_add[] = { OP_ICONST_3, OP_ICONST_4, OP_IADD, OP_IRETURN };

static const uint8_t test_sub[] = { OP_BIPUSH, 10, OP_ICONST_3, OP_ISUB, OP_IRETURN };

static const uint8_t test_mul[] = { OP_BIPUSH, 6, OP_BIPUSH, 7, OP_IMUL, OP_IRETURN };

static const uint8_t test_div[] = { OP_BIPUSH, 100, OP_BIPUSH, 4, OP_IDIV, OP_IRETURN };

static const uint8_t test_rem[] = { OP_BIPUSH, 17, OP_BIPUSH, 5, OP_IREM, OP_IRETURN };

static const uint8_t test_locals[] = {
    OP_BIPUSH, 10, OP_ISTORE_0,
    OP_BIPUSH, 20, OP_ISTORE_1,
    OP_ILOAD_0, OP_ILOAD_1, OP_IADD, OP_IRETURN
};

static const uint8_t test_iinc[] = {
    OP_BIPUSH, 5, OP_ISTORE_0,
    OP_IINC, 0, 3,
    OP_ILOAD_0, OP_IRETURN
};

static const uint8_t test_branch[] = {
    /* 0 */ OP_BIPUSH, 7,
    /* 2 */ OP_ISTORE_0,
    /* 3 */ OP_ILOAD_0,
    /* 4 */ OP_BIPUSH, 5,
    /* 6 */ OP_IF_ICMPLE, 0x00, 0x05,
    /* 9 */ OP_ICONST_1,
    /*10 */ OP_IRETURN,
    /*11 */ OP_ICONST_0,
    /*12 */ OP_IRETURN
};

static const uint8_t test_loop[] = {
    /* 0 */ OP_ICONST_0,
    /* 1 */ OP_ISTORE_0,
    /* 2 */ OP_ICONST_1,
    /* 3 */ OP_ISTORE_1,
    /* 4 */ OP_ILOAD_0,
    /* 5 */ OP_ILOAD_1,
    /* 6 */ OP_IADD,
    /* 7 */ OP_ISTORE_0,
    /* 8 */ OP_IINC, 1, 1,
    /*11 */ OP_ILOAD_1,
    /*12 */ OP_BIPUSH, 5,
    /*14 */ OP_IF_ICMPLE, 0xFF, 0xF6,
    /*17 */ OP_ILOAD_0,
    /*18 */ OP_IRETURN
};

static const uint8_t test_sipush[]  = { OP_SIPUSH, 0x03, 0xE8, OP_IRETURN };
static const uint8_t test_neg[]     = { OP_BIPUSH, 42, OP_INEG, OP_IRETURN };
static const uint8_t test_and[]     = { OP_SIPUSH, 0x00, 0xFF, OP_BIPUSH, 0x0F, OP_IAND, OP_IRETURN };
static const uint8_t test_shl[]     = { OP_ICONST_1, OP_ICONST_3, OP_ISHL, OP_IRETURN };
static const uint8_t test_dup[]     = { OP_ICONST_5, OP_DUP, OP_IADD, OP_IRETURN };

/* ════════════════════════════════════════════════════════════
 * New tests for previously missing opcodes
 * ════════════════════════════════════════════════════════════ */

/* fconst_1 / fconst_2 — push float constants (result cast to int via store trick) */
/* We just verify they don't crash (return VOID) */
static const uint8_t test_fconst[] = { OP_FCONST_1, OP_POP, OP_FCONST_2, OP_POP, OP_RETURN };

/* aconst_null — push a null reference */
static const uint8_t test_aconst_null[] = { OP_ACONST_NULL, OP_POP, OP_RETURN };

/* i2b — int to byte (sign-extend 8-bit) */
static const uint8_t test_i2b[] = { OP_SIPUSH, 0x01, 0x80, OP_I2B, OP_IRETURN };
/* 0x0180 = 384; (int8_t)384 = (int8_t)0x80 = -128 */

/* i2s — int to short */
static const uint8_t test_i2s[] = { OP_SIPUSH, 0x01, 0x00, OP_I2S, OP_IRETURN };
/* 0x0100 = 256; (int16_t)256 = 256 */

/* dup_x1 — dup top, insert below second */
/* stack before: [2, 1] (top=1), after: [2, 1, 2] — add → [2, 3], add → 5 */
static const uint8_t test_dup_x1[] = {
    OP_ICONST_1,    /* stack: [1]       */
    OP_ICONST_2,    /* stack: [1, 2]    */
    OP_DUP_X1,     /* stack: [2, 1, 2] */
    OP_IADD,        /* stack: [2, 3]    */
    OP_IADD,        /* stack: [5]       */
    OP_IRETURN
};

/* pop2 — discard two values
 * Stack grows upward; push 5, push 3, push 99, push 99.
 * POP2 removes the top two (99, 99), leaving [5, 3].
 * ISUB: 5 - 3 = 2. */
static const uint8_t test_pop2[] = {
    OP_ICONST_5,
    OP_ICONST_3,
    OP_BIPUSH, 99,
    OP_BIPUSH, 99,
    OP_POP2,        /* discard top two (99, 99) */
    OP_ISUB,        /* 5 - 3 = 2 */
    OP_IRETURN
};

/* swap */
static const uint8_t test_swap[] = {
    OP_BIPUSH, 10,
    OP_BIPUSH, 3,
    OP_SWAP,        /* stack: [3, 10] */
    OP_ISUB,        /* 3 - 10 = -7 */
    OP_IRETURN
};

/* aload_0 / astore_0 — store/load a ref via locals */
static const uint8_t test_astore_aload[] = {
    OP_ACONST_NULL,
    OP_ASTORE_0,
    OP_ALOAD_0,
    OP_POP,
    OP_BIPUSH, 42,
    OP_IRETURN
};

/* ifnull — branch if ref is null
 *
 * offset | bytes       | mnemonic
 *      0 | 01          | aconst_null
 *      1 | C6 00 05    | ifnull +5  → target = (4-3)+5 = 6
 *      4 | 03          | iconst_0
 *      5 | AC          | ireturn  ← not-null path (not taken)
 *      6 | 04          | iconst_1
 *      7 | AC          | ireturn  ← null path (taken)
 *
 * aconst_null → ifnull taken → return 1
 */
static const uint8_t test_ifnull[] = {
    /* 0 */ OP_ACONST_NULL,
    /* 1 */ OP_IFNULL, 0x00, 0x05,
    /* 4 */ OP_ICONST_0,
    /* 5 */ OP_IRETURN,
    /* 6 */ OP_ICONST_1,
    /* 7 */ OP_IRETURN
};

/* newarray + arraylength (stubs — just verify no crash, length = 0) */
static const uint8_t test_newarray[] = {
    OP_BIPUSH, 10,
    OP_NEWARRAY, T_INT,
    OP_ARRAYLENGTH,
    OP_IRETURN
};

/* ════════════════════════════════════════════════════════════
 * Error-condition tests (fix verification)
 * ════════════════════════════════════════════════════════════ */

/* FIX #3: fetch_u8 bounds check — bipush with no operand */
static const uint8_t test_truncated_bipush[] = { OP_BIPUSH }; /* no operand byte */

/* FIX #4: stack underflow propagates from arithmetic */
static const uint8_t test_underflow_iadd[] = { OP_IADD, OP_IRETURN };

/* FIX #5: goto with out-of-bounds target */
static const uint8_t test_bad_goto[] = { OP_GOTO, 0x7F, 0xFF }; /* huge forward jump */

/* FIX #5: negative branch wraps */
static const uint8_t test_bad_branch_neg[] = {
    OP_ICONST_0,
    OP_IFEQ, 0xFF, 0x00  /* offset -256 from pc=4 → target = 4-3-256 = negative */
};

/* divide by zero */
static const uint8_t test_div_zero[] = { OP_ICONST_1, OP_ICONST_0, OP_IDIV, OP_IRETURN };

/* ════════════════════════════════════════════════════════════
 * Verbose demo
 * ════════════════════════════════════════════════════════════ */

static void demo_verbose(void)
{
    printf("\n── Verbose trace: 3 + 4 ──\n");
    VM vm; vm_init(&vm, 1);
    int32_t ret = 0;
    vm_exec(&vm, test_add, sizeof(test_add), &ret);
    printf("  result = %d\n\n", ret);
}

/* ════════════════════════════════════════════════════════════
 * Entry point
 * ════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("╔══════════════════════════════════════════╗\n");
    printf("║  KolibriOS J2ME KVM — Bytecode Tests     ║\n");
    printf("╚══════════════════════════════════════════╝\n\n");

    printf("── Original 15 tests ──\n");
    RUN_TEST("return 42",           test_return42,  JVM_RETURN_INT,  42);
    RUN_TEST("3 + 4 = 7",           test_add,       JVM_RETURN_INT,  7);
    RUN_TEST("10 - 3 = 7",          test_sub,       JVM_RETURN_INT,  7);
    RUN_TEST("6 * 7 = 42",          test_mul,       JVM_RETURN_INT,  42);
    RUN_TEST("100 / 4 = 25",        test_div,       JVM_RETURN_INT,  25);
    RUN_TEST("17 %% 5 = 2",         test_rem,       JVM_RETURN_INT,  2);
    RUN_TEST("locals: 10+20 = 30",  test_locals,    JVM_RETURN_INT,  30);
    RUN_TEST("iinc: 5+3 = 8",       test_iinc,      JVM_RETURN_INT,  8);
    RUN_TEST("branch: 7>5 → 1",     test_branch,    JVM_RETURN_INT,  1);
    RUN_TEST("loop: sum(1..5)=15",  test_loop,      JVM_RETURN_INT,  15);
    RUN_TEST("sipush 1000",         test_sipush,    JVM_RETURN_INT,  1000);
    RUN_TEST("neg: -42",            test_neg,       JVM_RETURN_INT,  -42);
    RUN_TEST("0xFF & 0x0F = 15",    test_and,       JVM_RETURN_INT,  15);
    RUN_TEST("1 << 3 = 8",          test_shl,       JVM_RETURN_INT,  8);
    RUN_TEST("dup: 5+5 = 10",       test_dup,       JVM_RETURN_INT,  10);

    printf("\n── New opcode tests (issue #2) ──\n");
    RUN_TEST("fconst_1/2 no crash",    test_fconst,       JVM_RETURN_VOID, 0);
    RUN_TEST("aconst_null no crash",   test_aconst_null,  JVM_RETURN_VOID, 0);
    RUN_TEST("i2b: 0x180 → -128",      test_i2b,          JVM_RETURN_INT,  -128);
    RUN_TEST("i2s: 0x100 → 256",       test_i2s,          JVM_RETURN_INT,  256);
    RUN_TEST("dup_x1: result = 5",     test_dup_x1,       JVM_RETURN_INT,  5);
    RUN_TEST("pop2: 5-3 = 2",          test_pop2,         JVM_RETURN_INT,  2);
    RUN_TEST("swap: 3-10 = -7",        test_swap,         JVM_RETURN_INT,  -7);
    RUN_TEST("astore/aload_0",         test_astore_aload, JVM_RETURN_INT,  42);
    RUN_TEST("ifnull: null → 1",       test_ifnull,       JVM_RETURN_INT,  1);
    RUN_TEST("newarray+arraylength",   test_newarray,     JVM_RETURN_INT,  0);

    printf("\n── Error-condition tests (fixes #3 #4 #5) ──\n");
    RUN_ERR_TEST("fix#3: truncated bipush",    test_truncated_bipush,  JVM_ERR_TRUNCATED_BYTECODE);
    RUN_ERR_TEST("fix#4: underflow in iadd",   test_underflow_iadd,    JVM_ERR_STACK_UNDERFLOW);
    RUN_ERR_TEST("fix#5: bad goto forward",    test_bad_goto,          JVM_ERR_OUT_OF_BOUNDS);
    RUN_ERR_TEST("fix#5: bad branch negative", test_bad_branch_neg,    JVM_ERR_OUT_OF_BOUNDS);
    RUN_ERR_TEST("divide by zero",             test_div_zero,          JVM_ERR_DIVIDE_BY_ZERO);

    printf("\n%d / %d tests passed\n", tests_passed, tests_run);
    demo_verbose();
    return (tests_passed == tests_run) ? 0 : 1;
}
