#ifndef JVM_H
#define JVM_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>   /* isnan(), fmodf() */

/* ─── sizes ─── */
#define MAX_STACK   64
#define MAX_LOCALS  16
#define MAX_FRAMES  32

/* ─── newarray type constants (JVM spec Table 6.5.newarray-A) ─── */
#define T_BOOLEAN   4
#define T_CHAR      5
#define T_FLOAT     6
#define T_DOUBLE    7
#define T_BYTE      8
#define T_SHORT     9
#define T_INT       10
#define T_LONG      11

/* ─── JVM opcodes (CLDC 1.1 subset) ─── */
#define OP_NOP            0x00
#define OP_ACONST_NULL    0x01
#define OP_ICONST_M1      0x02
#define OP_ICONST_0       0x03
#define OP_ICONST_1       0x04
#define OP_ICONST_2       0x05
#define OP_ICONST_3       0x06
#define OP_ICONST_4       0x07
#define OP_ICONST_5       0x08
#define OP_FCONST_0       0x0B
#define OP_FCONST_1       0x0C
#define OP_FCONST_2       0x0D
#define OP_BIPUSH         0x10
#define OP_SIPUSH         0x11
#define OP_LDC            0x12
#define OP_ILOAD          0x15
#define OP_ALOAD          0x19
#define OP_ILOAD_0        0x1A
#define OP_ILOAD_1        0x1B
#define OP_ILOAD_2        0x1C
#define OP_ILOAD_3        0x1D
#define OP_ALOAD_0        0x2A
#define OP_ALOAD_1        0x2B
#define OP_ALOAD_2        0x2C
#define OP_ALOAD_3        0x2D
#define OP_IALOAD         0x2E
#define OP_AALOAD         0x32
#define OP_BALOAD         0x33
#define OP_CALOAD         0x34
#define OP_ISTORE         0x36
#define OP_ASTORE         0x3A
#define OP_ISTORE_0       0x3B
#define OP_ISTORE_1       0x3C
#define OP_ISTORE_2       0x3D
#define OP_ISTORE_3       0x3E
#define OP_ASTORE_0       0x4B
#define OP_ASTORE_1       0x4C
#define OP_ASTORE_2       0x4D
#define OP_ASTORE_3       0x4E
#define OP_IASTORE        0x4F
#define OP_AASTORE        0x53
#define OP_BASTORE        0x54
#define OP_CASTORE        0x55
#define OP_POP            0x57
#define OP_POP2           0x58
#define OP_DUP            0x59
#define OP_DUP_X1         0x5A
#define OP_DUP_X2         0x5B
#define OP_DUP2           0x5C
#define OP_SWAP           0x5F
#define OP_IADD           0x60
#define OP_FADD           0x62
#define OP_ISUB           0x64
#define OP_FSUB           0x66
#define OP_IMUL           0x68
#define OP_FMUL           0x6A
#define OP_IDIV           0x6C
#define OP_FDIV           0x6E
#define OP_IREM           0x70
#define OP_FREM           0x72
#define OP_INEG           0x74
#define OP_FNEG           0x76
#define OP_ISHL           0x78
#define OP_ISHR           0x7A
#define OP_IUSHR          0x7C
#define OP_IAND           0x7E
#define OP_IOR            0x80
#define OP_IXOR           0x82
#define OP_IINC           0x84
#define OP_I2F            0x86
#define OP_I2B            0x91
#define OP_I2C            0x92
#define OP_I2S            0x93
#define OP_F2I            0x8B
#define OP_FCMPL          0x95
#define OP_FCMPG          0x96
#define OP_IFEQ           0x99
#define OP_IFNE           0x9A
#define OP_IFLT           0x9B
#define OP_IFGE           0x9C
#define OP_IFGT           0x9D
#define OP_IFLE           0x9E
#define OP_IF_ICMPEQ      0x9F
#define OP_IF_ICMPNE      0xA0
#define OP_IF_ICMPLT      0xA1
#define OP_IF_ICMPGE      0xA2
#define OP_IF_ICMPGT      0xA3
#define OP_IF_ICMPLE      0xA4
#define OP_IF_ACMPEQ      0xA5
#define OP_IF_ACMPNE      0xA6
#define OP_GOTO           0xA7
#define OP_IRETURN        0xAC
#define OP_FRETURN        0xAE
#define OP_ARETURN        0xB0
#define OP_RETURN         0xB1
#define OP_GETSTATIC      0xB2
#define OP_PUTSTATIC      0xB3
#define OP_GETFIELD       0xB4
#define OP_PUTFIELD       0xB5
#define OP_INVOKEVIRTUAL  0xB6
#define OP_INVOKESPECIAL  0xB7
#define OP_INVOKESTATIC   0xB8
#define OP_NEW            0xBB
#define OP_NEWARRAY       0xBC
#define OP_ANEWARRAY      0xBD
#define OP_ARRAYLENGTH    0xBE
#define OP_ATHROW         0xBF
#define OP_CHECKCAST      0xC0
#define OP_INSTANCEOF     0xC1
#define OP_IFNULL         0xC6
#define OP_IFNONNULL      0xC7

/* ─── Constant pool entry (authoritative — classfile.h must not redefine) ─── */
typedef enum {
    CP_EMPTY   = 0,
    CP_INTEGER = 3,
    CP_FLOAT   = 4,
    CP_STRING  = 8
} CPTag;

typedef struct {
    CPTag tag;
    union {
        int32_t     ival;
        float       fval;
        const char *sval;
    };
} CPEntry;

/* ─── Exception table entry (authoritative) ─── */
typedef struct {
    uint32_t start_pc;
    uint32_t end_pc;
    uint32_t handler_pc;
    uint16_t catch_type;   /* 0 = catch-all */
} ExceptionEntry;

/* ─── Value types on the operand stack ─── */
typedef enum { VAL_INT = 0, VAL_FLOAT = 1, VAL_REF = 2 } ValType;

typedef struct {
    ValType type;
    union {
        int32_t ival;   /* integer or heap reference index */
        float   fval;
    };
} Value;

/* ─── Heap object ─── */
#define MAX_ARRAY_ELEMS 256
#define MAX_OBJECTS     512
#define GC_THRESHOLD    384

typedef enum { OBJ_NONE = 0, OBJ_INT_ARRAY, OBJ_REF_ARRAY } ObjType;

typedef struct {
    ObjType  type;
    int      in_use;
    int      marked;       /* GC mark bit */
    int32_t  length;
    int32_t  idata[MAX_ARRAY_ELEMS];
} JObject;

/* ─── One activation frame ─── */
typedef struct {
    const uint8_t        *code;
    uint32_t              code_len;
    uint32_t              pc;
    Value                 locals[MAX_LOCALS];
    Value                 stack[MAX_STACK];
    int                   sp;
    const CPEntry        *cp;
    uint16_t              cp_len;
    const ExceptionEntry *exc_table;
    uint16_t              exc_table_len;
} Frame;

/* ─── The VM ─── */
typedef struct {
    Frame   frames[MAX_FRAMES];
    int     fp;
    JObject objects[MAX_OBJECTS];
    int     obj_count;
    int     verbose;
} VM;

/* ─── Result codes ─── */
typedef enum {
    JVM_OK = 0,
    JVM_RETURN_INT,
    JVM_RETURN_FLOAT,
    JVM_RETURN_REF,              /* issue #14: added */
    JVM_RETURN_VOID,
    JVM_ERR_STACK_OVERFLOW,
    JVM_ERR_STACK_UNDERFLOW,
    JVM_ERR_UNKNOWN_OPCODE,
    JVM_ERR_DIVIDE_BY_ZERO,
    JVM_ERR_OUT_OF_BOUNDS,
    JVM_ERR_NO_FRAME,
    JVM_ERR_NULL_POINTER,
    JVM_ERR_INVALID_CP,
    JVM_ERR_ARRAY_INDEX,
    JVM_ERR_OUT_OF_MEMORY,
    JVM_ERR_NEGATIVE_ARRAY_SIZE
} JVMResult;

/* ─── Public API ─── */
void       vm_init     (VM *vm, int verbose);
void       vm_gc       (VM *vm);
JVMResult  vm_exec     (VM *vm, const uint8_t *code, uint32_t len,
                        int32_t *ret_val);
JVMResult  vm_exec_full(VM *vm, const uint8_t *code, uint32_t len,
                        const CPEntry *cp, uint16_t cp_len,
                        const ExceptionEntry *exc, uint16_t exc_len,
                        int32_t *ret_val);
const char *jvm_result_str(JVMResult r);

#endif /* JVM_H */
