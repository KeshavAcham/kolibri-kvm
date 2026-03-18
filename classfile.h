#ifndef CLASSFILE_H
#define CLASSFILE_H

#include "jvm.h"

/* ════════════════════════════════════════════════════════════
 *  Constant Pool tags  (JVMS §4.4)
 * ════════════════════════════════════════════════════════════ */
#define CP_UTF8               1
#define CP_INTEGER            3
#define CP_FLOAT              4
#define CP_LONG               5
#define CP_DOUBLE             6
#define CP_CLASS              7
#define CP_STRING             8
#define CP_FIELDREF           9
#define CP_METHODREF         10
#define CP_INTERFACEMETHODREF 11
#define CP_NAMEANDTYPE       12

/* ════════════════════════════════════════════════════════════
 *  A single constant-pool entry
 * ════════════════════════════════════════════════════════════ */
typedef struct {
    uint8_t tag;
    union {
        /* CP_UTF8 */
        struct { uint16_t length; char *bytes; } utf8;
        /* CP_INTEGER */
        struct { int32_t value; } integer;
        /* CP_CLASS */
        struct { uint16_t name_index; } cls;
        /* CP_STRING */
        struct { uint16_t string_index; } string;
        /* CP_FIELDREF / CP_METHODREF / CP_INTERFACEMETHODREF */
        struct { uint16_t class_index; uint16_t name_and_type_index; } ref;
        /* CP_NAMEANDTYPE */
        struct { uint16_t name_index; uint16_t descriptor_index; } nat;
    };
} CPEntry;

/* ════════════════════════════════════════════════════════════
 *  Exception table entry
 * ════════════════════════════════════════════════════════════ */
typedef struct {
    uint16_t start_pc;
    uint16_t end_pc;
    uint16_t handler_pc;
    uint16_t catch_type;
} ExceptionEntry;

/* ════════════════════════════════════════════════════════════
 *  One method in a class
 * ════════════════════════════════════════════════════════════ */
typedef struct {
    char    *name;            /* e.g. "main"          */
    char    *descriptor;      /* e.g. "([Ljava/lang/String;)V" */
    uint16_t access_flags;
    uint16_t max_stack;
    uint16_t max_locals;
    uint32_t code_length;
    uint8_t *code;            /* bytecode — heap-allocated copy */
    uint16_t exception_table_length;
    ExceptionEntry *exception_table;
} MethodInfo;

/* ════════════════════════════════════════════════════════════
 *  Parsed .class file
 * ════════════════════════════════════════════════════════════ */
typedef struct {
    char      *class_name;       /* e.g. "com/example/Hello"  */
    uint16_t   access_flags;
    uint16_t   constant_pool_count;
    CPEntry   *constant_pool;    /* index 1 .. count-1 (0 unused) */
    uint16_t   methods_count;
    MethodInfo *methods;
} ClassFile;

/* ════════════════════════════════════════════════════════════
 *  Class registry  (simple fixed-size table)
 * ════════════════════════════════════════════════════════════ */
#define MAX_CLASSES  64

typedef struct {
    ClassFile *classes[MAX_CLASSES];
    int        count;
} ClassRegistry;

/* ════════════════════════════════════════════════════════════
 *  Error codes for class loading
 * ════════════════════════════════════════════════════════════ */
typedef enum {
    CF_OK = 0,
    CF_ERR_IO,            /* file read error          */
    CF_ERR_MAGIC,         /* bad magic number         */
    CF_ERR_TRUNCATED,     /* unexpected end of data   */
    CF_ERR_UNSUPPORTED,   /* unsupported CP tag etc.  */
    CF_ERR_NOT_FOUND,     /* class / method not found */
    CF_ERR_REGISTRY_FULL  /* too many classes loaded  */
} CFResult;

/* ════════════════════════════════════════════════════════════
 *  Public API
 * ════════════════════════════════════════════════════════════ */

/* Parse a raw .class byte buffer */
CFResult classfile_parse(const uint8_t *data, uint32_t len,
                         ClassFile **out);

/* Free a parsed ClassFile */
void classfile_free(ClassFile *cf);

/* Load a .class file from disk and parse it */
CFResult classfile_load(const char *path, ClassFile **out);

/* Registry helpers */
void      registry_init  (ClassRegistry *reg);
CFResult  registry_add   (ClassRegistry *reg, ClassFile *cf);
ClassFile *registry_find (ClassRegistry *reg, const char *class_name);

/* Look up a method by name+descriptor (NULL descriptor = any) */
MethodInfo *classfile_find_method(const ClassFile *cf,
                                  const char *name,
                                  const char *descriptor);

/* Resolve a CP_UTF8 entry to a C string (points into cf, do not free) */
const char *cp_utf8(const ClassFile *cf, uint16_t index);

/* Human-readable CFResult */
const char *cf_result_str(CFResult r);

/* ════════════════════════════════════════════════════════════
 *  JAR loader
 * ════════════════════════════════════════════════════════════ */

/* Load every .class file from a JAR (ZIP) into the registry.
 * Uses a minimal built-in ZIP parser — no external dependencies. */
CFResult jar_load(const char *jar_path, ClassRegistry *reg);

#endif /* CLASSFILE_H */
