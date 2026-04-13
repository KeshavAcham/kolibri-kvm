# kolibri-kvm

```
██╗  ██╗██╗   ██╗███╗   ███╗
██║ ██╔╝██║   ██║████╗ ████║
█████╔╝ ██║   ██║██╔████╔██║
██╔═██╗ ╚██╗ ██╔╝██║╚██╔╝██║
██║  ██╗ ╚████╔╝ ██║ ╚═╝ ██║
╚═╝  ╚═╝  ╚═══╝  ╚═╝     ╚═╝
K Virtual Machine — KolibriOS J2ME Emulator
```



---

## Author

**Acham Keshav** — [github.com/KeshavAcham](https://github.com/KeshavAcham) — GSoC 2026 Applicant
# J2ME KVM — Bytecode Interpreter (GSoC 2026 Test Task)

A partial implementation of the **KVM (K Virtual Machine)** bytecode interpreter for the
[KolibriOS J2ME Emulator](http://wiki.kolibrios.org) GSoC 2026 project.

> *"Implement core KVM: bytecode interpreter, operand stack, local variables, object heap, mark-compact GC"*

---

## Architecture

```
jvm.h    — public API: VM, Frame, Value, HeapObject, ClassDesc, Method,
           opcode constants, result codes
jvm.c    — interpreter loop, object heap, class registry, mark-compact GC
main.c   — 22 hand-assembled bytecode test cases
Makefile — single-target build
```

### Key data structures

| Structure | Purpose |
|-----------|---------|
| `VM` | Top-level VM: frame stack, object pool, class registry, `system_out_ref` |
| `Frame` | Activation record: bytecode pointer, PC, operand stack, locals, `code_owned` flag |
| `Value` | Tagged union — `VAL_INT`, `VAL_REF`, `VAL_LONG`, `VAL_LONG2` |
| `HeapObject` | Heap-allocated Java object: class id, field slots, liveness bit, GC forwarding index |
| `ClassDesc` | Class descriptor with pre-resolved field slot indices for O(1) field access |
| `Method` | Method descriptor for `vm_invoke_method`: code, length, descriptor string, `code_owned` |

### Interpreter design

The interpreter uses a classic **switch-dispatch loop** over JVM opcodes, exactly as described
in the proposal's Section 6.1. The loop lives in the private `execute_frame()` function,
which is called by both `vm_exec` (for raw bytecode) and `vm_invoke_method` (for structured
method calls). Each iteration:

1. Fetches the next opcode byte at `frame->pc`
2. Dispatches to the corresponding `case`
3. Manipulates the operand stack and/or local variable array
4. Advances `pc` (branches modify `pc` directly)

---

## Supported opcodes (CLDC 1.1 subset)

| Category | Opcodes |
|----------|---------|
| Integer constants | `iconst_m1` … `iconst_5`, `bipush`, `sipush` |
| Long constants | `lconst_0`, `lconst_1` |
| Null | `aconst_null` |
| Integer locals | `iload` / `iload_0-3`, `istore` / `istore_0-3` |
| Long locals | `lload`, `lstore` (2-slot aware) |
| Reference locals | `aload` / `aload_0-3`, `astore` / `astore_0-3` |
| Integer arithmetic | `iadd`, `isub`, `imul`, `idiv`, `irem`, `ineg`, `iinc` |
| Long arithmetic | `ladd`, `lsub`, `lmul`, `ldiv`, `lrem`, `lneg`, `lcmp` |
| Bitwise / shift | `ishl`, `ishr`, `iushr`, `iand`, `ior`, `ixor` |
| Stack ops | `dup`, `dup2`, `pop`, `pop2`, `swap` |
| Type conversion | `i2l`, `l2i`, `i2c`, `i2b`, `i2s` |
| Integer branches | `ifeq`, `ifne`, `iflt`, `ifge`, `ifgt`, `ifle` |
| Integer compares | `if_icmpeq`, `if_icmpne`, `if_icmplt`, `if_icmpge`, `if_icmpgt`, `if_icmple` |
| Reference compares | `if_acmpeq`, `if_acmpne`, `ifnull`, `ifnonnull` |
| Jumps | `goto` |
| Returns | `ireturn`, `lreturn`, `areturn`, `return` |
| Object | `new`, `getfield`, `putfield`, `getstatic`, `putstatic` |
| Invocation | `invokevirtual`, `invokespecial`, `invokestatic` |

---

## Build & run

```
make
./kvm
```

Expected output:

```
╔══════════════════════════════════════════╗
║  KolibriOS J2ME KVM — Bytecode Tests v2  ║
╚══════════════════════════════════════════╝

── Original 15 tests ──
  [PASS] return 42
  [PASS] 3 + 4 = 7
  [PASS] 10 - 3 = 7
  [PASS] 6 * 7 = 42
  [PASS] 100 / 4 = 25
  [PASS] 17 % 5 = 2
  [PASS] locals: 10+20 = 30
  [PASS] iinc: 5+3 = 8
  [PASS] branch: 7>5 → 1
  [PASS] loop: sum(1..5) = 15
  [PASS] sipush 1000
  [PASS] neg: -42
  [PASS] 0xFF & 0x0F = 15
  [PASS] 1 << 3 = 8
  [PASS] dup: 5+5 = 10

── New tests (fixes verified) ──
  [PASS] long: 1L+2L = 3
  [PASS] i2b: (byte)300 = 44
  [PASS] lcmp: 10L>5L → 1
  [PASS] invoke square(7) = 49
  [PASS] object_fields: Point(10,20).x+y = 30
  [PASS] gc_forwarding: refs patched correctly after compaction
  [PASS] count_args: (I)→1, (JI)→3, (String;I)→2, (DD)→4

22 / 22 tests passed

── Verbose trace: 3 + 4 ──
  [pc=  0] opcode=0x06  sp=-1
  [pc=  1] opcode=0x07  sp=0
  [pc=  2] opcode=0x60  sp=1
  [pc=  3] opcode=0xAC  sp=0
  result = 7
```

---

## Bugs fixed in v2

Eight issues identified during mentor review, all resolved:

**1 — Double-frame push** `vm_invoke_method` pushed a frame, then `vm_exec`
checked `code != code` to avoid pushing again. On recursion with the same method
the check broke and the frame was skipped instead of pushed. Fixed by introducing
a private `execute_frame()` that contains the dispatch loop. Only `vm_exec` and
`vm_invoke_method` push frames — the loop itself never does.

**2 — `dispatch_native("println")` ignored descriptor** The original handler
didn't distinguish `println(int)` from `println(String)` and always popped
2 values (objectref + arg), corrupting the stack on `invokestatic` which has no
objectref. Fixed with `dispatch_println(f, is_virtual)` which inspects the
`ValType` tag on the popped value and skips the objectref pop for static calls.

**3 — `count_args` didn't account for 2-slot types** `J` (long) and `D` (double)
occupy 2 local variable slots per the JVM spec but were counted as 1, breaking any
`.class` file with long or double arguments. Fixed: `J` and `D` now contribute 2 to
the slot count. `lload`/`lstore` read/write slot `idx` as `VAL_LONG` and mark the
adjacent `idx+1` as `VAL_LONG2`.

**4 — GC ref-patching was O(n·m·k)** For each compacted object the GC scanned all
frames × all locals and stack slots — up to ~3 million comparisons worst case,
critical on KolibriOS where RAM is constrained and there is no virtual memory. Fixed
with a **forwarding table**: `fwd[old_index] = new_index` is built in one pass, then
all Value slots are patched in a single O(total_slots) sweep regardless of how many
objects moved.

**5 — `getstatic System.out` returned `objects[0]`** Hardcoded index 0 could point
to any previously allocated object and only worked by accident because
`dispatch_native` ignored the objectref. Fixed: `vm_init` allocates a real
`java/io/PrintStream` object and stores its index in `vm->system_out_ref`. The GC
treats it as a permanent root and patches it after every compaction.

**6 — `getfield`/`putfield` did linear field lookup by name on every access** O(n)
per access with n = field count. Fixed: `ClassDesc` stores pre-resolved
`field_slots[]` indices computed once at class registration. `vm_find_field()`
returns the cached index in O(1) on every subsequent call.

**7 — `classfile.h` / `classloader_demo.c` were dead code** Neither file appeared
in the Makefile, both duplicated definitions from `jvm.h`/`jvm.c` with conflicting
constants, and `classloader_demo.c` was unreachable from any translation unit.
Both files removed. The codebase is now exactly `jvm.h`, `jvm.c`, `main.c`,
`Makefile`.

**8 — `code_owned` not explicitly initialized** `test_method_invocation` set
`sq->code = square_code` but left `sq->code_owned` to whatever `memset` happened
to write — fragile and undefined if the struct is stack-allocated without zeroing.
Fixed: every `Method` initialisation sets `code_owned` explicitly. `execute_frame`
frees the code buffer only when `code_owned == 1`.

---

## GC design

The collector is **mark-compact** with forwarding-table reference patching.

```
Mark     — walk frame locals + operand stacks; transitively mark VAL_REF
           fields in heap objects; system_out_ref is always a GC root.

Forward  — one pass assigns each surviving object a compact new index:
           fwd[old] = new

Compact  — move surviving objects to the front of vm->objects[].

Patch    — one linear sweep over all Value slots in all frames and all
           surviving object fields rewrites every VAL_REF.ival through fwd[].
           Cost: O(frames × locals + frames × stack + live_objects × fields).
           Independent of the number of collected objects.
```

This replaces the previous approach that re-scanned the entire frame set for every
moved object — O(n·m·k) where n = moved objects, m = frames, k = slots per frame.

---

## Public API

```c
/* Initialise a VM. Pass verbose=1 to trace every opcode. */
void      vm_init          (VM *vm, int verbose);

/* Free any VM-owned resources (code buffers with code_owned=1). */
void      vm_destroy       (VM *vm);

/* Execute raw bytecode in a new frame; integer result via ret_val. */
JVMResult vm_exec          (VM *vm, const uint8_t *code, uint32_t len,
                             int32_t *ret_val);

/* Invoke a structured Method with typed arguments. */
JVMResult vm_invoke_method (VM *vm, const Method *m, Value *args, int argc,
                             Value *ret_val);

/* Allocate a heap object of the given class. Returns object index or -1. */
int       vm_alloc_object  (VM *vm, int class_id);

/* Run mark-compact GC; patches all live references via forwarding table. */
void      vm_gc            (VM *vm);

/* Register a class with named fields; returns class id. */
int       vm_register_class(VM *vm, const char *name,
                             const char **field_names, int nfields);

/* Resolve a field name to its cached slot index. O(1) after registration. */
int       vm_find_field    (VM *vm, int class_id, const char *name);
```

### Invoking a method

```c
/* int square(int x) { return x * x; } */
static const uint8_t square_code[] = {
    OP_ILOAD_0, OP_ILOAD_0, OP_IMUL, OP_IRETURN
};

VM vm; vm_init(&vm, 0);

Method m;
m.code       = square_code;
m.code_len   = sizeof(square_code);
m.code_owned = 0;          /* static array — VM must not free it */
m.descriptor = "(I)I";

Value arg = { VAL_INT, 7, 0 };
Value ret  = { VAL_INT, 0, 0 };
vm_invoke_method(&vm, &m, &arg, 1, &ret);
/* ret.ival == 49 */

vm_destroy(&vm);
```

### Allocating objects and accessing fields

```c
const char *fields[] = { "x", "y" };
int class_id = vm_register_class(&vm, "Point", fields, 2);
int obj      = vm_alloc_object(&vm, class_id);

int fx = vm_find_field(&vm, class_id, "x");   /* cached O(1) index */
int fy = vm_find_field(&vm, class_id, "y");

vm.objects[obj].fields[fx].value.ival = 10;
vm.objects[obj].fields[fy].value.ival = 20;
```

---

## Next steps (Phase 1 → Midterm)

- Class file parser (`.class` / `CAFEBABE` magic, constant pool, method table)
- JAR/ZIP loader (using miniz)
- `tableswitch` / `lookupswitch` opcodes
- `athrow` + exception table dispatch
- Array opcodes (`newarray`, `iaload`, `iastore`, …)
- `java.lang.Object`, `java.lang.String`, `java.lang.Math` native stubs
- Wide opcode prefix (`0xC4`)

---

## References

- [JVM Specification (SE 8)](https://docs.oracle.com/javase/specs/jvms/se8/html/)
- [CLDC 1.1 Specification — JSR 139](https://jcp.org/en/jsr/detail?id=139)
- [phoneME Feature — open-source J2ME](https://github.com/phoneme)
- [JamVM — lightweight JVM in C](http://jamvm.sourceforge.net)

