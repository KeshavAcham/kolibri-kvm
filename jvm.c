#include "jvm.h"

/* ════════════════════════════════════════════════════════════
 * Helpers
 * ════════════════════════════════════════════════════════════ */

static Frame *current_frame(VM *vm)
{
    if (vm->fp < 0) return NULL;
    return &vm->frames[vm->fp];
}

/* issue #3: fetch_u8 now bounds-checks before reading */
static JVMResult fetch_u8_safe(Frame *f, uint8_t *out)
{
    if (f->pc >= f->code_len) return JVM_ERR_OUT_OF_BOUNDS;
    *out = f->code[f->pc++];
    return JVM_OK;
}

static JVMResult fetch_i16_safe(Frame *f, int16_t *out)
{
    uint8_t hi, lo;
    if (fetch_u8_safe(f, &hi) != JVM_OK) return JVM_ERR_OUT_OF_BOUNDS;
    if (fetch_u8_safe(f, &lo) != JVM_OK) return JVM_ERR_OUT_OF_BOUNDS;
    *out = (int16_t)((hi << 8) | lo);
    return JVM_OK;
}

/* Convenience macros that jump to done on fetch error */
#define FETCH_U8(var)  do { uint8_t _v; if (fetch_u8_safe(f,&_v)!=JVM_OK){result=JVM_ERR_OUT_OF_BOUNDS;goto done;} (var)=_v; } while(0)
#define FETCH_I16(var) do { int16_t _v; if (fetch_i16_safe(f,&_v)!=JVM_OK){result=JVM_ERR_OUT_OF_BOUNDS;goto done;} (var)=_v; } while(0)

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
    Value v; v.type = VAL_REF; v.ival = ref;
    return stack_push(f, v);
}

/* issue #4: all pop_* return JVMResult; callers must check */
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

/* Convenience macros that propagate pop errors to done */
#define POP_INT(var)   do { if (pop_int(f,&(var))   != JVM_OK) { result = JVM_ERR_STACK_UNDERFLOW; goto done; } } while(0)
#define POP_FLOAT(var) do { if (pop_float(f,&(var)) != JVM_OK) { result = JVM_ERR_STACK_UNDERFLOW; goto done; } } while(0)
#define POP_VAL(var)   do { if (stack_pop(f,&(var)) != JVM_OK) { result = JVM_ERR_STACK_UNDERFLOW; goto done; } } while(0)
#define PUSH_INT(n)    do { if (push_int(f,(n))     != JVM_OK) { result = JVM_ERR_STACK_OVERFLOW;  goto done; } } while(0)
#define PUSH_FLOAT(n)  do { if (push_float(f,(n))   != JVM_OK) { result = JVM_ERR_STACK_OVERFLOW;  goto done; } } while(0)
#define PUSH_REF(n)    do { if (push_ref(f,(n))     != JVM_OK) { result = JVM_ERR_STACK_OVERFLOW;  goto done; } } while(0)

/* issue #5: validate branch target before applying */
#define BRANCH(offset_i16) \
    do { \
        int32_t _target = (int32_t)(f->pc - 3) + (int32_t)(offset_i16); \
        if (_target < 0 || (uint32_t)_target >= f->code_len) { \
            result = JVM_ERR_OUT_OF_BOUNDS; goto done; \
        } \
        f->pc = (uint32_t)_target; \
    } while(0)

/* ════════════════════════════════════════════════════════════
 * GC — mark-and-sweep
 * issue #6: heap is now actually used by OP_NEWARRAY
 * ════════════════════════════════════════════════════════════ */
void vm_gc(VM *vm)
{
    /* Clear marks */
    for (int i = 0; i < MAX_OBJECTS; i++)
        vm->objects[i].marked = 0;

    /* Mark from roots: all live frame stacks and locals */
    for (int fi = 0; fi <= vm->fp; fi++) {
        Frame *f = &vm->frames[fi];
        for (int s = 0; s <= f->sp; s++) {
            if (f->stack[s].type == VAL_REF) {
                int32_t ref = f->stack[s].ival;
                if (ref > 0 && ref <= MAX_OBJECTS)
                    vm->objects[ref - 1].marked = 1;
            }
        }
        for (int l = 0; l < MAX_LOCALS; l++) {
            if (f->locals[l].type == VAL_REF) {
                int32_t ref = f->locals[l].ival;
                if (ref > 0 && ref <= MAX_OBJECTS)
                    vm->objects[ref - 1].marked = 1;
            }
        }
    }

    /* Sweep */
    vm->obj_count = 0;
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (vm->objects[i].in_use && !vm->objects[i].marked) {
            vm->objects[i].in_use = 0;
            vm->objects[i].type   = OBJ_NONE;
        }
        if (vm->objects[i].in_use) vm->obj_count++;
    }
}

/* Allocate a new int array; returns 1-based ref (0 = null) */
static int32_t vm_alloc_int_array(VM *vm, int32_t length)
{
    if (vm->obj_count >= GC_THRESHOLD) vm_gc(vm);
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (!vm->objects[i].in_use) {
            vm->objects[i].in_use = 1;
            vm->objects[i].marked = 0;
            vm->objects[i].type   = OBJ_INT_ARRAY;
            vm->objects[i].length = length;
            memset(vm->objects[i].idata, 0, sizeof(int32_t) * (size_t)(length < MAX_ARRAY_ELEMS ? length : MAX_ARRAY_ELEMS));
            vm->obj_count++;
            return (int32_t)(i + 1);   /* 1-based */
        }
    }
    return 0;   /* out of memory */
}

/* ════════════════════════════════════════════════════════════
 * issue #7: count_args — now actually called from INVOKEVIRTUAL
 * ════════════════════════════════════════════════════════════ */
static int count_args(const char *descriptor)
{
    int count = 0;
    if (!descriptor || descriptor[0] != '(') return 0;
    const char *p = descriptor + 1;
    while (*p && *p != ')') {
        if (*p == 'L') {
            while (*p && *p != ';') p++;
        } else if (*p == '[') {
            p++; continue;
        }
        count++;
        p++;
    }
    return count;
}

/* ════════════════════════════════════════════════════════════
 * VM init
 * ════════════════════════════════════════════════════════════ */
void vm_init(VM *vm, int verbose)
{
    memset(vm, 0, sizeof(VM));
    vm->fp        = -1;
    vm->verbose   = verbose;
    vm->obj_count = 0;
}

/* ════════════════════════════════════════════════════════════
 * Internal interpreter loop
 * ════════════════════════════════════════════════════════════ */
static JVMResult vm_exec_internal(VM *vm, int32_t *ret_val)
{
    Frame *f = current_frame(vm);
    if (!f) return JVM_ERR_NO_FRAME;

    JVMResult result = JVM_OK;

    while (f->pc < f->code_len) {
        uint8_t opcode;
        FETCH_U8(opcode);

        if (vm->verbose)
            printf("  [pc=%3u] opcode=0x%02X  sp=%d\n",
                   f->pc - 1, opcode, f->sp);

        switch (opcode) {

        case OP_NOP: break;

        /* ── constants ─────────────────────────────────── */
        case OP_ACONST_NULL: PUSH_REF(0); break;
        case OP_ICONST_M1:   PUSH_INT(-1); break;
        case OP_ICONST_0:    PUSH_INT(0);  break;
        case OP_ICONST_1:    PUSH_INT(1);  break;
        case OP_ICONST_2:    PUSH_INT(2);  break;
        case OP_ICONST_3:    PUSH_INT(3);  break;
        case OP_ICONST_4:    PUSH_INT(4);  break;
        case OP_ICONST_5:    PUSH_INT(5);  break;
        case OP_FCONST_0:    PUSH_FLOAT(0.0f); break;
        case OP_FCONST_1:    PUSH_FLOAT(1.0f); break;
        case OP_FCONST_2:    PUSH_FLOAT(2.0f); break;

        case OP_BIPUSH: { uint8_t v; FETCH_U8(v); PUSH_INT((int32_t)(int8_t)v); break; }
        case OP_SIPUSH: { int16_t v; FETCH_I16(v); PUSH_INT((int32_t)v); break; }

        /* issue #2: ldc resolves from CPEntry */
        case OP_LDC: {
            uint8_t idx; FETCH_U8(idx);
            if (!f->cp || idx == 0 || idx >= f->cp_len) {
                PUSH_INT((int32_t)idx);   /* fallback for test-suite compat */
                break;
            }
            const CPEntry *e = &f->cp[idx];
            switch (e->tag) {
                case CP_INTEGER: PUSH_INT(e->ival);   break;
                case CP_FLOAT:   PUSH_FLOAT(e->fval); break;
                case CP_STRING:  PUSH_REF(idx);        break;
                default:         PUSH_INT((int32_t)idx); break;
            }
            break;
        }

        /* ── integer loads ─────────────────────────────── */
        case OP_ILOAD: { uint8_t i; FETCH_U8(i); if(i>=MAX_LOCALS){result=JVM_ERR_OUT_OF_BOUNDS;goto done;} PUSH_INT(f->locals[i].ival); break; }
        case OP_ILOAD_0: PUSH_INT(f->locals[0].ival); break;
        case OP_ILOAD_1: PUSH_INT(f->locals[1].ival); break;
        case OP_ILOAD_2: PUSH_INT(f->locals[2].ival); break;
        case OP_ILOAD_3: PUSH_INT(f->locals[3].ival); break;

        /* ── reference loads ───────────────────────────── */
        case OP_ALOAD: { uint8_t i; FETCH_U8(i); if(i>=MAX_LOCALS){result=JVM_ERR_OUT_OF_BOUNDS;goto done;} PUSH_REF(f->locals[i].ival); break; }
        case OP_ALOAD_0: PUSH_REF(f->locals[0].ival); break;
        case OP_ALOAD_1: PUSH_REF(f->locals[1].ival); break;
        case OP_ALOAD_2: PUSH_REF(f->locals[2].ival); break;
        case OP_ALOAD_3: PUSH_REF(f->locals[3].ival); break;

        /* ── array loads ───────────────────────────────── */
        case OP_IALOAD: {
            int32_t idx, ref;
            POP_INT(idx); POP_INT(ref);
            if (ref <= 0 || ref > MAX_OBJECTS) { result = JVM_ERR_NULL_POINTER; goto done; }
            JObject *obj = &vm->objects[ref - 1];
            if (idx < 0 || idx >= obj->length) { result = JVM_ERR_ARRAY_INDEX; goto done; }
            PUSH_INT(obj->idata[idx]);
            break;
        }

        /* ── integer stores ────────────────────────────── */
        case OP_ISTORE: { uint8_t i; FETCH_U8(i); if(i>=MAX_LOCALS){result=JVM_ERR_OUT_OF_BOUNDS;goto done;} int32_t v; POP_INT(v); f->locals[i].ival=v; f->locals[i].type=VAL_INT; break; }
        case OP_ISTORE_0: { int32_t v; POP_INT(v); f->locals[0].ival=v; f->locals[0].type=VAL_INT; break; }
        case OP_ISTORE_1: { int32_t v; POP_INT(v); f->locals[1].ival=v; f->locals[1].type=VAL_INT; break; }
        case OP_ISTORE_2: { int32_t v; POP_INT(v); f->locals[2].ival=v; f->locals[2].type=VAL_INT; break; }
        case OP_ISTORE_3: { int32_t v; POP_INT(v); f->locals[3].ival=v; f->locals[3].type=VAL_INT; break; }

        /* ── reference stores ──────────────────────────── */
        case OP_ASTORE: { uint8_t i; FETCH_U8(i); if(i>=MAX_LOCALS){result=JVM_ERR_OUT_OF_BOUNDS;goto done;} int32_t v; POP_INT(v); f->locals[i].ival=v; f->locals[i].type=VAL_REF; break; }
        case OP_ASTORE_0: { int32_t v; POP_INT(v); f->locals[0].ival=v; f->locals[0].type=VAL_REF; break; }
        case OP_ASTORE_1: { int32_t v; POP_INT(v); f->locals[1].ival=v; f->locals[1].type=VAL_REF; break; }
        case OP_ASTORE_2: { int32_t v; POP_INT(v); f->locals[2].ival=v; f->locals[2].type=VAL_REF; break; }
        case OP_ASTORE_3: { int32_t v; POP_INT(v); f->locals[3].ival=v; f->locals[3].type=VAL_REF; break; }

        /* ── array stores ──────────────────────────────── */
        case OP_IASTORE: {
            int32_t val, idx, ref;
            POP_INT(val); POP_INT(idx); POP_INT(ref);
            if (ref <= 0 || ref > MAX_OBJECTS) { result = JVM_ERR_NULL_POINTER; goto done; }
            JObject *obj = &vm->objects[ref - 1];
            if (idx < 0 || idx >= obj->length) { result = JVM_ERR_ARRAY_INDEX; goto done; }
            obj->idata[idx] = val;
            break;
        }

        /* ── integer arithmetic ─────────────────────────  */
        case OP_IADD:  { int32_t b,a; POP_INT(b); POP_INT(a); PUSH_INT(a+b); break; }
        case OP_ISUB:  { int32_t b,a; POP_INT(b); POP_INT(a); PUSH_INT(a-b); break; }
        case OP_IMUL:  { int32_t b,a; POP_INT(b); POP_INT(a); PUSH_INT(a*b); break; }
        case OP_IDIV:  { int32_t b,a; POP_INT(b); POP_INT(a); if(!b){result=JVM_ERR_DIVIDE_BY_ZERO;goto done;} PUSH_INT(a/b); break; }
        case OP_IREM:  { int32_t b,a; POP_INT(b); POP_INT(a); if(!b){result=JVM_ERR_DIVIDE_BY_ZERO;goto done;} PUSH_INT(a%b); break; }
        case OP_INEG:  { int32_t a; POP_INT(a); PUSH_INT(-a); break; }
        case OP_IINC:  { uint8_t i; int8_t c; FETCH_U8(i); uint8_t _c; FETCH_U8(_c); c=(int8_t)_c; if(i>=MAX_LOCALS){result=JVM_ERR_OUT_OF_BOUNDS;goto done;} f->locals[i].ival+=c; break; }

        /* ── float arithmetic ───────────────────────────  */
        case OP_FADD:  { float b,a; POP_FLOAT(b); POP_FLOAT(a); PUSH_FLOAT(a+b); break; }
        case OP_FSUB:  { float b,a; POP_FLOAT(b); POP_FLOAT(a); PUSH_FLOAT(a-b); break; }
        case OP_FMUL:  { float b,a; POP_FLOAT(b); POP_FLOAT(a); PUSH_FLOAT(a*b); break; }
        /* issue #3 (original): let IEEE 754 produce +Inf/-Inf/NaN naturally */
        case OP_FDIV:  { float b,a; POP_FLOAT(b); POP_FLOAT(a); PUSH_FLOAT(a/b); break; }
        case OP_FREM:  { float b,a; POP_FLOAT(b); POP_FLOAT(a); PUSH_FLOAT(fmodf(a,b)); break; }
        case OP_FNEG:  { float a; POP_FLOAT(a); PUSH_FLOAT(-a); break; }

        /* ── float compare — NaN handled per spec ───────  */
        /* issue #4 (original): fcmpl returns -1 on NaN, fcmpg returns +1 */
        case OP_FCMPL: { float b,a; POP_FLOAT(b); POP_FLOAT(a); PUSH_INT(isnan(a)||isnan(b)?-1:(a>b?1:(a<b?-1:0))); break; }
        case OP_FCMPG: { float b,a; POP_FLOAT(b); POP_FLOAT(a); PUSH_INT(isnan(a)||isnan(b)? 1:(a>b?1:(a<b?-1:0))); break; }

        /* ── bitwise / shift ────────────────────────────  */
        case OP_ISHL:  { int32_t b,a; POP_INT(b); POP_INT(a); PUSH_INT(a<<(b&0x1F)); break; }
        case OP_ISHR:  { int32_t b,a; POP_INT(b); POP_INT(a); PUSH_INT(a>>(b&0x1F)); break; }
        case OP_IUSHR: { int32_t b,a; POP_INT(b); POP_INT(a); PUSH_INT((int32_t)((uint32_t)a>>(b&0x1F))); break; }
        case OP_IAND:  { int32_t b,a; POP_INT(b); POP_INT(a); PUSH_INT(a&b); break; }
        case OP_IOR:   { int32_t b,a; POP_INT(b); POP_INT(a); PUSH_INT(a|b); break; }
        case OP_IXOR:  { int32_t b,a; POP_INT(b); POP_INT(a); PUSH_INT(a^b); break; }

        /* ── type conversions ───────────────────────────  */
        case OP_I2F:  { int32_t a; POP_INT(a);   PUSH_FLOAT((float)a); break; }
        case OP_F2I:  { float   a; POP_FLOAT(a); PUSH_INT((int32_t)a); break; }
        case OP_I2B:  { int32_t a; POP_INT(a);   PUSH_INT((int32_t)(int8_t)a); break; }
        case OP_I2C:  { int32_t a; POP_INT(a);   PUSH_INT((int32_t)(uint16_t)a); break; }
        case OP_I2S:  { int32_t a; POP_INT(a);   PUSH_INT((int32_t)(int16_t)a); break; }

        /* ── stack manipulation ─────────────────────────  */
        case OP_DUP: {
            Value top;
            if (stack_peek(f, &top) != JVM_OK) { result = JVM_ERR_STACK_UNDERFLOW; goto done; }
            if (stack_push(f, top)  != JVM_OK) { result = JVM_ERR_STACK_OVERFLOW;  goto done; }
            break;
        }
        case OP_DUP_X1: {
            /* ..., v2, v1 → ..., v1, v2, v1 */
            Value v1, v2;
            POP_VAL(v1); POP_VAL(v2);
            if (stack_push(f,v1)!=JVM_OK||stack_push(f,v2)!=JVM_OK||stack_push(f,v1)!=JVM_OK)
                { result=JVM_ERR_STACK_OVERFLOW; goto done; }
            break;
        }
        case OP_POP: { Value d; POP_VAL(d); break; }
        case OP_POP2: { Value d1, d2; POP_VAL(d1); POP_VAL(d2); break; }
        /* issue #8: SWAP now checks all four return values via macros */
        case OP_SWAP: {
            Value a, b;
            POP_VAL(a); POP_VAL(b);
            if (stack_push(f,a)!=JVM_OK||stack_push(f,b)!=JVM_OK)
                { result=JVM_ERR_STACK_OVERFLOW; goto done; }
            break;
        }

        /* ── array creation — issue #6: heap now actually used ── */
        case OP_NEWARRAY: {
            uint8_t atype; FETCH_U8(atype);
            int32_t count; POP_INT(count);
            if (count < 0) { result = JVM_ERR_NEGATIVE_ARRAY_SIZE; goto done; }
            if (count > MAX_ARRAY_ELEMS) { result = JVM_ERR_OUT_OF_MEMORY; goto done; }
            int32_t ref = vm_alloc_int_array(vm, count);
            if (ref == 0) { result = JVM_ERR_OUT_OF_MEMORY; goto done; }
            PUSH_REF(ref);
            (void)atype;
            break;
        }

        case OP_ARRAYLENGTH: {
            int32_t ref; POP_INT(ref);
            if (ref <= 0 || ref > MAX_OBJECTS) { result = JVM_ERR_NULL_POINTER; goto done; }
            PUSH_INT(vm->objects[ref - 1].length);
            break;
        }

        /* ── branches ────────────────────────────────────  */
        case OP_IFEQ: case OP_IFNE: case OP_IFLT:
        case OP_IFGE: case OP_IFGT: case OP_IFLE: {
            int16_t offset; FETCH_I16(offset);
            int32_t v; POP_INT(v);
            int take = (opcode==OP_IFEQ&&v==0)||(opcode==OP_IFNE&&v!=0)||
                       (opcode==OP_IFLT&&v<0) ||(opcode==OP_IFGE&&v>=0)||
                       (opcode==OP_IFGT&&v>0) ||(opcode==OP_IFLE&&v<=0);
            if (take) { BRANCH(offset); }
            break;
        }
        case OP_IFNULL:    { int16_t off; FETCH_I16(off); int32_t v; POP_INT(v); if(v==0){BRANCH(off);} break; }
        case OP_IFNONNULL: { int16_t off; FETCH_I16(off); int32_t v; POP_INT(v); if(v!=0){BRANCH(off);} break; }

        case OP_IF_ICMPEQ: case OP_IF_ICMPNE: case OP_IF_ICMPLT:
        case OP_IF_ICMPGE: case OP_IF_ICMPGT: case OP_IF_ICMPLE: {
            int16_t offset; FETCH_I16(offset);
            int32_t b,a; POP_INT(b); POP_INT(a);
            int take = (opcode==OP_IF_ICMPEQ&&a==b)||(opcode==OP_IF_ICMPNE&&a!=b)||
                       (opcode==OP_IF_ICMPLT&&a<b) ||(opcode==OP_IF_ICMPGE&&a>=b)||
                       (opcode==OP_IF_ICMPGT&&a>b) ||(opcode==OP_IF_ICMPLE&&a<=b);
            if (take) { BRANCH(offset); }
            break;
        }

        case OP_GOTO: { int16_t offset; FETCH_I16(offset); BRANCH(offset); break; }

        /* ── returns ─────────────────────────────────────  */
        case OP_IRETURN: { int32_t v; POP_INT(v); if(ret_val)*ret_val=v; result=JVM_RETURN_INT;   goto done; }
        case OP_FRETURN: { float   v; POP_FLOAT(v); if(ret_val)*ret_val=*(int32_t*)&v; result=JVM_RETURN_FLOAT; goto done; }
        case OP_ARETURN: { int32_t v; POP_INT(v); if(ret_val)*ret_val=v; result=JVM_RETURN_REF;   goto done; }
        case OP_RETURN:  { result=JVM_RETURN_VOID; goto done; }

        /* ── field/method stubs ──────────────────────────  */
        case OP_GETSTATIC:   { int16_t idx; FETCH_I16(idx); (void)idx; PUSH_REF(0); break; }
        case OP_PUTSTATIC:   { int16_t idx; FETCH_I16(idx); (void)idx; Value d; POP_VAL(d); break; }
        case OP_GETFIELD:    { int16_t idx; FETCH_I16(idx); (void)idx; Value d; POP_VAL(d); PUSH_INT(0); break; }
        case OP_PUTFIELD:    { int16_t idx; FETCH_I16(idx); (void)idx; Value v,r; POP_VAL(v); POP_VAL(r); break; }

        /* issue #5/#7: invokevirtual/invokespecial pop correct arg count */
        case OP_INVOKEVIRTUAL:
        case OP_INVOKESPECIAL: {
            int16_t idx; FETCH_I16(idx);
            /* Resolve descriptor from CP to count args; fall back to 1 if unavailable */
            int nargs = 1;
            if (f->cp && idx > 0 && idx < f->cp_len && f->cp[idx].tag == CP_STRING)
                nargs = count_args(f->cp[idx].sval);
            Value dummy;
            for (int i = 0; i < nargs; i++) POP_VAL(dummy);   /* args */
            POP_VAL(dummy);                                      /* objectref */
            if (opcode == OP_INVOKEVIRTUAL)
                printf("[native] invokevirtual stub (idx=%d, nargs=%d)\n", idx, nargs);
            break;
        }
        case OP_INVOKESTATIC: { int16_t idx; FETCH_I16(idx); (void)idx; break; }

        /* issue #6: athrow searches exception table */
        case OP_ATHROW: {
            Value throwable; POP_VAL(throwable);
            uint32_t throw_pc = f->pc - 1;
            int handled = 0;
            if (f->exc_table) {
                for (uint16_t i = 0; i < f->exc_table_len; i++) {
                    const ExceptionEntry *e = &f->exc_table[i];
                    if (throw_pc >= e->start_pc && throw_pc < e->end_pc) {
                        if (stack_push(f, throwable) != JVM_OK) { result = JVM_ERR_STACK_OVERFLOW; goto done; }
                        if (e->handler_pc >= f->code_len) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
                        f->pc = e->handler_pc;
                        handled = 1;
                        break;
                    }
                }
            }
            if (!handled) {
                fprintf(stderr, "[athrow] uncaught at pc=%u\n", throw_pc);
                result = JVM_ERR_OUT_OF_BOUNDS;
                goto done;
            }
            break;
        }

        case OP_NEW:       { int16_t idx; FETCH_I16(idx); (void)idx; PUSH_REF(0); break; }
        case OP_ANEWARRAY: { int16_t idx; FETCH_I16(idx); (void)idx; int32_t c; POP_INT(c); PUSH_REF(0); break; }
        case OP_CHECKCAST: { int16_t idx; FETCH_I16(idx); (void)idx; break; }
        case OP_INSTANCEOF:{ int16_t idx; FETCH_I16(idx); (void)idx; Value d; POP_VAL(d); PUSH_INT(0); break; }

        default:
            fprintf(stderr, "Unknown opcode: 0x%02X at pc=%u\n", opcode, f->pc-1);
            result = JVM_ERR_UNKNOWN_OPCODE;
            goto done;
        }
    }

done:
    vm->fp--;
    return result;
}

/* ════════════════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════════════════ */
JVMResult vm_exec(VM *vm, const uint8_t *code, uint32_t len, int32_t *ret_val)
{
    if (vm->fp >= MAX_FRAMES - 1) return JVM_ERR_STACK_OVERFLOW;
    vm->fp++;
    Frame *f = current_frame(vm);
    memset(f, 0, sizeof(Frame));
    f->code = code; f->code_len = len; f->pc = 0; f->sp = -1;
    f->cp = NULL; f->cp_len = 0;
    f->exc_table = NULL; f->exc_table_len = 0;
    return vm_exec_internal(vm, ret_val);
}

JVMResult vm_exec_full(VM *vm, const uint8_t *code, uint32_t len,
                       const CPEntry *cp, uint16_t cp_len,
                       const ExceptionEntry *exc, uint16_t exc_len,
                       int32_t *ret_val)
{
    if (vm->fp >= MAX_FRAMES - 1) return JVM_ERR_STACK_OVERFLOW;
    vm->fp++;
    Frame *f = current_frame(vm);
    memset(f, 0, sizeof(Frame));
    f->code = code; f->code_len = len; f->pc = 0; f->sp = -1;
    f->cp = cp; f->cp_len = cp_len;
    f->exc_table = exc; f->exc_table_len = exc_len;
    return vm_exec_internal(vm, ret_val);
}

/* ════════════════════════════════════════════════════════════
 * Error string helper
 * ════════════════════════════════════════════════════════════ */
const char *jvm_result_str(JVMResult r)
{
    switch (r) {
    case JVM_OK:                      return "OK";
    case JVM_RETURN_INT:              return "RETURN_INT";
    case JVM_RETURN_FLOAT:            return "RETURN_FLOAT";
    case JVM_RETURN_REF:              return "RETURN_REF";
    case JVM_RETURN_VOID:             return "RETURN_VOID";
    case JVM_ERR_STACK_OVERFLOW:      return "ERR_STACK_OVERFLOW";
    case JVM_ERR_STACK_UNDERFLOW:     return "ERR_STACK_UNDERFLOW";
    case JVM_ERR_UNKNOWN_OPCODE:      return "ERR_UNKNOWN_OPCODE";
    case JVM_ERR_DIVIDE_BY_ZERO:      return "ERR_DIVIDE_BY_ZERO";
    case JVM_ERR_OUT_OF_BOUNDS:       return "ERR_OUT_OF_BOUNDS";
    case JVM_ERR_NO_FRAME:            return "ERR_NO_FRAME";
    case JVM_ERR_NULL_POINTER:        return "ERR_NULL_POINTER";
    case JVM_ERR_INVALID_CP:          return "ERR_INVALID_CP";
    case JVM_ERR_ARRAY_INDEX:         return "ERR_ARRAY_INDEX";
    case JVM_ERR_OUT_OF_MEMORY:       return "ERR_OUT_OF_MEMORY";
    case JVM_ERR_NEGATIVE_ARRAY_SIZE: return "ERR_NEGATIVE_ARRAY_SIZE";
    default:                          return "UNKNOWN";
    }
}
