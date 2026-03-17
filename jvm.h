#ifndef JVM_H
#define JVM_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── sizes ─── */
#define MAX_STACK       64
#define MAX_LOCALS      16
#define MAX_FRAMES      32
#define HEAP_SIZE       (64 * 1024)   /* 64 KB Java heap */

/* ─── JVM opcodes (CLDC 1.1 subset) ─── */
#define OP_NOP          0x00
#define OP_ICONST_M1    0x02
#define OP_ICONST_0     0x03
#define OP_ICONST_1     0x04
#define OP_ICONST_2     0x05
#define OP_ICONST_3     0x06
#define OP_ICONST_4     0x07
#define OP_ICONST_5     0x08
#define OP_BIPUSH       0x10
#define OP_SIPUSH       0x11
#define OP_ILOAD        0x15
#define OP_ILOAD_0      0x1A
#define OP_ILOAD_1      0x1B
#define OP_ILOAD_2      0x1C
#define OP_ILOAD_3      0x1D
#define OP_ISTORE       0x36
#define OP_ISTORE_0     0x3B
#define OP_ISTORE_1     0x3C
#define OP_ISTORE_2     0x3D
#define OP_ISTORE_3     0x3E
#define OP_IADD         0x60
#define OP_ISUB         0x64
#define OP_IMUL         0x68
#define OP_IDIV         0x6C
#define OP_IREM         0x70
#define OP_INEG         0x74
#define OP_IINC         0x84
#define OP_IFEQ         0x99
#define OP_IFNE         0x9A
#define OP_IFLT         0x9B
#define OP_IFGE         0x9C
#define OP_IFGT         0x9D
#define OP_IFLE         0x9E
#define OP_IF_ICMPEQ    0x9F
#define OP_IF_ICMPNE    0xA0
#define OP_IF_ICMPLT    0xA1
#define OP_IF_ICMPGE    0xA2
#define OP_IF_ICMPGT    0xA3
#define OP_IF_ICMPLE    0xA4
#define OP_GOTO         0xA7
#define OP_IRETURN      0xAC
#define OP_RETURN       0xB1
#define OP_GETSTATIC    0xB2   /* stub — for System.out.println demo */
#define OP_INVOKEVIRTUAL 0xB6  /* stub — for System.out.println demo */
#define OP_INVOKESTATIC 0xB8   /* stub */
#define OP_ISHL         0x78
#define OP_ISHR         0x7A
#define OP_IUSHR        0x7C
#define OP_IAND         0x7E
#define OP_IOR          0x80
#define OP_IXOR         0x82
#define OP_I2C          0x92   /* int → char (truncate to 16-bit) */
#define OP_DUP          0x59
#define OP_POP          0x57
#define OP_SWAP         0x5F

/* ─── value types on the operand stack ─── */
typedef enum { VAL_INT = 0, VAL_REF = 1 } ValType;

typedef struct {
    ValType type;
    int32_t ival;   /* integer / reference value */
} Value;

/* ─── one activation frame ─── */
typedef struct {
    const uint8_t *code;        /* pointer to bytecode array        */
    uint32_t       code_len;    /* length of bytecode array         */
    uint32_t       pc;          /* program counter (index into code) */
    Value          locals[MAX_LOCALS];
    Value          stack[MAX_STACK];
    int            sp;          /* stack pointer (-1 = empty)        */
} Frame;

/* ─── the VM itself ─── */
typedef struct {
    Frame   frames[MAX_FRAMES];
    int     fp;             /* frame pointer (-1 = no frame)     */
    uint8_t heap[HEAP_SIZE];
    int     heap_top;
    int     verbose;        /* set to 1 to print each opcode     */
} VM;

/* ─── result codes ─── */
typedef enum {
    JVM_OK = 0,
    JVM_RETURN_INT,
    JVM_RETURN_VOID,
    JVM_ERR_STACK_OVERFLOW,
    JVM_ERR_STACK_UNDERFLOW,
    JVM_ERR_UNKNOWN_OPCODE,
    JVM_ERR_DIVIDE_BY_ZERO,
    JVM_ERR_OUT_OF_BOUNDS,
    JVM_ERR_NO_FRAME
} JVMResult;

/* ─── public API ─── */
void      vm_init   (VM *vm, int verbose);
JVMResult vm_exec   (VM *vm, const uint8_t *code, uint32_t len,
                     int32_t *ret_val);
const char *jvm_result_str(JVMResult r);

#endif /* JVM_H */ 
