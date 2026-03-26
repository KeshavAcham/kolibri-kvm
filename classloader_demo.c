/*
 * classloader_demo.c — minimal stub so the `kvm` Makefile target compiles.
 *
 * A real class-file parser would live here. For now this just demonstrates
 * driving vm_exec with a hand-assembled bytecode array, so the binary links
 * and runs without the missing classfile.h / vm_exec_full issues (#1, #9).
 */
#include "jvm.h"
#include <stdio.h>

int main(void)
{
    /*
     * Hand-assembled equivalent of:
     *   int sum = 0;
     *   for (int i = 1; i <= 10; i++) sum += i;
     *   return sum;   // expects 55
     */
    static const uint8_t bytecode[] = {
        /* 0 */ OP_ICONST_0,               /* sum = 0         */
        /* 1 */ OP_ISTORE_0,
        /* 2 */ OP_ICONST_1,               /* i = 1           */
        /* 3 */ OP_ISTORE_1,
        /* 4 */ OP_ILOAD_0,                /* loop: sum += i  */
        /* 5 */ OP_ILOAD_1,
        /* 6 */ OP_IADD,
        /* 7 */ OP_ISTORE_0,
        /* 8 */ OP_IINC, 1, 1,             /* i++             */
        /*11 */ OP_ILOAD_1,
        /*12 */ OP_BIPUSH, 10,
        /*14 */ OP_IF_ICMPLE, 0xFF, 0xF6,  /* if i<=10 → 4    */
        /*17 */ OP_ILOAD_0,
        /*18 */ OP_IRETURN
    };

    VM      vm;
    int32_t result = 0;

    vm_init(&vm, 0);
    JVMResult r = vm_exec(&vm, bytecode, sizeof(bytecode), &result);

    printf("classloader_demo: sum(1..10) = %d  [%s]\n",
           result, jvm_result_str(r));

    return (r == JVM_RETURN_INT && result == 55) ? 0 : 1;
}
