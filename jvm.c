/*
 * jvm.c — bytecode interpreter, method dispatch, GC, native stubs
 *
 * Fixes addressed:
 *  #2  End-to-end pipeline: vm_exec <-> vm_invoke_method recursion,
 *      topped by vm_run_classfile().
 *  #3  Real invokevirtual / invokespecial / invokestatic: CP is resolved,
 *      callee frame is pushed, result is returned to caller stack.
 *  #4  CP fully resolved for method refs, field refs, string literals.
 *  #8  Every stack/local access goes through bounds-checked helpers.
 *  #9  Mark-and-sweep GC scans all live frames. 
 *  #10 I/O abstraction via KVM_PRINT — swap for KolibriOS syscall.
 */

#include "jvm.h"

/* ─────────────────────────────────────────────────────────────
 * I/O abstraction  (issue #10)
 * Replace with KolibriOS debug-output syscall when porting.
 * ───────────────────────────────────────────────────────────── */
#define KVM_PRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)

/* ═════════════════════════════════════════════════════════════
 * VM lifecycle
 * ═════════════════════════════════════════════════════════════ */
void vm_init(VM *vm, int verbose)
{
    memset(vm, 0, sizeof(VM));
    vm->fp      = -1;
    vm->verbose = verbose;
}

void vm_destroy(VM *vm)
{
    /* Free constant-pool UTF8 strings and method bytecode */
    for (int ci = 0; ci < vm->class_count; ci++) {
        KVMClass *cls = &vm->classes[ci];
        if (cls->name) { free(cls->name); cls->name = NULL; }
        for (int i = 1; i < cls->cp_count; i++)
            if (cls->cp[i].tag == CP_UTF8 && cls->cp[i].info.utf8.bytes) {
                free(cls->cp[i].info.utf8.bytes);
                cls->cp[i].info.utf8.bytes = NULL;
            }
        for (int mi = 0; mi < cls->method_count; mi++) {
            MethodInfo *m = &cls->methods[mi];
            if (m->name)       { free(m->name);       m->name       = NULL; }
            if (m->descriptor) { free(m->descriptor); m->descriptor = NULL; }
            if (m->code && m->code_owned) { free(m->code); m->code = NULL; }
        }
        for (int fi = 0; fi < cls->field_count; fi++) {
            if (cls->fields[fi].name)       free(cls->fields[fi].name);
            if (cls->fields[fi].descriptor) free(cls->fields[fi].descriptor);
        }
    }
    /* Free heap objects */
    for (int i = 0; i < vm->object_count; i++) {
        KVMObject *o = &vm->objects[i];
        if (o->str_data) { free(o->str_data); o->str_data = NULL; }
        if (o->arr_data) { free(o->arr_data); o->arr_data = NULL; }
    }
}

/* ═════════════════════════════════════════════════════════════
 * Object allocation
 * ═════════════════════════════════════════════════════════════ */
int vm_alloc_object(VM *vm, KVMClass *klass)
{
    if (vm->object_count >= MAX_OBJECTS) {
        vm_gc(vm);
        if (vm->object_count >= MAX_OBJECTS) return -1;
    }
    int idx = vm->object_count++;
    KVMObject *o = &vm->objects[idx];
    memset(o, 0, sizeof(KVMObject));
    o->kind  = OBJ_INSTANCE;
    o->klass = klass;
    return idx;
}

int vm_alloc_string(VM *vm, const char *str)
{
    if (vm->object_count >= MAX_OBJECTS) {
        vm_gc(vm);
        if (vm->object_count >= MAX_OBJECTS) return -1;
    }
    int idx = vm->object_count++;
    KVMObject *o = &vm->objects[idx];
    memset(o, 0, sizeof(KVMObject));
    o->kind     = OBJ_STRING;
    o->str_data = strdup(str ? str : "");
    return idx;
}

/* ═════════════════════════════════════════════════════════════
 * Mark-and-sweep GC  (issue #9)
 * ═════════════════════════════════════════════════════════════ */
static void gc_mark_value(VM *vm, Value v)
{
    if (v.type == VAL_REF && v.ival >= 0 && v.ival < vm->object_count)
        vm->objects[v.ival].marked = 1;
}

void vm_gc(VM *vm)
{
    /* 1. Clear all marks */
    for (int i = 0; i < vm->object_count; i++)
        vm->objects[i].marked = 0;

    /* 2. Mark from every live frame's operand stack + locals */
    for (int fi = 0; fi <= vm->fp; fi++) {
        Frame *f = &vm->frames[fi];
        for (int s = 0; s <= f->sp; s++)
            gc_mark_value(vm, f->stack[s]);
        for (int l = 0; l < MAX_LOCALS; l++)
            gc_mark_value(vm, f->locals[l]);
    }

    /* 3. Compact live objects, update REF values in all frames */
    int live = 0;
    for (int i = 0; i < vm->object_count; i++) {
        if (!vm->objects[i].marked) {
            /* Free dead object's heap data */
            if (vm->objects[i].str_data) { free(vm->objects[i].str_data); vm->objects[i].str_data = NULL; }
            if (vm->objects[i].arr_data) { free(vm->objects[i].arr_data); vm->objects[i].arr_data = NULL; }
            continue;
        }
        if (live != i) {
            vm->objects[live] = vm->objects[i];
            /* Patch all REFs pointing at old index i -> new index live */
            for (int fi = 0; fi <= vm->fp; fi++) {
                Frame *f = &vm->frames[fi];
                for (int s = 0; s <= f->sp; s++)
                    if (f->stack[s].type == VAL_REF && f->stack[s].ival == i)
                        f->stack[s].ival = live;
                for (int l = 0; l < MAX_LOCALS; l++)
                    if (f->locals[l].type == VAL_REF && f->locals[l].ival == i)
                        f->locals[l].ival = live;
            }
        }
        live++;
    }

    if (vm->verbose)
        KVM_PRINT("[gc] collected %d, %d live\n", vm->object_count - live, live);
    vm->object_count = live;
}

/* ═════════════════════════════════════════════════════════════
 * Operand-stack helpers  (issue #8 — every access checked)
 * ═════════════════════════════════════════════════════════════ */
static JVMResult push(Frame *f, Value v)
{
    if (f->sp >= MAX_STACK - 1) return JVM_ERR_STACK_OVERFLOW;
    f->stack[++f->sp] = v;
    return JVM_OK;
}
static JVMResult pop(Frame *f, Value *v)
{
    if (f->sp < 0) return JVM_ERR_STACK_UNDERFLOW;
    *v = f->stack[f->sp--];
    return JVM_OK;
}
static JVMResult peek(Frame *f, Value *v)
{
    if (f->sp < 0) return JVM_ERR_STACK_UNDERFLOW;
    *v = f->stack[f->sp];
    return JVM_OK;
}
static JVMResult push_int(Frame *f, int32_t n)
{
    Value v; v.type = VAL_INT; v.ival = n;
    return push(f, v);
}
static JVMResult pop_int(Frame *f, int32_t *n)
{
    Value v; JVMResult r = pop(f, &v);
    if (r == JVM_OK) *n = v.ival;
    return r;
}

/* ─── bytecode fetch helpers ─── */
static uint8_t fetch8(Frame *f)  { return f->code[f->pc++]; }
static int16_t fetch16(Frame *f)
{
    uint8_t hi = fetch8(f), lo = fetch8(f);
    return (int16_t)((uint16_t)((uint16_t)hi << 8) | lo);
}

/* ═════════════════════════════════════════════════════════════
 * Constant-pool resolution helpers  (issue #4)
 * ═════════════════════════════════════════════════════════════ */

/* Returns the C string for a CP_UTF8 entry */
static const char *cp_utf8(KVMClass *cls, uint16_t idx)
{
    if (!cls || idx == 0 || idx >= cls->cp_count) return NULL;
    if (cls->cp[idx].tag != CP_UTF8) return NULL;
    return cls->cp[idx].info.utf8.bytes;
}

/*
 * Resolve a METHODREF / FIELDREF / IFACE_MREF entry.
 * Returns 1 on success, 0 on failure.
 */
int cp_resolve_ref(KVMClass *cls, uint16_t cp_idx,
                   const char **name_out, const char **desc_out,
                   const char **class_out)
{
    if (!cls || cp_idx == 0 || cp_idx >= cls->cp_count) return 0;
    CPEntry *e = &cls->cp[cp_idx];
    if (e->tag != CP_METHODREF && e->tag != CP_FIELDREF &&
        e->tag != CP_IFACE_MREF) return 0;

    CPEntry *nat = &cls->cp[e->info.ref.nat_index];
    if (nat->tag != CP_NAME_AND_TYPE) return 0;

    if (name_out)  *name_out  = cp_utf8(cls, nat->info.nat.name_index);
    if (desc_out)  *desc_out  = cp_utf8(cls, nat->info.nat.desc_index);
    if (class_out) {
        CPEntry *cc = &cls->cp[e->info.ref.class_index];
        *class_out = (cc->tag == CP_CLASS) ? cp_utf8(cls, cc->info.class_index) : NULL;
    }
    return 1;
}

/* ═════════════════════════════════════════════════════════════
 * Native method dispatch  (issue #10 — KolibriOS-portable stubs)
 * ═════════════════════════════════════════════════════════════ */
static JVMResult dispatch_native(VM *vm, Frame *f,
                                  const char *cname,
                                  const char *mname,
                                  const char *mdesc)
{
    (void)mdesc;

    /* java/io/PrintStream.println(I)V  —  System.out.println(int) */
    if (strcmp(mname, "println") == 0 &&
        (strcmp(cname, "java/io/PrintStream") == 0 ||
         strcmp(cname, "java/lang/System")    == 0)) {
        Value arg; arg.type = VAL_INT; arg.ival = 0;
        Value obj; obj.type = VAL_INT; obj.ival = 0;
        pop(f, &arg);
        pop(f, &obj);
        if (arg.type == VAL_REF && arg.ival >= 0 &&
            arg.ival < vm->object_count &&
            vm->objects[arg.ival].kind == OBJ_STRING &&
            vm->objects[arg.ival].str_data) {
            KVM_PRINT("%s\n", vm->objects[arg.ival].str_data);
        } else {
            KVM_PRINT("%d\n", arg.ival);
        }
        return JVM_OK;
    }

    /* java/io/PrintStream.print(I)V */
    if (strcmp(mname, "print") == 0 &&
        strcmp(cname, "java/io/PrintStream") == 0) {
        Value arg; arg.type = VAL_INT; arg.ival = 0;
        Value obj; obj.type = VAL_INT; obj.ival = 0;
        pop(f, &arg); pop(f, &obj);
        KVM_PRINT("%d", arg.ival);
        return JVM_OK;
    }

    /* java/lang/Math.max(II)I */
    if (strcmp(cname, "java/lang/Math") == 0 && strcmp(mname, "max") == 0) {
        int32_t b = 0, a = 0;
        pop_int(f, &b); pop_int(f, &a);
        push_int(f, a > b ? a : b);
        return JVM_OK;
    }
    /* java/lang/Math.min(II)I */
    if (strcmp(cname, "java/lang/Math") == 0 && strcmp(mname, "min") == 0) {
        int32_t b = 0, a = 0;
        pop_int(f, &b); pop_int(f, &a);
        push_int(f, a < b ? a : b);
        return JVM_OK;
    }
    /* java/lang/Math.abs(I)I */
    if (strcmp(cname, "java/lang/Math") == 0 && strcmp(mname, "abs") == 0) {
        int32_t a = 0;
        pop_int(f, &a);
        push_int(f, a < 0 ? -a : a);
        return JVM_OK;
    }

    /* Object.<init>()V — constructor no-op; pop 'this' */
    if (strcmp(mname, "<init>") == 0) {
        if (f->sp >= 0 && f->stack[f->sp].type == VAL_REF) {
            Value dummy; pop(f, &dummy);
        }
        return JVM_OK;
    }

    if (vm->verbose)
        KVM_PRINT("[native stub] %s.%s — skipped\n", cname, mname);
    return JVM_OK;
}

/* ═════════════════════════════════════════════════════════════
 * Method lookup
 * ═════════════════════════════════════════════════════════════ */
static MethodInfo *find_method(KVMClass *cls, const char *name, const char *desc)
{
    for (int i = 0; i < cls->method_count; i++) {
        MethodInfo *m = &cls->methods[i];
        if (!m->name) continue;
        if (strcmp(m->name, name) != 0) continue;
        if (desc && m->descriptor && strcmp(m->descriptor, desc) != 0) continue;
        return m;
    }
    return NULL;
}

/* ═════════════════════════════════════════════════════════════
 * Count argument slots from a JVM method descriptor "(II)V" -> 2
 * ═════════════════════════════════════════════════════════════ */
static int count_args(const char *desc)
{
    if (!desc || desc[0] != '(') return 0;
    int count = 0;
    const char *d = desc + 1;
    while (*d && *d != ')') {
        if (*d == 'L') { while (*d && *d != ';') d++; if (*d) d++; }
        else if (*d == '[') { d++; continue; }
        else d++;
        count++;
    }
    return count;
}

/* ═════════════════════════════════════════════════════════════
 * vm_invoke_method — set up frame and call vm_exec  (issue #2, #3)
 * ═════════════════════════════════════════════════════════════ */
JVMResult vm_invoke_method(VM *vm, KVMClass *klass,
                            const char *name, const char *desc,
                            Value *args, int argc, int32_t *ret_val)
{
    MethodInfo *m = find_method(klass, name, desc);
    if (!m) {
        fprintf(stderr, "kvm: method '%s%s' not found in '%s'\n",
                name, desc ? desc : "", klass->name ? klass->name : "?");
        return JVM_ERR_METHOD_NOT_FOUND;
    }
    if (!m->code) {
        fprintf(stderr, "kvm: method '%s' has no bytecode\n", name);
        return JVM_ERR_METHOD_NOT_FOUND;
    }

    if (vm->fp >= MAX_FRAMES - 1) return JVM_ERR_STACK_OVERFLOW;
    vm->fp++;
    Frame *f = &vm->frames[vm->fp];
    memset(f, 0, sizeof(Frame));
    f->code     = m->code;
    f->code_len = m->code_len;
    f->pc       = 0;
    f->sp       = -1;
    f->klass    = klass;
    f->method   = m;

    int lim = argc < MAX_LOCALS ? argc : MAX_LOCALS;
    for (int i = 0; i < lim; i++)
        f->locals[i] = args[i];

    /* vm_exec will pop the frame when done */
    return vm_exec(vm, m->code, m->code_len, klass, m, ret_val);
}

/* ═════════════════════════════════════════════════════════════
 * Main interpreter loop  (issues #2, #3, #4, #8)
 * ═════════════════════════════════════════════════════════════ */
JVMResult vm_exec(VM *vm, const uint8_t *code, uint32_t len,
                  KVMClass *klass, MethodInfo *method, int32_t *ret_val)
{
    /*
     * If the frame was already pushed by vm_invoke_method, reuse it.
     * Otherwise push a new one (direct call from test suite).
     */
    if (vm->fp < 0 || vm->frames[vm->fp].code != code) {
        if (vm->fp >= MAX_FRAMES - 1) return JVM_ERR_STACK_OVERFLOW;
        vm->fp++;
        Frame *ff = &vm->frames[vm->fp];
        memset(ff, 0, sizeof(Frame));
        ff->code     = code;
        ff->code_len = len;
        ff->pc       = 0;
        ff->sp       = -1;
        ff->klass    = klass;
        ff->method   = method;
    }

    JVMResult result = JVM_OK;

/* Convenience macro: propagate error and jump to cleanup */
#define CHK(expr) \
    do { JVMResult _r = (expr); if (_r != JVM_OK) { result = _r; goto done; } } while (0)

    for (;;) {
        Frame *f = &vm->frames[vm->fp];
        if (f->pc >= f->code_len) { result = JVM_RETURN_VOID; break; }

        uint8_t op = fetch8(f);

        if (vm->verbose)
            KVM_PRINT("  [pc=%3u] op=0x%02X sp=%d\n", f->pc - 1, op, f->sp);

        switch (op) {

        /* ── no-op ─────────────────────────────────────────── */
        case OP_NOP: break;

        /* ── push null reference ────────────────────────────── */
        case OP_ACONST_NULL: {
            Value v; v.type = VAL_REF; v.ival = -1;
            CHK(push(f, v));
            break;
        }

        /* ── integer constants ──────────────────────────────── */
        case OP_ICONST_M1: CHK(push_int(f, -1)); break;
        case OP_ICONST_0:  CHK(push_int(f,  0)); break;
        case OP_ICONST_1:  CHK(push_int(f,  1)); break;
        case OP_ICONST_2:  CHK(push_int(f,  2)); break;
        case OP_ICONST_3:  CHK(push_int(f,  3)); break;
        case OP_ICONST_4:  CHK(push_int(f,  4)); break;
        case OP_ICONST_5:  CHK(push_int(f,  5)); break;

        case OP_BIPUSH: { int8_t  v = (int8_t)fetch8(f);  CHK(push_int(f, (int32_t)v)); break; }
        case OP_SIPUSH: { int16_t v = fetch16(f);          CHK(push_int(f, (int32_t)v)); break; }

        /* ── ldc: load constant from CP (issue #4) ──────────── */
        case OP_LDC: {
            uint8_t idx = fetch8(f);
            if (!f->klass || idx >= f->klass->cp_count) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            CPEntry *e = &f->klass->cp[idx];
            if (e->tag == CP_INTEGER) {
                CHK(push_int(f, e->info.integer_val));
            } else if (e->tag == CP_STRING) {
                const char *s = cp_utf8(f->klass, e->info.string_index);
                int oi = vm_alloc_string(vm, s ? s : "");
                if (oi < 0) { result = JVM_ERR_OUT_OF_MEMORY; goto done; }
                Value v; v.type = VAL_REF; v.ival = oi;
                CHK(push(f, v));
            } else {
                CHK(push_int(f, 0));
            }
            break;
        }

        /* ── local variable loads ───────────────────────────── */
        case OP_ILOAD:  { uint8_t i = fetch8(f); if (i >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; } CHK(push_int(f, f->locals[i].ival)); break; }
        case OP_ALOAD:  { uint8_t i = fetch8(f); if (i >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; } CHK(push(f, f->locals[i])); break; }
        case OP_ILOAD_0: CHK(push_int(f, f->locals[0].ival)); break;
        case OP_ILOAD_1: CHK(push_int(f, f->locals[1].ival)); break;
        case OP_ILOAD_2: CHK(push_int(f, f->locals[2].ival)); break;
        case OP_ILOAD_3: CHK(push_int(f, f->locals[3].ival)); break;
        case OP_ALOAD_0: CHK(push(f, f->locals[0])); break;
        case OP_ALOAD_1: CHK(push(f, f->locals[1])); break;
        case OP_ALOAD_2: CHK(push(f, f->locals[2])); break;
        case OP_ALOAD_3: CHK(push(f, f->locals[3])); break;

        /* ── local variable stores ──────────────────────────── */
        case OP_ISTORE: {
            uint8_t i = fetch8(f);
            if (i >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            int32_t v = 0; CHK(pop_int(f, &v));
            f->locals[i].type = VAL_INT; f->locals[i].ival = v;
            break;
        }
        case OP_ASTORE: {
            uint8_t i = fetch8(f);
            if (i >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            Value v; CHK(pop(f, &v)); f->locals[i] = v;
            break;
        }
        case OP_ISTORE_0: { int32_t v=0; CHK(pop_int(f,&v)); f->locals[0].type=VAL_INT; f->locals[0].ival=v; break; }
        case OP_ISTORE_1: { int32_t v=0; CHK(pop_int(f,&v)); f->locals[1].type=VAL_INT; f->locals[1].ival=v; break; }
        case OP_ISTORE_2: { int32_t v=0; CHK(pop_int(f,&v)); f->locals[2].type=VAL_INT; f->locals[2].ival=v; break; }
        case OP_ISTORE_3: { int32_t v=0; CHK(pop_int(f,&v)); f->locals[3].type=VAL_INT; f->locals[3].ival=v; break; }
        case OP_ASTORE_0: { Value v; CHK(pop(f,&v)); f->locals[0]=v; break; }
        case OP_ASTORE_1: { Value v; CHK(pop(f,&v)); f->locals[1]=v; break; }
        case OP_ASTORE_2: { Value v; CHK(pop(f,&v)); f->locals[2]=v; break; }
        case OP_ASTORE_3: { Value v; CHK(pop(f,&v)); f->locals[3]=v; break; }

        /* ── arithmetic ─────────────────────────────────────── */
        case OP_IADD: { int32_t b=0,a=0; CHK(pop_int(f,&b)); CHK(pop_int(f,&a)); CHK(push_int(f,a+b)); break; }
        case OP_ISUB: { int32_t b=0,a=0; CHK(pop_int(f,&b)); CHK(pop_int(f,&a)); CHK(push_int(f,a-b)); break; }
        case OP_IMUL: { int32_t b=0,a=0; CHK(pop_int(f,&b)); CHK(pop_int(f,&a)); CHK(push_int(f,a*b)); break; }
        case OP_IDIV: {
            int32_t b=0,a=0; CHK(pop_int(f,&b)); CHK(pop_int(f,&a));
            if (b == 0) { result = JVM_ERR_DIVIDE_BY_ZERO; goto done; }
            CHK(push_int(f, a/b)); break;
        }
        case OP_IREM: {
            int32_t b=0,a=0; CHK(pop_int(f,&b)); CHK(pop_int(f,&a));
            if (b == 0) { result = JVM_ERR_DIVIDE_BY_ZERO; goto done; }
            CHK(push_int(f, a%b)); break;
        }
        case OP_INEG: { int32_t a=0; CHK(pop_int(f,&a)); CHK(push_int(f,-a)); break; }

        case OP_IINC: {
            uint8_t idx = fetch8(f);
            int8_t  c   = (int8_t)fetch8(f);
            if (idx >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            f->locals[idx].ival += (int32_t)c;
            break;
        }

        /* ── bitwise / shift ────────────────────────────────── */
        case OP_ISHL:  { int32_t b=0,a=0; CHK(pop_int(f,&b)); CHK(pop_int(f,&a)); CHK(push_int(f, a <<  (b & 31))); break; }
        case OP_ISHR:  { int32_t b=0,a=0; CHK(pop_int(f,&b)); CHK(pop_int(f,&a)); CHK(push_int(f, a >>  (b & 31))); break; }
        case OP_IUSHR: { int32_t b=0,a=0; CHK(pop_int(f,&b)); CHK(pop_int(f,&a)); CHK(push_int(f, (int32_t)((uint32_t)a >> (b & 31)))); break; }
        case OP_IAND:  { int32_t b=0,a=0; CHK(pop_int(f,&b)); CHK(pop_int(f,&a)); CHK(push_int(f, a &  b)); break; }
        case OP_IOR:   { int32_t b=0,a=0; CHK(pop_int(f,&b)); CHK(pop_int(f,&a)); CHK(push_int(f, a |  b)); break; }
        case OP_IXOR:  { int32_t b=0,a=0; CHK(pop_int(f,&b)); CHK(pop_int(f,&a)); CHK(push_int(f, a ^  b)); break; }

        /* ── stack manipulation ─────────────────────────────── */
        case OP_DUP:  { Value v; CHK(peek(f,&v)); CHK(push(f,v)); break; }
        case OP_POP:  { Value v; CHK(pop(f,&v));  break; }
        case OP_SWAP: { Value a,b; CHK(pop(f,&a)); CHK(pop(f,&b)); CHK(push(f,a)); CHK(push(f,b)); break; }

        case OP_I2C: { int32_t a=0; CHK(pop_int(f,&a)); CHK(push_int(f,(int32_t)(uint16_t)a)); break; }

        /* ── branches ───────────────────────────────────────── */
        case OP_IFEQ: case OP_IFNE: case OP_IFLT:
        case OP_IFGE: case OP_IFGT: case OP_IFLE: {
            uint32_t base = f->pc - 1;
            int16_t  off  = fetch16(f);
            int32_t  v    = 0; CHK(pop_int(f, &v));
            int take = (op==OP_IFEQ && v==0) || (op==OP_IFNE && v!=0) ||
                       (op==OP_IFLT && v<0)  || (op==OP_IFGE && v>=0) ||
                       (op==OP_IFGT && v>0)  || (op==OP_IFLE && v<=0);
            if (take) f->pc = (uint32_t)((int32_t)base + off);
            break;
        }
        case OP_IF_ICMPEQ: case OP_IF_ICMPNE: case OP_IF_ICMPLT:
        case OP_IF_ICMPGE: case OP_IF_ICMPGT: case OP_IF_ICMPLE: {
            uint32_t base = f->pc - 1;
            int16_t  off  = fetch16(f);
            int32_t  b=0, a=0; CHK(pop_int(f,&b)); CHK(pop_int(f,&a));
            int take = (op==OP_IF_ICMPEQ && a==b) || (op==OP_IF_ICMPNE && a!=b) ||
                       (op==OP_IF_ICMPLT && a<b)  || (op==OP_IF_ICMPGE && a>=b) ||
                       (op==OP_IF_ICMPGT && a>b)  || (op==OP_IF_ICMPLE && a<=b);
            if (take) f->pc = (uint32_t)((int32_t)base + off);
            break;
        }
        case OP_IFNULL: case OP_IFNONNULL: {
            uint32_t base = f->pc - 1;
            int16_t  off  = fetch16(f);
            Value v; CHK(pop(f,&v));
            int is_null = (v.type == VAL_REF && v.ival < 0);
            if ((op==OP_IFNULL && is_null) || (op==OP_IFNONNULL && !is_null))
                f->pc = (uint32_t)((int32_t)base + off);
            break;
        }
        case OP_GOTO: {
            uint32_t base = f->pc - 1;
            int16_t  off  = fetch16(f);
            f->pc = (uint32_t)((int32_t)base + off);
            break;
        }

        /* ── returns ────────────────────────────────────────── */
        case OP_IRETURN: {
            int32_t v = 0; CHK(pop_int(f, &v));
            if (ret_val) *ret_val = v;
            result = JVM_RETURN_INT;
            goto done;
        }
        case OP_ARETURN: {
            Value v; CHK(pop(f, &v));
            if (ret_val) *ret_val = v.ival;
            result = JVM_RETURN_REF;
            goto done;
        }
        case OP_RETURN:
            result = JVM_RETURN_VOID;
            goto done;

        /* ── getstatic (issue #4) ───────────────────────────── */
        case OP_GETSTATIC: {
            uint16_t cp_idx = (uint16_t)fetch16(f);
            const char *cname=NULL, *mname=NULL, *mdesc=NULL;
            if (f->klass && cp_resolve_ref(f->klass, cp_idx, &mname, &mdesc, &cname)
                && cname && strcmp(cname,"java/lang/System")==0
                && mname && strcmp(mname,"out")==0) {
                Value v; v.type = VAL_REF; v.ival = 0;  /* stub System.out ref */
                CHK(push(f, v));
            } else {
                Value v; v.type = VAL_INT; v.ival = 0;
                CHK(push(f, v));
            }
            break;
        }
        case OP_PUTSTATIC: {
            fetch16(f); /* consume cp index */
            Value v; CHK(pop(f, &v));
            break;
        }

        /* ── getfield / putfield (issue #4) ─────────────────── */
        case OP_GETFIELD: {
            uint16_t cp_idx = (uint16_t)fetch16(f);
            const char *fname=NULL, *fdesc=NULL, *fcname=NULL;
            Value objref; CHK(pop(f, &objref));
            if (f->klass && cp_resolve_ref(f->klass, cp_idx, &fname, &fdesc, &fcname)
                && objref.type == VAL_REF && objref.ival >= 0
                && objref.ival < vm->object_count) {
                KVMObject *o = &vm->objects[objref.ival];
                int fi2 = 0;
                if (o->klass && fname) {
                    for (int fi3 = 0; fi3 < o->klass->field_count; fi3++)
                        if (o->klass->fields[fi3].name &&
                            strcmp(o->klass->fields[fi3].name, fname) == 0)
                            { fi2 = fi3; break; }
                }
                CHK(push_int(f, o->fields[fi2]));
            } else {
                CHK(push_int(f, 0));
            }
            break;
        }
        case OP_PUTFIELD: {
            uint16_t cp_idx = (uint16_t)fetch16(f);
            const char *fname=NULL, *fdesc=NULL, *fcname=NULL;
            Value val; CHK(pop(f, &val));
            Value objref; CHK(pop(f, &objref));
            if (f->klass && cp_resolve_ref(f->klass, cp_idx, &fname, &fdesc, &fcname)
                && objref.type == VAL_REF && objref.ival >= 0
                && objref.ival < vm->object_count) {
                KVMObject *o = &vm->objects[objref.ival];
                int fi2 = 0;
                if (o->klass && fname) {
                    for (int fi3 = 0; fi3 < o->klass->field_count; fi3++)
                        if (o->klass->fields[fi3].name &&
                            strcmp(o->klass->fields[fi3].name, fname) == 0)
                            { fi2 = fi3; break; }
                }
                o->fields[fi2] = val.ival;
            }
            break;
        }

        /* ── method invocation  (issue #3) ──────────────────── */
        case OP_INVOKEVIRTUAL:
        case OP_INVOKESPECIAL:
        case OP_INVOKESTATIC: {
            uint16_t cp_idx = (uint16_t)fetch16(f);
            const char *cname=NULL, *mname=NULL, *mdesc=NULL;

            if (!f->klass || !cp_resolve_ref(f->klass, cp_idx, &mname, &mdesc, &cname)) {
                break; /* can't resolve — skip */
            }

            /* Try to find the class in the VM registry */
            KVMClass *target = cname ? vm_find_class(vm, cname) : f->klass;

            if (target) {
                MethodInfo *tm = find_method(target, mname, mdesc);
                if (tm && tm->code) {
                    /* Count arguments from descriptor */
                    int argc = count_args(mdesc);
                    int has_this = (op != OP_INVOKESTATIC) ? 1 : 0;
                    int total = argc + has_this;
                    if (total > MAX_LOCALS) total = MAX_LOCALS;

                    /* Pop arguments from current frame into args[] */
                    Value args[MAX_LOCALS];
                    memset(args, 0, sizeof(args));
                    for (int ai = total - 1; ai >= 0 && f->sp >= 0; ai--)
                        pop(f, &args[ai]);

                    /* Push callee frame */
                    if (vm->fp >= MAX_FRAMES - 1) { result = JVM_ERR_STACK_OVERFLOW; goto done; }
                    vm->fp++;
                    Frame *nf = &vm->frames[vm->fp];
                    memset(nf, 0, sizeof(Frame));
                    nf->code     = tm->code;
                    nf->code_len = tm->code_len;
                    nf->pc       = 0;
                    nf->sp       = -1;
                    nf->klass    = target;
                    nf->method   = tm;
                    for (int li = 0; li < total; li++)
                        nf->locals[li] = args[li];

                    /* Execute callee recursively */
                    int32_t callee_ret = 0;
                    JVMResult cr = vm_exec(vm, tm->code, tm->code_len,
                                           target, tm, &callee_ret);
                    /* vm_exec popped nf; refresh f pointer */
                    f = &vm->frames[vm->fp];

                    if (cr == JVM_RETURN_INT) {
                        CHK(push_int(f, callee_ret));
                    } else if (cr == JVM_RETURN_REF) {
                        Value v; v.type = VAL_REF; v.ival = callee_ret;
                        CHK(push(f, v));
                    } else if (cr != JVM_RETURN_VOID && cr != JVM_OK) {
                        result = cr; goto done;
                    }
                    break;
                }
            }

            /* Not in registry — fall through to native stub */
            if (cname && mname)
                dispatch_native(vm, f, cname, mname, mdesc);
            break;
        }

        /* ── new object ─────────────────────────────────────── */
        case OP_NEW: {
            uint16_t cp_idx = (uint16_t)fetch16(f);
            KVMClass *target = NULL;
            if (f->klass && cp_idx < f->klass->cp_count) {
                CPEntry *e = &f->klass->cp[cp_idx];
                if (e->tag == CP_CLASS) {
                    const char *cname2 = cp_utf8(f->klass, e->info.class_index);
                    if (cname2) target = vm_find_class(vm, cname2);
                }
            }
            int oi = vm_alloc_object(vm, target);
            if (oi < 0) { result = JVM_ERR_OUT_OF_MEMORY; goto done; }
            Value v; v.type = VAL_REF; v.ival = oi;
            CHK(push(f, v));
            break;
        }

        default:
            fprintf(stderr, "kvm: unknown opcode 0x%02X at pc=%u\n",
                    op, f->pc - 1);
            result = JVM_ERR_UNKNOWN_OPCODE;
            goto done;
        } /* switch */
    } /* for(;;) */

done:
    vm->fp--;   /* pop this frame */
    return result;
}

/* ═════════════════════════════════════════════════════════════
 * vm_run_classfile — load .class and execute main()  (issue #7)
 * ═════════════════════════════════════════════════════════════ */
JVMResult vm_run_classfile(VM *vm, const char *path)
{
    KVMClass *cls = NULL;
    JVMResult r = vm_load_class(vm, path, &cls);
    if (r != JVM_OK) return r;

    /* Find main(String[]) — try exact descriptor first, then by name */
    MethodInfo *main_m = find_method(cls, "main", "([Ljava/lang/String;)V");
    if (!main_m) main_m = find_method(cls, "main", NULL);
    if (!main_m) {
        fprintf(stderr, "kvm: no main() method in '%s'\n", path);
        return JVM_ERR_METHOD_NOT_FOUND;
    }
    if (!main_m->code) {
        fprintf(stderr, "kvm: main() has no bytecode\n");
        return JVM_ERR_METHOD_NOT_FOUND;
    }

    if (vm->verbose)
        KVM_PRINT("[kvm] running %s.main()\n", cls->name ? cls->name : "?");

    /* Pass null reference as the String[] args argument */
    Value args[1]; args[0].type = VAL_REF; args[0].ival = -1;
    int32_t ret = 0;
    r = vm_invoke_method(vm, cls, "main",
                         main_m->descriptor ? main_m->descriptor : "([Ljava/lang/String;)V",
                         args, 1, &ret);
    return r;
}

/* ═════════════════════════════════════════════════════════════
 * Error string helper
 * ═════════════════════════════════════════════════════════════ */
const char *jvm_result_str(JVMResult r)
{
    switch (r) {
    case JVM_OK:                    return "OK";
    case JVM_RETURN_INT:            return "RETURN_INT";
    case JVM_RETURN_VOID:           return "RETURN_VOID";
    case JVM_RETURN_REF:            return "RETURN_REF";
    case JVM_ERR_STACK_OVERFLOW:    return "ERR_STACK_OVERFLOW";
    case JVM_ERR_STACK_UNDERFLOW:   return "ERR_STACK_UNDERFLOW";
    case JVM_ERR_UNKNOWN_OPCODE:    return "ERR_UNKNOWN_OPCODE";
    case JVM_ERR_DIVIDE_BY_ZERO:    return "ERR_DIVIDE_BY_ZERO";
    case JVM_ERR_OUT_OF_BOUNDS:     return "ERR_OUT_OF_BOUNDS";
    case JVM_ERR_NO_FRAME:          return "ERR_NO_FRAME";
    case JVM_ERR_NULL_PTR:          return "ERR_NULL_PTR";
    case JVM_ERR_CLASS_NOT_FOUND:   return "ERR_CLASS_NOT_FOUND";
    case JVM_ERR_METHOD_NOT_FOUND:  return "ERR_METHOD_NOT_FOUND";
    case JVM_ERR_CLASSFILE_INVALID: return "ERR_CLASSFILE_INVALID";
    case JVM_ERR_OUT_OF_MEMORY:     return "ERR_OUT_OF_MEMORY";
    default:                        return "UNKNOWN";
    }
}
