#include "jvm.h"
#include <math.h>

/* ── human-readable result names ── */
const char *jvm_result_str(JVMResult r) {
    switch (r) {
        case JVM_OK:                    return "OK";
        case JVM_RETURN_INT:            return "RETURN_INT";
        case JVM_RETURN_VOID:           return "RETURN_VOID";
        case JVM_RETURN_REF:            return "RETURN_REF";
        case JVM_RETURN_FLOAT:          return "RETURN_FLOAT";
        case JVM_ERR_STACK_OVERFLOW:    return "ERR_STACK_OVERFLOW";
        case JVM_ERR_STACK_UNDERFLOW:   return "ERR_STACK_UNDERFLOW";
        case JVM_ERR_UNKNOWN_OPCODE:    return "ERR_UNKNOWN_OPCODE";
        case JVM_ERR_DIVIDE_BY_ZERO:    return "ERR_DIVIDE_BY_ZERO";
        case JVM_ERR_OUT_OF_BOUNDS:     return "ERR_OUT_OF_BOUNDS";
        case JVM_ERR_NO_FRAME:          return "ERR_NO_FRAME";
        case JVM_ERR_TRUNCATED_BYTECODE:return "ERR_TRUNCATED_BYTECODE";
        case JVM_ERR_NULL_POINTER:      return "ERR_NULL_POINTER";
        case JVM_ERR_ARRAY_INDEX:       return "ERR_ARRAY_INDEX";
        case JVM_ERR_OUT_OF_MEMORY:     return "ERR_OUT_OF_MEMORY";
        case JVM_ERR_NEGATIVE_ARRAY_SIZE:return "ERR_NEGATIVE_ARRAY_SIZE";
        default:                        return "UNKNOWN";
    }
}

/* ── initialise VM ── */
void vm_init(VM *vm, int verbose) {
    memset(vm, 0, sizeof(VM));
    vm->fp      = -1;
    vm->verbose = verbose;
    vm->obj_count = 0;
}

/* ── helpers ── */

/* bounds-checked bytecode fetch */
static JVMResult fetch_u8(Frame *f, uint8_t *out) {
    if (f->pc >= f->code_len) return JVM_ERR_OUT_OF_BOUNDS;
    *out = f->code[f->pc++];
    return JVM_OK;
}

static JVMResult fetch_i16(Frame *f, int16_t *out) {
    if (f->pc + 2 > f->code_len) return JVM_ERR_OUT_OF_BOUNDS;
    *out = (int16_t)((f->code[f->pc] << 8) | f->code[f->pc + 1]);
    f->pc += 2;
    return JVM_OK;
}

/* stack push / pop with overflow/underflow checks */
static JVMResult push(Frame *f, Value v) {
    if (f->sp + 1 >= MAX_STACK) return JVM_ERR_STACK_OVERFLOW;
    f->stack[++f->sp] = v;
    return JVM_OK;
}

static JVMResult push_int(Frame *f, int32_t v) {
    Value val; val.type = VAL_INT; val.ival = v;
    return push(f, val);
}

static JVMResult push_float(Frame *f, float v) {
    Value val; val.type = VAL_FLOAT; val.fval = v;
    return push(f, val);
}

static JVMResult push_ref(Frame *f, int32_t ref) {
    Value val; val.type = VAL_REF; val.ival = ref;
    return push(f, val);
}

static JVMResult pop(Frame *f, Value *out) {
    if (f->sp < 0) return JVM_ERR_STACK_UNDERFLOW;
    *out = f->stack[f->sp--];
    return JVM_OK;
}

static JVMResult pop_int(Frame *f, int32_t *out) {
    Value v; JVMResult r = pop(f, &v); if (r) return r;
    *out = v.ival;
    return JVM_OK;
}

static JVMResult pop_float(Frame *f, float *out) {
    Value v; JVMResult r = pop(f, &v); if (r) return r;
    *out = v.fval;
    return JVM_OK;
}

/* ── simple object heap ── */
static int vm_alloc_array_int(VM *vm, int32_t count, int32_t *ref_out) {
    if (count < 0) return JVM_ERR_NEGATIVE_ARRAY_SIZE;
    if (vm->obj_count >= MAX_OBJECTS) return JVM_ERR_OUT_OF_MEMORY;
    int idx = vm->obj_count++;
    JObject *obj = &vm->heap[idx];
    memset(obj, 0, sizeof(JObject));
    obj->type   = OBJ_INT_ARRAY;
    obj->length = count;
    obj->marked = 0;
    if (count > 0) {
        obj->idata = (int32_t *)calloc((size_t)count, sizeof(int32_t));
        if (!obj->idata) return JVM_ERR_OUT_OF_MEMORY;
    }
    *ref_out = idx + 1;   /* 1-based ref; 0 = null */
    return JVM_OK;
}

/* ── the interpreter ── */
JVMResult vm_exec(VM *vm, const uint8_t *code, uint32_t len,
                  int32_t *ret_val)
{
    /* push an initial frame */
    if (vm->fp + 1 >= MAX_FRAMES) return JVM_ERR_STACK_OVERFLOW;
    Frame *f = &vm->frames[++vm->fp];
    memset(f, 0, sizeof(Frame));
    f->code     = code;
    f->code_len = len;
    f->pc       = 0;
    f->sp       = -1;

    JVMResult r;
    *ret_val = 0;

    for (;;) {
        uint8_t op;
        r = fetch_u8(f, &op);
        if (r) return r;

        if (vm->verbose)
            printf("  [pc=%3u] opcode=0x%02X  sp=%d\n",
                   f->pc - 1, op, f->sp);

        switch (op) {

        /* ── constants ── */
        case OP_NOP: break;

        case OP_ACONST_NULL: r = push_ref(f, 0);  if (r) return r; break;

        case OP_ICONST_M1: r = push_int(f, -1); if (r) return r; break;
        case OP_ICONST_0:  r = push_int(f,  0); if (r) return r; break;
        case OP_ICONST_1:  r = push_int(f,  1); if (r) return r; break;
        case OP_ICONST_2:  r = push_int(f,  2); if (r) return r; break;
        case OP_ICONST_3:  r = push_int(f,  3); if (r) return r; break;
        case OP_ICONST_4:  r = push_int(f,  4); if (r) return r; break;
        case OP_ICONST_5:  r = push_int(f,  5); if (r) return r; break;

        case OP_FCONST_0:  r = push_float(f, 0.0f); if (r) return r; break;
        case OP_FCONST_1:  r = push_float(f, 1.0f); if (r) return r; break;
        case OP_FCONST_2:  r = push_float(f, 2.0f); if (r) return r; break;

        case OP_BIPUSH: {
            uint8_t b; r = fetch_u8(f, &b); if (r) return r;
            r = push_int(f, (int32_t)(int8_t)b); if (r) return r;
            break;
        }
        case OP_SIPUSH: {
            int16_t s; r = fetch_i16(f, &s); if (r) return r;
            r = push_int(f, (int32_t)s); if (r) return r;
            break;
        }

        /* ── loads ── */
        case OP_ILOAD: {
            uint8_t idx; r = fetch_u8(f, &idx); if (r) return r;
            if (idx >= MAX_LOCALS) return JVM_ERR_OUT_OF_BOUNDS;
            r = push(f, f->locals[idx]); if (r) return r;
            break;
        }
        case OP_ILOAD_0: r = push(f, f->locals[0]); if (r) return r; break;
        case OP_ILOAD_1: r = push(f, f->locals[1]); if (r) return r; break;
        case OP_ILOAD_2: r = push(f, f->locals[2]); if (r) return r; break;
        case OP_ILOAD_3: r = push(f, f->locals[3]); if (r) return r; break;

        case OP_ALOAD: {
            uint8_t idx; r = fetch_u8(f, &idx); if (r) return r;
            if (idx >= MAX_LOCALS) return JVM_ERR_OUT_OF_BOUNDS;
            r = push(f, f->locals[idx]); if (r) return r;
            break;
        }
        case OP_ALOAD_0: r = push(f, f->locals[0]); if (r) return r; break;
        case OP_ALOAD_1: r = push(f, f->locals[1]); if (r) return r; break;
        case OP_ALOAD_2: r = push(f, f->locals[2]); if (r) return r; break;
        case OP_ALOAD_3: r = push(f, f->locals[3]); if (r) return r; break;

        /* ── stores ── */
        case OP_ISTORE: {
            uint8_t idx; r = fetch_u8(f, &idx); if (r) return r;
            if (idx >= MAX_LOCALS) return JVM_ERR_OUT_OF_BOUNDS;
            Value v; r = pop(f, &v); if (r) return r;
            f->locals[idx] = v;
            break;
        }
        case OP_ISTORE_0: { Value v; r=pop(f,&v); if(r) return r; f->locals[0]=v; break; }
        case OP_ISTORE_1: { Value v; r=pop(f,&v); if(r) return r; f->locals[1]=v; break; }
        case OP_ISTORE_2: { Value v; r=pop(f,&v); if(r) return r; f->locals[2]=v; break; }
        case OP_ISTORE_3: { Value v; r=pop(f,&v); if(r) return r; f->locals[3]=v; break; }

        case OP_ASTORE: {
            uint8_t idx; r = fetch_u8(f, &idx); if (r) return r;
            if (idx >= MAX_LOCALS) return JVM_ERR_OUT_OF_BOUNDS;
            Value v; r = pop(f, &v); if (r) return r;
            f->locals[idx] = v;
            break;
        }
        case OP_ASTORE_0: { Value v; r=pop(f,&v); if(r) return r; f->locals[0]=v; break; }
        case OP_ASTORE_1: { Value v; r=pop(f,&v); if(r) return r; f->locals[1]=v; break; }
        case OP_ASTORE_2: { Value v; r=pop(f,&v); if(r) return r; f->locals[2]=v; break; }
        case OP_ASTORE_3: { Value v; r=pop(f,&v); if(r) return r; f->locals[3]=v; break; }

        /* ── array load/store ── */
        case OP_IALOAD: {
            int32_t idx; r = pop_int(f, &idx); if (r) return r;
            int32_t ref; r = pop_int(f, &ref); if (r) return r;
            if (ref == 0) return JVM_ERR_NULL_POINTER;
            JObject *obj = &vm->heap[ref - 1];
            if (idx < 0 || idx >= obj->length) return JVM_ERR_ARRAY_INDEX;
            r = push_int(f, obj->idata[idx]); if (r) return r;
            break;
        }
        case OP_IASTORE: {
            int32_t val; r = pop_int(f, &val); if (r) return r;
            int32_t idx; r = pop_int(f, &idx); if (r) return r;
            int32_t ref; r = pop_int(f, &ref); if (r) return r;
            if (ref == 0) return JVM_ERR_NULL_POINTER;
            JObject *obj = &vm->heap[ref - 1];
            if (idx < 0 || idx >= obj->length) return JVM_ERR_ARRAY_INDEX;
            obj->idata[idx] = val;
            break;
        }

        /* ── stack ops ── */
        case OP_POP: {
            Value v; r = pop(f, &v); if (r) return r;
            break;
        }
        case OP_POP2: {
            Value v; r = pop(f, &v); if (r) return r;
            r = pop(f, &v); if (r) return r;
            break;
        }
        case OP_DUP: {
            if (f->sp < 0) return JVM_ERR_STACK_UNDERFLOW;
            r = push(f, f->stack[f->sp]); if (r) return r;
            break;
        }
        case OP_DUP_X1: {
            if (f->sp < 1) return JVM_ERR_STACK_UNDERFLOW;
            Value a = f->stack[f->sp];
            Value b = f->stack[f->sp - 1];
            f->stack[f->sp - 1] = a;
            f->stack[f->sp]     = b;
            r = push(f, a); if (r) return r;
            break;
        }
        case OP_SWAP: {
            if (f->sp < 1) return JVM_ERR_STACK_UNDERFLOW;
            Value tmp = f->stack[f->sp];
            f->stack[f->sp]     = f->stack[f->sp - 1];
            f->stack[f->sp - 1] = tmp;
            break;
        }

        /* ── int arithmetic ── */
        case OP_IADD: {
            int32_t b, a; r=pop_int(f,&b); if(r) return r;
            r=pop_int(f,&a); if(r) return r;
            r=push_int(f, a+b); if(r) return r; break;
        }
        case OP_ISUB: {
            int32_t b, a; r=pop_int(f,&b); if(r) return r;
            r=pop_int(f,&a); if(r) return r;
            r=push_int(f, a-b); if(r) return r; break;
        }
        case OP_IMUL: {
            int32_t b, a; r=pop_int(f,&b); if(r) return r;
            r=pop_int(f,&a); if(r) return r;
            r=push_int(f, a*b); if(r) return r; break;
        }
        case OP_IDIV: {
            int32_t b, a; r=pop_int(f,&b); if(r) return r;
            if (b == 0) return JVM_ERR_DIVIDE_BY_ZERO;
            r=pop_int(f,&a); if(r) return r;
            r=push_int(f, a/b); if(r) return r; break;
        }
        case OP_IREM: {
            int32_t b, a; r=pop_int(f,&b); if(r) return r;
            if (b == 0) return JVM_ERR_DIVIDE_BY_ZERO;
            r=pop_int(f,&a); if(r) return r;
            r=push_int(f, a%b); if(r) return r; break;
        }
        case OP_INEG: {
            int32_t a; r=pop_int(f,&a); if(r) return r;
            r=push_int(f, -a); if(r) return r; break;
        }

        /* ── bitwise / shift ── */
        case OP_ISHL: {
            int32_t b, a; r=pop_int(f,&b); if(r) return r;
            r=pop_int(f,&a); if(r) return r;
            r=push_int(f, a << (b & 0x1f)); if(r) return r; break;
        }
        case OP_ISHR: {
            int32_t b, a; r=pop_int(f,&b); if(r) return r;
            r=pop_int(f,&a); if(r) return r;
            r=push_int(f, a >> (b & 0x1f)); if(r) return r; break;
        }
        case OP_IUSHR: {
            int32_t b, a; r=pop_int(f,&b); if(r) return r;
            r=pop_int(f,&a); if(r) return r;
            r=push_int(f, (int32_t)((uint32_t)a >> (b & 0x1f))); if(r) return r; break;
        }
        case OP_IAND: {
            int32_t b, a; r=pop_int(f,&b); if(r) return r;
            r=pop_int(f,&a); if(r) return r;
            r=push_int(f, a&b); if(r) return r; break;
        }
        case OP_IOR: {
            int32_t b, a; r=pop_int(f,&b); if(r) return r;
            r=pop_int(f,&a); if(r) return r;
            r=push_int(f, a|b); if(r) return r; break;
        }
        case OP_IXOR: {
            int32_t b, a; r=pop_int(f,&b); if(r) return r;
            r=pop_int(f,&a); if(r) return r;
            r=push_int(f, a^b); if(r) return r; break;
        }

        /* ── float arithmetic ── */
        case OP_FADD: {
            float b, a; r=pop_float(f,&b); if(r) return r;
            r=pop_float(f,&a); if(r) return r;
            r=push_float(f, a+b); if(r) return r; break;
        }
        case OP_FSUB: {
            float b, a; r=pop_float(f,&b); if(r) return r;
            r=pop_float(f,&a); if(r) return r;
            r=push_float(f, a-b); if(r) return r; break;
        }
        case OP_FMUL: {
            float b, a; r=pop_float(f,&b); if(r) return r;
            r=pop_float(f,&a); if(r) return r;
            r=push_float(f, a*b); if(r) return r; break;
        }
        case OP_FDIV: {
            float b, a; r=pop_float(f,&b); if(r) return r;
            r=pop_float(f,&a); if(r) return r;
            r=push_float(f, a/b); if(r) return r; break;
        }
        case OP_FREM: {
            float b, a; r=pop_float(f,&b); if(r) return r;
            r=pop_float(f,&a); if(r) return r;
            r=push_float(f, fmodf(a,b)); if(r) return r; break;
        }
        case OP_FNEG: {
            float a; r=pop_float(f,&a); if(r) return r;
            r=push_float(f, -a); if(r) return r; break;
        }

        /* ── type conversions ── */
        case OP_I2F: {
            int32_t a; r=pop_int(f,&a); if(r) return r;
            r=push_float(f, (float)a); if(r) return r; break;
        }
        case OP_F2I: {
            float a; r=pop_float(f,&a); if(r) return r;
            r=push_int(f, (int32_t)a); if(r) return r; break;
        }
        case OP_I2B: {
            int32_t a; r=pop_int(f,&a); if(r) return r;
            r=push_int(f, (int32_t)(int8_t)a); if(r) return r; break;
        }
        case OP_I2C: {
            int32_t a; r=pop_int(f,&a); if(r) return r;
            r=push_int(f, a & 0xFFFF); if(r) return r; break;
        }
        case OP_I2S: {
            int32_t a; r=pop_int(f,&a); if(r) return r;
            r=push_int(f, (int32_t)(int16_t)a); if(r) return r; break;
        }

        /* ── iinc ── */
        case OP_IINC: {
            uint8_t idx; r = fetch_u8(f, &idx); if (r) return r;
            uint8_t cv;  r = fetch_u8(f, &cv);  if (r) return r;
            if (idx >= MAX_LOCALS) return JVM_ERR_OUT_OF_BOUNDS;
            f->locals[idx].ival += (int32_t)(int8_t)cv;
            break;
        }

        /* ── branches (single operand) ── */
        case OP_IFEQ: case OP_IFNE: case OP_IFLT:
        case OP_IFGE: case OP_IFGT: case OP_IFLE: {
            uint32_t branch_pc = f->pc - 1;
            int16_t offset; r = fetch_i16(f, &offset); if (r) return r;
            int32_t val; r = pop_int(f, &val); if (r) return r;
            int take = 0;
            switch (op) {
                case OP_IFEQ: take = (val == 0); break;
                case OP_IFNE: take = (val != 0); break;
                case OP_IFLT: take = (val <  0); break;
                case OP_IFGE: take = (val >= 0); break;
                case OP_IFGT: take = (val >  0); break;
                case OP_IFLE: take = (val <= 0); break;
            }
            if (take) {
                int32_t target = (int32_t)branch_pc + offset;
                if (target < 0 || (uint32_t)target >= f->code_len)
                    return JVM_ERR_OUT_OF_BOUNDS;
                f->pc = (uint32_t)target;
            }
            break;
        }

        /* ── branches (two int operands) ── */
        case OP_IF_ICMPEQ: case OP_IF_ICMPNE: case OP_IF_ICMPLT:
        case OP_IF_ICMPGE: case OP_IF_ICMPGT: case OP_IF_ICMPLE: {
            uint32_t branch_pc = f->pc - 1;
            int16_t offset; r = fetch_i16(f, &offset); if (r) return r;
            int32_t b, a; r=pop_int(f,&b); if(r) return r;
            r=pop_int(f,&a); if(r) return r;
            int take = 0;
            switch (op) {
                case OP_IF_ICMPEQ: take = (a == b); break;
                case OP_IF_ICMPNE: take = (a != b); break;
                case OP_IF_ICMPLT: take = (a <  b); break;
                case OP_IF_ICMPGE: take = (a >= b); break;
                case OP_IF_ICMPGT: take = (a >  b); break;
                case OP_IF_ICMPLE: take = (a <= b); break;
            }
            if (take) {
                int32_t target = (int32_t)branch_pc + offset;
                if (target < 0 || (uint32_t)target >= f->code_len)
                    return JVM_ERR_OUT_OF_BOUNDS;
                f->pc = (uint32_t)target;
            }
            break;
        }

        /* ── null/ref branches ── */
        case OP_IFNULL: case OP_IFNONNULL: {
            uint32_t branch_pc = f->pc - 1;
            int16_t offset; r = fetch_i16(f, &offset); if (r) return r;
            Value v; r = pop(f, &v); if (r) return r;
            int take = (op == OP_IFNULL) ? (v.ival == 0) : (v.ival != 0);
            if (take) {
                int32_t target = (int32_t)branch_pc + offset;
                if (target < 0 || (uint32_t)target >= f->code_len)
                    return JVM_ERR_OUT_OF_BOUNDS;
                f->pc = (uint32_t)target;
            }
            break;
        }

        /* ── goto ── */
        case OP_GOTO: {
            uint32_t branch_pc = f->pc - 1;
            int16_t offset; r = fetch_i16(f, &offset); if (r) return r;
            int32_t target = (int32_t)branch_pc + offset;
            if (target < 0 || (uint32_t)target >= f->code_len)
                return JVM_ERR_OUT_OF_BOUNDS;
            f->pc = (uint32_t)target;
            break;
        }

        /* ── returns ── */
        case OP_IRETURN: {
            int32_t v; r = pop_int(f, &v); if (r) return r;
            *ret_val = v;
            vm->fp--;
            return JVM_RETURN_INT;
        }
        case OP_FRETURN: {
            float v; r = pop_float(f, &v); if (r) return r;
            *ret_val = (int32_t)v;
            vm->fp--;
            return JVM_RETURN_FLOAT;
        }
        case OP_ARETURN: {
            Value v; r = pop(f, &v); if (r) return r;
            *ret_val = v.ival;
            vm->fp--;
            return JVM_RETURN_REF;
        }
        case OP_RETURN: {
            vm->fp--;
            return JVM_RETURN_VOID;
        }

        /* ── newarray ── */
        case OP_NEWARRAY: {
            uint8_t atype; r = fetch_u8(f, &atype); if (r) return r;
            int32_t count; r = pop_int(f, &count); if (r) return r;
            int32_t ref;
            JVMResult ar = vm_alloc_array_int(vm, count, &ref);
            if (ar) return ar;
            r = push_ref(f, ref); if (r) return r;
            break;
        }

        /* ── arraylength ── */
        case OP_ARRAYLENGTH: {
            int32_t ref; r = pop_int(f, &ref); if (r) return r;
            if (ref == 0) return JVM_ERR_NULL_POINTER;
            JObject *obj = &vm->heap[ref - 1];
            r = push_int(f, obj->length); if (r) return r;
            break;
        }

        /* ── stubs ── */
        case OP_GETSTATIC: { r = fetch_i16(f, &(int16_t){0}); if(r) return r; break; }
        case OP_INVOKEVIRTUAL:
        case OP_INVOKESTATIC: { r = fetch_i16(f, &(int16_t){0}); if(r) return r; break; }
        case OP_NEW: { r = fetch_i16(f, &(int16_t){0}); if(r) return r;
                       r = push_ref(f, 0); if(r) return r; break; }

        default:
            fprintf(stderr, "[vm] unknown opcode 0x%02X at pc=%u\n",
                    op, f->pc - 1);
            return JVM_ERR_UNKNOWN_OPCODE;
        }
    }
}
