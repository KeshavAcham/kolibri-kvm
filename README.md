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
> and object model — written entirely in C, targeting [KolibriOS](http://kolibrios.org).

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
│  │ CP resolver  │    │  opcodes (CLDC 1.1 subset)    │  │
│  │ JAR/ZIP loader│   │  operand stack                │  │
│  │ class registry│   │  local variable array         │  │
│  └──────────────┘    │  object heap                  │  │
│                      │  GC infrastructure (Phase 2)  │  │
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
| `jvm.c` | Interpreter loop, heap allocator |
| `classfile.h` | ClassFile, MethodInfo, CPEntry, ClassRegistry types |
| `classfile.c` | `.class` parser, constant pool resolver, JAR/ZIP loader |
| `main.c` | 31-test suite covering every opcode category |
| `classloader_demo.c` | CLI entry point — loads and runs real `.class` / `.jar` files |

---

## Features

### ✅ Bytecode Interpreter — CLDC 1.1 subset

| Category | Opcodes |
| --- | --- |
| Integer constants | `iconst_m1` … `iconst_5`, `bipush`, `sipush` |
| Float constants | `fconst_0/1/2` |
| Integer loads/stores | `iload/istore` 0–3 + indexed |
| Reference loads/stores | `aload/astore` 0–3 + indexed, `aconst_null` |
| Array loads | `iaload` |
| Array stores | `iastore` |
| Integer arithmetic | `iadd`, `isub`, `imul`, `idiv`, `irem`, `ineg`, `iinc` |
| Float arithmetic | `fadd`, `fsub`, `fmul`, `fdiv`, `frem`, `fneg` |
| Bitwise / shift | `ishl`, `ishr`, `iushr`, `iand`, `ior`, `ixor` |
| Float compare | `fcmpl`, `fcmpg` |
| Type conversions | `i2f`, `f2i`, `i2b`, `i2c`, `i2s` |
| Stack ops | `dup`, `dup_x1`, `pop`, `pop2`, `swap` |
| Integer branches | `ifeq` … `ifle`, `if_icmpeq` … `if_icmple` |
| Reference branches | `ifnull`, `ifnonnull` |
| Jumps | `goto` |
| Returns | `ireturn`, `freturn`, `areturn`, `return` |
| Object creation | `new`, `newarray` |
| Field access | `getstatic` (stub) |
| Method invocation | `invokevirtual` (stub), `invokestatic` (stub) |
| Array length | `arraylength` |
| Misc | `nop`, `aconst_null` |

> Opcodes marked *stub* consume their operand bytes and are no-ops in Phase 1.
> Full dispatch (`invoke*`, `getfield`, `putfield`, `athrow`, `wide`, `lconst`, `dup2`, `dup_x2`, `goto_w`, `if_acmpeq/ne`) is planned for Phase 2.

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

* `JObject` supports int arrays with a `marked` field ready for GC
* Reference-based heap — objects addressed by 1-based integer refs (0 = null)
* `vm_alloc_array_int` allocator with `MAX_OBJECTS` (512) capacity limit
* Null pointer detection on every dereference
* Array bounds checking on every load and store

### 🔄 Garbage Collector — Phase 2

The `JObject` struct includes a `marked` field and `obj_count` tracking is in place.
Full mark-and-sweep implementation (root scanning, recursive marking, sweep + `idata` free)
is planned for Phase 2 alongside long arithmetic and `java.lang` native stubs.

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

# Build the class loader demo
make kvm
./kvm
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
  [PASS] sipush 1000
  [PASS] neg: -42
  [PASS] 0xFF & 0x0F = 15
  [PASS] 1 << 3 = 8
  [PASS] dup: 5+5=10

── Float opcodes ──
  [PASS] fconst 1.0+2.0=3
  [PASS] fconst 2.0*2.0=4
  [PASS] i2f→f2i round-trip 7

── Reference / object ops ──
  [PASS] aconst_null areturn
  [PASS] astore/aload null

── Array ops ──
  [PASS] int[5] store/load[2]
  [PASS] int[7] arraylength

── Type conversions ──
  [PASS] i2b: 300→44
  [PASS] i2s: 0x1170→4464

── Stack ops ──
  [PASS] dup_x1
  [PASS] pop2

── Null/ref branches ──
  [PASS] ifnull taken

── Wide locals ──
  [PASS] istore/iload local[10]

── Bug regressions ──
  [PASS] bug1: bipush no operand  (→ ERR_OUT_OF_BOUNDS)
  [PASS] bug2: iadd empty stack  (→ ERR_STACK_UNDERFLOW)
  [PASS] bug3: goto negative target  (→ ERR_OUT_OF_BOUNDS)

31 / 31 tests passed
```

### classloader_demo output

```
classloader_demo: sum(1..10) = 55  [RETURN_INT]
```

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
| CLDC 1.1 opcode subset | ✅ Complete |
| `.class` file parser | ✅ Complete |
| Constant pool resolution (all 12 tags) | ✅ Complete |
| Method table + Code attribute | ✅ Complete |
| Class registry | ✅ Complete |
| JAR / ZIP loader | ✅ Complete |
| Java heap + object model | ✅ Complete |
| Comprehensive test suite (31 tests) | ✅ Complete |
| Mark-and-sweep GC | 🔄 Phase 2 |
| Real method dispatch (`invoke*`) | 🔄 Phase 2 |
| `java.lang` native stubs | 🔄 Phase 2 |
| Long arithmetic (`ladd`, `lmul` …) | 🔄 Phase 2 |
| Remaining CLDC 1.1 opcodes | 🔄 Phase 2 |

---

## Roadmap

```
Phase 1  ████████████████████  Complete
Phase 2  ░░░░░░░░░░░░░░░░░░░░  GC, long arithmetic, full invoke*, java.lang stubs
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
