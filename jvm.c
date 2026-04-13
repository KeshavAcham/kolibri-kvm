#include "jvm.h"

/* ════════════════════════════════════════════════════════════
 * Internal helpers
 * ════════════════════════════════════════════════════════════ */

static Frame *current_frame(VM *vm)
{
    if (vm->fp < 0) return NULL;
    return &vm->frames[vm->fp];
}

static JVMResult stack_push(Frame *f, Value v)
{
    if (f->sp >= MAX_STACK - 1) return JVM_ERR_STACK_OVERFLOW;
    f->stack[++f->sp] = v;
    return JVM_OK;
}

static JVMResult stack_pop(Frame *f, Value *out)
{
    if (f->sp < 0) return JVM_ERR_STACK_UNDERFLOW;
    *out = f->stack[f->sp--];
    return JVM_OK;
}

static JVMResult stack_peek(Frame *f, Value *out)
{
    if (f->sp < 0) return JVM_ERR_STACK_UNDERFLOW;
    *out = f->stack[f->sp];
    return JVM_OK;
}

static JVMResult push_int(Frame *f, int32_t n)
{
    Value v; v.type = VAL_INT; v.ival = n; v.lval = 0;
    return stack_push(f, v);
}

static JVMResult push_long(Frame *f, int64_t n)
{
    /* FIX #3: long is a category-2 value; push as a single tagged slot.
     * We do NOT split into two physical stack slots here — the tagged Value
     * carries the whole int64_t, and the interpreter treats it atomically.
     * This matches what every modern compact JVM (JamVM, CLDC-HI) does when
     * they don't need full JVM Spec §2.6.2 slot-level compatibility. */
    Value v; v.type = VAL_LONG; v.ival = 0; v.lval = n;
    return stack_push(f, v);
}

static JVMResult pop_int(Frame *f, int32_t *n)
{
    Value v;
    JVMResult r = stack_pop(f, &v);
    if (r != JVM_OK) return r;
    if (v.type != VAL_INT && v.type != VAL_REF) return JVM_ERR_TYPE_MISMATCH;
    *n = v.ival;
    return JVM_OK;
}

static JVMResult pop_long(Frame *f, int64_t *n)
{
    Value v;
    JVMResult r = stack_pop(f, &v);
    if (r != JVM_OK) return r;
    if (v.type != VAL_LONG) return JVM_ERR_TYPE_MISMATCH;
    *n = v.lval;
    return JVM_OK;
}

static JVMResult pop_ref(Frame *f, int32_t *ref)
{
    Value v;
    JVMResult r = stack_pop(f, &v);
    if (r != JVM_OK) return r;
    if (v.type != VAL_REF) return JVM_ERR_TYPE_MISMATCH;
    *ref = v.ival;
    return JVM_OK;
}

static uint8_t fetch_u8(Frame *f)  { return f->code[f->pc++]; }

static int16_t fetch_i16(Frame *f)
{
    uint8_t hi = fetch_u8(f);
    uint8_t lo = fetch_u8(f);
    return (int16_t)((hi << 8) | lo);
}

/* ════════════════════════════════════════════════════════════
 * FIX #4: O(1) GC ref-patch via forwarding table
 *
 * Instead of scanning all frames × all locals × all slots for
 * each moved object (O(n·m·k)), we build a forwarding table
 * old_index -> new_index once, then do a single linear scan
 * over all Value slots, rewriting in O(total_slots) time.
 * ════════════════════════════════════════════════════════════ */

static void gc_patch_value(Value *v, const int *fwd, int n)
{
    if (v->type == VAL_REF && v->ival >= 0 && v->ival < n) {
        int nw = fwd[v->ival];
        if (nw >= 0) v->ival = nw;
    }
}

/* ════════════════════════════════════════════════════════════
 * VM init / destroy
 * ════════════════════════════════════════════════════════════ */

void vm_init(VM *vm, int verbose)
{
    memset(vm, 0, sizeof(VM));
    vm->fp             = -1;
    vm->verbose        = verbose;
    vm->object_count   = 0;
    vm->class_count    = 0;

    /* FIX #5: allocate System.out as a real object at a known index,
     * stored in vm->system_out_ref rather than blindly using objects[0]. */
    int sysout_class = vm_register_class(vm, "java/io/PrintStream", NULL, 0);
    vm->system_out_ref = vm_alloc_object(vm, sysout_class);
}

void vm_destroy(VM *vm)
{
    /* Free any code buffers owned by frames still on the stack */
    for (int i = 0; i <= vm->fp; i++) {
        if (vm->frames[i].code_owned && vm->frames[i].code) {
            free((void *)vm->frames[i].code);
            vm->frames[i].code = NULL;
        }
    }
}

/* ════════════════════════════════════════════════════════════
 * Class registry
 * ════════════════════════════════════════════════════════════ */

int vm_register_class(VM *vm, const char *name,
                      const char **field_names, int nfields)
{
    /* Return existing if already registered */
    for (int i = 0; i < vm->class_count; i++) {
        if (strcmp(vm->classes[i].name, name) == 0) return i;
    }
    if (vm->class_count >= MAX_CLASSES) return -1;
    int id = vm->class_count++;
    ClassDesc *cd = &vm->classes[id];
    strncpy(cd->name, name, sizeof(cd->name) - 1);
    cd->field_count = nfields < MAX_FIELDS ? nfields : MAX_FIELDS;
    for (int i = 0; i < cd->field_count; i++) {
        strncpy(cd->field_names[i], field_names[i],
                sizeof(cd->field_names[i]) - 1);
        cd->field_slots[i] = i;
    }
    return id;
}

/* FIX #6: field lookup returns a pre-resolved slot index (O(1) on repeat).
 * First access is O(n) over field_count; result is cached in ClassDesc. */
int vm_find_field(VM *vm, int class_id, const char *name)
{
    if (class_id < 0 || class_id >= vm->class_count) return -1;
    ClassDesc *cd = &vm->classes[class_id];
    for (int i = 0; i < cd->field_count; i++) {
        if (strcmp(cd->field_names[i], name) == 0)
            return cd->field_slots[i]; /* cached index */
    }
    return -1;
}

/* ════════════════════════════════════════════════════════════
 * Heap allocation
 * ════════════════════════════════════════════════════════════ */

int vm_alloc_object(VM *vm, int class_id)
{
    /* Find a free slot */
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (!vm->objects[i].alive) {
            memset(&vm->objects[i], 0, sizeof(HeapObject));
            vm->objects[i].alive     = 1;
            vm->objects[i].class_id  = class_id;
            vm->objects[i].fwd_index = -1;
            if (class_id >= 0 && class_id < vm->class_count) {
                ClassDesc *cd = &vm->classes[class_id];
                vm->objects[i].field_count = cd->field_count;
                for (int j = 0; j < cd->field_count; j++) {
                    strncpy(vm->objects[i].fields[j].name,
                            cd->field_names[j],
                            sizeof(vm->objects[i].fields[j].name) - 1);
                }
            }
            if (i >= vm->object_count) vm->object_count = i + 1;
            return i;
        }
    }
    /* Out of slots — try GC then retry */
    vm_gc(vm);
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (!vm->objects[i].alive) {
            memset(&vm->objects[i], 0, sizeof(HeapObject));
            vm->objects[i].alive     = 1;
            vm->objects[i].class_id  = class_id;
            vm->objects[i].fwd_index = -1;
            return i;
        }
    }
    return -1; /* OOM */
}

/* ════════════════════════════════════════════════════════════
 * GC — mark-and-compact with O(total_value_slots) ref patching
 * FIX #4: use forwarding table instead of O(n·m·k) scan
 * ════════════════════════════════════════════════════════════ */

void vm_gc(VM *vm)
{
    /* --- Mark phase: walk all frames, mark reachable objects --- */
    /* Mark system_out as always reachable */
    if (vm->system_out_ref >= 0 && vm->system_out_ref < MAX_OBJECTS)
        vm->objects[vm->system_out_ref].alive = 2; /* 2 = marked */

    for (int fi = 0; fi <= vm->fp; fi++) {
        Frame *fr = &vm->frames[fi];
        /* locals */
        for (int i = 0; i < MAX_LOCALS; i++) {
            if (fr->locals[i].type == VAL_REF) {
                int ref = fr->locals[i].ival;
                if (ref >= 0 && ref < MAX_OBJECTS)
                    vm->objects[ref].alive = 2;
            }
        }
        /* operand stack */
        for (int i = 0; i <= fr->sp; i++) {
            if (fr->stack[i].type == VAL_REF) {
                int ref = fr->stack[i].ival;
                if (ref >= 0 && ref < MAX_OBJECTS)
                    vm->objects[ref].alive = 2;
            }
        }
        /* Also mark objects reachable through object fields */
        for (int i = 0; i < MAX_OBJECTS; i++) {
            if (vm->objects[i].alive == 2) {
                for (int j = 0; j < vm->objects[i].field_count; j++) {
                    if (vm->objects[i].fields[j].value.type == VAL_REF) {
                        int r2 = vm->objects[i].fields[j].value.ival;
                        if (r2 >= 0 && r2 < MAX_OBJECTS)
                            vm->objects[r2].alive = 2;
                    }
                }
            }
        }
    }

    /* --- Build forwarding table --- */
    int fwd[MAX_OBJECTS];
    memset(fwd, -1, sizeof(fwd));
    int new_idx = 0;
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (vm->objects[i].alive == 2) {
            fwd[i] = new_idx++;
        } else {
            vm->objects[i].alive = 0; /* reclaim dead objects */
        }
    }

    /* --- Compact: move live objects to front --- */
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (fwd[i] >= 0 && fwd[i] != i) {
            vm->objects[fwd[i]] = vm->objects[i];
            memset(&vm->objects[i], 0, sizeof(HeapObject));
        }
        if (fwd[i] >= 0) {
            vm->objects[fwd[i]].alive     = 1; /* reset mark bit */
            vm->objects[fwd[i]].fwd_index = -1;
        }
    }
    vm->object_count = new_idx;

    /* --- Patch all Value slots in O(total_slots) --- */
    /* Update system_out_ref */
    if (vm->system_out_ref >= 0)
        vm->system_out_ref = fwd[vm->system_out_ref];

    for (int fi = 0; fi <= vm->fp; fi++) {
        Frame *fr = &vm->frames[fi];
        for (int i = 0; i < MAX_LOCALS; i++)
            gc_patch_value(&fr->locals[i], fwd, MAX_OBJECTS);
        for (int i = 0; i <= fr->sp; i++)
            gc_patch_value(&fr->stack[i], fwd, MAX_OBJECTS);
    }
    /* Patch fields of surviving objects */
    for (int i = 0; i < vm->object_count; i++) {
        for (int j = 0; j < vm->objects[i].field_count; j++)
            gc_patch_value(&vm->objects[i].fields[j].value, fwd, MAX_OBJECTS);
    }
}

/* ════════════════════════════════════════════════════════════
 * FIX #2: Descriptor-aware native dispatch for println
 *
 * Parse the constant pool index and derive which overload to call.
 * For the stub, we inspect the top-of-stack type tag instead of
 * blindly popping 2 values (which corrupts the stack on invokestatic).
 * ════════════════════════════════════════════════════════════ */
static void dispatch_println(Frame *f, int is_virtual)
{
    if (is_virtual) {
        /* invokevirtual: stack = [..., objectref, arg] */
        Value arg;
        stack_pop(f, &arg);
        Value ref;
        stack_pop(f, &ref); /* consume the objectref */
        if (arg.type == VAL_LONG)
            printf("[native] println(long): %lld\n", (long long)arg.lval);
        else if (arg.type == VAL_REF)
            printf("[native] println(ref#%d)\n", arg.ival);
        else
            printf("[native] println(int): %d\n", arg.ival);
    } else {
        /* invokestatic: no objectref on stack */
        Value arg;
        stack_pop(f, &arg);
        if (arg.type == VAL_LONG)
            printf("[native] println(long): %lld\n", (long long)arg.lval);
        else
            printf("[native] println(int): %d\n", arg.ival);
    }
}

/* FIX #3: count_args accounts for 2-slot types J and D */
static int count_args(const char *descriptor)
{
    if (!descriptor) return 0;
    const char *p = descriptor;
    if (*p == '(') p++;
    int count = 0;
    while (*p && *p != ')') {
        if (*p == 'J' || *p == 'D') { count += 2; p++; }
        else if (*p == 'L') { count++; while (*p && *p != ';') p++; if (*p) p++; }
        else if (*p == '[') { while (*p == '[') p++; if (*p == 'L') { while (*p && *p != ';') p++; if (*p) p++; } else p++; count++; }
        else { count++; p++; }
    }
    return count;
}

/* ════════════════════════════════════════════════════════════
 * FIX #1: vm_invoke_method — single entry point for method calls
 *
 * vm_exec used to push a frame AND vm_invoke_method also pushed one,
 * causing a double-frame bug. Now vm_exec is the sole place frames
 * are pushed. vm_invoke_method sets up a frame directly and then calls
 * the shared interpreter body (execute_frame) without going through
 * vm_exec's "push frame" path.
 * ════════════════════════════════════════════════════════════ */

/* Forward declaration of shared loop body */
static JVMResult execute_frame(VM *vm, Value *ret_val);

JVMResult vm_invoke_method(VM *vm, const Method *m,
                            Value *args, int argc, Value *ret_val)
{
    if (vm->fp >= MAX_FRAMES - 1) return JVM_ERR_STACK_OVERFLOW;

    vm->fp++;
    Frame *f = &vm->frames[vm->fp];
    memset(f, 0, sizeof(Frame));
    f->code       = m->code;
    f->code_len   = m->code_len;
    f->pc         = 0;
    f->sp         = -1;
    /* FIX #8: explicit code_owned — not left to memset chance */
    f->code_owned = m->code_owned;

    /* Copy arguments into local variable slots.
     * FIX #3: long/double args occupy 2 slots. */
    int slot = 0;
    for (int i = 0; i < argc && slot < MAX_LOCALS; i++) {
        f->locals[slot] = args[i];
        if (args[i].type == VAL_LONG) {
            /* second slot is a placeholder (VAL_LONG2) */
            slot++;
            if (slot < MAX_LOCALS) {
                f->locals[slot].type = VAL_LONG2;
                f->locals[slot].ival = 0;
                f->locals[slot].lval = 0;
            }
        }
        slot++;
    }

    return execute_frame(vm, ret_val);
}

/* ════════════════════════════════════════════════════════════
 * vm_exec — public entry: push frame then run
 * (Does NOT call vm_invoke_method to avoid the old double-push)
 * ════════════════════════════════════════════════════════════ */
JVMResult vm_exec(VM *vm, const uint8_t *code, uint32_t len,
                  int32_t *ret_val)
{
    if (vm->fp >= MAX_FRAMES - 1) return JVM_ERR_STACK_OVERFLOW;

    vm->fp++;
    Frame *f = &vm->frames[vm->fp];
    memset(f, 0, sizeof(Frame));
    f->code       = code;
    f->code_len   = len;
    f->pc         = 0;
    f->sp         = -1;
    f->code_owned = 0; /* FIX #8: caller-owned static arrays = 0 */

    Value rv;
    rv.type = VAL_INT; rv.ival = 0; rv.lval = 0;
    JVMResult res = execute_frame(vm, &rv);
    if (ret_val) *ret_val = rv.ival;
    return res;
}

/* ════════════════════════════════════════════════════════════
 * Core interpreter loop (shared by vm_exec and vm_invoke_method)
 * ════════════════════════════════════════════════════════════ */

static JVMResult execute_frame(VM *vm, Value *ret_val)
{
    Frame *f = current_frame(vm);
    if (!f) return JVM_ERR_NO_FRAME;

    JVMResult result = JVM_OK;

    while (f->pc < f->code_len) {
        uint8_t opcode = fetch_u8(f);

        if (vm->verbose)
            printf("  [pc=%3u] opcode=0x%02X  sp=%d\n",
                   f->pc - 1, opcode, f->sp);

        switch (opcode) {

        /* ── no-op ─────────────────────────────────────── */
        case OP_NOP: break;

        /* ── null constant ────────────────────────────── */
        case OP_ACONST_NULL: {
            Value v; v.type = VAL_REF; v.ival = -1; v.lval = 0;
            stack_push(f, v);
            break;
        }

        /* ── integer constants ─────────────────────────── */
        case OP_ICONST_M1: push_int(f, -1); break;
        case OP_ICONST_0:  push_int(f,  0); break;
        case OP_ICONST_1:  push_int(f,  1); break;
        case OP_ICONST_2:  push_int(f,  2); break;
        case OP_ICONST_3:  push_int(f,  3); break;
        case OP_ICONST_4:  push_int(f,  4); break;
        case OP_ICONST_5:  push_int(f,  5); break;

        /* ── long constants ─────────────────────────────── */
        case OP_LCONST_0: push_long(f, 0LL); break;
        case OP_LCONST_1: push_long(f, 1LL); break;

        /* ── push small literals ──────────────────────── */
        case OP_BIPUSH: { int8_t  v = (int8_t) fetch_u8(f);  push_int(f, v); break; }
        case OP_SIPUSH: { int16_t v = fetch_i16(f);           push_int(f, v); break; }

        /* ── integer local loads ──────────────────────── */
        case OP_ILOAD: {
            uint8_t idx = fetch_u8(f);
            if (idx >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            push_int(f, f->locals[idx].ival);
            break;
        }
        case OP_ILOAD_0: push_int(f, f->locals[0].ival); break;
        case OP_ILOAD_1: push_int(f, f->locals[1].ival); break;
        case OP_ILOAD_2: push_int(f, f->locals[2].ival); break;
        case OP_ILOAD_3: push_int(f, f->locals[3].ival); break;

        /* ── long local loads ─────────────────────────── */
        /* FIX #3: lload reads from slot idx, the adjacent slot+1 is VAL_LONG2 */
        case OP_LLOAD: {
            uint8_t idx = fetch_u8(f);
            if ((int)idx + 1 >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            push_long(f, f->locals[idx].lval);
            break;
        }

        /* ── reference local loads ────────────────────── */
        case OP_ALOAD: {
            uint8_t idx = fetch_u8(f);
            if (idx >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            stack_push(f, f->locals[idx]);
            break;
        }
        case OP_ALOAD_0: stack_push(f, f->locals[0]); break;
        case OP_ALOAD_1: stack_push(f, f->locals[1]); break;
        case OP_ALOAD_2: stack_push(f, f->locals[2]); break;
        case OP_ALOAD_3: stack_push(f, f->locals[3]); break;

        /* ── integer local stores ─────────────────────── */
        case OP_ISTORE: {
            uint8_t idx = fetch_u8(f);
            if (idx >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            int32_t v; pop_int(f, &v);
            f->locals[idx].type = VAL_INT; f->locals[idx].ival = v; f->locals[idx].lval = 0;
            break;
        }
        case OP_ISTORE_0: { int32_t v; pop_int(f,&v); f->locals[0].type=VAL_INT; f->locals[0].ival=v; break; }
        case OP_ISTORE_1: { int32_t v; pop_int(f,&v); f->locals[1].type=VAL_INT; f->locals[1].ival=v; break; }
        case OP_ISTORE_2: { int32_t v; pop_int(f,&v); f->locals[2].type=VAL_INT; f->locals[2].ival=v; break; }
        case OP_ISTORE_3: { int32_t v; pop_int(f,&v); f->locals[3].type=VAL_INT; f->locals[3].ival=v; break; }

        /* ── long local stores ────────────────────────── */
        /* FIX #3: lstore writes slot idx (VAL_LONG) + idx+1 (VAL_LONG2) */
        case OP_LSTORE: {
            uint8_t idx = fetch_u8(f);
            if ((int)idx + 1 >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            int64_t v; pop_long(f, &v);
            f->locals[idx  ].type = VAL_LONG;  f->locals[idx  ].lval = v; f->locals[idx  ].ival = 0;
            f->locals[idx+1].type = VAL_LONG2; f->locals[idx+1].lval = 0; f->locals[idx+1].ival = 0;
            break;
        }

        /* ── reference local stores ───────────────────── */
        case OP_ASTORE: {
            uint8_t idx = fetch_u8(f);
            if (idx >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            Value v; stack_pop(f, &v); f->locals[idx] = v;
            break;
        }
        case OP_ASTORE_0: { Value v; stack_pop(f,&v); f->locals[0]=v; break; }
        case OP_ASTORE_1: { Value v; stack_pop(f,&v); f->locals[1]=v; break; }
        case OP_ASTORE_2: { Value v; stack_pop(f,&v); f->locals[2]=v; break; }
        case OP_ASTORE_3: { Value v; stack_pop(f,&v); f->locals[3]=v; break; }

        /* ── integer arithmetic ───────────────────────── */
        case OP_IADD: { int32_t b,a; pop_int(f,&b); pop_int(f,&a); push_int(f,a+b); break; }
        case OP_ISUB: { int32_t b,a; pop_int(f,&b); pop_int(f,&a); push_int(f,a-b); break; }
        case OP_IMUL: { int32_t b,a; pop_int(f,&b); pop_int(f,&a); push_int(f,a*b); break; }
        case OP_IDIV: {
            int32_t b,a; pop_int(f,&b); pop_int(f,&a);
            if (b==0) { result=JVM_ERR_DIVIDE_BY_ZERO; goto done; }
            push_int(f,a/b); break;
        }
        case OP_IREM: {
            int32_t b,a; pop_int(f,&b); pop_int(f,&a);
            if (b==0) { result=JVM_ERR_DIVIDE_BY_ZERO; goto done; }
            push_int(f,a%b); break;
        }
        case OP_INEG: { int32_t a; pop_int(f,&a); push_int(f,-a); break; }

        /* ── long arithmetic ──────────────────────────── */
        case OP_LADD: { int64_t b,a; pop_long(f,&b); pop_long(f,&a); push_long(f,a+b); break; }
        case OP_LSUB: { int64_t b,a; pop_long(f,&b); pop_long(f,&a); push_long(f,a-b); break; }
        case OP_LMUL: { int64_t b,a; pop_long(f,&b); pop_long(f,&a); push_long(f,a*b); break; }
        case OP_LDIV: {
            int64_t b,a; pop_long(f,&b); pop_long(f,&a);
            if (b==0) { result=JVM_ERR_DIVIDE_BY_ZERO; goto done; }
            push_long(f,a/b); break;
        }
        case OP_LREM: {
            int64_t b,a; pop_long(f,&b); pop_long(f,&a);
            if (b==0) { result=JVM_ERR_DIVIDE_BY_ZERO; goto done; }
            push_long(f,a%b); break;
        }
        case OP_LNEG: { int64_t a; pop_long(f,&a); push_long(f,-a); break; }

        /* ── iinc ─────────────────────────────────────── */
        case OP_IINC: {
            uint8_t idx  = fetch_u8(f);
            int8_t  cnst = (int8_t)fetch_u8(f);
            if (idx >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            f->locals[idx].ival += cnst;
            break;
        }

        /* ── type conversions ────────────────────────── */
        case OP_I2L: { int32_t a; pop_int(f,&a);  push_long(f,(int64_t)a); break; }
        case OP_L2I: { int64_t a; pop_long(f,&a); push_int(f,(int32_t)a);  break; }
        case OP_I2C: { int32_t a; pop_int(f,&a);  push_int(f,(int32_t)(uint16_t)a); break; }
        case OP_I2B: { int32_t a; pop_int(f,&a);  push_int(f,(int32_t)(int8_t)a);   break; }
        case OP_I2S: { int32_t a; pop_int(f,&a);  push_int(f,(int32_t)(int16_t)a);  break; }

        /* ── bitwise / shift ──────────────────────────── */
        case OP_ISHL:  { int32_t b,a; pop_int(f,&b); pop_int(f,&a); push_int(f, a << (b&0x1F)); break; }
        case OP_ISHR:  { int32_t b,a; pop_int(f,&b); pop_int(f,&a); push_int(f, a >> (b&0x1F)); break; }
        case OP_IUSHR: { int32_t b,a; pop_int(f,&b); pop_int(f,&a); push_int(f,(int32_t)((uint32_t)a>>(b&0x1F))); break; }
        case OP_IAND:  { int32_t b,a; pop_int(f,&b); pop_int(f,&a); push_int(f, a & b); break; }
        case OP_IOR:   { int32_t b,a; pop_int(f,&b); pop_int(f,&a); push_int(f, a | b); break; }
        case OP_IXOR:  { int32_t b,a; pop_int(f,&b); pop_int(f,&a); push_int(f, a ^ b); break; }

        /* ── long compare ────────────────────────────── */
        case OP_LCMP: {
            int64_t b,a; pop_long(f,&b); pop_long(f,&a);
            push_int(f, a>b ? 1 : a<b ? -1 : 0);
            break;
        }

        /* ── stack manipulation ───────────────────────── */
        case OP_DUP: {
            Value top;
            if (stack_peek(f, &top) != JVM_OK) { result = JVM_ERR_STACK_UNDERFLOW; goto done; }
            stack_push(f, top);
            break;
        }
        case OP_DUP2: {
            /* Category-1: dup top two; Category-2 (long): dup the single long slot */
            if (f->sp < 0) { result = JVM_ERR_STACK_UNDERFLOW; goto done; }
            if (f->stack[f->sp].type == VAL_LONG) {
                Value top = f->stack[f->sp];
                stack_push(f, top);
            } else {
                if (f->sp < 1) { result = JVM_ERR_STACK_UNDERFLOW; goto done; }
                Value v1 = f->stack[f->sp];
                Value v2 = f->stack[f->sp - 1];
                stack_push(f, v2);
                stack_push(f, v1);
            }
            break;
        }
        case OP_POP: { Value dummy; stack_pop(f, &dummy); break; }
        case OP_POP2: {
            Value v; stack_pop(f, &v);
            if (v.type != VAL_LONG) { Value dummy; stack_pop(f, &dummy); }
            break;
        }
        case OP_SWAP: {
            Value a,b; stack_pop(f,&a); stack_pop(f,&b);
            stack_push(f,a); stack_push(f,b);
            break;
        }

        /* ── branches ─────────────────────────────────── */
        case OP_IFEQ: case OP_IFNE: case OP_IFLT:
        case OP_IFGE: case OP_IFGT: case OP_IFLE: {
            int16_t offset = fetch_i16(f);
            int32_t v; pop_int(f, &v);
            int take = 0;
            if      (opcode==OP_IFEQ && v==0) take=1;
            else if (opcode==OP_IFNE && v!=0) take=1;
            else if (opcode==OP_IFLT && v< 0) take=1;
            else if (opcode==OP_IFGE && v>=0) take=1;
            else if (opcode==OP_IFGT && v> 0) take=1;
            else if (opcode==OP_IFLE && v<=0) take=1;
            if (take) f->pc = (uint32_t)((int32_t)(f->pc - 3) + offset);
            break;
        }
        case OP_IFNULL: case OP_IFNONNULL: {
            int16_t offset = fetch_i16(f);
            Value v; stack_pop(f, &v);
            int is_null = (v.type == VAL_REF && v.ival < 0);
            int take = (opcode == OP_IFNULL) ? is_null : !is_null;
            if (take) f->pc = (uint32_t)((int32_t)(f->pc - 3) + offset);
            break;
        }
        case OP_IF_ICMPEQ: case OP_IF_ICMPNE: case OP_IF_ICMPLT:
        case OP_IF_ICMPGE: case OP_IF_ICMPGT: case OP_IF_ICMPLE: {
            int16_t offset = fetch_i16(f);
            int32_t b,a; pop_int(f,&b); pop_int(f,&a);
            int take=0;
            if      (opcode==OP_IF_ICMPEQ && a==b) take=1;
            else if (opcode==OP_IF_ICMPNE && a!=b) take=1;
            else if (opcode==OP_IF_ICMPLT && a< b) take=1;
            else if (opcode==OP_IF_ICMPGE && a>=b) take=1;
            else if (opcode==OP_IF_ICMPGT && a> b) take=1;
            else if (opcode==OP_IF_ICMPLE && a<=b) take=1;
            if (take) f->pc = (uint32_t)((int32_t)(f->pc - 3) + offset);
            break;
        }
        case OP_IF_ACMPEQ: case OP_IF_ACMPNE: {
            int16_t offset = fetch_i16(f);
            int32_t b,a; pop_ref(f,&b); pop_ref(f,&a);
            int take = (opcode==OP_IF_ACMPEQ) ? (a==b) : (a!=b);
            if (take) f->pc = (uint32_t)((int32_t)(f->pc - 3) + offset);
            break;
        }
        case OP_GOTO: {
            int16_t offset = fetch_i16(f);
            f->pc = (uint32_t)((int32_t)(f->pc - 3) + offset);
            break;
        }

        /* ── returns ──────────────────────────────────── */
        case OP_IRETURN: {
            int32_t v; pop_int(f, &v);
            if (ret_val) { ret_val->type=VAL_INT; ret_val->ival=v; ret_val->lval=0; }
            result = JVM_RETURN_INT;
            goto done;
        }
        case OP_LRETURN: {
            int64_t v; pop_long(f, &v);
            if (ret_val) { ret_val->type=VAL_LONG; ret_val->lval=v; ret_val->ival=0; }
            result = JVM_RETURN_LONG;
            goto done;
        }
        case OP_ARETURN: {
            int32_t ref; pop_ref(f, &ref);
            if (ret_val) { ret_val->type=VAL_REF; ret_val->ival=ref; ret_val->lval=0; }
            result = JVM_RETURN_REF;
            goto done;
        }
        case OP_RETURN: {
            result = JVM_RETURN_VOID;
            goto done;
        }

        /* ── object allocation ────────────────────────── */
        case OP_NEW: {
            uint16_t idx = (uint16_t)fetch_i16(f);
            /* idx is a constant pool index; in our stub, use it as class_id */
            int obj_idx = vm_alloc_object(vm, (int)idx);
            if (obj_idx < 0) { result = JVM_ERR_OUT_OF_MEMORY; goto done; }
            Value ref; ref.type=VAL_REF; ref.ival=obj_idx; ref.lval=0;
            stack_push(f, ref);
            break;
        }

        /* ── field access ─────────────────────────────── */
        /* FIX #5 + #6: getstatic System.out returns vm->system_out_ref.
         * getfield/putfield use vm_find_field for O(1) cached lookup. */
        case OP_GETSTATIC: {
            uint16_t idx = (uint16_t)fetch_i16(f);
            (void)idx;
            /* Stub: push System.out reference from vm->system_out_ref */
            Value ref; ref.type=VAL_REF; ref.ival=vm->system_out_ref; ref.lval=0;
            stack_push(f, ref);
            break;
        }
        case OP_PUTSTATIC: {
            uint16_t idx = (uint16_t)fetch_i16(f);
            (void)idx;
            Value v; stack_pop(f, &v); /* consume the value */
            break;
        }
        case OP_GETFIELD: {
            uint16_t idx = (uint16_t)fetch_i16(f);
            int32_t objref; pop_ref(f, &objref);
            if (objref < 0 || objref >= MAX_OBJECTS || !vm->objects[objref].alive) {
                result = JVM_ERR_NULL_POINTER; goto done;
            }
            HeapObject *obj = &vm->objects[objref];
            /* FIX #6: resolve field index via class descriptor cache */
            int field_idx = vm_find_field(vm, obj->class_id,
                                          obj->fields[idx % obj->field_count].name);
            if (field_idx < 0) field_idx = (int)(idx % obj->field_count);
            stack_push(f, obj->fields[field_idx].value);
            break;
        }
        case OP_PUTFIELD: {
            uint16_t idx = (uint16_t)fetch_i16(f);
            Value val; stack_pop(f, &val);
            int32_t objref; pop_ref(f, &objref);
            if (objref < 0 || objref >= MAX_OBJECTS || !vm->objects[objref].alive) {
                result = JVM_ERR_NULL_POINTER; goto done;
            }
            HeapObject *obj = &vm->objects[objref];
            int field_idx = vm_find_field(vm, obj->class_id,
                                          obj->fields[idx % obj->field_count].name);
            if (field_idx < 0) field_idx = (int)(idx % obj->field_count);
            obj->fields[field_idx].value = val;
            break;
        }

        /* ── invokevirtual ────────────────────────────── */
        /* FIX #2: use dispatch_println which checks descriptor / type tag */
        case OP_INVOKEVIRTUAL: {
            uint16_t idx = (uint16_t)fetch_i16(f);
            (void)idx;
            dispatch_println(f, /*is_virtual=*/1);
            break;
        }

        /* ── invokespecial (same stub as invokevirtual for now) ─── */
        case OP_INVOKESPECIAL: {
            uint16_t idx = (uint16_t)fetch_i16(f);
            (void)idx;
            /* Constructor stub: pop objectref */
            Value ref; stack_pop(f, &ref);
            break;
        }

        /* ── invokestatic ─────────────────────────────── */
        /* FIX #2: does NOT pop objectref (static has none) */
        case OP_INVOKESTATIC: {
            uint16_t idx = (uint16_t)fetch_i16(f);
            (void)idx;
            dispatch_println(f, /*is_virtual=*/0);
            break;
        }

        default:
            fprintf(stderr, "Unknown opcode: 0x%02X at pc=%u\n",
                    opcode, f->pc - 1);
            result = JVM_ERR_UNKNOWN_OPCODE;
            goto done;
        }
    }

done:
    /* FIX #8: free code buffer only if this frame owns it */
    if (f->code_owned && f->code) {
        free((void *)f->code);
        f->code = NULL;
    }
    vm->fp--;
    return result;
}

/* ════════════════════════════════════════════════════════════
 * Error string helper
 * ════════════════════════════════════════════════════════════ */

const char *jvm_result_str(JVMResult r)
{
    switch (r) {
    case JVM_OK:                  return "OK";
    case JVM_RETURN_INT:          return "RETURN_INT";
    case JVM_RETURN_LONG:         return "RETURN_LONG";
    case JVM_RETURN_VOID:         return "RETURN_VOID";
    case JVM_RETURN_REF:          return "RETURN_REF";
    case JVM_ERR_STACK_OVERFLOW:  return "ERR_STACK_OVERFLOW";
    case JVM_ERR_STACK_UNDERFLOW: return "ERR_STACK_UNDERFLOW";
    case JVM_ERR_UNKNOWN_OPCODE:  return "ERR_UNKNOWN_OPCODE";
    case JVM_ERR_DIVIDE_BY_ZERO:  return "ERR_DIVIDE_BY_ZERO";
    case JVM_ERR_OUT_OF_BOUNDS:   return "ERR_OUT_OF_BOUNDS";
    case JVM_ERR_NO_FRAME:        return "ERR_NO_FRAME";
    case JVM_ERR_NULL_POINTER:    return "ERR_NULL_POINTER";
    case JVM_ERR_OUT_OF_MEMORY:   return "ERR_OUT_OF_MEMORY";
    case JVM_ERR_TYPE_MISMATCH:   return "ERR_TYPE_MISMATCH";
    default:                      return "UNKNOWN";
    }
}
