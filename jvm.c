#define _POSIX_C_SOURCE 200809L
#include "jvm.h"

/* ════════════════════════════════════════════════════════════
 *  Internal helpers
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
    Value v; v.type = VAL_INT; v.ival = n;
    return stack_push(f, v);
}

static JVMResult push_float(Frame *f, float n)
{
    Value v; v.type = VAL_FLOAT; v.fval = n;
    return stack_push(f, v);
}

static JVMResult push_ref(Frame *f, int32_t ref)
{
    Value v; v.type = VAL_REF; v.ref = ref;
    return stack_push(f, v);
}

static JVMResult pop_int(Frame *f, int32_t *n)
{
    Value v;
    JVMResult r = stack_pop(f, &v);
    if (r != JVM_OK) return r;
    *n = v.ival;
    return JVM_OK;
}

static JVMResult pop_float(Frame *f, float *n)
{
    Value v;
    JVMResult r = stack_pop(f, &v);
    if (r != JVM_OK) return r;
    *n = v.fval;
    return JVM_OK;
}

static JVMResult pop_ref(Frame *f, int32_t *ref)
{
    Value v;
    JVMResult r = stack_pop(f, &v);
    if (r != JVM_OK) return r;
    *ref = v.ref;
    return JVM_OK;
}

/* ════════════════════════════════════════════════════════════
 *  Bytecode fetch helpers
 * ════════════════════════════════════════════════════════════ */

static JVMResult fetch_u8(Frame *f, uint8_t *out)
{
    if (f->pc >= f->code_len) return JVM_ERR_OUT_OF_BOUNDS;
    *out = f->code[f->pc++];
    return JVM_OK;
}

static JVMResult fetch_i16(Frame *f, int16_t *out)
{
    uint8_t hi, lo;
    JVMResult r;
    r = fetch_u8(f, &hi); if (r != JVM_OK) return r;
    r = fetch_u8(f, &lo); if (r != JVM_OK) return r;
    *out = (int16_t)((hi << 8) | lo);
    return JVM_OK;
}

static JVMResult fetch_u16(Frame *f, uint16_t *out)
{
    int16_t v;
    JVMResult r = fetch_i16(f, &v);
    if (r != JVM_OK) return r;
    *out = (uint16_t)v;
    return JVM_OK;
}

static JVMResult fetch_i32(Frame *f, int32_t *out)
{
    uint8_t b0,b1,b2,b3;
    JVMResult r;
    r = fetch_u8(f,&b0); if(r) return r;
    r = fetch_u8(f,&b1); if(r) return r;
    r = fetch_u8(f,&b2); if(r) return r;
    r = fetch_u8(f,&b3); if(r) return r;
    *out = (int32_t)(((uint32_t)b0<<24)|((uint32_t)b1<<16)|
                     ((uint32_t)b2<< 8)| (uint32_t)b3);
    return JVM_OK;
}

/* ════════════════════════════════════════════════════════════
 *  Safe fetch / pop macros
 * ════════════════════════════════════════════════════════════ */

#define FETCH_U8(var) \
    do { JVMResult _r=fetch_u8(f,&(var)); if(_r){result=_r;goto done;} } while(0)
#define FETCH_I16(var) \
    do { JVMResult _r=fetch_i16(f,&(var)); if(_r){result=_r;goto done;} } while(0)
#define FETCH_U16(var) \
    do { JVMResult _r=fetch_u16(f,&(var)); if(_r){result=_r;goto done;} } while(0)
#define FETCH_I32(var) \
    do { JVMResult _r=fetch_i32(f,&(var)); if(_r){result=_r;goto done;} } while(0)
#define POP_INT_OR_FAIL(var) \
    do { JVMResult _r=pop_int(f,&(var)); if(_r){result=_r;goto done;} } while(0)
#define POP_FLOAT_OR_FAIL(var) \
    do { JVMResult _r=pop_float(f,&(var)); if(_r){result=_r;goto done;} } while(0)
#define POP_REF_OR_FAIL(var) \
    do { JVMResult _r=pop_ref(f,&(var)); if(_r){result=_r;goto done;} } while(0)
#define PUSH_INT(val) \
    do { JVMResult _r=push_int(f,(val)); if(_r){result=_r;goto done;} } while(0)
#define PUSH_FLOAT(val) \
    do { JVMResult _r=push_float(f,(val)); if(_r){result=_r;goto done;} } while(0)
#define PUSH_REF(val) \
    do { JVMResult _r=push_ref(f,(val)); if(_r){result=_r;goto done;} } while(0)

/* ════════════════════════════════════════════════════════════
 *  Heap allocator
 * ════════════════════════════════════════════════════════════ */

int32_t vm_alloc_object(VM *vm, const char *class_name)
{
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (vm->objects[i].type == OBJ_FREE) {
            memset(&vm->objects[i], 0, sizeof(JObject));
            vm->objects[i].type       = OBJ_PLAIN;
            vm->objects[i].class_name = class_name ? strdup(class_name) : NULL;
            return i + 1;
        }
    }
    return 0;
}

int32_t vm_alloc_array(VM *vm, int length)
{
    if (length < 0) return -1;
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (vm->objects[i].type == OBJ_FREE) {
            memset(&vm->objects[i], 0, sizeof(JObject));
            vm->objects[i].type   = OBJ_INTARRAY;
            vm->objects[i].length = length;
            vm->objects[i].idata  = length > 0
                ? (int32_t *)calloc((size_t)length, sizeof(int32_t)) : NULL;
            return i + 1;
        }
    }
    return 0;
}

static int32_t vm_alloc_refarray(VM *vm, int length)
{
    if (length < 0) return -1;
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (vm->objects[i].type == OBJ_FREE) {
            memset(&vm->objects[i], 0, sizeof(JObject));
            vm->objects[i].type   = OBJ_REFARRAY;
            vm->objects[i].length = length;
            vm->objects[i].rdata  = length > 0
                ? (int32_t *)calloc((size_t)length, sizeof(int32_t)) : NULL;
            return i + 1;
        }
    }
    return 0;
}

int32_t vm_alloc_string(VM *vm, const char *str)
{
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (vm->objects[i].type == OBJ_FREE) {
            memset(&vm->objects[i], 0, sizeof(JObject));
            vm->objects[i].type = OBJ_STRING;
            vm->objects[i].str  = str ? strdup(str) : NULL;
            return i + 1;
        }
    }
    return 0;
}

static JObject *deref(VM *vm, int32_t ref)
{
    if (ref <= 0 || ref > MAX_OBJECTS) return NULL;
    JObject *o = &vm->objects[ref - 1];
    return (o->type == OBJ_FREE) ? NULL : o;
}

/* ════════════════════════════════════════════════════════════
 *  Mark-and-sweep GC
 * ════════════════════════════════════════════════════════════ */

static void gc_mark_value(VM *vm, Value v)
{
    if (v.type != VAL_REF) return;
    int32_t ref = v.ref;
    if (ref <= 0 || ref > MAX_OBJECTS) return;
    JObject *o = &vm->objects[ref - 1];
    if (o->type == OBJ_FREE || o->marked) return;
    o->marked = 1;
    for (int i = 0; i < o->field_count; i++)
        gc_mark_value(vm, o->fields[i]);
    if (o->type == OBJ_REFARRAY && o->rdata)
        for (int i = 0; i < o->length; i++) {
            Value rv; rv.type = VAL_REF; rv.ref = o->rdata[i];
            gc_mark_value(vm, rv);
        }
}

void vm_gc(VM *vm)
{
    for (int i = 0; i < MAX_OBJECTS; i++)
        vm->objects[i].marked = 0;

    for (int fi = 0; fi <= vm->fp; fi++) {
        Frame *fr = &vm->frames[fi];
        for (int i = 0; i <= fr->sp; i++)
            gc_mark_value(vm, fr->stack[i]);
        for (int i = 0; i < MAX_LOCALS; i++)
            gc_mark_value(vm, fr->locals[i]);
    }

    for (int i = 0; i < MAX_OBJECTS; i++) {
        JObject *o = &vm->objects[i];
        if (o->type != OBJ_FREE && !o->marked) {
            free(o->class_name);
            free(o->idata);
            free(o->rdata);
            free(o->str);
            memset(o, 0, sizeof(JObject));
        }
    }
    if (vm->verbose) printf("  [GC] sweep complete\n");
}

/* ════════════════════════════════════════════════════════════
 *  VM init
 * ════════════════════════════════════════════════════════════ */

void vm_init(VM *vm, int verbose)
{
    memset(vm, 0, sizeof(VM));
    vm->fp      = -1;
    vm->verbose = verbose;
}

/* ════════════════════════════════════════════════════════════
 *  Branch helper
 * ════════════════════════════════════════════════════════════ */

static JVMResult do_branch(Frame *f, int16_t offset, uint32_t opcode_pc)
{
    int32_t target = (int32_t)opcode_pc + (int32_t)offset;
    if (target < 0 || (uint32_t)target >= f->code_len)
        return JVM_ERR_OUT_OF_BOUNDS;
    f->pc = (uint32_t)target;
    return JVM_OK;
}

/* ════════════════════════════════════════════════════════════
 *  Main interpreter loop
 * ════════════════════════════════════════════════════════════ */

JVMResult vm_exec(VM *vm, const uint8_t *code, uint32_t len,
                  int32_t *ret_val)
{
    if (vm->fp >= MAX_FRAMES - 1) return JVM_ERR_STACK_OVERFLOW;
    vm->fp++;
    Frame *f = current_frame(vm);
    memset(f, 0, sizeof(Frame));
    f->code     = code;
    f->code_len = len;
    f->pc       = 0;
    f->sp       = -1;

    JVMResult result = JVM_OK;

    while (f->pc < f->code_len) {

        uint32_t opcode_pc = f->pc;
        uint8_t  opcode;
        FETCH_U8(opcode);

        if (vm->verbose)
            printf("  [pc=%3u] opcode=0x%02X  sp=%d\n",
                   opcode_pc, opcode, f->sp);

        switch (opcode) {

        case OP_NOP: break;

        /* ── constants ─────────────────────────────────── */
        case OP_ACONST_NULL: PUSH_REF(0);  break;
        case OP_ICONST_M1:   PUSH_INT(-1); break;
        case OP_ICONST_0:    PUSH_INT(0);  break;
        case OP_ICONST_1:    PUSH_INT(1);  break;
        case OP_ICONST_2:    PUSH_INT(2);  break;
        case OP_ICONST_3:    PUSH_INT(3);  break;
        case OP_ICONST_4:    PUSH_INT(4);  break;
        case OP_ICONST_5:    PUSH_INT(5);  break;
        case OP_LCONST_0:    PUSH_INT(0);  break;
        case OP_LCONST_1:    PUSH_INT(1);  break;
        case OP_FCONST_0:    PUSH_FLOAT(0.0f); break;
        case OP_FCONST_1:    PUSH_FLOAT(1.0f); break;
        case OP_FCONST_2:    PUSH_FLOAT(2.0f); break;

        case OP_BIPUSH: {
            uint8_t raw; FETCH_U8(raw);
            PUSH_INT((int32_t)(int8_t)raw);
            break;
        }
        case OP_SIPUSH: {
            int16_t val; FETCH_I16(val);
            PUSH_INT((int32_t)val);
            break;
        }
        case OP_LDC: {
            uint8_t idx; FETCH_U8(idx);
            PUSH_INT((int32_t)idx);
            break;
        }
        case OP_LDC_W: {
            uint16_t idx; FETCH_U16(idx);
            PUSH_INT((int32_t)idx);
            break;
        }

        /* ── integer loads ─────────────────────────────── */
        case OP_ILOAD: {
            uint8_t idx; FETCH_U8(idx);
            if (idx >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            PUSH_INT(f->locals[idx].ival); break;
        }
        case OP_ILOAD_0: PUSH_INT(f->locals[0].ival); break;
        case OP_ILOAD_1: PUSH_INT(f->locals[1].ival); break;
        case OP_ILOAD_2: PUSH_INT(f->locals[2].ival); break;
        case OP_ILOAD_3: PUSH_INT(f->locals[3].ival); break;

        /* ── reference loads ───────────────────────────── */
        case OP_ALOAD: {
            uint8_t idx; FETCH_U8(idx);
            if (idx >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            PUSH_REF(f->locals[idx].ref); break;
        }
        case OP_ALOAD_0: PUSH_REF(f->locals[0].ref); break;
        case OP_ALOAD_1: PUSH_REF(f->locals[1].ref); break;
        case OP_ALOAD_2: PUSH_REF(f->locals[2].ref); break;
        case OP_ALOAD_3: PUSH_REF(f->locals[3].ref); break;

        /* ── array loads ───────────────────────────────── */
        case OP_IALOAD: case OP_BALOAD: case OP_CALOAD: {
            int32_t idx, ref;
            POP_INT_OR_FAIL(idx); POP_REF_OR_FAIL(ref);
            JObject *arr = deref(vm, ref);
            if (!arr) { result = JVM_ERR_NULL_POINTER; goto done; }
            if (idx < 0 || idx >= arr->length) { result = JVM_ERR_ARRAY_INDEX; goto done; }
            PUSH_INT(arr->idata[idx]); break;
        }
        case OP_AALOAD: {
            int32_t idx, ref;
            POP_INT_OR_FAIL(idx); POP_REF_OR_FAIL(ref);
            JObject *arr = deref(vm, ref);
            if (!arr) { result = JVM_ERR_NULL_POINTER; goto done; }
            if (idx < 0 || idx >= arr->length) { result = JVM_ERR_ARRAY_INDEX; goto done; }
            PUSH_REF(arr->rdata[idx]); break;
        }

        /* ── integer stores ────────────────────────────── */
        case OP_ISTORE: {
            uint8_t idx; FETCH_U8(idx);
            if (idx >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            int32_t v; POP_INT_OR_FAIL(v);
            f->locals[idx].type = VAL_INT; f->locals[idx].ival = v; break;
        }
        case OP_ISTORE_0: { int32_t v; POP_INT_OR_FAIL(v); f->locals[0].ival=v; f->locals[0].type=VAL_INT; break; }
        case OP_ISTORE_1: { int32_t v; POP_INT_OR_FAIL(v); f->locals[1].ival=v; f->locals[1].type=VAL_INT; break; }
        case OP_ISTORE_2: { int32_t v; POP_INT_OR_FAIL(v); f->locals[2].ival=v; f->locals[2].type=VAL_INT; break; }
        case OP_ISTORE_3: { int32_t v; POP_INT_OR_FAIL(v); f->locals[3].ival=v; f->locals[3].type=VAL_INT; break; }

        /* ── reference stores ──────────────────────────── */
        case OP_ASTORE: {
            uint8_t idx; FETCH_U8(idx);
            if (idx >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            int32_t ref; POP_REF_OR_FAIL(ref);
            f->locals[idx].type = VAL_REF; f->locals[idx].ref = ref; break;
        }
        case OP_ASTORE_0: { int32_t r; POP_REF_OR_FAIL(r); f->locals[0].ref=r; f->locals[0].type=VAL_REF; break; }
        case OP_ASTORE_1: { int32_t r; POP_REF_OR_FAIL(r); f->locals[1].ref=r; f->locals[1].type=VAL_REF; break; }
        case OP_ASTORE_2: { int32_t r; POP_REF_OR_FAIL(r); f->locals[2].ref=r; f->locals[2].type=VAL_REF; break; }
        case OP_ASTORE_3: { int32_t r; POP_REF_OR_FAIL(r); f->locals[3].ref=r; f->locals[3].type=VAL_REF; break; }

        /* ── array stores ──────────────────────────────── */
        case OP_IASTORE: case OP_BASTORE: case OP_CASTORE: {
            int32_t val, idx, ref;
            POP_INT_OR_FAIL(val); POP_INT_OR_FAIL(idx); POP_REF_OR_FAIL(ref);
            JObject *arr = deref(vm, ref);
            if (!arr) { result = JVM_ERR_NULL_POINTER; goto done; }
            if (idx < 0 || idx >= arr->length) { result = JVM_ERR_ARRAY_INDEX; goto done; }
            arr->idata[idx] = val; break;
        }
        case OP_AASTORE: {
            int32_t val, idx, ref;
            POP_REF_OR_FAIL(val); POP_INT_OR_FAIL(idx); POP_REF_OR_FAIL(ref);
            JObject *arr = deref(vm, ref);
            if (!arr) { result = JVM_ERR_NULL_POINTER; goto done; }
            if (idx < 0 || idx >= arr->length) { result = JVM_ERR_ARRAY_INDEX; goto done; }
            arr->rdata[idx] = val; break;
        }

        /* ── stack ops ─────────────────────────────────── */
        case OP_POP: {
            Value d; if(stack_pop(f,&d)!=JVM_OK){result=JVM_ERR_STACK_UNDERFLOW;goto done;} break;
        }
        case OP_POP2: {
            Value a,b;
            if(stack_pop(f,&a)!=JVM_OK||stack_pop(f,&b)!=JVM_OK){result=JVM_ERR_STACK_UNDERFLOW;goto done;} break;
        }
        case OP_DUP: {
            Value top;
            if(stack_peek(f,&top)!=JVM_OK){result=JVM_ERR_STACK_UNDERFLOW;goto done;}
            if(stack_push(f,top)!=JVM_OK){result=JVM_ERR_STACK_OVERFLOW;goto done;} break;
        }
        case OP_DUP_X1: {
            Value v1,v2;
            if(stack_pop(f,&v1)!=JVM_OK||stack_pop(f,&v2)!=JVM_OK){result=JVM_ERR_STACK_UNDERFLOW;goto done;}
            stack_push(f,v1); stack_push(f,v2); stack_push(f,v1); break;
        }
        case OP_DUP_X2: {
            Value v1,v2,v3;
            if(stack_pop(f,&v1)!=JVM_OK||stack_pop(f,&v2)!=JVM_OK||stack_pop(f,&v3)!=JVM_OK)
                {result=JVM_ERR_STACK_UNDERFLOW;goto done;}
            stack_push(f,v1); stack_push(f,v3); stack_push(f,v2); stack_push(f,v1); break;
        }
        case OP_DUP2: {
            Value v1,v2;
            if(stack_pop(f,&v1)!=JVM_OK||stack_pop(f,&v2)!=JVM_OK){result=JVM_ERR_STACK_UNDERFLOW;goto done;}
            stack_push(f,v2); stack_push(f,v1); stack_push(f,v2); stack_push(f,v1); break;
        }
        case OP_SWAP: {
            Value a,b;
            if(stack_pop(f,&a)!=JVM_OK||stack_pop(f,&b)!=JVM_OK){result=JVM_ERR_STACK_UNDERFLOW;goto done;}
            stack_push(f,a); stack_push(f,b); break;
        }

        /* ── integer arithmetic ────────────────────────── */
        case OP_IADD: { int32_t b,a; POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a); PUSH_INT(a+b); break; }
        case OP_ISUB: { int32_t b,a; POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a); PUSH_INT(a-b); break; }
        case OP_IMUL: { int32_t b,a; POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a); PUSH_INT(a*b); break; }
        case OP_IDIV: {
            int32_t b,a; POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a);
            if(b==0){result=JVM_ERR_DIVIDE_BY_ZERO;goto done;} PUSH_INT(a/b); break;
        }
        case OP_IREM: {
            int32_t b,a; POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a);
            if(b==0){result=JVM_ERR_DIVIDE_BY_ZERO;goto done;} PUSH_INT(a%b); break;
        }
        case OP_INEG: { int32_t a; POP_INT_OR_FAIL(a); PUSH_INT(-a); break; }

        /* ── float arithmetic ──────────────────────────── */
        case OP_FADD: { float b,a; POP_FLOAT_OR_FAIL(b); POP_FLOAT_OR_FAIL(a); PUSH_FLOAT(a+b); break; }
        case OP_FSUB: { float b,a; POP_FLOAT_OR_FAIL(b); POP_FLOAT_OR_FAIL(a); PUSH_FLOAT(a-b); break; }
        case OP_FMUL: { float b,a; POP_FLOAT_OR_FAIL(b); POP_FLOAT_OR_FAIL(a); PUSH_FLOAT(a*b); break; }
        case OP_FDIV: {
            float b,a; POP_FLOAT_OR_FAIL(b); POP_FLOAT_OR_FAIL(a);
            PUSH_FLOAT(b==0.0f?(a>0?1e30f:-1e30f):a/b); break;
        }
        case OP_FREM: {
            float b,a; POP_FLOAT_OR_FAIL(b); POP_FLOAT_OR_FAIL(a);
            if(b==0.0f){result=JVM_ERR_DIVIDE_BY_ZERO;goto done;}
            PUSH_FLOAT(a-(float)((int)(a/b))*b); break;
        }
        case OP_FNEG: { float a; POP_FLOAT_OR_FAIL(a); PUSH_FLOAT(-a); break; }

        /* ── iinc ──────────────────────────────────────── */
        case OP_IINC: {
            uint8_t idx,raw; FETCH_U8(idx); FETCH_U8(raw);
            if(idx>=MAX_LOCALS){result=JVM_ERR_OUT_OF_BOUNDS;goto done;}
            f->locals[idx].ival+=(int8_t)raw; break;
        }

        /* ── bitwise / shift ───────────────────────────── */
        case OP_ISHL:  { int32_t b,a; POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a); PUSH_INT(a<<(b&0x1F)); break; }
        case OP_ISHR:  { int32_t b,a; POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a); PUSH_INT(a>>(b&0x1F)); break; }
        case OP_IUSHR: { int32_t b,a; POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a); PUSH_INT((int32_t)((uint32_t)a>>(b&0x1F))); break; }
        case OP_IAND:  { int32_t b,a; POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a); PUSH_INT(a&b); break; }
        case OP_IOR:   { int32_t b,a; POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a); PUSH_INT(a|b); break; }
        case OP_IXOR:  { int32_t b,a; POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a); PUSH_INT(a^b); break; }

        /* ── float compare ─────────────────────────────── */
        case OP_FCMPL: case OP_FCMPG: {
            float b,a; POP_FLOAT_OR_FAIL(b); POP_FLOAT_OR_FAIL(a);
            PUSH_INT(a>b?1:(a<b?-1:0)); break;
        }

        /* ── type conversions ──────────────────────────── */
        case OP_I2F: { int32_t a; POP_INT_OR_FAIL(a);   PUSH_FLOAT((float)a);          break; }
        case OP_F2I: { float   a; POP_FLOAT_OR_FAIL(a); PUSH_INT((int32_t)a);           break; }
        case OP_I2B: { int32_t a; POP_INT_OR_FAIL(a);   PUSH_INT((int32_t)(int8_t)a);  break; }
        case OP_I2C: { int32_t a; POP_INT_OR_FAIL(a);   PUSH_INT((int32_t)(uint16_t)a);break; }
        case OP_I2S: { int32_t a; POP_INT_OR_FAIL(a);   PUSH_INT((int32_t)(int16_t)a); break; }

        /* ── branches: single-value ────────────────────── */
        case OP_IFEQ: case OP_IFNE: case OP_IFLT:
        case OP_IFGE: case OP_IFGT: case OP_IFLE: {
            int16_t offset; FETCH_I16(offset);
            int32_t v; POP_INT_OR_FAIL(v);
            int take=(opcode==OP_IFEQ&&v==0)||(opcode==OP_IFNE&&v!=0)||
                     (opcode==OP_IFLT&&v<0) ||(opcode==OP_IFGE&&v>=0)||
                     (opcode==OP_IFGT&&v>0) ||(opcode==OP_IFLE&&v<=0);
            if(take){result=do_branch(f,offset,opcode_pc);if(result)goto done;}
            break;
        }

        /* ── branches: two-value int ───────────────────── */
        case OP_IF_ICMPEQ: case OP_IF_ICMPNE: case OP_IF_ICMPLT:
        case OP_IF_ICMPGE: case OP_IF_ICMPGT: case OP_IF_ICMPLE: {
            int16_t offset; FETCH_I16(offset);
            int32_t b,a; POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a);
            int take=(opcode==OP_IF_ICMPEQ&&a==b)||(opcode==OP_IF_ICMPNE&&a!=b)||
                     (opcode==OP_IF_ICMPLT&&a<b) ||(opcode==OP_IF_ICMPGE&&a>=b)||
                     (opcode==OP_IF_ICMPGT&&a>b) ||(opcode==OP_IF_ICMPLE&&a<=b);
            if(take){result=do_branch(f,offset,opcode_pc);if(result)goto done;}
            break;
        }

        /* ── branches: reference ───────────────────────── */
        case OP_IF_ACMPEQ: case OP_IF_ACMPNE: {
            int16_t offset; FETCH_I16(offset);
            int32_t b,a; POP_REF_OR_FAIL(b); POP_REF_OR_FAIL(a);
            int take=(opcode==OP_IF_ACMPEQ)?(a==b):(a!=b);
            if(take){result=do_branch(f,offset,opcode_pc);if(result)goto done;}
            break;
        }
        case OP_IFNULL: case OP_IFNONNULL: {
            int16_t offset; FETCH_I16(offset);
            int32_t ref; POP_REF_OR_FAIL(ref);
            int take=(opcode==OP_IFNULL)?(ref==0):(ref!=0);
            if(take){result=do_branch(f,offset,opcode_pc);if(result)goto done;}
            break;
        }

        /* ── jumps ─────────────────────────────────────── */
        case OP_GOTO: {
            int16_t offset; FETCH_I16(offset);
            result=do_branch(f,offset,opcode_pc); if(result)goto done; break;
        }
        case OP_GOTO_W: {
            int32_t offset; FETCH_I32(offset);
            int32_t target=(int32_t)opcode_pc+offset;
            if(target<0||(uint32_t)target>=f->code_len){result=JVM_ERR_OUT_OF_BOUNDS;goto done;}
            f->pc=(uint32_t)target; break;
        }

        /* ── returns ───────────────────────────────────── */
        case OP_IRETURN: { int32_t v; POP_INT_OR_FAIL(v); if(ret_val)*ret_val=v; result=JVM_RETURN_INT;   goto done; }
        case OP_FRETURN: { float   v; POP_FLOAT_OR_FAIL(v); if(ret_val)*ret_val=*(int32_t*)&v; result=JVM_RETURN_FLOAT; goto done; }
        case OP_ARETURN: { int32_t v; POP_REF_OR_FAIL(v);  if(ret_val)*ret_val=v; result=JVM_RETURN_REF;   goto done; }
        case OP_RETURN:  { result=JVM_RETURN_VOID; goto done; }

        /* ── object / array creation ───────────────────── */
        case OP_NEW: {
            uint16_t idx; FETCH_U16(idx); (void)idx;
            int32_t ref=vm_alloc_object(vm,NULL);
            if(!ref){result=JVM_ERR_OUT_OF_MEMORY;goto done;}
            PUSH_REF(ref); break;
        }
        case OP_NEWARRAY: {
            uint8_t atype; FETCH_U8(atype); (void)atype;
            int32_t count; POP_INT_OR_FAIL(count);
            if(count<0){result=JVM_ERR_NEGATIVE_ARRAY_SIZE;goto done;}
            int32_t ref=vm_alloc_array(vm,count);
            if(!ref){result=JVM_ERR_OUT_OF_MEMORY;goto done;}
            PUSH_REF(ref); break;
        }
        case OP_ANEWARRAY: {
            uint16_t idx; FETCH_U16(idx); (void)idx;
            int32_t count; POP_INT_OR_FAIL(count);
            if(count<0){result=JVM_ERR_NEGATIVE_ARRAY_SIZE;goto done;}
            int32_t ref=vm_alloc_refarray(vm,count);
            if(!ref){result=JVM_ERR_OUT_OF_MEMORY;goto done;}
            PUSH_REF(ref); break;
        }

        /* ── array length ──────────────────────────────── */
        case OP_ARRAYLENGTH: {
            int32_t ref; POP_REF_OR_FAIL(ref);
            JObject *arr=deref(vm,ref);
            if(!arr){result=JVM_ERR_NULL_POINTER;goto done;}
            PUSH_INT(arr->length); break;
        }

        /* ── field access ──────────────────────────────── */
        case OP_GETFIELD: {
            uint16_t idx; FETCH_U16(idx);
            int32_t ref; POP_REF_OR_FAIL(ref);
            JObject *obj=deref(vm,ref);
            if(!obj){result=JVM_ERR_NULL_POINTER;goto done;}
            uint16_t fi=idx%MAX_FIELDS;
            Value fv=(fi<(uint16_t)obj->field_count)?obj->fields[fi]:(Value){VAL_INT,{0}};
            if(stack_push(f,fv)!=JVM_OK){result=JVM_ERR_STACK_OVERFLOW;goto done;}
            break;
        }
        case OP_PUTFIELD: {
            uint16_t idx; FETCH_U16(idx);
            Value val; if(stack_pop(f,&val)!=JVM_OK){result=JVM_ERR_STACK_UNDERFLOW;goto done;}
            int32_t ref; POP_REF_OR_FAIL(ref);
            JObject *obj=deref(vm,ref);
            if(!obj){result=JVM_ERR_NULL_POINTER;goto done;}
            uint16_t fi=idx%MAX_FIELDS;
            obj->fields[fi]=val;
            if((int)fi>=obj->field_count) obj->field_count=fi+1;
            break;
        }
        case OP_GETSTATIC: {
            uint16_t idx; FETCH_U16(idx); (void)idx;
            Value ref; ref.type=VAL_REF; ref.ref=0;
            if(stack_push(f,ref)!=JVM_OK){result=JVM_ERR_STACK_OVERFLOW;goto done;} break;
        }
        case OP_PUTSTATIC: {
            uint16_t idx; FETCH_U16(idx); (void)idx;
            Value d; if(stack_pop(f,&d)!=JVM_OK){result=JVM_ERR_STACK_UNDERFLOW;goto done;} break;
        }

        /* ── method invocation ─────────────────────────── */
        case OP_INVOKEVIRTUAL:
        case OP_INVOKESPECIAL:
        case OP_INVOKESTATIC: {
            uint16_t idx; FETCH_U16(idx); (void)idx;
            if(opcode!=OP_INVOKESTATIC && f->sp>=1) {
                Value arg,ref;
                stack_pop(f,&arg); stack_pop(f,&ref);
                if(ref.type==VAL_REF && ref.ref==0)
                    printf("[native] System.out.println(%d)\n",arg.ival);
            }
            break;
        }
        case OP_INVOKEINTERFACE: {
            uint16_t idx; FETCH_U16(idx); (void)idx;
            uint8_t count,zero; FETCH_U8(count); FETCH_U8(zero); (void)count;(void)zero; break;
        }

        /* ── type checks ───────────────────────────────── */
        case OP_CHECKCAST: { uint16_t idx; FETCH_U16(idx); (void)idx; break; }
        case OP_INSTANCEOF: {
            uint16_t idx; FETCH_U16(idx); (void)idx;
            int32_t ref; POP_REF_OR_FAIL(ref);
            PUSH_INT(ref!=0?1:0); break;
        }

        /* ── athrow ────────────────────────────────────── */
        case OP_ATHROW: {
            int32_t ref; POP_REF_OR_FAIL(ref);
            if(ref==0){result=JVM_ERR_NULL_POINTER;goto done;}
            result=JVM_ERR_OUT_OF_BOUNDS; goto done;
        }

        /* ── wide prefix ───────────────────────────────── */
        case OP_WIDE: {
            uint8_t wop; FETCH_U8(wop);
            uint16_t widx; FETCH_U16(widx);
            switch(wop) {
            case OP_ILOAD:  if(widx>=MAX_LOCALS){result=JVM_ERR_OUT_OF_BOUNDS;goto done;} PUSH_INT(f->locals[widx].ival); break;
            case OP_ALOAD:  if(widx>=MAX_LOCALS){result=JVM_ERR_OUT_OF_BOUNDS;goto done;} PUSH_REF(f->locals[widx].ref);  break;
            case OP_ISTORE: if(widx>=MAX_LOCALS){result=JVM_ERR_OUT_OF_BOUNDS;goto done;} { int32_t v; POP_INT_OR_FAIL(v); f->locals[widx].ival=v; f->locals[widx].type=VAL_INT; } break;
            case OP_ASTORE: if(widx>=MAX_LOCALS){result=JVM_ERR_OUT_OF_BOUNDS;goto done;} { int32_t r; POP_REF_OR_FAIL(r); f->locals[widx].ref=r;  f->locals[widx].type=VAL_REF;  } break;
            case OP_IINC: { int16_t wc; FETCH_I16(wc); if(widx>=MAX_LOCALS){result=JVM_ERR_OUT_OF_BOUNDS;goto done;} f->locals[widx].ival+=wc; break; }
            default: fprintf(stderr,"Unknown wide opcode: 0x%02X\n",wop); result=JVM_ERR_UNKNOWN_OPCODE; goto done;
            }
            break;
        }

        default:
            fprintf(stderr,"Unknown opcode: 0x%02X at pc=%u\n",opcode,opcode_pc);
            result=JVM_ERR_UNKNOWN_OPCODE;
            goto done;
        }
    }

done:
    vm->fp--;
    return result;
}

/* ════════════════════════════════════════════════════════════
 *  Error string
 * ════════════════════════════════════════════════════════════ */

const char *jvm_result_str(JVMResult r)
{
    switch(r) {
    case JVM_OK:                      return "OK";
    case JVM_RETURN_INT:              return "RETURN_INT";
    case JVM_RETURN_VOID:             return "RETURN_VOID";
    case JVM_RETURN_REF:              return "RETURN_REF";
    case JVM_RETURN_FLOAT:            return "RETURN_FLOAT";
    case JVM_ERR_STACK_OVERFLOW:      return "ERR_STACK_OVERFLOW";
    case JVM_ERR_STACK_UNDERFLOW:     return "ERR_STACK_UNDERFLOW";
    case JVM_ERR_UNKNOWN_OPCODE:      return "ERR_UNKNOWN_OPCODE";
    case JVM_ERR_DIVIDE_BY_ZERO:      return "ERR_DIVIDE_BY_ZERO";
    case JVM_ERR_OUT_OF_BOUNDS:       return "ERR_OUT_OF_BOUNDS";
    case JVM_ERR_NO_FRAME:            return "ERR_NO_FRAME";
    case JVM_ERR_NULL_POINTER:        return "ERR_NULL_POINTER";
    case JVM_ERR_ARRAY_INDEX:         return "ERR_ARRAY_INDEX";
    case JVM_ERR_OUT_OF_MEMORY:       return "ERR_OUT_OF_MEMORY";
    case JVM_ERR_NEGATIVE_ARRAY_SIZE: return "ERR_NEGATIVE_ARRAY_SIZE";
    default:                          return "UNKNOWN";
    }
}
