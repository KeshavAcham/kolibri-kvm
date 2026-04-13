#ifndef JVM_H
#define JVM_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── sizes ─── */
#define MAX_STACK    64
#define MAX_LOCALS   32          /* enlarged: long/double use 2 slots */
#define MAX_FRAMES   32
#define HEAP_SIZE    (64 * 1024) /* 64 KB Java heap */
#define MAX_OBJECTS  256         /* max live heap objects */
#define MAX_FIELDS   16          /* max fields per object class */
#define MAX_CLASSES  32          /* max loaded classes */

/* ─── JVM opcodes (CLDC 1.1 subset) ─── */
#define OP_NOP          0x00
#define OP_ACONST_NULL  0x01
#define OP_ICONST_M1    0x02
#define OP_ICONST_0     0x03
#define OP_ICONST_1     0x04
#define OP_ICONST_2     0x05
#define OP_ICONST_3     0x06
#define OP_ICONST_4     0x07
#define OP_ICONST_5     0x08
#define OP_LCONST_0     0x09
#define OP_LCONST_1     0x0A
#define OP_BIPUSH       0x10
#define OP_SIPUSH       0x11
#define OP_ILOAD        0x15
#define OP_LLOAD        0x16    /* load long (2-slot) from local */
#define OP_ALOAD        0x19    /* load reference from local */
#define OP_ILOAD_0      0x1A
#define OP_ILOAD_1      0x1B
#define OP_ILOAD_2      0x1C
#define OP_ILOAD_3      0x1D
#define OP_ALOAD_0      0x2A
#define OP_ALOAD_1      0x2B
#define OP_ALOAD_2      0x2C
#define OP_ALOAD_3      0x2D
#define OP_ISTORE       0x36
#define OP_LSTORE       0x37    /* store long (2-slot) into local */
#define OP_ASTORE       0x3A    /* store reference into local */
#define OP_ISTORE_0     0x3B
#define OP_ISTORE_1     0x3C
#define OP_ISTORE_2     0x3D
#define OP_ISTORE_3     0x3E
#define OP_ASTORE_0     0x4B
#define OP_ASTORE_1     0x4C
#define OP_ASTORE_2     0x4D
#define OP_ASTORE_3     0x4E
#define OP_IADD         0x60
#define OP_LADD         0x61
#define OP_ISUB         0x64
#define OP_LSUB         0x65
#define OP_IMUL         0x68
#define OP_LMUL         0x69
#define OP_IDIV         0x6C
#define OP_LDIV         0x6D
#define OP_IREM         0x70
#define OP_LREM         0x71
#define OP_INEG         0x74
#define OP_LNEG         0x75
#define OP_IINC         0x84
#define OP_I2L          0x85    /* int -> long */
#define OP_L2I          0x88    /* long -> int */
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
#define OP_IF_ACMPEQ    0xA5
#define OP_IF_ACMPNE    0xA6
#define OP_GOTO         0xA7
#define OP_IRETURN      0xAC
#define OP_LRETURN      0xAD
#define OP_ARETURN      0xB0
#define OP_RETURN       0xB1
#define OP_GETSTATIC    0xB2
#define OP_PUTSTATIC    0xB3
#define OP_GETFIELD     0xB4
#define OP_PUTFIELD     0xB5
#define OP_INVOKEVIRTUAL  0xB6
#define OP_INVOKESPECIAL  0xB7
#define OP_INVOKESTATIC   0xB8
#define OP_NEW          0xBB
#define OP_IFNULL       0xC6
#define OP_IFNONNULL    0xC7
#define OP_ISHL         0x78
#define OP_ISHR         0x7A
#define OP_IUSHR        0x7C
#define OP_IAND         0x7E
#define OP_IOR          0x80
#define OP_IXOR         0x82
#define OP_I2C          0x92    /* int -> char (16-bit) */
#define OP_I2B          0x91    /* int -> byte */
#define OP_I2S          0x93    /* int -> short */
#define OP_DUP          0x59
#define OP_DUP2         0x5C    /* dup top 2 values (or one cat-2) */
#define OP_POP          0x57
#define OP_POP2         0x58
#define OP_SWAP         0x5F
#define OP_LCMP         0x94    /* long compare */

/* ─── value categories ─── */
typedef enum {
    VAL_INT  = 0,   /* category 1: int, char, byte, short, boolean */
    VAL_REF  = 1,   /* category 1: reference */
    VAL_LONG = 2,   /* category 2: long (occupies 2 local slots) */
    VAL_LONG2= 3,   /* upper half of a long slot (not pushed directly) */
} ValType;

/* ─── stack/local value ─── */
typedef struct {
    ValType  type;
    int32_t  ival;   /* int or reference (object index into vm->objects) */
    int64_t  lval;   /* long value (only valid when type == VAL_LONG) */
} Value;

/* ─── field descriptor (for getfield/putfield caching) ─── */
typedef struct {
    char     name[32];
    Value    value;
} FieldSlot;

/* ─── heap object ─── */
typedef struct {
    int       alive;                  /* 1 = reachable, 0 = free after GC */
    int       class_id;               /* which class this belongs to       */
    int       field_count;
    FieldSlot fields[MAX_FIELDS];

    /* GC forwarding pointer (used during compaction) */
    int       fwd_index;              /* -1 = not moved yet */
} HeapObject;

/* ─── simple class descriptor (for field-index caching) ─── */
typedef struct {
    char  name[64];
    int   field_count;
    char  field_names[MAX_FIELDS][32];
    int   field_slots[MAX_FIELDS];   /* pre-computed slot indices */
} ClassDesc;

/* ─── native method descriptor ─── */
typedef enum {
    DESC_VOID_INT    = 0,  /* (I)V  */
    DESC_VOID_STRING = 1,  /* (Ljava/lang/String;)V */
    DESC_VOID_VOID   = 2,  /* ()V   */
    DESC_INT_VOID    = 3,  /* ()I   */
    DESC_UNKNOWN     = 99
} MethodDesc;

/* ─── one activation frame ─── */
typedef struct {
    const uint8_t *code;
    uint32_t       code_len;
    uint32_t       pc;
    int            code_owned;        /* FIX #8: explicit ownership flag */
    Value          locals[MAX_LOCALS];
    Value          stack[MAX_STACK];
    int            sp;                /* -1 = empty */
} Frame;

/* ─── the VM ─── */
typedef struct {
    Frame      frames[MAX_FRAMES];
    int        fp;                    /* frame pointer (-1 = no frame)  */

    /* Heap (object pool — replaces raw byte array for structured GC) */
    HeapObject objects[MAX_OBJECTS];
    int        object_count;

    /* Class registry */
    ClassDesc  classes[MAX_CLASSES];
    int        class_count;

    /* System.out handle — FIX #5: not hardcoded to index 0 */
    int        system_out_ref;        /* index into objects[], set on init */

    int        verbose;
} VM;

/* ─── result codes ─── */
typedef enum {
    JVM_OK = 0,
    JVM_RETURN_INT,
    JVM_RETURN_LONG,
    JVM_RETURN_VOID,
    JVM_RETURN_REF,
    JVM_ERR_STACK_OVERFLOW,
    JVM_ERR_STACK_UNDERFLOW,
    JVM_ERR_UNKNOWN_OPCODE,
    JVM_ERR_DIVIDE_BY_ZERO,
    JVM_ERR_OUT_OF_BOUNDS,
    JVM_ERR_NO_FRAME,
    JVM_ERR_NULL_POINTER,
    JVM_ERR_OUT_OF_MEMORY,
    JVM_ERR_TYPE_MISMATCH
} JVMResult;

/* ─── method struct (for vm_invoke_method) ─── */
typedef struct {
    const uint8_t *code;
    uint32_t       code_len;
    int            code_owned;        /* FIX #8: must be set explicitly  */
    int            max_locals;
    int            max_stack;
    const char    *descriptor;        /* e.g. "(II)I" */
} Method;

/* ─── public API ─── */
void      vm_init          (VM *vm, int verbose);
void      vm_destroy       (VM *vm);
JVMResult vm_exec          (VM *vm, const uint8_t *code, uint32_t len,
                             int32_t *ret_val);
JVMResult vm_invoke_method (VM *vm, const Method *m, Value *args, int argc,
                             Value *ret_val);
int       vm_alloc_object  (VM *vm, int class_id);
void      vm_gc            (VM *vm);
int       vm_register_class(VM *vm, const char *name,
                             const char **field_names, int nfields);
int       vm_find_field    (VM *vm, int class_id, const char *name);

const char *jvm_result_str (JVMResult r);

#endif /* JVM_H */
