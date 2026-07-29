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
dispatch loop); it depends solely on `pyro_runtime.h`. The **AOT** translator
(`burnout/aot_pyro.py`, `.pyro` → C) is a second engine over the same runtime, so
a natively compiled program inherits these semantics by construction rather than
reimplementing them.

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
| `function` | first-class function value (holds a function-table index) | immediate; prints as `<fn#N>` |

There is no distinct `struct` type: **structs and enum variants are maps** of
string keys (e.g. `Ok(v)` → `{"tag": "Ok", "val0": v}`). Data-less enums are
integer constants resolved at compile time.

### Numeric promotion
Mixed `int`/`float` operations promote the integer to `float`; the result is
`float`. Between two `int`s, arithmetic is integer (division truncates toward zero).

### Equality (`value_eq`)
- Same numeric category compares by value (with `int`/`float` promotion).
- `string` compares by content; `bool` by value.
- `null` compares equal only to `null` (`null == null` is true; `x == null` is false for any non-null container, scalar or function).
- `array` and `map` compare by **reference identity** (pointer equality).

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
| | | | | 28 | `read_file` |
| | | | | 29 | `args` |
| | | | | 30 | `http_serve` |
| 31 | `clamp` | 38 | `sort` | 45 | `count` |
| 32 | `sign` | 39 | `reverse` | 46 | `sum` |
| 33 | `gcd` | 40 | `slice` | 47 | `now_ms` |
| 34 | `hypot` | 41 | `index_of` | 48 | `monotonic_ms` |
| 35 | `starts_with` | 42 | `pad_start` | 49 | `random` |
| 36 | `ends_with` | 43 | `pad_end` | 50 | `random_int` |
| 37 | `repeat` | 44 | `concat` | 51 | `seed` |
| | | | | 52 | `http_listen` |
| | | | | 53 | `http_accept` |
| | | | | 54 | `http_respond` |
| | | | | 55 | `file_exists` |
| | | | | 56 | `is_dir` |
| | | | | 57 | `list_dir` |
| | | | | 58 | `make_dir` |
| | | | | 59 | `delete_file` |
| | | | | 60 | `file_size` |
| | | | | 61 | `write_file` |
| | | | | 62 | `env` |
| | | | | 63 | `exec` |
| | | | | 55 | `file_exists` |
| | | | | 56 | `is_dir` |
| | | | | 57 | `list_dir` |
| | | | | 58 | `make_dir` |
| | | | | 59 | `delete_file` |
| | | | | 60 | `file_size` |
| | | | | 61 | `write_file` |
| | | | | 62 | `env` |
| | | | | 63 | `exec` |

Ids 30 and below are listed in the left three columns above; 31–63 continue here.
An id is **permanent** once shipped: renumbering silently breaks every `.pyro`
already on disk, so new builtins only ever append.

- **`input(prompt)`** reads a line from stdin (I/O).
- **`json_encode`/`json_decode`** serialize/deserialize the value tree (object keys
  become maps; JSON integers become `int`).
- **`http_get`/`http_post`** make network requests; **`sleep(ms)`** pauses.
- **`write_bytes(path, int[]) -> bool`** writes an integer array as raw bytes
  (each element truncated to `& 0xFF`) to a file — the binary output that lets a
  program on the VM emit a `.pyro` (enables the self-hosted compiler).
- **`read_file(path) -> string`** reads a whole file, `""` on any error. Together
  with `args` and `write_bytes` it is what lets the self-hosted compiler run as a
  real command-line program (`pyroc in.cryo out.pyro`).
- **`args() -> string[]`** returns the program's arguments — everything after the
  `.pyro` path for the VMs, everything after `argv[0]` for an AOT binary, so the
  same program sees the same list either way.
- **`http_serve(port, dir)`** serves `dir` over HTTP and **blocks** (it only
  returns on a fatal error). `.wasm` is sent as `application/wasm` so browsers can
  stream-compile it; missing paths give 404 and any attempt to escape the root
  (`..`, backslash, drive letter) gives 403. This is what makes a Cryo program a
  web server — see the full-stack example in `cryo/examples/fullstack/`.
- **`slice(x, start, end)`** is **polymorphic over arrays and strings** — it
  returns a new array for an array and a new (byte-indexed) string for a string.
  It is the single lowering target for the slice syntax `xs[a..b]` / `s[a..b]`
  (roadmap 10.9): the parser desugars slicing *before* types are known, so it
  cannot pick an array-only or string-only builtin. Both bounds are **clamped**,
  never an error — `start` below 0 becomes 0, `end` past the length becomes the
  length, and `start > end` yields an empty result. Any other operand type
  aborts. The result is always a **copy**; mutating it never affects the source.
- **`http_listen(port) -> bool`**, **`http_accept() -> map`** and
  **`http_respond(status, content_type, body) -> bool`** are a *dynamic* HTTP
  server, in contrast to `http_serve` which only serves static files. The
  program owns the accept loop:

  ```cryo
  http_listen(8080);
  while (true) {
      map<string,string> req = http_accept();
      if (len(req["method"]) > 0) {
          http_respond(200, "application/json", route(req));
      }
  }
  ```

  An accept **loop** rather than an `http_api(port, handler)` callback is a
  deliberate choice: a callback would have to invoke a Cryo function value
  from inside a native, re-entering the interpreter — and the Go VM keeps its
  stack/frames/pc as locals of `run()` while the C VM keeps them in file-scope
  globals, so that is a different refactor per engine. Owning the loop needs
  no re-entrancy anywhere and makes the sequencing explicit.

  Requests are served **strictly one at a time**: one listener and one
  in-flight connection, which is what makes this safe with no locking.
  `http_accept` **always returns a map**, never `null` — a failed or malformed
  request yields an empty `method`. The keys are `method`, `path`, `query`
  and `body`. Calling `http_accept` before `http_listen` aborts; calling it
  again without responding closes the unanswered connection rather than
  leaking it. `http_listen` is gated by the sandbox.
- **Filesystem and process (11.7)** — `file_exists`, `is_dir`, `file_size`
  and `list_dir` are pure queries and ungated: they reveal no more than a path
  lookup. `make_dir`, `delete_file`, `write_file`, `env` and `exec` **mutate the
  machine or read its environment** and are sandbox-gated, like
  `read_file`/`write_bytes`.

  - `list_dir(path) -> string[]` returns entry **names** (not paths), `.` and
    `..` excluded, **sorted by byte order** so every engine returns the same
    sequence. An unreadable path gives an empty array, not an error.
  - `file_size(path) -> int` is `-1` when the path cannot be read, which is
    distinguishable from a legitimately empty file.
  - `make_dir(path) -> bool` creates **every missing component** and returns
    true if the directory already exists.
  - **`delete_file` removes FILES ONLY** and returns false for a directory.
    This is a deliberate narrowing: Go's `os.Remove` drops an empty directory,
    MSVCRT's `remove()` refuses, and POSIX's removes it — one call would have
    meant three different things. It is also never recursive. Directory
    removal is not offered yet.
  - `exec(cmd) -> string` returns the command's **stdout**, `""` on failure.
    It runs through `cmd /C` on Windows and `sh -c` elsewhere.
- `to_int`/`to_number` of a non-numeric string **abort** (fail-fast).
- **`replace(s, old, new)`** replaces all occurrences of `old` with `new`. When `old` is an empty string (`""`), `new` is inserted at every position boundary (e.g. `replace("abc", "", "-")` yields `"-a-b-c-"`, and `replace("", "", "-")` yields `"-"`).

### Sandbox policy
The runtime exposes `pyro_sandboxed` (turned on by the host via the `.pyro` `bit2`
flag or `PYRO_SANDBOX=1`). When active, the **network/machine** natives (`http_get`,
`http_post`, `write_bytes`, `read_file`, `http_serve`) are refused with a security
abort. `sleep` and `args` stay allowed.

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
| `int pyro_argc` / `char** pyro_argv` | host sets, runtime reads | program arguments, returned by `args()` |

Everything else (the operand stack, call frames, exception handlers, the debug
section, decoding and dispatch) belongs to the **engine** and is not visible to the
runtime. Thus any engine (stack VM, native-C translator, etc.) that provides these
symbols gets identical semantics.

### Building the C runtime

`pyro_runtime.c` uses sockets for `http_serve`, so on Windows every link that
includes it also needs winsock:

```bash
gcc -O2 -std=c11 main.c pyro_runtime.c -lm -lws2_32 -o pyrovm     # -lws2_32: Windows only
```

The runtime is written to compile under strict ISO mode. `strdup`, `_popen`,
`_pclose` and `_getpid` are POSIX/MSVCRT rather than ISO C, so `-std=c11` hides
their declarations; the runtime supplies its own `strdup` and declares the MSVCRT
ones explicitly. Leaving them implicitly declared makes them return `int`, which
silently truncates the returned pointer on any 64-bit host.

---

## 8. Parity requirements

The Go and C runtimes must be **observationally identical**; `test_c_vm.py` is the
executable form of that contract, comparing stdout, stderr and exit code across the
examples plus targeted cases. Two classes of bug are worth calling out, because
both hid from the test suite for a long time:

- **Float arithmetic must be exercised through variables.** The front-end
  constant-folds literal-only expressions, so `print(1.0 - 1.0)` never reaches the
  runtime's float path. A float `SUB` implemented as `x * y` therefore passed every
  test until the self-hosted compiler — whose IEEE-754 mantissa loop subtracts
  `1.0` once per bit — started emitting corrupt float constants.
- **Reference counting on container reads.** `index_get` returns an **owned**
  reference: the array branch must `retain`, exactly as the map branch does,
  because callers release the result. Returning a borrowed reference frees the
  element while it is still in the array.
