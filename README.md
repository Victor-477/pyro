# 🔥 Pyro — The Native Bytecode Specification & Multi-VM Runtime

[![Language](https://img.shields.io/badge/Language-Go%20%2F%20C-blue.svg)](https://github.com/Victor-477/Pyro_Cryo)
[![Bytecode](https://img.shields.io/badge/Bytecode-PYRO--v2-red.svg)](PYRO_BYTECODE.md)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

**Pyro** is the system's low-level, high-performance execution target. It defines the custom binary bytecode format (`.pyro`), the Instruction Set Architecture (ISA), and contains two independent virtual machine implementations: the high-performance **Go VM** and the lightweight, portable **C VM**.

---

## ⚡ Key Highlights

* **Custom ISA Specification:** Invented instruction set containing register-push-pop, local variable slots, nested scopes, runtime call frames, container instantiation, and native function routes.
* **Double VM Implementation:**
  - **Go VM:** Multi-platform VM implemented in Go, featuring fast execution, reflection-based parsing, and automated garbage collection.
  - **C VM:** Lightweight VM implemented in pure C, featuring deterministic **Reference Counting** memory management (`retain`/`release`) and absolute portability.
* **Code Obfuscation:** The bytecode section of `.pyro` executables is obfuscated using a rolling XOR key system to prevent trivial decompilation.
* **Native VM Capabilities:** Direct system integration including native I/O (`input`, `read_file`, `write_bytes`, `args`), HTTP networking (`http_get`, `http_post`) and **serving** (`http_serve`), runtime control (`pyro_exec`, `pyro_env`, `pyro_exit`), and JSON serialization.
* **Complete Parity System:** The C VM features byte-for-byte stdout, stderr, and exit code parity against the Go VM (validated on every build) — extending to HTTP responses served by `http_serve`.
* **Ahead-of-Time Native Compilation:** A `.pyro` can be lowered to C ([`aot_pyro.py`](../Burnout/aot_pyro.py)) and linked against this same runtime, producing an executable that carries no VM and no bytecode at runtime. The AOT is a second engine over the runtime, so it inherits VM semantics by construction.

---

## 📂 File Architecture

| File / Directory | Component | Description |
| :--- | :--- | :--- |
| 📁 [`vm/`](vm/) | **Virtual Machines** | Implements the execution systems: |
| ├── 📄 [`main.go`](vm/main.go) | *Go Interpreter* | The main execution entry point for the Go-based Pyro VM. |
| ├── 📄 [`main.c`](vm/main.c) | *C Interpreter* | Fully-featured interpreted loop (`vm_run`) with custom JSON parsers, Reference Counting, and network adapters. |
| ├── 📄 [`pyro_runtime.c`](vm/pyro_runtime.c) | *C Runtime Core* | Implements internal C VM structures for arrays, maps, strings, and garbage collection mechanisms. |
| ├── 📄 [`pyro_runtime.h`](vm/pyro_runtime.h) | *C Runtime Headers* | Header definitions for memory management, values, and runtime operations. |
| 📄 [`PYRO_BYTECODE.md`](PYRO_BYTECODE.md) | **Bytecode Spec** | Formal specification of magic headers, version indicators, XOR keys, function declarations, and opcodes. |
| 📄 [`PYRO_RUNTIME.md`](PYRO_RUNTIME.md) | **Runtime Spec** | Formal specification of value layouts, memory boundaries, and standard builtins. |
| 📄 [`PYRO.md`](PYRO.md) | **Native Direction** | Design logs and structural choices of the native layer. |

---

## 💾 Bytecode Binary Layout

A `.pyro` file is structured as a compact binary format:

```text
┌───────────────┬────────────────────────────────────────────────────────┐
│  Magic Bytes  │ 'P', 'Y', 'R', 'O' (4 bytes)                           │
├───────────────┼────────────────────────────────────────────────────────┤
│    Version    │ Target VM Version (1 byte)                             │
├───────────────┼────────────────────────────────────────────────────────┤
│  Header Flags │ Binary features, e.g., debug information, sandbox mode  │
├───────────────┼────────────────────────────────────────────────────────┤
│ Constant Pool │ Encoded floats, strings, booleans, and nulls           │
├───────────────┼────────────────────────────────────────────────────────┤
│   Functions   │ String descriptors, arity, code size, local slots      │
├───────────────┼────────────────────────────────────────────────────────┤
│ Code Section  │ Instructions (XOR encrypted)                           │
└───────────────┴────────────────────────────────────────────────────────┘
```

---

## 🛠️ Verification & Compilation

### Compiling and Running the VMs

To compile both the Go and C virtual machines, you need a Go compiler (Go 1.18+) and a C compiler (GCC, Clang, or MSVC).

**Go VM Compilation:**
```bash
cd Pyro/vm
go build -o pyrovm_go.exe .
```

Build the package, not `main.go` — the debugger and the sampling profiler
(11.25) live in `debug.go` and `profile.go` beside it, and naming one file
leaves them out.

**C VM Compilation (GCC/MinGW/Clang):**
```bash
gcc -O2 -std=c11 -o pyrovm.exe Pyro/vm/main.c Pyro/vm/pyro_runtime.c -lm -lws2_32
```

`-lws2_32` is Windows-only: the runtime uses sockets for `http_serve`. With MSVC,
the equivalent is `cl /O2 /utf-8 /Fe:pyrovm.exe main.c pyro_runtime.c ws2_32.lib`
(`/utf-8` keeps the accented error messages byte-identical to the Go VM's).

The runtime compiles under strict ISO mode. Note that `strdup`, `_popen`,
`_pclose` and `_getpid` are POSIX/MSVCRT rather than ISO C, so `-std=c11` hides
their declarations — the runtime supplies its own `strdup` and declares the others
explicitly, because leaving them implicit makes them return `int` and silently
truncate pointers on 64-bit hosts.

### Running the Parity Tests
To verify absolute parity between the virtual machines, run the integration test runner:
```bash
python Burnout/tests/test_c_vm.py
```
It builds both interpreters (auto-detecting gcc/clang/MSVC) and compares stdout,
stderr and exit codes across the examples, abort/try-catch cases, `write_bytes`
output, runtime semantics regressions, and the responses served by `http_serve` —
33 scenarios. It skips cleanly when no C toolchain is present.

---

## 🐞 Debugging and profiling (11.25)

Both are options of the VM, and both read the **debug section** the compiler
already writes (`flags` bit1, `pc → line`). Nothing about the file format
changes, so a `.pyro` built before either existed can be debugged and profiled.

Options come **before** the program; everything after it is the program's, so a
script that takes its own `--debug` is never intercepted.

### Debugger

```bash
python Burnout/pyro.py debug program.cryo
# or, on a .pyro directly:
Pyro/vm/pyrovm_go.exe --debug --source=program.cryo program.pyro
```

| | |
|---|---|
| `break <line>` / `break <function>` | stop there — `delete <line>`, `delete all` |
| `step` / `next` | one source line, into or over calls |
| `stepi` | one bytecode instruction |
| `finish` | run until the current function returns |
| `continue` | run to the next breakpoint |
| `backtrace`, `list`, `info locals`, `info break` | inspect where you are |

Breakpoints resolve to **statement boundaries**, which is exactly what the
debug section records. `break 12` on a line that begins no statement is refused
and names the nearest line that does, rather than arming a breakpoint that can
never fire. `break <function>` resolves to the function's first statement — not
to its entry pc, which is prologue and belongs to whatever came before it.

**Locals are shown by slot**, not by name: the debug section holds `pc → line`
and nothing else, so the names are not in the file to show. Parameters come
first, in declaration order.

### Sampling profiler

```bash
python Burnout/pyro.py profile program.cryo --hz 5000
```

```
=== Pyro profile — 30.8 ms, 51 samples at 5000 Hz ===
   samples     self    self ms  function
        51  100.00%       30.8  slow
```

It reports **self** time — the function on top of the stack when the sample was
taken. Time inside natives (`sleep`, `http_get`, file reads) is charged to the
Cryo function that called them. A program that finishes inside one sampling
period is told so rather than shown an empty table; raise `--hz`.

Neither costs anything when unused: see [BENCHMARKS.md](BENCHMARKS.md), where
the per-instruction check is measured rather than argued about.
