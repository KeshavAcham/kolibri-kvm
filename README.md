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

> **Google Summer of Code 2026** — A bytecode interpreter, class file parser, JAR loader,
> object model, and mark-and-sweep garbage collector — written entirely in C,
> targeting [KolibriOS](http://kolibrios.org).

---

## What is this?

**kolibri-kvm** is a from-scratch implementation of the KVM (K Virtual Machine),
the lightweight JVM at the heart of Java 2 Micro Edition (J2ME).
The goal is to bring thousands of classic J2ME mobile games and apps to KolibriOS —
an assembly-optimized OS that fits on a floppy disk.

This repository is the **Phase 1 deliverable** of the GSoC 2026 project:
a working bytecode interpreter that can parse real `.class` files, load `.jar` archives,
manage a Java heap, and execute CLDC 1.1 bytecode — with zero external dependencies.

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                     kolibri-kvm                         │
│                                                         │
│  ┌──────────────┐    ┌───────────────────────────────┐  │
│  │  classfile.c │    │           jvm.c               │  │
│  │              │    │                               │  │
│  │ .class parser│───▶│  switch-dispatch loop         │  │
│  │ CP resolver  │    │  107 CLDC 1.1 opcodes         │  │
│  │ JAR/ZIP loader│   │  operand stack                │  │
│  │ class registry│   │  local variable array         │  │
│  └──────────────┘    │  object heap                  │  │
│                      │  mark-and-sweep GC            │  │
│  ┌──────────────┐    └───────────────────────────────┘  │
│  │    jvm.h     │                                        │
│  │              │    ┌───────────────────────────────┐  │
│  │ VM / Frame   │    │       classloader_demo.c      │  │
│  │ Value / Obj  │    │  ./kvm Hello.class            │  │
│  │ JVMResult    │    │  ./kvm app.jar com/Main       │  │
│  └──────────────┘    └───────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

| File | Purpose |
| --- | --- |
| `jvm.h` | All structs, opcode constants, result codes — the public API |
| `jvm.c` | Interpreter loop, heap allocator, mark-and-sweep GC |
| `classfile.h` | ClassFile, MethodInfo, CPEntry, ClassRegistry types |
| `classfile.c` | `.class` parser, constant pool resolver, JAR/ZIP loader |
| `main.c` | 31-test suite covering every opcode category |
| `classloader_demo.c` | CLI entry point — loads and runs real `.class` / `.jar` files |

---

## Features

### ✅ Bytecode Interpreter — 107 opcodes

| Category | Opcodes |
| --- | --- |
| Integer constants | `iconst_m1` … `iconst_5`, `bipush`, `sipush`, `ldc`, `ldc_w` |
| Float constants | `fconst_0/1/2` |
| Long constants | `lconst_0/1` |
| Integer loads/stores | `iload/istore` 0–3 + indexed |
| Reference loads/stores | `aload/astore` 0–3 + indexed, `aconst_null` |
| Array loads | `iaload`, `aaload`, `baload`, `caload` |
| Array stores | `iastore`, `aastore`, `bastore`, `castore` |
| Integer arithmetic | `iadd`, `isub`, `imul`, `idiv`, `irem`, `ineg`, `iinc` |
| Float arithmetic | `fadd`, `fsub`, `fmul`, `fdiv`, `frem`, `fneg` |
| Bitwise / shift | `ishl`, `ishr`, `iushr`, `iand`, `ior`, `ixor` |
| Float compare | `fcmpl`, `fcmpg` |
| Type conversions | `i2f`, `f2i`, `i2b`, `i2c`, `i2s` |
| Stack ops | `dup`, `dup_x1`, `dup_x2`, `dup2`, `pop`, `pop2`, `swap` |
| Integer branches | `ifeq` … `ifle`, `if_icmpeq` … `if_icmple` |
| Reference branches | `if_acmpeq`, `if_acmpne`, `ifnull`, `ifnonnull` |
| Jumps | `goto`, `goto_w` |
| Returns | `ireturn`, `freturn`, `areturn`, `return` |
| Object creation | `new`, `newarray`, `anewarray` |
| Field access | `getfield`, `putfield`, `getstatic`, `putstatic` |
| Method invocation | `invokevirtual`, `invokespecial`, `invokestatic`, `invokeinterface` |
| Type checks | `checkcast`, `instanceof`, `athrow` |
| Array length | `arraylength` |
| Wide prefix | `wide iload/aload/istore/astore/iinc` |
| Misc | `nop`, `lconst_0/1` |

### ✅ Class File Parser

* Full `.class` file parsing (magic `0xCAFEBABE`, minor/major version)
* Complete constant pool resolution — all 12 CP tags including `UTF8`, `Class`, `Methodref`, `Fieldref`, `NameAndType`, `String`, `Integer`
* Method table extraction with `Code` attribute parsing
* Exception table parsing
* Field table skipping (with attribute awareness)

### ✅ JAR / ZIP Loader

* Minimal ZIP parser — no external dependencies (no `zlib`, no `miniz`)
* Reads ZIP central directory to locate all entries
* Extracts and parses every `.class` file from a JAR
* Supports STORED (method=0) entries — standard for `.class` files in JARs

### ✅ Class Registry

* Fixed-size class registry supporting up to 64 simultaneously loaded classes
* O(n) lookup by class name
* Full lifecycle management with `classfile_free`

### ✅ Object Model & Heap

* `JObject` supports plain objects, int arrays, ref arrays, and strings
* Reference-based heap — objects addressed by 1-based integer refs (0 = null)
* `vm_alloc_object`, `vm_alloc_array`, `vm_alloc_string` allocators
* Null pointer detection on every dereference
* Array bounds checking on every load and store

### ✅ Mark-and-Sweep Garbage Collector

* Roots scanned from all live frame operand stacks and local variable arrays
* Recursive marking through object fields and reference array elements
* Sweep phase frees all unmarked objects and reclaims their memory
* Auto-triggered when object count exceeds `GC_THRESHOLD` (384/512 slots)
* Zero external allocator dependencies — pure C `malloc`/`free`

### ✅ Robust Error Handling

Every single opcode is hardened against:

* Buffer over-read (bounds-checked `fetch_u8` / `fetch_i16`)
* Stack underflow on every `pop` operation
* Stack overflow on every `push` operation
* Out-of-bounds branch targets (validated before every jump)
* Null pointer dereference on object/array access
* Array index out of bounds
* Division by zero
* Invalid local variable indices
* Truncated bytecode streams

---

## Build & Run

```
# Build the test suite
make test
./test

# Build the class loader
make kvm
./kvm Hello.class
./kvm app.jar com/example/Main
```

### Expected test output

```
╔══════════════════════════════════════════╗
║  KolibriOS J2ME KVM — Full Test Suite    ║
╚══════════════════════════════════════════╝

── Original tests ──
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
  ...

── Float opcodes ──
  [PASS] fconst 1.0+2.0=3
  [PASS] i2f→f2i round-trip 7

── Array ops ──
  [PASS] int[5] store/load[2]
  [PASS] int[7] arraylength

── Bug regressions ──
  [PASS] bug1: bipush no operand  (→ ERR_OUT_OF_BOUNDS)
  [PASS] bug2: iadd empty stack   (→ ERR_STACK_UNDERFLOW)
  [PASS] bug3: goto negative target (→ ERR_OUT_OF_BOUNDS)

31 / 31 tests passed
```

---

## Running a real `.class` file

```
$ ./kvm Hello.class
[kvm] Loading class file: Hello.class
[kvm] Class: Hello  (1 methods)
[kvm] Methods:
  [0] run ()I
[kvm] Running method: run()I
  [pc=  0] opcode=0x05  sp=-1
  [pc=  1] opcode=0x06  sp=0
  [pc=  2] opcode=0x60  sp=1
  [pc=  3] opcode=0xAC  sp=0
[kvm] Result: RETURN_INT  return value = 5
```

---

## Bug Fixes (post-review)

Following mentor code review, these correctness issues were resolved:

| Bug | Fix |
| --- | --- |
| `ldc` pushed CP index instead of value | Now resolves `CP_INTEGER`, `CP_FLOAT`, `CP_STRING` from `CPEntry`; falls back to raw index if no CP attached |
| `fdiv` returned `1e30f` instead of IEEE 754 Infinity | Removed magic constant — C `float` division produces `+Inf`/`-Inf`/`NaN` natively per IEEE 754 |
| `fcmpl`/`fcmpg` returned 0 for NaN | Added `isnan()` check — `fcmpl` returns -1, `fcmpg` returns +1 per JVM spec |
| `invokevirtual` always popped exactly 2 values | Now pops N args + object ref; `count_args()` helper parses method descriptor |
| `athrow` returned `OUT_OF_BOUNDS` unconditionally | Now walks `exc_table`, finds handler covering throw PC, jumps to `handler_pc` |
| `vm_alloc_array` leaked `idata`/`rdata` on exit | Added `obj_count` tracking; auto-GC triggers at `GC_THRESHOLD = 384` objects |

---

## Error codes

| Code | Meaning |
| --- | --- |
| `JVM_OK` | Normal completion |
| `JVM_RETURN_INT/FLOAT/REF/VOID` | Method returned a value |
| `JVM_ERR_STACK_OVERFLOW` | Operand stack or frame stack full |
| `JVM_ERR_STACK_UNDERFLOW` | Pop on empty stack |
| `JVM_ERR_UNKNOWN_OPCODE` | Unrecognised bytecode |
| `JVM_ERR_DIVIDE_BY_ZERO` | Integer/float division by zero |
| `JVM_ERR_OUT_OF_BOUNDS` | PC overrun, bad branch target, local OOB |
| `JVM_ERR_NULL_POINTER` | Field/array access on null reference |
| `JVM_ERR_ARRAY_INDEX` | Array index out of range |
| `JVM_ERR_OUT_OF_MEMORY` | Heap exhausted |
| `JVM_ERR_NEGATIVE_ARRAY_SIZE` | `newarray` with negative count |

---

## Phase 1 Progress

> *"Implement core KVM: bytecode interpreter, class file parser, operand stack, local variables, basic class loading from JAR"*
> — GSoC 2026 Proposal, Phase 1

| Deliverable | Status |
| --- | --- |
| Bytecode interpreter (switch-dispatch) | ✅ Complete |
| Operand stack with full error checking | ✅ Complete |
| Local variable array (int, float, ref) | ✅ Complete |
| 107 CLDC 1.1 opcodes | ✅ Complete |
| `.class` file parser | ✅ Complete |
| Constant pool resolution (all 12 tags) | ✅ Complete |
| Method table + Code attribute | ✅ Complete |
| Class registry | ✅ Complete |
| JAR / ZIP loader | ✅ Complete |
| Java heap + object model | ✅ Complete |
| Mark-and-sweep GC | ✅ Complete |
| Exception handler table dispatch (`athrow`) | ✅ Complete |
| Comprehensive test suite (31 tests) | ✅ Complete |
| Real method dispatch (`invoke*`) | 🔄 Phase 2 |
| `java.lang` native stubs | 🔄 Phase 2 |
| Long arithmetic (`ladd`, `lmul` …) | 🔄 Phase 2 |

---

## Roadmap

```
Phase 1  ██████████████████░░  ~90% ← you are here
Phase 2  ░░░░░░░░░░░░░░░░░░░░  CLDC 1.1 libraries, GC, strings, exceptions
Phase 3  ░░░░░░░░░░░░░░░░░░░░  MIDP 2.0 Display/Canvas, MIDlet lifecycle
Polish   ░░░░░░░░░░░░░░░░░░░░  Real J2ME app testing, docs, final submission
```

---

## References

* [JVM Specification (SE 8)](https://docs.oracle.com/javase/specs/jvms/se8/html/)
* [CLDC 1.1 Specification — JSR 139](https://jcp.org/en/jsr/detail?id=139)
* [MIDP 2.0 Specification — JSR 118](https://jcp.org/en/jsr/detail?id=118)
* [phoneME Feature — open-source J2ME](https://github.com/phoneme)
* [JamVM — lightweight JVM in C](http://jamvm.sourceforge.net)
* [KolibriOS Developer Wiki](http://wiki.kolibrios.org)

---

## Author

**Acham Keshav** — [github.com/KeshavAcham](https://github.com/KeshavAcham) — GSoC 2026 Applicant

*KolibriOS Project | Mentor: Burer*
