#include "jvm.h"
#include <stdio.h>
#include <stdlib.h>

static int tests_run = 0, tests_passed = 0;

#define RUN_TEST(name, code_arr, expected_ret, expected_val) \
    do { \
        tests_run++; \
        VM vm; vm_init(&vm, 0); \
        int32_t ret = 0; \
        JVMResult r = vm_exec(&vm, code_arr, sizeof(code_arr), &ret); \
        if (r == (expected_ret) && ret == (expected_val)) { \
            printf("  [PASS] %s\n", name); tests_passed++; \
        } else { \
            printf("  [FAIL] %-35s result=%-22s got=%d want=%d\n", \
                   name, jvm_result_str(r), ret, (int)(expected_val)); \
        } \
    } while(0)

#define RUN_TEST_ERR(name, code_arr, expected_err) \
    do { \
        tests_run++; \
        VM vm; vm_init(&vm, 0); \
        int32_t ret = 0; \
        JVMResult r = vm_exec(&vm, code_arr, sizeof(code_arr), &ret); \
        if (r == (expected_err)) { \
            printf("  [PASS] %s  (→ %s)\n", name, jvm_result_str(r)); tests_passed++; \
        } else { \
            printf("  [FAIL] %-35s got=%s want=%s\n", name, jvm_result_str(r), jvm_result_str(expected_err)); \
        } \
    } while(0)

/* ══════════════════════════════════════════════════════
 *  Original 15 tests
 * ══════════════════════════════════════════════════════ */
static const uint8_t test_return42[]  = { OP_BIPUSH, 42, OP_IRETURN };
static const uint8_t test_add[]       = { OP_ICONST_3, OP_ICONST_4, OP_IADD, OP_IRETURN };
static const uint8_t test_sub[]       = { OP_BIPUSH,10, OP_ICONST_3, OP_ISUB, OP_IRETURN };
static const uint8_t test_mul[]       = { OP_BIPUSH,6, OP_BIPUSH,7, OP_IMUL, OP_IRETURN };
static const uint8_t test_div[]       = { OP_BIPUSH,100, OP_BIPUSH,4, OP_IDIV, OP_IRETURN };
static const uint8_t test_rem[]       = { OP_BIPUSH,17, OP_BIPUSH,5, OP_IREM, OP_IRETURN };
static const uint8_t test_locals[]    = { OP_BIPUSH,10, OP_ISTORE_0, OP_BIPUSH,20, OP_ISTORE_1,
                                          OP_ILOAD_0, OP_ILOAD_1, OP_IADD, OP_IRETURN };
static const uint8_t test_iinc[]      = { OP_BIPUSH,5, OP_ISTORE_0, OP_IINC,0,3, OP_ILOAD_0, OP_IRETURN };
static const uint8_t test_branch[]    = { OP_BIPUSH,7, OP_ISTORE_0, OP_ILOAD_0, OP_BIPUSH,5,
                                          OP_IF_ICMPLE,0x00,0x05, OP_ICONST_1, OP_IRETURN,
                                          OP_ICONST_0, OP_IRETURN };
static const uint8_t test_loop[]      = { OP_ICONST_0, OP_ISTORE_0, OP_ICONST_1, OP_ISTORE_1,
                                          OP_ILOAD_0, OP_ILOAD_1, OP_IADD, OP_ISTORE_0,
                                          OP_IINC,1,1, OP_ILOAD_1, OP_BIPUSH,5,
                                          OP_IF_ICMPLE,0xFF,0xF6, OP_ILOAD_0, OP_IRETURN };
static const uint8_t test_sipush[]    = { OP_SIPUSH,0x03,0xE8, OP_IRETURN };
static const uint8_t test_neg[]       = { OP_BIPUSH,42, OP_INEG, OP_IRETURN };
static const uint8_t test_and[]       = { OP_SIPUSH,0x00,0xFF, OP_BIPUSH,0x0F, OP_IAND, OP_IRETURN };
static const uint8_t test_shl[]       = { OP_ICONST_1, OP_ICONST_3, OP_ISHL, OP_IRETURN };
static const uint8_t test_dup[]       = { OP_ICONST_5, OP_DUP, OP_IADD, OP_IRETURN };

/* ══════════════════════════════════════════════════════
 *  New: float opcodes
 * ══════════════════════════════════════════════════════ */
/* fconst_1 + fconst_2 + fadd → 3.0 → f2i → 3 */
static const uint8_t test_float_add[] = { OP_FCONST_1, OP_FCONST_2, OP_FADD, OP_F2I, OP_IRETURN };
/* fconst_2 * fconst_2 = 4.0 → 4 */
static const uint8_t test_float_mul[] = { OP_FCONST_2, OP_FCONST_2, OP_FMUL, OP_F2I, OP_IRETURN };
/* i2f then f2i round-trip: 7 → 7.0 → 7 */
static const uint8_t test_i2f[]       = { OP_BIPUSH,7, OP_I2F, OP_F2I, OP_IRETURN };

/* ══════════════════════════════════════════════════════
 *  New: reference / object ops
 * ══════════════════════════════════════════════════════ */
/* aconst_null → areturn → ret_val=0 */
static const uint8_t test_aconst_null[] = { OP_ACONST_NULL, OP_ARETURN };

/* new → astore_0 → aload_0 → arraylength would need array;
   test astore/aload round-trip via ref */
static const uint8_t test_astore_load[] = {
    OP_ACONST_NULL,   /* push null ref */
    OP_ASTORE_0,      /* store in local[0] */
    OP_ALOAD_0,       /* load back */
    OP_ARETURN        /* return ref (0=null) */
};

/* ══════════════════════════════════════════════════════
 *  New: array ops
 * ══════════════════════════════════════════════════════ */
/* newarray int[5], store 42 at [2], load [2] → ireturn */
static const uint8_t test_intarray[] = {
    OP_BIPUSH, 5,           /* count=5 */
    OP_NEWARRAY, T_INT,     /* int[5] */
    OP_ASTORE_0,            /* arr = local[0] */
    OP_ALOAD_0,             /* arr */
    OP_BIPUSH, 2,           /* index=2 */
    OP_BIPUSH, 42,          /* value=42 */
    OP_IASTORE,             /* arr[2]=42 */
    OP_ALOAD_0,             /* arr */
    OP_BIPUSH, 2,           /* index=2 */
    OP_IALOAD,              /* arr[2] */
    OP_IRETURN
};

/* arraylength: new int[7] → arraylength → ireturn → 7 */
static const uint8_t test_arraylength[] = {
    OP_BIPUSH, 7,
    OP_NEWARRAY, T_INT,
    OP_ARRAYLENGTH,
    OP_IRETURN
};

/* ══════════════════════════════════════════════════════
 *  New: type conversions
 * ══════════════════════════════════════════════════════ */
/* i2b: 300 → (byte) → 44 */
static const uint8_t test_i2b[] = { OP_SIPUSH,0x01,0x2C, OP_I2B, OP_IRETURN };
/* i2s: 70000 → (short) → 4464 */
static const uint8_t test_i2s[] = { OP_SIPUSH,0x11,0x70, OP_I2S, OP_IRETURN };

/* ══════════════════════════════════════════════════════
 *  New: stack ops
 * ══════════════════════════════════════════════════════ */
/* dup_x1: push 1,2 → dup_x1 → stack: 1,2,1... pop top three and verify */
/* easier: push 3, push 5, dup_x1 gives 5,3,5 → iadd → 8, swap, pop → ireturn 3+5=8 */
static const uint8_t test_dup_x1[] = {
    OP_ICONST_3,   /* stack: 3 */
    OP_ICONST_5,   /* stack: 3,5 */
    OP_DUP_X1,     /* stack: 5,3,5 */
    OP_POP,        /* stack: 5,3 */
    OP_IADD,       /* stack: 8 */
    OP_IRETURN
};

/* pop2: push two values, pop2, push 99 */
static const uint8_t test_pop2[] = {
    OP_BIPUSH, 10,
    OP_BIPUSH, 20,
    OP_POP2,
    OP_BIPUSH, 99,
    OP_IRETURN
};

/* ══════════════════════════════════════════════════════
 *  New: null/ref branches
 * ══════════════════════════════════════════════════════ */
/* ifnull: push null → ifnull → return 1, else return 0 */
static const uint8_t test_ifnull[] = {
    OP_ACONST_NULL,
    OP_IFNULL, 0x00, 0x05,   /* if null → jump to offset 7 */
    OP_ICONST_0,
    OP_IRETURN,
    OP_ICONST_1,             /* offset 7 */
    OP_IRETURN
};

/* ══════════════════════════════════════════════════════
 *  New: wider locals (iload/istore with index)
 * ══════════════════════════════════════════════════════ */
static const uint8_t test_iload_istore[] = {
    OP_BIPUSH, 55,
    OP_ISTORE, 10,    /* store in local[10] */
    OP_ILOAD,  10,    /* load from local[10] */
    OP_IRETURN
};

/* ══════════════════════════════════════════════════════
 *  Bug regression
 * ══════════════════════════════════════════════════════ */
static const uint8_t bug1[] = { OP_BIPUSH };
static const uint8_t bug2[] = { OP_IADD, OP_IRETURN };
static const uint8_t bug3[] = { OP_GOTO, 0xFF, 0x00 };

/* ══════════════════════════════════════════════════════
 *  Entry point
 * ══════════════════════════════════════════════════════ */
int main(void) {
    printf("╔══════════════════════════════════════════╗\n");
    printf("║  KolibriOS J2ME KVM — Full Test Suite    ║\n");
    printf("╚══════════════════════════════════════════╝\n\n");

    printf("── Original tests ──\n");
    RUN_TEST("return 42",             test_return42, JVM_RETURN_INT,  42);
    RUN_TEST("3 + 4 = 7",            test_add,      JVM_RETURN_INT,   7);
    RUN_TEST("10 - 3 = 7",           test_sub,      JVM_RETURN_INT,   7);
    RUN_TEST("6 * 7 = 42",           test_mul,      JVM_RETURN_INT,  42);
    RUN_TEST("100 / 4 = 25",         test_div,      JVM_RETURN_INT,  25);
    RUN_TEST("17 %% 5 = 2",          test_rem,      JVM_RETURN_INT,   2);
    RUN_TEST("locals: 10+20=30",     test_locals,   JVM_RETURN_INT,  30);
    RUN_TEST("iinc: 5+3=8",          test_iinc,     JVM_RETURN_INT,   8);
    RUN_TEST("branch: 7>5 → 1",      test_branch,   JVM_RETURN_INT,   1);
    RUN_TEST("loop: sum(1..5)=15",   test_loop,     JVM_RETURN_INT,  15);
    RUN_TEST("sipush 1000",          test_sipush,   JVM_RETURN_INT,1000);
    RUN_TEST("neg: -42",             test_neg,      JVM_RETURN_INT, -42);
    RUN_TEST("0xFF & 0x0F = 15",     test_and,      JVM_RETURN_INT,  15);
    RUN_TEST("1 << 3 = 8",           test_shl,      JVM_RETURN_INT,   8);
    RUN_TEST("dup: 5+5=10",          test_dup,      JVM_RETURN_INT,  10);

    printf("\n── Float opcodes ──\n");
    RUN_TEST("fconst 1.0+2.0=3",     test_float_add, JVM_RETURN_INT,  3);
    RUN_TEST("fconst 2.0*2.0=4",     test_float_mul, JVM_RETURN_INT,  4);
    RUN_TEST("i2f→f2i round-trip 7", test_i2f,       JVM_RETURN_INT,  7);

    printf("\n── Reference / object ops ──\n");
    RUN_TEST("aconst_null areturn",  test_aconst_null, JVM_RETURN_REF, 0);
    RUN_TEST("astore/aload null",    test_astore_load, JVM_RETURN_REF, 0);

    printf("\n── Array ops ──\n");
    RUN_TEST("int[5] store/load[2]", test_intarray,   JVM_RETURN_INT, 42);
    RUN_TEST("int[7] arraylength",   test_arraylength, JVM_RETURN_INT,  7);

    printf("\n── Type conversions ──\n");
    RUN_TEST("i2b: 300→44",          test_i2b, JVM_RETURN_INT, 44);
    RUN_TEST("i2s: 0x1170→4464",     test_i2s, JVM_RETURN_INT, 4464);

    printf("\n── Stack ops ──\n");
    RUN_TEST("dup_x1",               test_dup_x1, JVM_RETURN_INT, 8);
    RUN_TEST("pop2",                 test_pop2,   JVM_RETURN_INT, 99);

    printf("\n── Null/ref branches ──\n");
    RUN_TEST("ifnull taken",         test_ifnull, JVM_RETURN_INT, 1);

    printf("\n── Wide locals ──\n");
    RUN_TEST("istore/iload local[10]", test_iload_istore, JVM_RETURN_INT, 55);

    printf("\n── Bug regressions ──\n");
    RUN_TEST_ERR("bug1: bipush no operand", bug1, JVM_ERR_OUT_OF_BOUNDS); 
    RUN_TEST_ERR("bug2: iadd empty stack", bug2, JVM_ERR_STACK_UNDERFLOW);
    RUN_TEST_ERR("bug3: goto negative target", bug3, JVM_ERR_OUT_OF_BOUNDS);

    printf("\n%d / %d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
