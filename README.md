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

> **Google Summer of Code 2026** — A CLDC 1.1 subset bytecode interpreter
> written entirely in C, targeting [KolibriOS](http://kolibrios.org).

---

## What is this?

**kolibri-kvm** is a from-scratch implementation of the KVM (K Virtual Machine),
the lightweight JVM at the heart of Java 2 Micro Edition (J2ME).
The long-term goal is to bring thousands of classic J2ME mobile games and apps
to KolibriOS — an assembly-optimized OS that fits on a floppy disk.

This repository is the **GSoC 2026 test task deliverable**: a working bytecode
interpreter with a self-contained test suite, plus a minimal `.class` file loader
that wires the parser directly to the interpreter for single-method execution.

### Current state (honest)

| Component | Status |
| --- | --- |
| Bytecode interpreter (`jvm.c`) | ✅ Working — 107+ opcodes, full error checking |
| Opcode-level test suite (`main.c`) | ✅ 18 tests passing |
| `.class` file loader (`classloader_demo.c`) | ✅ Loads and executes single methods from real `.class` files |
| Constant pool parsing | ✅ Integer, Float, String entries resolved |
| Exception table parsing | ✅ Parsed and passed to VM |
| Mark-and-sweep GC | ✅ Implemented, auto-triggered at threshold |
| Multi-method dispatch (`invokevirtual` etc.) | 🔄 Phase 2 — stubs only |
| Full `java.lang` / CLDC 1.1 libraries | 🔄 Phase 2 |
| JAR / ZIP loader | 🔄 Phase 2 |
| MIDP 2.0 graphics / Canvas | 🔄 Phase 3 |
| KolibriOS integration layer | 🔄 Phase 3 |

---

## Architecture

```
classloader_demo.c
  │
  ├── parses .class file
  │     constant pool  → CPEntry[]
  │     Code attribute → bytecode[]
  │     exception table → ExcEntry[]
  │
  └── calls vm_exec_full()
            │
            ▼
        jvm.c
  switch-dispatch interpreter
  operand stack / local variables
  heap + mark-and-sweep GC
```

| File | Purpose |
| --- | --- |
| `jvm.h` | Structs, opcode constants, result codes — public API |
| `jvm.c` | Interpreter loop, heap, GC |
| `main.c` | 18-test opcode-level test suite |
| `classloader_demo.c` | Loads a real `.class` file and executes one method via `vm_exec_full()` |
| `Makefile` | Builds `kvm_test` (test suite) and `kvm` (class loader) |

---

## Build & Run

```bash
# Run the opcode test suite
make test

# Build the class loader
make kvm

# Execute a real .class file (first method)
./kvm Hello.class

# Execute a named method
./kvm Hello.class run
```

### Test suite output

```
╔══════════════════════════════════════╗
║  KolibriOS J2ME KVM — Bytecode Tests ║
╚══════════════════════════════════════╝

  [PASS] return 42
  [PASS] 3 + 4 = 7
  ...
  [PASS] newarray iastore/iaload
  [PASS] arraylength=7

18 / 18 tests passed
```

### Class loader output (real .class file)

```
$ ./kvm Hello.class
[kvm] Hello.class — 1 method(s)
[kvm]   [0] run
[kvm] Running method: run
[kvm] Bytecode: 4 bytes  max_stack=2  max_locals=0
  [pc=  0] opcode=0x06  sp=-1
  [pc=  1] opcode=0x07  sp=0
  [pc=  2] opcode=0x60  sp=1
  [pc=  3] opcode=0xAC  sp=0
[kvm] Result: RETURN_INT  return = 7
```

---

## Supported opcodes

| Category | Opcodes |
| --- | --- |
| Integer constants | `iconst_m1`…`iconst_5`, `bipush`, `sipush`, `ldc` |
| Float constants | `fconst_0/1/2` |
| Integer loads/stores | `iload/istore` 0–3 + indexed |
| Reference loads/stores | `aload/astore` 0–3 + indexed, `aconst_null` |
| Array ops | `newarray`, `iaload`, `iastore`, `arraylength` |
| Integer arithmetic | `iadd`, `isub`, `imul`, `idiv`, `irem`, `ineg`, `iinc` |
| Float arithmetic | `fadd`, `fsub`, `fmul`, `fdiv`, `frem`, `fneg` |
| Bitwise / shift | `ishl`, `ishr`, `iushr`, `iand`, `ior`, `ixor` |
| Float compare | `fcmpl`, `fcmpg` (NaN-correct per JVM spec) |
| Type conversions | `i2f`, `f2i`, `i2b`, `i2c`, `i2s` |
| Stack ops | `dup`, `dup_x1`, `pop`, `pop2`, `swap` |
| Branches | `ifeq`…`ifle`, `if_icmpeq`…`if_icmple`, `ifnull`, `goto` |
| Returns | `ireturn`, `freturn`, `areturn`, `return` |
| Exception | `athrow` (searches exception table, jumps to handler) |
| Stubs | `invokevirtual`, `invokespecial`, `invokestatic`, `getstatic` |

---

## Bug Fixes (from mentor review)

| Bug | Fix |
| --- | --- |
| `ldc` pushed CP index instead of value | Resolves `CP_INTEGER`, `CP_FLOAT`, `CP_STRING` from `CPEntry` |
| `fdiv` returned `1e30f` instead of IEEE 754 Infinity | C `float` division handles `+Inf/-Inf/NaN` natively |
| `fcmpl`/`fcmpg` returned 0 for NaN | `fcmpl` returns -1, `fcmpg` returns +1 per spec |
| `invokevirtual` always popped 2 values | `count_args()` parses descriptor; pops correct N args |
| `athrow` killed VM unconditionally | Walks `exc_table`, jumps to `handler_pc` |
| `fetch_u8` had no bounds check | `fetch_u8_safe` checks `pc < code_len` before every read |
| `pop_int`/`pop_float` return values discarded | `POP_INT`/`POP_FLOAT` macros propagate all errors |
| Branch targets not validated | `BRANCH` macro checks `0 ≤ target < code_len` |
| 17 missing opcodes | All added including `OP_FCONST_1/2`, `OP_NEWARRAY`, `OP_IASTORE/IALOAD`, `OP_ARETURN`, etc. |
| `JVM_RETURN_REF` missing from enum | Added |
| `-lm` missing from Makefile | Added `LDFLAGS = -lm` |
| Duplicate `CPEntry`/`ExceptionEntry` definitions | Authoritative definitions in `jvm.h` only |

---

## Phase 1 Progress

| Deliverable | Status |
| --- | --- |
| Bytecode interpreter (switch-dispatch) | ✅ Complete |
| Operand stack with full error checking | ✅ Complete |
| Local variable array (int, float, ref) | ✅ Complete |
| 107+ CLDC 1.1 opcodes | ✅ Complete |
| Mark-and-sweep GC (auto-triggered) | ✅ Complete |
| 18-test opcode-level suite | ✅ Complete |
| `.class` file loader (CP + Code attr) | ✅ Complete |
| `vm_exec_full()` wired to loader | ✅ Complete — `./kvm Hello.class` works |
| Multi-method dispatch (`invoke*`) | 🔄 Phase 2 |
| Full class file parser (`classfile.c`) | 🔄 Phase 2 |
| JAR / ZIP loader | 🔄 Phase 2 |
| `java.lang` native stubs | 🔄 Phase 2 |
| MIDP 2.0 Display/Canvas | 🔄 Phase 3 |
| KolibriOS integration | 🔄 Phase 3 |

---

## Roadmap

```
GSoC Test Task  ████████████████████  ✅ Complete
Phase 2         ░░░░░░░░░░░░░░░░░░░░  Full class loader, method dispatch, CLDC 1.1 libs
Phase 3         ░░░░░░░░░░░░░░░░░░░░  MIDP 2.0 graphics, MIDlet lifecycle
Polish          ░░░░░░░░░░░░░░░░░░░░  Real J2ME app testing, KolibriOS port, docs
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
