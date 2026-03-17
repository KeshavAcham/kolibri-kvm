# J2ME KVM — Bytecode Interpreter (GSoC 2026 Test Task)

A partial implementation of the **KVM (K Virtual Machine)** bytecode interpreter for the
[KolibriOS J2ME Emulator](http://wiki.kolibrios.org) GSoC 2026 project.


> *"Implement core KVM: bytecode interpreter, operand stack, local variables"*

---

## Architecture

```
jvm.h   — public API: VM struct, Frame struct, opcode constants, result codes
jvm.c   — interpreter loop (switch-dispatch over JVM opcodes)
main.c  — test suite with 15 hand-assembled bytecode programs
```

### Key data structures

| Structure | Purpose |
|-----------|---------|
| `VM`      | Top-level VM: frame stack, heap pool, verbose flag |
| `Frame`   | Activation record: bytecode pointer, PC, operand stack, local variables |
| `Value`   | Tagged union for stack/local values (int or reference) |

### Interpreter design

The interpreter uses a classic **switch-dispatch loop** over JVM opcodes, exactly as described
in the proposal's Section 6.1. Each iteration:

1. Fetches the next opcode byte at `frame->pc`
2. Dispatches to the corresponding `case`
3. Manipulates the operand stack and/or local variable array
4. Advances `pc` (branches modify `pc` directly)

---

## Supported opcodes (CLDC 1.1 subset)

| Category           | Opcodes |
|--------------------|---------|
| Integer constants  | `iconst_m1` … `iconst_5`, `bipush`, `sipush` |
| Local variables    | `iload` / `iload_0-3`, `istore` / `istore_0-3` |
| Arithmetic         | `iadd`, `isub`, `imul`, `idiv`, `irem`, `ineg`, `iinc` |
| Bitwise / shift    | `ishl`, `ishr`, `iushr`, `iand`, `ior`, `ixor` |
| Stack ops          | `dup`, `pop`, `swap` |
| Type conversion    | `i2c` |
| Branches           | `ifeq`, `ifne`, `iflt`, `ifge`, `ifgt`, `ifle` |
| Integer compares   | `if_icmpeq`, `if_icmpne`, `if_icmplt`, `if_icmpge`, `if_icmpgt`, `if_icmple` |
| Jumps              | `goto` |
| Returns            | `ireturn`, `return` |
| Object stubs       | `getstatic`, `invokevirtual`, `invokestatic` (stubs, print to stdout) |

---

## Build & run

```bash
make
./kvm
```

Expected output:
```
╔══════════════════════════════════════╗
║  KolibriOS J2ME KVM — Bytecode Tests ║
╚══════════════════════════════════════╝

  [PASS] return 42
  [PASS] 3 + 4 = 7
  ...
  [PASS] dup: 5+5 = 10

15 / 15 tests passed

── Verbose trace: 3 + 4 ──
  [pc=  1] opcode=0x05  sp=-1
  ...
  result = 7
```

---

## Next steps (Phase 1 → Midterm)

- [ ] Class file parser (`.class` / `CAFEBABE` magic, constant pool, method table)
- [ ] JAR/ZIP loader (using miniz)
- [ ] Multi-frame method invocation (`invokevirtual`, `invokespecial`, `invokestatic`)
- [ ] `java.lang.Object`, `java.lang.String`, `java.lang.Math` native stubs
- [ ] Exception handling (`athrow`, `try/catch` table)
- [ ] Mark-and-sweep GC for the Java heap

---

## References

- [JVM Specification (SE 8)](https://docs.oracle.com/javase/specs/jvms/se8/html/)
- [CLDC 1.1 Specification](https://jcp.org/en/jsr/detail?id=139)
- [phoneME Feature — open-source J2ME](https://github.com/phoneme)
- [JamVM — lightweight JVM in C](http://jamvm.sourceforge.net)
 
