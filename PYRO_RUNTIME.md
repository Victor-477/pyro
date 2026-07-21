# Pyro Runtime — minimal runtime specification

This document specifies the **runtime** of the Pyro target language: the value
model, memory management, container/string semantics, I/O, the native builtins
(`NATIVE`) and the error/abort contract.

The runtime is **independent of the execution engine**. Today there are two
runtime implementations that must be **semantically identical**:

| Implementation | Files | Toolchain |
|---|---|---|
| C runtime (isolated) | `pyro/vm/pyro_runtime.h` + `pyro_runtime.c` | gcc / MinGW / MSVC |
| Go runtime (embedded in the Go VM) | `pyro/vm/main.go` | Go |

The C VM (`pyro/vm/main.c`) is only the **engine** (loading, decoding and the
dispatch loop); it depends solely on `pyro_runtime.h`. Future targets (e.g. native
C code, WASM) reuse the same runtime and inherit this semantics without
reimplementing it.

> The ISA (opcodes, `.pyro` format) is specified in [`PYRO_BYTECODE.md`](PYRO_BYTECODE.md).
> Here we cover only the runtime.

---

## 1. Value model

A `Value` is dynamic and carries its type tag at runtime:

| Type | Representation | Notes |
|---|---|---|
| `int` | signed 64-bit integer (`int64`) | `+ - *` overflow aborts (fail-fast) |
| `float` | 64-bit floating point (`double`) | |
| `bool` | boolean | |
| `string` | UTF-8, immutable, reference-counted | length in **bytes** |
| `null` | absence of value | |
| `array` | contiguous list of `Value`, reference-counted | |
| `map` | hash table `Value → Value`, reference-counted | keys by value equality |

There is no distinct `struct` type: **structs and enum variants are maps** of
string keys (e.g. `Ok(v)` → `{"tag": "Ok", "val0": v}`). Data-less enums are
integer constants resolved at compile time.

### Numeric promotion
Mixed `int`/`float` operations promote the integer to `float`; the result is
`float`. Between two `int`s, arithmetic is integer (division truncates toward zero).

### Equality (`value_eq`)
- Same numeric category compares by value (with `int`/`float` promotion).
- `string` compares by content; `bool` by value; `null == null` is true.
- `array`/`map` compare by **reference identity**.

### Truthiness (`value_truthy`)
`false`, `null`, `0` (int), `0.0` (float) and `""` are falsy; everything else is truthy.

---

## 2. Memory management (reference counting)

`string`, `array` and `map` are reference-counted objects (`ref_count`). `int`,
`float`, `bool` and `null` are immediate values (no allocation).

Invariants:

- **`retain_value(v)`** increments the object's `ref_count` (no-op for immediates).
- **`release_value(v)`** decrements; at zero, it frees the object and
  **recursively releases** its elements (array) or key/value pairs (map).
- A freshly created object starts with `ref_count = 1`.
- When **storing** a value into a slot/container that takes ownership, `retain`;
  when **overwriting/discarding**, `release`.
- There is no cycle collector: since arrays/maps compare by identity and the
  language exposes no mutation that creates ownership cycles, refcount is enough.

> The reference semantics of arrays/maps (aliasing by reference) is part of the
> contract: passing an array to a function shares the same object.

---

## 3. Strings

- Immutable; length measured in **bytes** (`len(s)`).
- Indexing `s[i]` returns byte `i` as a 1-character string; out-of-range index
  **aborts** (see §6).
- Concatenation `a + b` with at least one string operand converts the other via
  `value_to_string` and produces a new string.
- `value_to_string` defines the canonical textual form of each type (used by
  `print`, concatenation and `json_encode` of keys).

---

## 4. Containers

### Arrays
- `arr.push(v)` appends to the end (grows capacity as needed).
- `len(arr)` returns the number of elements.
- `arr[i]` reads/writes by index; out-of-range **aborts** (fail-fast).

### Maps
- Hash table with resizing; keys compared via `value_eq`.
- `m[k]` reads (missing key → `null`) and writes; `has(m, k)` tests presence;
  `remove(m, k)` removes; `keys(m)` returns the keys **sorted by their textual
  form** (deterministic — guarantees parity across implementations).

---

## 5. I/O and native builtins (`NATIVE`)

The `NATIVE id, argc` instruction consumes `argc` values from the stack and pushes
the result. The id table is **mirrored** between the generator (`NATIVES` in
`burnout/codegen_pyro.py`) and each runtime:

| id | name | id | name | id | name |
|---|---|---|---|---|---|
| 0 | `sqrt` | 9 | `to_int` | 18 | `substr` |
| 1 | `pow` | 10 | `to_number` | 19 | `split` |
| 2 | `abs` | 11 | `remove` | 20 | `join` |
| 3 | `min` | 12 | `upper` | 21 | `input` |
| 4 | `max` | 13 | `lower` | 22 | `json_encode` |
| 5 | `floor` | 14 | `trim` | 23 | `json_decode` |
| 6 | `ceil` | 15 | `contains` | 24 | `http_get` |
| 7 | `round` | 16 | `find` | 25 | `http_post` |
| 8 | `to_string` | 17 | `replace` | 26 | `sleep` |
| | | | | 27 | `write_bytes` |

- **`input(prompt)`** reads a line from stdin (I/O).
- **`json_encode`/`json_decode`** serialize/deserialize the value tree (object keys
  become maps; JSON integers become `int`).
- **`http_get`/`http_post`** make network requests; **`sleep(ms)`** pauses.
- **`write_bytes(path, int[]) -> bool`** writes an integer array as raw bytes
  (each element truncated to `& 0xFF`) to a file — the binary output that lets a
  program on the VM emit a `.pyro` (enables the self-hosted compiler).
- `to_int`/`to_number` of a non-numeric string **abort** (fail-fast).

### Sandbox policy
The runtime exposes `pyro_sandboxed` (turned on by the host via the `.pyro` `bit2`
flag or `PYRO_SANDBOX=1`). When active, the **network/machine** natives (`http_get`,
`http_post`, `write_bytes`) are refused with a security abort. `sleep` stays allowed.

---

## 6. Error and abort contract

There are two regimes:

- **Fail-fast (non-catchable)** — low-level safety: integer overflow,
  division/modulo by zero, out-of-range index (array/string), invalid
  `to_int`/`to_number`, sandbox. They call `fatal()`.
- **Catchable (`try`/`catch`)** — `throw`, a failing `assert`, and `unwrap`
  (`x!`) of `null`. They raise an exception; if no handler is active, they become
  `fatal()`.

`fatal(msg)` is the **host callback**: it prints `"[Pyro VM] " + msg` to stderr,
followed by the stack trace (if a debug section is present), and exits with code 1.
The messages are standardized and **identical** across implementations (the Go VM
and the C VM produce byte-for-byte the same output, verified by the parity tests):

```
[Pyro VM] [Cryo Security] DivByZero: integer division
  stack trace (most recent first):
    at divide (line 2)
    at main (line 8)
```

Canonical messages (prefix `[Cryo Security]` for safety):
`DivByZero: integer division` / `: modulo`; `Overflow: INT64_MIN / -1`;
`IndexError: index N out of bounds (len=M)` (array get), `IndexError: index N out
of bounds` (array set), `IndexError: string index out of bounds`;
`unwrap of null value`; `to_int: '…' is not a valid integer`; `to_number: '…' is
not a valid number`; `Sandbox: http_get() blocked by sandbox policy`. Outside
safety: `[Cryo Assert] <msg>` and `uncaught exception: <value>`.

---

## 7. Runtime ↔ host boundary

The runtime is engine-agnostic; it depends only on:

| Symbol | Direction | Role |
|---|---|---|
| `void fatal(const char* msg)` | host → runtime | aborts with a message + stack trace |
| `bool pyro_sandboxed` | host sets, runtime reads | sandbox policy |

Everything else (the operand stack, call frames, exception handlers, the debug
section, decoding and dispatch) belongs to the **engine** and is not visible to the
runtime. Thus any engine (stack VM, native-C translator, etc.) that provides these
two symbols gets identical semantics.
