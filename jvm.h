#ifndef JVM_H
#define JVM_H

#define _DEFAULT_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─────────────────────────────────────────────────────────────
 * Compile-time limits  (issue #8 — all bounds checked at runtime)
 * ───────────────────────────────────────────────────────────── */
#define MAX_STACK       64
#define MAX_LOCALS      32
#define MAX_FRAMES      32
#define MAX_CLASSES     64
#define MAX_CP_ENTRIES  256
#define MAX_METHODS     64
#define MAX_FIELDS      32
#define MAX_OBJECTS     1024
#define HEAP_SIZE       (256 * 1024)   /* 256 KB raw byte heap */

/* ─────────────────────────────────────────────────────────────
 * JVM opcodes — CLDC 1.1 subset
 * ───────────────────────────────────────────────────────────── */
#define OP_NOP            0x00
#define OP_ACONST_NULL    0x01
#define OP_ICONST_M1      0x02
#define OP_ICONST_0       0x03
#define OP_ICONST_1       0x04
#define OP_ICONST_2       0x05
#define OP_ICONST_3       0x06
#define OP_ICONST_4       0x07
#define OP_ICONST_5       0x08
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
#define OP_POP            0x57
#define OP_DUP            0x59
#define OP_SWAP           0x5F
#define OP_IADD           0x60
#define OP_ISUB           0x64
#define OP_IMUL           0x68
#define OP_IDIV           0x6C
#define OP_IREM           0x70
#define OP_INEG           0x74
#define OP_ISHL           0x78
#define OP_ISHR           0x7A
#define OP_IUSHR          0x7C
#define OP_IAND           0x7E
#define OP_IOR            0x80
#define OP_IXOR           0x82
#define OP_IINC           0x84
#define OP_I2C            0x92
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
#define OP_GOTO           0xA7
#define OP_IRETURN        0xAC
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
#define OP_IFNULL         0xC6
#define OP_IFNONNULL      0xC7

/* ─────────────────────────────────────────────────────────────
 * Constant-pool tags  (JVMS §4.4)
 * ───────────────────────────────────────────────────────────── */
#define CP_UTF8           1
#define CP_INTEGER        3
#define CP_FLOAT          4
#define CP_CLASS          7
#define CP_STRING         8
#define CP_FIELDREF       9
#define CP_METHODREF      10
#define CP_IFACE_MREF     11
#define CP_NAME_AND_TYPE  12

/* ─────────────────────────────────────────────────────────────
 * Constant-pool entry
 * ───────────────────────────────────────────────────────────── */
typedef struct {
    uint8_t tag;
    union {
        struct { char *bytes; uint16_t length; } utf8;
        int32_t  integer_val;
        float    float_val;
        uint16_t class_index;
        uint16_t string_index;
        struct { uint16_t class_index; uint16_t nat_index;  } ref;
        struct { uint16_t name_index;  uint16_t desc_index; } nat;
    } info;
} CPEntry;

/* ─────────────────────────────────────────────────────────────
 * Method / Field descriptors
 * ───────────────────────────────────────────────────────────── */
typedef struct {
    char    *name;
    char    *descriptor;
    uint16_t access_flags;
    uint8_t *code;          /* NULL if native */
    uint32_t code_len;
    uint16_t max_stack;
    uint16_t max_locals;
    int      code_owned;  /* 1 if code was malloc'd and must be freed */
} MethodInfo;

typedef struct {
    char    *name;
    char    *descriptor;
    uint16_t access_flags;
    int32_t  value;         /* static field value */
} FieldInfo;

/* ─────────────────────────────────────────────────────────────
 * Loaded class  (issue #5 — integrated class registry)
 * ───────────────────────────────────────────────────────────── */
typedef struct KVMClass {
    char       *name;
    uint16_t    cp_count;
    CPEntry     cp[MAX_CP_ENTRIES];
    uint16_t    method_count;
    MethodInfo  methods[MAX_METHODS];
    uint16_t    field_count;
    FieldInfo   fields[MAX_FIELDS];
    int         loaded;
} KVMClass;

/* ─────────────────────────────────────────────────────────────
 * Heap object  (issue #9 — GC-managed)
 * ───────────────────────────────────────────────────────────── */
typedef enum { OBJ_INSTANCE = 0, OBJ_ARRAY, OBJ_STRING } ObjKind;

typedef struct KVMObject {
    ObjKind    kind;
    KVMClass  *klass;
    int32_t    fields[MAX_FIELDS];
    char      *str_data;
    int32_t   *arr_data;
    uint32_t   arr_len;
    int        marked;     /* GC mark bit */
} KVMObject;

/* ─────────────────────────────────────────────────────────────
 * Operand-stack value
 * ───────────────────────────────────────────────────────────── */
typedef enum { VAL_INT = 0, VAL_REF = 1 } ValType;
typedef struct {
    ValType type;
    int32_t ival;   /* integer, or index into vm->objects[] for REF */
} Value;

/* ─────────────────────────────────────────────────────────────
 * Activation frame
 * ───────────────────────────────────────────────────────────── */
typedef struct {
    const uint8_t *code;
    uint32_t       code_len;
    uint32_t       pc;
    KVMClass      *klass;    /* owning class (for CP resolution) */
    MethodInfo    *method;
    Value          locals[MAX_LOCALS];
    Value          stack[MAX_STACK];
    int            sp;       /* -1 = empty */
} Frame;

/* ─────────────────────────────────────────────────────────────
 * The VM  (issue #2 — all components wired together)
 * ───────────────────────────────────────────────────────────── */
typedef struct {
    Frame     frames[MAX_FRAMES];
    int       fp;            /* -1 = no frame */

    KVMClass  classes[MAX_CLASSES];
    int       class_count;

    KVMObject objects[MAX_OBJECTS];
    int       object_count;

    uint8_t   heap[HEAP_SIZE];
    int       heap_top;

    int       verbose;
} VM;

/* ─────────────────────────────────────────────────────────────
 * Result codes
 * ───────────────────────────────────────────────────────────── */
typedef enum {
    JVM_OK = 0,
    JVM_RETURN_INT,
    JVM_RETURN_VOID,
    JVM_RETURN_REF,
    JVM_ERR_STACK_OVERFLOW,
    JVM_ERR_STACK_UNDERFLOW,
    JVM_ERR_UNKNOWN_OPCODE,
    JVM_ERR_DIVIDE_BY_ZERO,
    JVM_ERR_OUT_OF_BOUNDS,
    JVM_ERR_NO_FRAME,
    JVM_ERR_NULL_PTR,
    JVM_ERR_CLASS_NOT_FOUND,
    JVM_ERR_METHOD_NOT_FOUND,
    JVM_ERR_CLASSFILE_INVALID,
    JVM_ERR_OUT_OF_MEMORY
} JVMResult;

/* ─────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────── */

/* VM lifecycle */
void        vm_init(VM *vm, int verbose);
void        vm_destroy(VM *vm);

/* Core interpreter — executes raw bytecode in a new frame */
JVMResult   vm_exec(VM *vm, const uint8_t *code, uint32_t len,
                    KVMClass *klass, MethodInfo *method, int32_t *ret_val);

/* Class loading (issue #5) */
JVMResult   vm_load_class(VM *vm, const char *path, KVMClass **out_class);
KVMClass   *vm_find_class(VM *vm, const char *name);

/* Method dispatch (issue #3) */
JVMResult   vm_invoke_method(VM *vm, KVMClass *klass,
                              const char *name, const char *desc,
                              Value *args, int argc, int32_t *ret_val);

/* Top-level entry point: load .class and run main() (issue #7) */
JVMResult   vm_run_classfile(VM *vm, const char *path);

/* Mark-and-sweep GC (issue #9) */
void        vm_gc(VM *vm);

/* Object allocation */
int         vm_alloc_object(VM *vm, KVMClass *klass);
int         vm_alloc_string(VM *vm, const char *str);

/* Utilities */
const char *jvm_result_str(JVMResult r);

/* CP resolution — also used by classfile.c */
int cp_resolve_ref(KVMClass *cls, uint16_t cp_idx,
                   const char **name_out, const char **desc_out,
                   const char **class_out);

#endif /* JVM_H */
