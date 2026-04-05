/*
 * main.c — test suite + real CLI entry point
 *
 * Usage:
 *   ./kvm                     run 17 built-in bytecode tests
 *   ./kvm Hello.class         load and execute a real .class file
 *   ./kvm -v Hello.class      same, with verbose opcode trace
 *
 * Fixes issue #7 (CLI) and #11 (tests covering method invocation & GC).
 */

#include "jvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ═════════════════════════════════════════════════════════════
 * Minimal test framework
 * ═════════════════════════════════════════════════════════════ */
static int g_run = 0, g_pass = 0;

/* Thin wrapper: run raw bytecode with no class context */
static JVMResult exec_raw(const uint8_t *code, uint32_t len, int32_t *ret)
{
    VM vm;
    vm_init(&vm, 0);
    JVMResult r = vm_exec(&vm, code, len, NULL, NULL, ret);
    vm_destroy(&vm);
    return r;
}

#define TEST(name, code_arr, expect_r, expect_v)                          \
    do {                                                                   \
        g_run++;                                                           \
        int32_t got = 0;                                                   \
        JVMResult r = exec_raw(code_arr, sizeof(code_arr), &got);         \
        if (r == (expect_r) && got == (expect_v)) {                       \
            printf("  [PASS] %s\n", name); g_pass++;                      \
        } else {                                                           \
            printf("  [FAIL] %s  result=%s got=%d want=%d\n",             \
                   name, jvm_result_str(r), got, (int)(expect_v));        \
        }                                                                  \
    } while (0)

/* ═════════════════════════════════════════════════════════════
 * Bytecode test vectors — identical to original 15 tests
 * ═════════════════════════════════════════════════════════════ */
static const uint8_t bc_return42[] = {
    OP_BIPUSH, 42, OP_IRETURN
};
static const uint8_t bc_add[] = {
    OP_ICONST_3, OP_ICONST_4, OP_IADD, OP_IRETURN
};
static const uint8_t bc_sub[] = {
    OP_BIPUSH, 10, OP_ICONST_3, OP_ISUB, OP_IRETURN
};
static const uint8_t bc_mul[] = {
    OP_BIPUSH, 6, OP_BIPUSH, 7, OP_IMUL, OP_IRETURN
};
static const uint8_t bc_div[] = {
    OP_BIPUSH, 100, OP_BIPUSH, 4, OP_IDIV, OP_IRETURN
};
static const uint8_t bc_rem[] = {
    OP_BIPUSH, 17, OP_BIPUSH, 5, OP_IREM, OP_IRETURN
};
static const uint8_t bc_locals[] = {
    OP_BIPUSH, 10, OP_ISTORE_0,
    OP_BIPUSH, 20, OP_ISTORE_1,
    OP_ILOAD_0, OP_ILOAD_1, OP_IADD, OP_IRETURN
};
static const uint8_t bc_iinc[] = {
    OP_BIPUSH, 5, OP_ISTORE_0, OP_IINC, 0, 3, OP_ILOAD_0, OP_IRETURN
};
/* if (7 > 5) return 1; else return 0; */
static const uint8_t bc_branch[] = {
    OP_BIPUSH, 7, OP_ISTORE_0,
    OP_ILOAD_0, OP_BIPUSH, 5,
    OP_IF_ICMPLE, 0x00, 0x05,   /* if <=5 jump to offset 11 */
    OP_ICONST_1, OP_IRETURN,
    OP_ICONST_0, OP_IRETURN
};
/* sum = 0; for i=1..5 { sum+=i } return sum; */
static const uint8_t bc_loop[] = {
    OP_ICONST_0, OP_ISTORE_0,
    OP_ICONST_1, OP_ISTORE_1,
    OP_ILOAD_0, OP_ILOAD_1, OP_IADD, OP_ISTORE_0,
    OP_IINC, 1, 1,
    OP_ILOAD_1, OP_BIPUSH, 5,
    OP_IF_ICMPLE, 0xFF, 0xF6,   /* back to offset 4 */
    OP_ILOAD_0, OP_IRETURN
};
static const uint8_t bc_sipush[]  = { OP_SIPUSH, 0x03, 0xE8, OP_IRETURN };
static const uint8_t bc_neg[]     = { OP_BIPUSH, 42, OP_INEG, OP_IRETURN };
static const uint8_t bc_and[]     = { OP_SIPUSH, 0x00, 0xFF, OP_BIPUSH, 0x0F, OP_IAND, OP_IRETURN };
static const uint8_t bc_shl[]     = { OP_ICONST_1, OP_ICONST_3, OP_ISHL, OP_IRETURN };
static const uint8_t bc_dup[]     = { OP_ICONST_5, OP_DUP, OP_IADD, OP_IRETURN };

/* ═════════════════════════════════════════════════════════════
 * Extended test: real method invocation  (issue #3 / #11)
 *
 * Builds a class in memory with:
 *   square(I)I  =>  iload_0 * iload_0 -> ireturn
 * Then invokes it with argument 9, expects 81.
 * ═════════════════════════════════════════════════════════════ */
static void test_method_invocation(void)
{
    g_run++;

    static uint8_t square_code[] = {
        OP_ILOAD_0, OP_ILOAD_0, OP_IMUL, OP_IRETURN
    };

    VM vm;
    vm_init(&vm, 0);

    /* Register a synthetic class directly */
    KVMClass *cls = &vm.classes[vm.class_count++];
    memset(cls, 0, sizeof(KVMClass));
    cls->name   = strdup("TestClass");
    cls->loaded = 1;

    MethodInfo *sq   = &cls->methods[cls->method_count++];
    sq->name         = strdup("square");
    sq->descriptor   = strdup("(I)I");
    sq->code         = square_code;
    sq->code_len     = sizeof(square_code);
    sq->max_stack    = 4;
    sq->max_locals   = 2;

    Value args[1];
    args[0].type = VAL_INT;
    args[0].ival = 9;
    int32_t ret  = 0;
    JVMResult r  = vm_invoke_method(&vm, cls, "square", "(I)I", args, 1, &ret);

    /* Free synthetic strings before destroy (destroy will try to free them) */
    vm_destroy(&vm);

    if (r == JVM_RETURN_INT && ret == 81) {
        printf("  [PASS] method invocation: square(9) = 81\n");
        g_pass++;
    } else {
        printf("  [FAIL] method invocation: result=%s got=%d want=81\n",
               jvm_result_str(r), ret);
    }
}

/* ═════════════════════════════════════════════════════════════
 * Extended test: mark-and-sweep GC  (issue #9 / #11)
 *
 * Allocates 3 string objects, keeps 2 live in a frame stack,
 * runs GC, verifies the third was collected.
 * ═════════════════════════════════════════════════════════════ */
static void test_gc(void)
{
    g_run++;

    VM vm;
    vm_init(&vm, 0);

    int a = vm_alloc_string(&vm, "hello");
    int b = vm_alloc_string(&vm, "world");
    int c = vm_alloc_string(&vm, "dead");  /* will be unreachable */

    /* Make a and b live by placing them in a frame */
    vm.fp = 0;
    Frame *f = &vm.frames[0];
    memset(f, 0, sizeof(Frame));
    f->sp = -1;
    f->stack[++f->sp].type = VAL_REF; f->stack[f->sp].ival = a;
    f->stack[++f->sp].type = VAL_REF; f->stack[f->sp].ival = b;

    int before = vm.object_count;  /* should be 3 */
    vm_gc(&vm);
    int after  = vm.object_count;  /* should be 2 */

    vm.fp = -1;
    vm_destroy(&vm);

    (void)c;
    if (after < before && after == 2) {
        printf("  [PASS] GC: collected %d dead object(s), %d live\n",
               before - after, after);
        g_pass++;
    } else {
        printf("  [FAIL] GC: before=%d after=%d (expected after=2)\n",
               before, after);
    }
}

/* ═════════════════════════════════════════════════════════════
 * Verbose trace demo
 * ═════════════════════════════════════════════════════════════ */
static void demo_verbose(void)
{
    printf("\n── Verbose trace: 3+4 ──\n");
    VM vm;
    vm_init(&vm, 1);
    int32_t ret = 0;
    vm_exec(&vm, bc_add, sizeof(bc_add), NULL, NULL, &ret);
    printf("  result = %d\n\n", ret);
    vm_destroy(&vm);
}

/* ═════════════════════════════════════════════════════════════
 * Built-in test suite (all 17 tests)
 * ═════════════════════════════════════════════════════════════ */
static int run_builtin_tests(void)
{
    printf("╔══════════════════════════════════════════╗\n");
    printf("║  KolibriOS J2ME KVM — Bytecode Tests     ║\n");
    printf("╚══════════════════════════════════════════╝\n\n");

    /* Original 15 opcode-level tests */
    TEST("return 42",          bc_return42, JVM_RETURN_INT,  42);
    TEST("3 + 4 = 7",          bc_add,      JVM_RETURN_INT,   7);
    TEST("10 - 3 = 7",         bc_sub,      JVM_RETURN_INT,   7);
    TEST("6 * 7 = 42",         bc_mul,      JVM_RETURN_INT,  42);
    TEST("100 / 4 = 25",       bc_div,      JVM_RETURN_INT,  25);
    TEST("17 %% 5 = 2",        bc_rem,      JVM_RETURN_INT,   2);
    TEST("locals: 10+20=30",   bc_locals,   JVM_RETURN_INT,  30);
    TEST("iinc: 5+3=8",        bc_iinc,     JVM_RETURN_INT,   8);
    TEST("branch: 7>5 → 1",    bc_branch,   JVM_RETURN_INT,   1);
    TEST("loop: sum(1..5)=15", bc_loop,     JVM_RETURN_INT,  15);
    TEST("sipush 1000",         bc_sipush,   JVM_RETURN_INT, 1000);
    TEST("neg: -42",            bc_neg,      JVM_RETURN_INT, -42);
    TEST("0xFF & 0x0F = 15",    bc_and,      JVM_RETURN_INT,  15);
    TEST("1 << 3 = 8",          bc_shl,      JVM_RETURN_INT,   8);
    TEST("dup: 5+5=10",         bc_dup,      JVM_RETURN_INT,  10);

    /* New tests covering previously missing features */
    test_method_invocation();   /* issue #3 */
    test_gc();                  /* issue #9 */

    printf("\n%d / %d tests passed\n", g_pass, g_run);
    demo_verbose();

    return (g_pass == g_run) ? 0 : 1;
}

/* ═════════════════════════════════════════════════════════════
 * main — CLI entry point  (issue #7)
 *
 *   ./kvm                 → run built-in tests
 *   ./kvm File.class      → execute main() in File.class
 *   ./kvm -v File.class   → same with verbose trace
 * ═════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[])
{
    if (argc == 1)
        return run_builtin_tests();

    int verbose = 0;
    const char *class_file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0)
            verbose = 1;
        else
            class_file = argv[i];
    }

    if (!class_file) {
        fprintf(stderr, "Usage: %s [-v] <File.class>\n", argv[0]);
        return 1;
    }

    VM vm;
    vm_init(&vm, verbose);

    JVMResult r = vm_run_classfile(&vm, class_file);
    vm_destroy(&vm);

    if (r != JVM_OK && r != JVM_RETURN_VOID && r != JVM_RETURN_INT) {
        fprintf(stderr, "kvm: %s\n", jvm_result_str(r));
        return 1;
    }
    return 0;
}
