#include "jvm.h"

/* ════════════════════════════════════════════════════════════
 *  Helpers
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
    Value v = { VAL_INT, n };
    return stack_push(f, v);
}

/* Convenience: pop an int */
static JVMResult pop_int(Frame *f, int32_t *n)
{
    Value v;
    JVMResult r = stack_pop(f, &v);
    if (r != JVM_OK) return r;
    *n = v.ival;
    return JVM_OK;
}

/* Read next byte from bytecode stream — returns JVM_ERR_OUT_OF_BOUNDS on overrun */
static JVMResult fetch_u8(Frame *f, uint8_t *out)
{
    if (f->pc >= f->code_len) return JVM_ERR_OUT_OF_BOUNDS;
    *out = f->code[f->pc++];
    return JVM_OK;
}

/* Read next signed 16-bit big-endian from bytecode stream */
static JVMResult fetch_i16(Frame *f, int16_t *out)
{
    uint8_t hi, lo;
    JVMResult r;
    r = fetch_u8(f, &hi); if (r != JVM_OK) return r;
    r = fetch_u8(f, &lo); if (r != JVM_OK) return r;
    *out = (int16_t)((hi << 8) | lo);
    return JVM_OK;
}

/* ════════════════════════════════════════════════════════════
 *  Convenience macros for safe fetch / pop inside the loop
 * ════════════════════════════════════════════════════════════ */
#define FETCH_U8(var) \
    do { JVMResult _r = fetch_u8(f, &(var)); if (_r != JVM_OK) { result = _r; goto done; } } while(0)

#define FETCH_I16(var) \
    do { JVMResult _r = fetch_i16(f, &(var)); if (_r != JVM_OK) { result = _r; goto done; } } while(0)

#define POP_INT_OR_FAIL(var) \
    do { JVMResult _r = pop_int(f, &(var)); if (_r != JVM_OK) { result = _r; goto done; } } while(0)



void vm_init(VM *vm, int verbose)
{
    memset(vm, 0, sizeof(VM));
    vm->fp      = -1;
    vm->verbose = verbose;
}

/* ════════════════════════════════════════════════════════════
 *  Main interpreter loop
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

        uint8_t opcode; FETCH_U8(opcode);

        if (vm->verbose)
            printf("  [pc=%3u] opcode=0x%02X  sp=%d\n",
                   f->pc - 1, opcode, f->sp);

        switch (opcode) {

        /* ── no-op ─────────────────────────────────────── */
        case OP_NOP:
            break;

        /* ── integer constants ─────────────────────────── */
        case OP_ICONST_M1: push_int(f, -1); break;
        case OP_ICONST_0:  push_int(f,  0); break;
        case OP_ICONST_1:  push_int(f,  1); break;
        case OP_ICONST_2:  push_int(f,  2); break;
        case OP_ICONST_3:  push_int(f,  3); break;
        case OP_ICONST_4:  push_int(f,  4); break;
        case OP_ICONST_5:  push_int(f,  5); break;

        /* ── push small literal ────────────────────────── */
        case OP_BIPUSH: {
            uint8_t raw; FETCH_U8(raw);
            push_int(f, (int32_t)(int8_t)raw);
            break;
        }
        case OP_SIPUSH: {
            int16_t val; FETCH_I16(val);
            push_int(f, (int32_t)val);
            break;
        }

        /* ── local variable loads ──────────────────────── */
        case OP_ILOAD: {
            uint8_t idx; FETCH_U8(idx);
            if (idx >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            push_int(f, f->locals[idx].ival);
            break;
        }
        case OP_ILOAD_0: push_int(f, f->locals[0].ival); break;
        case OP_ILOAD_1: push_int(f, f->locals[1].ival); break;
        case OP_ILOAD_2: push_int(f, f->locals[2].ival); break;
        case OP_ILOAD_3: push_int(f, f->locals[3].ival); break;

        /* ── local variable stores ─────────────────────── */
        case OP_ISTORE: {
            uint8_t idx; FETCH_U8(idx);
            if (idx >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            int32_t v; POP_INT_OR_FAIL(v);
            f->locals[idx].ival = v;
            f->locals[idx].type = VAL_INT;
            break;
        }
        case OP_ISTORE_0: { int32_t v; POP_INT_OR_FAIL(v); f->locals[0].ival=v; f->locals[0].type=VAL_INT; break; }
        case OP_ISTORE_1: { int32_t v; POP_INT_OR_FAIL(v); f->locals[1].ival=v; f->locals[1].type=VAL_INT; break; }
        case OP_ISTORE_2: { int32_t v; POP_INT_OR_FAIL(v); f->locals[2].ival=v; f->locals[2].type=VAL_INT; break; }
        case OP_ISTORE_3: { int32_t v; POP_INT_OR_FAIL(v); f->locals[3].ival=v; f->locals[3].type=VAL_INT; break; }

        /* ── arithmetic ────────────────────────────────── */
        case OP_IADD: {
            int32_t b, a;
            POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a);
            push_int(f, a + b);
            break;
        }
        case OP_ISUB: {
            int32_t b, a;
            POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a);
            push_int(f, a - b);
            break;
        }
        case OP_IMUL: {
            int32_t b, a;
            POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a);
            push_int(f, a * b);
            break;
        }
        case OP_IDIV: {
            int32_t b, a;
            POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a);
            if (b == 0) { result = JVM_ERR_DIVIDE_BY_ZERO; goto done; }
            push_int(f, a / b);
            break;
        }
        case OP_IREM: {
            int32_t b, a;
            POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a);
            if (b == 0) { result = JVM_ERR_DIVIDE_BY_ZERO; goto done; }
            push_int(f, a % b);
            break;
        }
        case OP_INEG: {
            int32_t a; POP_INT_OR_FAIL(a);
            push_int(f, -a);
            break;
        }

        /* ── iinc (increment local variable by constant) ─ */
        case OP_IINC: {
            uint8_t idx; FETCH_U8(idx);
            uint8_t raw; FETCH_U8(raw);
            int8_t cnst = (int8_t)raw;
            if (idx >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            f->locals[idx].ival += cnst;
            break;
        }

        /* ── bitwise / shift ───────────────────────────── */
        case OP_ISHL:  { int32_t b,a; POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a); push_int(f, a << (b & 0x1F)); break; }
        case OP_ISHR:  { int32_t b,a; POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a); push_int(f, a >> (b & 0x1F)); break; }
        case OP_IUSHR: { int32_t b,a; POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a); push_int(f, (int32_t)((uint32_t)a >> (b & 0x1F))); break; }
        case OP_IAND:  { int32_t b,a; POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a); push_int(f, a & b); break; }
        case OP_IOR:   { int32_t b,a; POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a); push_int(f, a | b); break; }
        case OP_IXOR:  { int32_t b,a; POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a); push_int(f, a ^ b); break; }

        /* ── stack manipulation ────────────────────────── */
        case OP_DUP: {
            Value top;
            if (stack_peek(f, &top) != JVM_OK) { result = JVM_ERR_STACK_UNDERFLOW; goto done; }
            stack_push(f, top);
            break;
        }
        case OP_POP: {
            Value dummy;
            if (stack_pop(f, &dummy) != JVM_OK) { result = JVM_ERR_STACK_UNDERFLOW; goto done; }
            break;
        }
        case OP_SWAP: {
            Value a, b;
            if (stack_pop(f, &a) != JVM_OK || stack_pop(f, &b) != JVM_OK)
                { result = JVM_ERR_STACK_UNDERFLOW; goto done; }
            stack_push(f, a); stack_push(f, b);
            break;
        }

        /* ── type conversion ───────────────────────────── */
        case OP_I2C: {
            int32_t a; POP_INT_OR_FAIL(a);
            push_int(f, (int32_t)(uint16_t)a);
            break;
        }

        /* ── branch: single-value comparisons ─────────── */
        case OP_IFEQ: case OP_IFNE: case OP_IFLT:
        case OP_IFGE: case OP_IFGT: case OP_IFLE: {
            int16_t offset; FETCH_I16(offset);
            int32_t v; POP_INT_OR_FAIL(v);
            int take = 0;
            if      (opcode == OP_IFEQ && v == 0) take = 1;
            else if (opcode == OP_IFNE && v != 0) take = 1;
            else if (opcode == OP_IFLT && v <  0) take = 1;
            else if (opcode == OP_IFGE && v >= 0) take = 1;
            else if (opcode == OP_IFGT && v >  0) take = 1;
            else if (opcode == OP_IFLE && v <= 0) take = 1;
            if (take) {
                int32_t target = (int32_t)(f->pc - 3) + offset;
                if (target < 0 || (uint32_t)target >= f->code_len)
                    { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
                f->pc = (uint32_t)target;
            }
            break;
        }

        /* ── branch: two-value integer comparisons ─────── */
        case OP_IF_ICMPEQ: case OP_IF_ICMPNE: case OP_IF_ICMPLT:
        case OP_IF_ICMPGE: case OP_IF_ICMPGT: case OP_IF_ICMPLE: {
            int16_t offset; FETCH_I16(offset);
            int32_t b, a;
            POP_INT_OR_FAIL(b); POP_INT_OR_FAIL(a);
            int take = 0;
            if      (opcode == OP_IF_ICMPEQ && a == b) take = 1;
            else if (opcode == OP_IF_ICMPNE && a != b) take = 1;
            else if (opcode == OP_IF_ICMPLT && a <  b) take = 1;
            else if (opcode == OP_IF_ICMPGE && a >= b) take = 1;
            else if (opcode == OP_IF_ICMPGT && a >  b) take = 1;
            else if (opcode == OP_IF_ICMPLE && a <= b) take = 1;
            if (take) {
                int32_t target = (int32_t)(f->pc - 3) + offset;
                if (target < 0 || (uint32_t)target >= f->code_len)
                    { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
                f->pc = (uint32_t)target;
            }
            break;
        }

        /* ── unconditional jump ────────────────────────── */
        case OP_GOTO: {
            int16_t offset; FETCH_I16(offset);
            int32_t target = (int32_t)(f->pc - 3) + offset;
            if (target < 0 || (uint32_t)target >= f->code_len)
                { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            f->pc = (uint32_t)target;
            break;
        }

        /* ── return ────────────────────────────────────── */
        case OP_IRETURN: {
            int32_t v; POP_INT_OR_FAIL(v);
            if (ret_val) *ret_val = v;
            result = JVM_RETURN_INT;
            goto done;
        }
        case OP_RETURN: {
            result = JVM_RETURN_VOID;
            goto done;
        }

        /* ── stubs for object / static calls ───────────── *
         * In a full KVM these would resolve the constant   *
         * pool and dispatch to native or interpreted code. *
         * Here we print a diagnostic and consume the       *
         * 2-byte index operand.                            */
        case OP_GETSTATIC: {
            int16_t raw; FETCH_I16(raw); uint16_t idx = (uint16_t)raw;
            (void)idx;
            Value ref = { VAL_REF, 0 };
            stack_push(f, ref);
            break;
        }
        case OP_INVOKEVIRTUAL: {
            int16_t raw; FETCH_I16(raw); uint16_t idx = (uint16_t)raw;
            (void)idx;
            Value arg, ref;
            stack_pop(f, &arg);
            stack_pop(f, &ref);
            printf("[native] System.out.println(%d)\n", arg.ival);
            break;
        }
        case OP_INVOKESTATIC: {
            int16_t raw; FETCH_I16(raw); uint16_t idx = (uint16_t)raw;
            (void)idx;
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
    vm->fp--;   /* pop the frame */
    return result;
}

/* ════════════════════════════════════════════════════════════
 *  Error string helper
 * ════════════════════════════════════════════════════════════ */

const char *jvm_result_str(JVMResult r)
{
    switch (r) {
    case JVM_OK:                  return "OK";
    case JVM_RETURN_INT:          return "RETURN_INT";
    case JVM_RETURN_VOID:         return "RETURN_VOID";
    case JVM_ERR_STACK_OVERFLOW:  return "ERR_STACK_OVERFLOW";
    case JVM_ERR_STACK_UNDERFLOW: return "ERR_STACK_UNDERFLOW";
    case JVM_ERR_UNKNOWN_OPCODE:  return "ERR_UNKNOWN_OPCODE";
    case JVM_ERR_DIVIDE_BY_ZERO:  return "ERR_DIVIDE_BY_ZERO";
    case JVM_ERR_OUT_OF_BOUNDS:   return "ERR_OUT_OF_BOUNDS";
    case JVM_ERR_NO_FRAME:        return "ERR_NO_FRAME";
    default:                      return "UNKNOWN";
    }
}
