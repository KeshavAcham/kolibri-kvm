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

/* Read next byte from bytecode stream */
static uint8_t fetch_u8(Frame *f)
{
    return f->code[f->pc++];
}

/* Read next signed 16-bit big-endian from bytecode stream */
static int16_t fetch_i16(Frame *f)
{
    uint8_t hi = fetch_u8(f);
    uint8_t lo = fetch_u8(f);
    return (int16_t)((hi << 8) | lo);
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

        uint8_t opcode = fetch_u8(f);

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
            int8_t val = (int8_t)fetch_u8(f);
            push_int(f, (int32_t)val);
            break;
        }
        case OP_SIPUSH: {
            int16_t val = fetch_i16(f);
            push_int(f, (int32_t)val);
            break;
        }

        /* ── local variable loads ──────────────────────── */
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

        /* ── local variable stores ─────────────────────── */
        case OP_ISTORE: {
            uint8_t idx = fetch_u8(f);
            if (idx >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            int32_t v; pop_int(f, &v);
            f->locals[idx].ival = v;
            f->locals[idx].type = VAL_INT;
            break;
        }
        case OP_ISTORE_0: { int32_t v; pop_int(f,&v); f->locals[0].ival=v; break; }
        case OP_ISTORE_1: { int32_t v; pop_int(f,&v); f->locals[1].ival=v; break; }
        case OP_ISTORE_2: { int32_t v; pop_int(f,&v); f->locals[2].ival=v; break; }
        case OP_ISTORE_3: { int32_t v; pop_int(f,&v); f->locals[3].ival=v; break; }

        /* ── arithmetic ────────────────────────────────── */
        case OP_IADD: {
            int32_t b, a;
            pop_int(f, &b); pop_int(f, &a);
            push_int(f, a + b);
            break;
        }
        case OP_ISUB: {
            int32_t b, a;
            pop_int(f, &b); pop_int(f, &a);
            push_int(f, a - b);
            break;
        }
        case OP_IMUL: {
            int32_t b, a;
            pop_int(f, &b); pop_int(f, &a);
            push_int(f, a * b);
            break;
        }
        case OP_IDIV: {
            int32_t b, a;
            pop_int(f, &b); pop_int(f, &a);
            if (b == 0) { result = JVM_ERR_DIVIDE_BY_ZERO; goto done; }
            push_int(f, a / b);
            break;
        }
        case OP_IREM: {
            int32_t b, a;
            pop_int(f, &b); pop_int(f, &a);
            if (b == 0) { result = JVM_ERR_DIVIDE_BY_ZERO; goto done; }
            push_int(f, a % b);
            break;
        }
        case OP_INEG: {
            int32_t a; pop_int(f, &a);
            push_int(f, -a);
            break;
        }

        /* ── iinc (increment local variable by constant) ─ */
        case OP_IINC: {
            uint8_t idx   = fetch_u8(f);
            int8_t  cnst  = (int8_t)fetch_u8(f);
            if (idx >= MAX_LOCALS) { result = JVM_ERR_OUT_OF_BOUNDS; goto done; }
            f->locals[idx].ival += cnst;
            break;
        }

        /* ── bitwise / shift ───────────────────────────── */
        case OP_ISHL:  { int32_t b,a; pop_int(f,&b); pop_int(f,&a); push_int(f, a << (b & 0x1F)); break; }
        case OP_ISHR:  { int32_t b,a; pop_int(f,&b); pop_int(f,&a); push_int(f, a >> (b & 0x1F)); break; }
        case OP_IUSHR: { int32_t b,a; pop_int(f,&b); pop_int(f,&a); push_int(f, (int32_t)((uint32_t)a >> (b & 0x1F))); break; }
        case OP_IAND:  { int32_t b,a; pop_int(f,&b); pop_int(f,&a); push_int(f, a & b); break; }
        case OP_IOR:   { int32_t b,a; pop_int(f,&b); pop_int(f,&a); push_int(f, a | b); break; }
        case OP_IXOR:  { int32_t b,a; pop_int(f,&b); pop_int(f,&a); push_int(f, a ^ b); break; }

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
            stack_pop(f, &a); stack_pop(f, &b);
            stack_push(f, a); stack_push(f, b);
            break;
        }

        /* ── type conversion ───────────────────────────── */
        case OP_I2C: {
            int32_t a; pop_int(f, &a);
            push_int(f, (int32_t)(uint16_t)a);
            break;
        }

        /* ── branch: single-value comparisons ─────────── */
        case OP_IFEQ: case OP_IFNE: case OP_IFLT:
        case OP_IFGE: case OP_IFGT: case OP_IFLE: {
            int16_t offset = fetch_i16(f);
            int32_t v; pop_int(f, &v);
            int take = 0;
            if      (opcode == OP_IFEQ && v == 0) take = 1;
            else if (opcode == OP_IFNE && v != 0) take = 1;
            else if (opcode == OP_IFLT && v <  0) take = 1;
            else if (opcode == OP_IFGE && v >= 0) take = 1;
            else if (opcode == OP_IFGT && v >  0) take = 1;
            else if (opcode == OP_IFLE && v <= 0) take = 1;
            if (take) f->pc = (uint32_t)((int32_t)(f->pc - 3) + offset);
            break;
        }

        /* ── branch: two-value integer comparisons ─────── */
        case OP_IF_ICMPEQ: case OP_IF_ICMPNE: case OP_IF_ICMPLT:
        case OP_IF_ICMPGE: case OP_IF_ICMPGT: case OP_IF_ICMPLE: {
            int16_t offset = fetch_i16(f);
            int32_t b, a;
            pop_int(f, &b); pop_int(f, &a);
            int take = 0;
            if      (opcode == OP_IF_ICMPEQ && a == b) take = 1;
            else if (opcode == OP_IF_ICMPNE && a != b) take = 1;
            else if (opcode == OP_IF_ICMPLT && a <  b) take = 1;
            else if (opcode == OP_IF_ICMPGE && a >= b) take = 1;
            else if (opcode == OP_IF_ICMPGT && a >  b) take = 1;
            else if (opcode == OP_IF_ICMPLE && a <= b) take = 1;
            if (take) f->pc = (uint32_t)((int32_t)(f->pc - 3) + offset);
            break;
        }

        /* ── unconditional jump ────────────────────────── */
        case OP_GOTO: {
            int16_t offset = fetch_i16(f);
            f->pc = (uint32_t)((int32_t)(f->pc - 3) + offset);
            break;
        }

        /* ── return ────────────────────────────────────── */
        case OP_IRETURN: {
            int32_t v; pop_int(f, &v);
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
            uint16_t idx = (uint16_t)fetch_i16(f);
            (void)idx;
            /* push a dummy reference so INVOKEVIRTUAL can pop it */
            Value ref = { VAL_REF, 0 };
            stack_push(f, ref);
            break;
        }
        case OP_INVOKEVIRTUAL: {
            uint16_t idx = (uint16_t)fetch_i16(f);
            (void)idx;
            /* pop the argument and the object reference */
            Value arg, ref;
            stack_pop(f, &arg);
            stack_pop(f, &ref);
            printf("[native] System.out.println(%d)\n", arg.ival);
            break;
        }
        case OP_INVOKESTATIC: {
            uint16_t idx = (uint16_t)fetch_i16(f);
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
