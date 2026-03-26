#include "jvm.h"

/* ════════════════════════════════════════════════════════════
 * Helpers
 * ════════════════════════════════════════════════════════════ */

static Frame *current_frame(VM *vm)
{
    if (vm->fp < 0) return NULL;
    return &vm->frames[vm->fp];
}

/* Push a value onto the operand stack */
static JVMResult stack_push(Frame *f, Value v)
{
    if (f->sp >= MAX_STACK - 1) return JVM_ERR_STACK_OVERFLOW;
    f->stack[++f->sp] = v;
    return JVM_OK;
}

/* Pop a value from the operand stack */
static JVMResult stack_pop(Frame *f, Value *out)
{
    if (f->sp < 0) return JVM_ERR_STACK_UNDERFLOW;
    *out = f->stack[f->sp--];
    return JVM_OK;
}

/* Peek without popping */
static JVMResult stack_peek(Frame *f, Value *out)
{
    if (f->sp < 0) return JVM_ERR_STACK_UNDERFLOW;
    *out = f->stack[f->sp];
    return JVM_OK;
}

/* Convenience: push an int */
static JVMResult push_int(Frame *f, int32_t n)
{
    Value v; v.type = VAL_INT; v.ival = n;
    return stack_push(f, v);
}

/* Convenience: pop an int — returns error to caller */
static JVMResult pop_int(Frame *f, int32_t *n)
{
    Value v;
    JVMResult r = stack_pop(f, &v);
    if (r != JVM_OK) return r;
    *n = v.ival;
    return JVM_OK;
}

/* Convenience: push a float */
static JVMResult push_float(Frame *f, float x)
{
    Value v; v.type = VAL_FLOAT; v.fval = x;
    return stack_push(f, v);
}

/* Convenience: pop a float (used by POP_FLOAT macro in float opcodes) */
static JVMResult pop_float(Frame *f, float *x) __attribute__((unused));
static JVMResult pop_float(Frame *f, float *x)
{
    Value v;
    JVMResult r = stack_pop(f, &v);
    if (r != JVM_OK) return r;
    *x = v.fval;
    return JVM_OK;
}

/* Read next byte from bytecode stream — FIX #3: bounds-checked */
static JVMResult fetch_u8(Frame *f, uint8_t *out)
{
    if (f->pc >= f->code_len) return JVM_ERR_TRUNCATED_BYTECODE;
    *out = f->code[f->pc++];
    return JVM_OK;
}

/* Read next signed 16-bit big-endian — FIX #3: propagates bounds error */
static JVMResult fetch_i16(Frame *f, int16_t *out)
{
    uint8_t hi, lo;
    JVMResult r;
    r = fetch_u8(f, &hi); if (r != JVM_OK) return r;
    r = fetch_u8(f, &lo); if (r != JVM_OK) return r;
    *out = (int16_t)((hi << 8) | lo);
    return JVM_OK;
}

/* Helper macro: fetch u8 operand or bail */
#define FETCH_U8(dst) \
    do { uint8_t _b; JVMResult _r = fetch_u8(f, &_b); \
         if (_r != JVM_OK) { result = _r; goto done; } (dst) = _b; } while(0)

/* Helper macro: fetch i16 operand or bail */
#define FETCH_I16(dst) \
    do { int16_t _s; JVMResult _r = fetch_i16(f, &_s); \
         if (_r != JVM_OK) { result = _r; goto done; } (dst) = _s; } while(0)

/* Helper macro: pop int or bail — FIX #4 */
#define POP_INT(dst) \
    do { JVMResult _r = pop_int(f, &(dst)); \
         if (_r != JVM_OK) { result = _r; goto done; } } while(0)

/* Helper macro: pop float or bail */
#define POP_FLOAT(dst) \
    do { JVMResult _r = pop_float(f, &(dst)); \
         if (_r != JVM_OK) { result = _r; goto done; } } while(0)

/* Helper macro: push int or bail */
#define PUSH_INT(val) \
    do { JVMResult _r = push_int(f, (val)); \
         if (_r != JVM_OK) { result = _r; goto done; } } while(0)

/* Helper macro: push float or bail */
#define PUSH_FLOAT(val) \
    do { JVMResult _r = push_float(f, (val)); \
         if (_r != JVM_OK) { result = _r; goto done; } } while(0)

/* FIX #5: validate branch target before assigning to pc */
#define BRANCH(offset_expr) \
    do { int32_t _target = (int32_t)(f->pc - 3) + (offset_expr); \
         if (_target < 0 || (uint32_t)_target >= f->code_len) \
             { result = JVM_ERR_OUT_OF_BOUNDS; goto done; } \
         f->pc = (uint32_t)_target; } while(0)

/* ════════════════════════════════════════════════════════════
 * VM init
 * ════════════════════════════════════════════════════════════ */

void vm_init(VM *vm, int verbose)
{
    memset(vm, 0, sizeof(VM));
    vm->fp = -1;
    vm->verbose = verbose;
}

/* ════════════════════════════════════════════════════════════
 * Main interpreter loop
 * ════════════════════════════════════════════════════════════ */

JVMResult vm_exec(VM *vm, const uint8_t *code, uint32_t len,
                  int32_t *ret_val)
{
    /* Push a new frame */
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
        uint8_t opcode;
        /* FIX #3: use checked fetch for the opcode itself */
        { JVMResult _r = fetch_u8(f, &opcode);
          if (_r != JVM_OK) { result = _r; goto done; } }

        if (vm->verbose)
            printf("  [pc=%3u] opcode=0x%02X  sp=%d\n",
                   f->pc - 1, opcode, f->sp);

        switch (opcode) {

        /* ── no-op ─────────────────────────────────────── */
        case OP_NOP: break;

        /* ── null reference constant ──────────────────── */
        case OP_ACONST_NULL: {
            Value v; v.type = VAL_REF; v.ival = 0;
            JVMResult r = stack_push(f, v);
            if (r != JVM_OK) { result = r; goto done; }
            break;
        }

        /* ── integer constants ─────────────────────────── */
        case OP_ICONST_M1: PUSH_INT(-1); break;
        case OP_ICONST_0:  PUSH_INT(0);  break;
        case OP_ICONST_1:  PUSH_INT(1);  break;
        case OP_ICONST_2:  PUSH_INT(2);  break;
        case OP_ICONST_3:  PUSH_INT(3);  break;
        case OP_ICONST_4:  PUSH_INT(4);  break;
        case OP_ICONST_5:  PUSH_INT(5);  break;

        /* ── float constants ───────────────────────────── */
        case OP_FCONST_0: PUSH_FLOAT(0.0f); break;
        case OP_FCONST_1: PUSH_FLOAT(1.0f); break;
        case OP_FCONST_2: PUSH_FLOAT(2.0f); break;

        /* ── push small literal ────────────────────────── */
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

        /* ── local variable loads ──────────────────────── */
        case OP_ILOAD: {
            uint8_t idx; FETCH_U8(idx);
            if (idx >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            PUSH_INT(f->locals[idx].ival);
            break;
        }
        case OP_ALOAD: {
            uint8_t idx; FETCH_U8(idx);
            if (idx >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            JVMResult r = stack_push(f, f->locals[idx]);
            if (r != JVM_OK) { result = r; goto done; }
            break;
        }
        case OP_ILOAD_0: PUSH_INT(f->locals[0].ival); break;
        case OP_ILOAD_1: PUSH_INT(f->locals[1].ival); break;
        case OP_ILOAD_2: PUSH_INT(f->locals[2].ival); break;
        case OP_ILOAD_3: PUSH_INT(f->locals[3].ival); break;

        case OP_ALOAD_0: { JVMResult r = stack_push(f, f->locals[0]); if (r != JVM_OK) { result = r; goto done; } break; }
        case OP_ALOAD_1: { JVMResult r = stack_push(f, f->locals[1]); if (r != JVM_OK) { result = r; goto done; } break; }
        case OP_ALOAD_2: { JVMResult r = stack_push(f, f->locals[2]); if (r != JVM_OK) { result = r; goto done; } break; }
        case OP_ALOAD_3: { JVMResult r = stack_push(f, f->locals[3]); if (r != JVM_OK) { result = r; goto done; } break; }

        /* ── array loads (stub: treat ref as int index into nowhere) ── */
        case OP_IALOAD: {
            int32_t idx; POP_INT(idx);
            int32_t ref; POP_INT(ref);
            (void)ref; (void)idx;
            /* stub: push 0 — real impl needs heap array support */
            PUSH_INT(0);
            break;
        }

        /* ── local variable stores ─────────────────────── */
        case OP_ISTORE: {
            uint8_t idx; FETCH_U8(idx);
            if (idx >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            int32_t v; POP_INT(v);
            f->locals[idx].ival = v;
            f->locals[idx].type = VAL_INT;
            break;
        }
        case OP_ASTORE: {
            uint8_t idx; FETCH_U8(idx);
            if (idx >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            Value v;
            JVMResult r = stack_pop(f, &v);
            if (r != JVM_OK) { result = r; goto done; }
            f->locals[idx] = v;
            break;
        }
        /* FIX #4 (minor): istore_N shortcuts now set .type */
        case OP_ISTORE_0: { int32_t v; POP_INT(v); f->locals[0].ival = v; f->locals[0].type = VAL_INT; break; }
        case OP_ISTORE_1: { int32_t v; POP_INT(v); f->locals[1].ival = v; f->locals[1].type = VAL_INT; break; }
        case OP_ISTORE_2: { int32_t v; POP_INT(v); f->locals[2].ival = v; f->locals[2].type = VAL_INT; break; }
        case OP_ISTORE_3: { int32_t v; POP_INT(v); f->locals[3].ival = v; f->locals[3].type = VAL_INT; break; }

        case OP_ASTORE_0: { Value v; JVMResult r = stack_pop(f,&v); if(r!=JVM_OK){result=r;goto done;} f->locals[0]=v; break; }
        case OP_ASTORE_1: { Value v; JVMResult r = stack_pop(f,&v); if(r!=JVM_OK){result=r;goto done;} f->locals[1]=v; break; }
        case OP_ASTORE_2: { Value v; JVMResult r = stack_pop(f,&v); if(r!=JVM_OK){result=r;goto done;} f->locals[2]=v; break; }
        case OP_ASTORE_3: { Value v; JVMResult r = stack_pop(f,&v); if(r!=JVM_OK){result=r;goto done;} f->locals[3]=v; break; }

        /* ── array stores (stub) ───────────────────────── */
        case OP_IASTORE: {
            int32_t val; POP_INT(val);
            int32_t idx; POP_INT(idx);
            int32_t ref; POP_INT(ref);
            (void)val; (void)idx; (void)ref;
            /* stub: real impl needs heap array support */
            break;
        }

        /* ── arithmetic ────────────────────────────────── */
        /* FIX #4: all arithmetic ops now propagate pop_int errors via POP_INT macro */
        case OP_IADD: { int32_t b, a; POP_INT(b); POP_INT(a); PUSH_INT(a + b); break; }
        case OP_ISUB: { int32_t b, a; POP_INT(b); POP_INT(a); PUSH_INT(a - b); break; }
        case OP_IMUL: { int32_t b, a; POP_INT(b); POP_INT(a); PUSH_INT(a * b); break; }
        case OP_IDIV: {
            int32_t b, a; POP_INT(b); POP_INT(a);
            if (b == 0) { result = JVM_ERR_DIVIDE_BY_ZERO; goto done; }
            PUSH_INT(a / b); break;
        }
        case OP_IREM: {
            int32_t b, a; POP_INT(b); POP_INT(a);
            if (b == 0) { result = JVM_ERR_DIVIDE_BY_ZERO; goto done; }
            PUSH_INT(a % b); break;
        }
        case OP_INEG: { int32_t a; POP_INT(a); PUSH_INT(-a); break; }

        /* ── iinc ─────────────────────────────────────── */
        case OP_IINC: {
            uint8_t idx; FETCH_U8(idx);
            uint8_t raw; FETCH_U8(raw);
            if (idx >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            f->locals[idx].ival += (int8_t)raw;
            break;
        }

        /* ── bitwise / shift ───────────────────────────── */
        case OP_ISHL:  { int32_t b,a; POP_INT(b); POP_INT(a); PUSH_INT(a << (b & 0x1F)); break; }
        case OP_ISHR:  { int32_t b,a; POP_INT(b); POP_INT(a); PUSH_INT(a >> (b & 0x1F)); break; }
        case OP_IUSHR: { int32_t b,a; POP_INT(b); POP_INT(a); PUSH_INT((int32_t)((uint32_t)a >> (b & 0x1F))); break; }
        case OP_IAND:  { int32_t b,a; POP_INT(b); POP_INT(a); PUSH_INT(a & b); break; }
        case OP_IOR:   { int32_t b,a; POP_INT(b); POP_INT(a); PUSH_INT(a | b); break; }
        case OP_IXOR:  { int32_t b,a; POP_INT(b); POP_INT(a); PUSH_INT(a ^ b); break; }

        /* ── stack manipulation ────────────────────────── */
        case OP_DUP: {
            Value top;
            JVMResult r = stack_peek(f, &top);
            if (r != JVM_OK) { result = r; goto done; }
            r = stack_push(f, top);
            if (r != JVM_OK) { result = r; goto done; }
            break;
        }
        /* FIX #8: DUP_X1 — duplicate top value and insert two slots down */
        case OP_DUP_X1: {
            Value v1, v2;
            JVMResult r;
            r = stack_pop(f, &v1); if (r != JVM_OK) { result = r; goto done; }
            r = stack_pop(f, &v2); if (r != JVM_OK) { result = r; goto done; }
            r = stack_push(f, v1); if (r != JVM_OK) { result = r; goto done; }
            r = stack_push(f, v2); if (r != JVM_OK) { result = r; goto done; }
            r = stack_push(f, v1); if (r != JVM_OK) { result = r; goto done; }
            break;
        }
        case OP_POP: {
            Value dummy;
            JVMResult r = stack_pop(f, &dummy);
            if (r != JVM_OK) { result = r; goto done; }
            break;
        }
        case OP_POP2: {
            Value dummy;
            JVMResult r;
            r = stack_pop(f, &dummy); if (r != JVM_OK) { result = r; goto done; }
            r = stack_pop(f, &dummy); if (r != JVM_OK) { result = r; goto done; }
            break;
        }
        /* FIX #8: SWAP now checks all four stack operation results */
        case OP_SWAP: {
            Value a, b;
            JVMResult r;
            r = stack_pop(f, &a); if (r != JVM_OK) { result = r; goto done; }
            r = stack_pop(f, &b); if (r != JVM_OK) { result = r; goto done; }
            r = stack_push(f, a); if (r != JVM_OK) { result = r; goto done; }
            r = stack_push(f, b); if (r != JVM_OK) { result = r; goto done; }
            break;
        }

        /* ── type conversion ───────────────────────────── */
        case OP_I2C: { int32_t a; POP_INT(a); PUSH_INT((int32_t)(uint16_t)a); break; }
        case OP_I2B: { int32_t a; POP_INT(a); PUSH_INT((int32_t)(int8_t)a);   break; }
        case OP_I2S: { int32_t a; POP_INT(a); PUSH_INT((int32_t)(int16_t)a);  break; }

        /* ── branch: single-value comparisons ─────────── */
        case OP_IFEQ: case OP_IFNE: case OP_IFLT:
        case OP_IFGE: case OP_IFGT: case OP_IFLE: {
            int16_t offset; FETCH_I16(offset);
            int32_t v; POP_INT(v);
            int take = 0;
            if      (opcode == OP_IFEQ && v == 0) take = 1;
            else if (opcode == OP_IFNE && v != 0) take = 1;
            else if (opcode == OP_IFLT && v <  0) take = 1;
            else if (opcode == OP_IFGE && v >= 0) take = 1;
            else if (opcode == OP_IFGT && v >  0) take = 1;
            else if (opcode == OP_IFLE && v <= 0) take = 1;
            /* FIX #5: validate branch target */
            if (take) BRANCH(offset);
            break;
        }

        /* ── branch: null checks ─────────────────────── */
        case OP_IFNULL:
        case OP_IFNONNULL: {
            int16_t offset; FETCH_I16(offset);
            Value v;
            JVMResult r = stack_pop(f, &v);
            if (r != JVM_OK) { result = r; goto done; }
            int take = (opcode == OP_IFNULL) ? (v.ival == 0) : (v.ival != 0);
            if (take) BRANCH(offset);
            break;
        }

        /* ── branch: two-value integer comparisons ─────── */
        case OP_IF_ICMPEQ: case OP_IF_ICMPNE: case OP_IF_ICMPLT:
        case OP_IF_ICMPGE: case OP_IF_ICMPGT: case OP_IF_ICMPLE: {
            int16_t offset; FETCH_I16(offset);
            int32_t b, a; POP_INT(b); POP_INT(a);
            int take = 0;
            if      (opcode == OP_IF_ICMPEQ && a == b) take = 1;
            else if (opcode == OP_IF_ICMPNE && a != b) take = 1;
            else if (opcode == OP_IF_ICMPLT && a <  b) take = 1;
            else if (opcode == OP_IF_ICMPGE && a >= b) take = 1;
            else if (opcode == OP_IF_ICMPGT && a >  b) take = 1;
            else if (opcode == OP_IF_ICMPLE && a <= b) take = 1;
            /* FIX #5: validate branch target */
            if (take) BRANCH(offset);
            break;
        }

        /* ── unconditional jump ────────────────────────── */
        case OP_GOTO: {
            int16_t offset; FETCH_I16(offset);
            /* FIX #5: validate branch target */
            BRANCH(offset);
            break;
        }

        /* ── return ────────────────────────────────────── */
        case OP_IRETURN: {
            int32_t v; POP_INT(v);
            if (ret_val) *ret_val = v;
            result = JVM_RETURN_INT;
            goto done;
        }
        case OP_ARETURN: {
            Value v;
            JVMResult r = stack_pop(f, &v);
            if (r != JVM_OK) { result = r; goto done; }
            if (ret_val) *ret_val = v.ival;
            result = JVM_RETURN_REF;
            goto done;
        }
        case OP_RETURN: {
            result = JVM_RETURN_VOID;
            goto done;
        }

        /* ── stubs for object / static calls ──────────── */
        case OP_GETSTATIC: {
            int16_t idx; FETCH_I16(idx); (void)idx;
            Value ref; ref.type = VAL_REF; ref.ival = 0;
            JVMResult r = stack_push(f, ref);
            if (r != JVM_OK) { result = r; goto done; }
            break;
        }
        case OP_INVOKEVIRTUAL: {
            int16_t idx; FETCH_I16(idx); (void)idx;
            Value arg, ref;
            JVMResult r;
            r = stack_pop(f, &arg); if (r != JVM_OK) { result = r; goto done; }
            r = stack_pop(f, &ref); if (r != JVM_OK) { result = r; goto done; }
            printf("[native] System.out.println(%d)\n", arg.ival);
            break;
        }
        case OP_INVOKESTATIC: {
            int16_t idx; FETCH_I16(idx); (void)idx;
            break;
        }
        case OP_NEW: {
            int16_t idx; FETCH_I16(idx); (void)idx;
            /* stub: push null ref — real impl needs heap */
            Value ref; ref.type = VAL_REF; ref.ival = 0;
            JVMResult r = stack_push(f, ref);
            if (r != JVM_OK) { result = r; goto done; }
            break;
        }
        case OP_NEWARRAY: {
            uint8_t atype; FETCH_U8(atype); (void)atype;
            int32_t count; POP_INT(count); (void)count;
            /* stub: push null ref — real impl needs heap */
            Value ref; ref.type = VAL_REF; ref.ival = 0;
            JVMResult r = stack_push(f, ref);
            if (r != JVM_OK) { result = r; goto done; }
            break;
        }
        case OP_ARRAYLENGTH: {
            Value ref;
            JVMResult r = stack_pop(f, &ref);
            if (r != JVM_OK) { result = r; goto done; }
            /* stub: push 0 — real impl needs heap */
            PUSH_INT(0);
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
    vm->fp--;  /* pop the frame */
    return result;
}

/* ════════════════════════════════════════════════════════════
 * Error string helper
 * ════════════════════════════════════════════════════════════ */

const char *jvm_result_str(JVMResult r)
{
    switch (r) {
    case JVM_OK:                     return "OK";
    case JVM_RETURN_INT:             return "RETURN_INT";
    case JVM_RETURN_VOID:            return "RETURN_VOID";
    case JVM_RETURN_REF:             return "RETURN_REF";
    case JVM_ERR_STACK_OVERFLOW:     return "ERR_STACK_OVERFLOW";
    case JVM_ERR_STACK_UNDERFLOW:    return "ERR_STACK_UNDERFLOW";
    case JVM_ERR_UNKNOWN_OPCODE:     return "ERR_UNKNOWN_OPCODE";
    case JVM_ERR_DIVIDE_BY_ZERO:     return "ERR_DIVIDE_BY_ZERO";
    case JVM_ERR_OUT_OF_BOUNDS:      return "ERR_OUT_OF_BOUNDS";
    case JVM_ERR_NO_FRAME:           return "ERR_NO_FRAME";
    case JVM_ERR_TRUNCATED_BYTECODE: return "ERR_TRUNCATED_BYTECODE";
    default:                         return "UNKNOWN";
    }
}
