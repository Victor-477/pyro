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
* **Native VM Capabilities:** Direct system integration including native I/O (`input`, `write_bytes`), HTTP networking (`http_get`, `http_post`), runtime control (`pyro_exec`, `pyro_env`, `pyro_exit`), and JSON serialization.
* **Complete Parity System:** The C VM features byte-for-byte stdout, stderr, and exit code parity against the Go VM (validated on every build).

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
go build -o pyrovm_go.exe main.go
```

**C VM Compilation (using MSVC on Windows):**
```bash
cd Pyro/vm
cl /O2 /utf-8 /Fe:pyrovm.exe main.c pyro_runtime.c
```

### Running the Parity Tests
To verify absolute parity between the virtual machines, run the integration test runner:
```bash
python Burnout/tests/test_c_vm.py
```
This script compiles both interpreters and verifies their stdout, stderr, and exit codes over 25+ end-to-end integration scenarios.
