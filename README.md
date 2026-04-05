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

*KolibriOS Project | Mentor: Burer*
# kolibri-kvm

**J2ME KVM Bytecode Interpreter for KolibriOS** — GSoC 2026 Test Task

A from-scratch implementation of the KVM (K Virtual Machine) bytecode interpreter targeting the [KolibriOS J2ME Emulator](http://wiki.kolibrios.org). Interprets CLDC 1.1 Java bytecode, parses real `.class` files, performs method invocation with proper frame management, and includes a mark-and-sweep garbage collector.

---

## Quick Start

```sh
git clone https://github.com/KeshavAcham/kolibri-kvm
cd kolibri-kvm
make
./kvm                      # run 17 built-in tests → 17/17 pass
./kvm Hello.class          # execute a real .class file
./kvm -v Hello.class       # with per-opcode verbose trace
```

Expected output of `./kvm`:

```
╔══════════════════════════════════════════╗
║  KolibriOS J2ME KVM — Bytecode Tests     ║
╚══════════════════════════════════════════╝

  [PASS] return 42
  [PASS] 3 + 4 = 7
  [PASS] 10 - 3 = 7
  [PASS] 6 * 7 = 42
  [PASS] 100 / 4 = 25
  [PASS] 17 % 5 = 2
  [PASS] locals: 10+20=30
  [PASS] iinc: 5+3=8
  [PASS] branch: 7>5 → 1
  [PASS] loop: sum(1..5)=15
  [PASS] sipush 1000
  [PASS] neg: -42
  [PASS] 0xFF & 0x0F = 15
  [PASS] 1 << 3 = 8
  [PASS] dup: 5+5=10
  [PASS] method invocation: square(9) = 81
  [PASS] GC: collected 1 dead object(s), 2 live

17 / 17 tests passed
```

---

## Repository Layout

```
kolibri-kvm/
├── jvm.h          Public API, all structs, opcode and CP-tag constants   (283 lines)
├── classfile.c    .class file parser — CAFEBABE magic through Code attr  (274 lines)
├── jvm.c          Interpreter loop, method dispatch, GC, native stubs    (799 lines)
├── main.c         17-test suite + ./kvm <File.class> CLI                 (279 lines)
└── Makefile
```

---

## Architecture

The four components form a fully wired end-to-end pipeline:

```
./kvm File.class
  └─ vm_run_classfile()                        main.c / jvm.c
       │
       ├─ vm_load_class()                      classfile.c
       │    ├─ slurp()  — read file to buffer
       │    ├─ verify CAFEBABE magic
       │    ├─ parse constant pool             all 12 tag types
       │    ├─ parse fields
       │    └─ parse methods + Code attribute  → MethodInfo.code[]
       │
       └─ vm_invoke_method()                   jvm.c
            └─ vm_exec()   ← main loop
                 │
                 ├─ invokevirtual / invokespecial / invokestatic
                 │    ├─ cp_resolve_ref()      CP → class + name + desc
                 │    ├─ vm_find_class()       class registry lookup
                 │    ├─ find_method()         name+descriptor match
                 │    └─ vm_exec()  ←─────── recursive callee frame
                 │
                 ├─ getstatic / putstatic
                 ├─ getfield  / putfield       field index resolved by name
                 ├─ new                        vm_alloc_object()
                 ├─ ldc                        CP string → heap object
                 └─ dispatch_native()          println, Math.*, <init>
```

### Key Data Structures

| Struct | File | Purpose |
|---|---|---|
| `VM` | `jvm.h` | Top-level state: frame stack, class registry, object heap |
| `Frame` | `jvm.h` | Activation record: bytecode pointer, PC, operand stack, locals |
| `KVMClass` | `jvm.h` | Loaded class: constant pool, method table, field table |
| `MethodInfo` | `jvm.h` | Method: name, descriptor, `uint8_t *code`, max stack/locals |
| `CPEntry` | `jvm.h` | Tagged union over all 12 constant-pool entry types |
| `KVMObject` | `jvm.h` | Heap object: instance fields, string data, array data, GC mark bit |
| `Value` | `jvm.h` | Tagged operand-stack value: `VAL_INT` or `VAL_REF` |

---

## Supported Opcodes (CLDC 1.1 Subset)

75 opcodes are defined and handled by the interpreter switch:

| Category | Opcodes |
|---|---|
| Integer constants | `iconst_m1` … `iconst_5`, `bipush`, `sipush`, `ldc` |
| Local variable loads | `iload` / `iload_0-3`, `aload` / `aload_0-3` |
| Local variable stores | `istore` / `istore_0-3`, `astore` / `astore_0-3` |
| Arithmetic | `iadd`, `isub`, `imul`, `idiv`, `irem`, `ineg`, `iinc` |
| Bitwise / shift | `ishl`, `ishr`, `iushr`, `iand`, `ior`, `ixor` |
| Type conversion | `i2c` |
| Stack manipulation | `dup`, `pop`, `swap` |
| Null / reference | `aconst_null` |
| Integer branches | `ifeq`, `ifne`, `iflt`, `ifge`, `ifgt`, `ifle` |
| Integer comparisons | `if_icmpeq`, `if_icmpne`, `if_icmplt`, `if_icmpge`, `if_icmpgt`, `if_icmple` |
| Reference branches | `ifnull`, `ifnonnull` |
| Unconditional jump | `goto` |
| Returns | `ireturn`, `areturn`, `return` |
| Field access | `getstatic`, `putstatic`, `getfield`, `putfield` |
| Method invocation | `invokevirtual`, `invokespecial`, `invokestatic` |
| Object creation | `new` |

---

## Constant Pool Parsing

`classfile.c` parses all 12 standard constant-pool tag types:

| Tag | Constant | Description |
|-----|----------|-------------|
| 1 | `CP_UTF8` | Modified UTF-8 string (used for names and descriptors) |
| 3 | `CP_INTEGER` | 32-bit integer literal |
| 4 | `CP_FLOAT` | 32-bit float (parsed, not yet executed) |
| 7 | `CP_CLASS` | Class reference → UTF8 index |
| 8 | `CP_STRING` | String literal → UTF8 index |
| 9 | `CP_FIELDREF` | Field reference → class + name-and-type |
| 10 | `CP_METHODREF` | Method reference → class + name-and-type |
| 11 | `CP_IFACE_MREF` | Interface method reference |
| 12 | `CP_NAME_AND_TYPE` | Name + descriptor pair |
| 5/6 | Long / Double | Two-slot entries — consumed and index advanced |

Resolution at runtime uses `cp_resolve_ref()`, which walks the chain: `METHODREF → NAME_AND_TYPE → UTF8` to produce the three strings (`class_name`, `method_name`, `descriptor`) needed for dispatch.

---

## Method Invocation (How It Works)

When the interpreter hits `invokevirtual`, `invokespecial`, or `invokestatic`:

1. **CP resolution** — `cp_resolve_ref()` reads the 2-byte CP index and resolves it to a class name, method name, and descriptor string.
2. **Class lookup** — `vm_find_class()` searches the class registry for the target class.
3. **Method lookup** — `find_method()` scans the class's `methods[]` array matching name and descriptor.
4. **Argument popping** — `count_args()` parses the descriptor (e.g. `(II)V` → 2 args) to know how many slots to pop from the current frame's operand stack.
5. **Frame push** — A new `Frame` is initialised with the callee's bytecode, the popped args placed in `locals[]`, and pushed onto `vm->frames[]`.
6. **Recursive `vm_exec()`** — The interpreter loop is re-entered for the callee. When it hits `ireturn` / `areturn` / `return`, it pops the frame and returns the result code.
7. **Result forwarding** — If the callee returned `JVM_RETURN_INT` or `JVM_RETURN_REF`, the return value is pushed onto the *caller's* operand stack before the main loop continues.

Native methods (`System.out.println`, `Math.max/min/abs`, `Object.<init>`) are handled by `dispatch_native()` before the registry search, so they never require a loaded class.

---

## Garbage Collector

A mark-and-sweep GC is implemented in `vm_gc()` (`jvm.c`):

**Mark phase** — walks every live `Frame` on the call stack, marks every `VAL_REF` value on the operand stack and in the `locals[]` array.

**Sweep + compact phase** — iterates `vm->objects[]`, frees heap data (string data, array data) of unmarked objects, and compacts live objects forward in the array.

**Reference patching** — after compacting, all `VAL_REF` values in all live frames are updated to the new indices so no dangling references remain.

GC is triggered automatically when `vm_alloc_object()` or `vm_alloc_string()` finds `object_count >= MAX_OBJECTS`. It can also be called explicitly.

---

## Runtime Limits

All limits are enforced at runtime through bounds-checked helper functions (`push`, `pop`, `peek`, `pop_int`, `push_int`). No silent overflow is possible.

| Constant | Value | Controls |
|---|---|---|
| `MAX_STACK` | 64 | Operand stack slots per frame |
| `MAX_LOCALS` | 32 | Local variable slots per frame |
| `MAX_FRAMES` | 32 | Maximum call-stack depth |
| `MAX_CLASSES` | 64 | Classes in the registry |
| `MAX_CP_ENTRIES` | 256 | Constant-pool entries per class |
| `MAX_METHODS` | 64 | Methods per class |
| `MAX_FIELDS` | 32 | Fields per class |
| `MAX_OBJECTS` | 1024 | Live heap objects (GC triggers at limit) |
| `HEAP_SIZE` | 256 KB | Raw byte heap |

---

## Native Stubs

The following Java library methods are handled natively without requiring class files:

| Java method | Behaviour |
|---|---|
| `System.out.println(int)` | Calls `KVM_PRINT("%d\n", value)` |
| `System.out.println(String)` | Prints the heap object's `str_data` |
| `System.out.print(int)` | Calls `KVM_PRINT("%d", value)` |
| `Math.max(int, int)` | Returns the larger of two integers |
| `Math.min(int, int)` | Returns the smaller of two integers |
| `Math.abs(int)` | Returns absolute value |
| `Object.<init>()` | No-op constructor stub |

---

## KolibriOS Porting

The only host-OS dependency in the interpreter is a single macro in `jvm.c`:

```c
/* Current: standard C I/O */
#define KVM_PRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
```

Replace with a KolibriOS debug-board output syscall:

```c
/* KolibriOS target */
#define KVM_PRINT(fmt, ...) kolibri_debug_printf(fmt, ##__VA_ARGS__)
```

The rest of the VM is pure C11 with no POSIX or platform dependencies beyond `malloc`/`free`/`fopen`/`fread`, all of which have KolibriOS equivalents in its standard C library.

---

## Test Coverage

17 tests run automatically with `./kvm`:

| # | Test | What it covers |
|---|---|---|
| 1 | `return 42` | `bipush`, `ireturn` |
| 2 | `3 + 4 = 7` | `iconst`, `iadd` |
| 3 | `10 - 3 = 7` | `isub` |
| 4 | `6 * 7 = 42` | `imul` |
| 5 | `100 / 4 = 25` | `idiv` |
| 6 | `17 % 5 = 2` | `irem` |
| 7 | `locals: 10+20=30` | `istore_N`, `iload_N` |
| 8 | `iinc: 5+3=8` | `iinc` |
| 9 | `branch: 7>5 → 1` | `if_icmple`, conditional branch |
| 10 | `loop: sum(1..5)=15` | `iinc`, `if_icmple` back-branch, while loop |
| 11 | `sipush 1000` | `sipush` (big-endian 16-bit) |
| 12 | `neg: -42` | `ineg` |
| 13 | `0xFF & 0x0F = 15` | `iand`, bitwise |
| 14 | `1 << 3 = 8` | `ishl`, shift |
| 15 | `dup: 5+5=10` | `dup` |
| 16 | `method invocation: square(9)=81` | Frame push/pop, `vm_invoke_method`, `imul` across frames |
| 17 | `GC: collected 1 dead object` | Mark-and-sweep, object compaction, REF patching |

### Running Against a Real `.class` File

```sh
# Write and compile Hello.java
cat > Hello.java << 'JAVA'
public class Hello {
    static int add(int a, int b) { return a + b; }
    public static void main(String[] args) {
        int result = add(3, 4);
        System.out.println(result);
    }
}
JAVA
javac Hello.java

# Execute with kvm
./kvm Hello.class     # prints: 7
./kvm -v Hello.class  # prints: 7 with full opcode trace
```

---

## Error Codes

Every VM operation returns a `JVMResult`:

| Code | Meaning |
|---|---|
| `JVM_OK` | Success / still running |
| `JVM_RETURN_INT` | `ireturn` executed |
| `JVM_RETURN_VOID` | `return` executed |
| `JVM_RETURN_REF` | `areturn` executed |
| `JVM_ERR_STACK_OVERFLOW` | Operand stack or call stack full |
| `JVM_ERR_STACK_UNDERFLOW` | Pop on empty stack |
| `JVM_ERR_UNKNOWN_OPCODE` | Unrecognised bytecode |
| `JVM_ERR_DIVIDE_BY_ZERO` | `idiv` or `irem` with zero divisor |
| `JVM_ERR_OUT_OF_BOUNDS` | Local variable index out of range |
| `JVM_ERR_NULL_PTR` | Null reference dereference |
| `JVM_ERR_CLASS_NOT_FOUND` | `vm_load_class()` could not open file |
| `JVM_ERR_METHOD_NOT_FOUND` | Named method absent in class |
| `JVM_ERR_CLASSFILE_INVALID` | Bad magic, truncated file, unknown CP tag |
| `JVM_ERR_OUT_OF_MEMORY` | Object heap exhausted after GC |

---

## Issues Fixed

This implementation addresses all 11 issues raised against the original submission:

| # | Issue | Resolution |
|---|---|---|
| 1 | README overclaims real `.class` execution | README now accurately documents scope and next steps |
| 2 | No end-to-end pipeline | `vm_exec ↔ vm_invoke_method ↔ vm_run_classfile` fully wired |
| 3 | `invokevirtual` / `invokestatic` stubbed | Full CP resolution, frame push, recursive execution, result forwarding |
| 4 | Constant pool not usable | All 12 CP tag types parsed; `cp_resolve_ref()` used by every field and method op |
| 5 | Class loader not integrated | `vm_load_class()` parses `.class` bytes into `KVMClass`; `vm_find_class()` wired into dispatch |
| 6 | JAR supports STORED only | Deferred to Phase 1 midterm (miniz integration planned) |
| 7 | No CLI entry point | `./kvm [-v] File.class` works; `vm_run_classfile()` finds and runs `main()` |
| 8 | Fixed limits without safe fallback | Every stack/local access through bounds-checked helpers; errors propagate cleanly |
| 9 | No GC | Mark-and-sweep `vm_gc()` with compaction and REF-index patching |
| 10 | No KolibriOS abstraction | `KVM_PRINT` macro is the only I/O dependency; one-line swap to port |
| 11 | Tests cover opcode-level only | Tests 16–17 cover inter-frame method dispatch and GC; real `.class` execution verified |

---

## Next Steps (Phase 1 → Midterm)

- **JAR/ZIP loader** — DEFLATE decompression via miniz; STORED already trivial
- **Exception handling** — `athrow`, `try/catch` table parsing, exception propagation across frames
- **`java.lang.String` natives** — `length()`, `charAt()`, `equals()`, `valueOf(int)`
- **Long / double** — category-2 values (64-bit, two operand-stack slots)
- **Virtual dispatch table** — class hierarchy, `super` lookup, proper `invokespecial` semantics
- **Static initializers** — `<clinit>` execution on first class use
- **Array opcodes** — `newarray`, `arraylength`, `iaload`, `iastore`

---

## References

- [JVM Specification SE 8](https://docs.oracle.com/javase/specs/jvms/se8/html/)
- [CLDC 1.1 Specification — JSR 139](https://jcp.org/en/jsr/detail?id=139)
- [KolibriOS Wiki](http://wiki.kolibrios.org)
- [JamVM — lightweight JVM in C](http://jamvm.sourceforge.net)
- [phoneME Feature — open-source J2ME](https://github.com/phoneme)
