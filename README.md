# PYRO — the native target language (`.pyro`)

**Pyro** is the system's **custom target language**: a **binary bytecode** with an
instruction set (ISA) invented here — it is **not** x86, Go or C. The compiler
(**Burnout**) generates `.pyro` from `.cryo`, and the **Pyro VM** (in Go) loads and
runs that bytecode on the machine.

```
  .cryo ──►  Burnout (compiler)  ──►  .pyro (custom bytecode)  ──►  Pyro VM (Go)  ──►  execution
```

## Contents

| Path | Role |
|---|---|
| `vm/main.go` · `vm/go.mod` | The **Pyro VM** (in Go): loads, decodes and runs `.pyro` |
| `vm/main.c` · `vm/pyro_runtime.{c,h}` | The **Pyro VM in C** (no Go dependency) + isolated runtime |
| `PYRO_BYTECODE.md` | ISA and `.pyro` format specification |
| `PYRO_RUNTIME.md` | Specification of the minimal shared runtime |
| `PYRO.md` | Native-layer direction notes (historical) |

## What the VM supports

- Scalars: `int`, `number` (float64), `bool`, `string` (dynamic typing).
- Arithmetic, comparison, bitwise, logic (short-circuit), unary.
- Variables, `if/else`, `while`, `do/while`, `for`, `for-each`, `switch`, ternary,
  `break`/`continue`.
- Functions, recursion, calls (their own stack frames).
- **Containers**: arrays (`[...]`, `push`, indexing, `len`), maps (`{k:v}`, `has`,
  `keys`, indexing), structs (= map of fields, `s.field`).
- Safety: division by zero and `assert` abort; out-of-bounds indices abort.

## Why it is its own language

- **Runs on the machine** via the VM (portable: a single Go binary).
- **Compact/opaque** ("encrypted") — the code section is encoded (rolling XOR).
- **Native to the system**, great as training data for AI agents (instructions
  already in the form the machine executes).
- **Custom** — not derived from existing languages.

Full ISA and format spec in [PYRO_BYTECODE.md](PYRO_BYTECODE.md). Disassembler
(`.pyro` → readable text) in the compiler: `--backend pyro --dis`.

## Dependencies

The VM is **self-contained** (Go standard library only). The `.pyro` it runs is
produced by **Burnout**. Distributed as its own repository (the target language).
