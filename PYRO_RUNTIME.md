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
- `array` and `map` compare by **reference identity** (pointer equality). Two
  containers with equal contents are **not** equal — `[1] == [1]` is false,
  `a == a` is true. This is observable, so it is the rule every backend follows
  rather than a VM detail.
- The `x == null` rule covers values that have no null to be. `""`, `0`, `0.0`,
  `false` and a struct value are all **not** null (and `!= null` is true for
  them), on every backend — including the ones where the comparison has to be
  folded at compile time because the target language would reject it (go) or
  answer differently (C, where `0 == NULL` is true).

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

### 3.1 `value_to_string` — canonical textual form

This form is **normative for every backend**, not just the two runtimes. It is the
single rendering behind `print(x)`, `to_string(x)`, `"${x}"` (which the front-end
desugars to `to_string(x)`) and string concatenation, so all four agree.

| Type | Form | Example |
|---|---|---|
| `int` | decimal, no separators | `-42` |
| `float` | shortest round-tripping form; `+Inf`, `-Inf`, `NaN` for the non-finite values | `1.5`, `2` (for `2.0`) |
| `bool` | `true` / `false` | `true` |
| `string` | the bytes themselves — **never quoted, never escaped** | `hi` |
| `null` | `null` | `null` |
| `array` | `[` elements `]`, separated by `, ` (comma **and** space); empty is `[]` | `[0, 1, 2]` |
| `map` | `{` pairs `}` where each pair is `key: value`, separated by `, `; empty is `{}` | `{a: 1, b: 2}` |
| `function` | `<fn#N>` for function-table index N | `<fn#3>` |

Rules that the container forms depend on:

- **Recursive.** Elements, keys and values are rendered by `value_to_string`
  itself, so nesting composes: `[{k: 1}]`, `{a: [3], b: [1, 2]}`.
- **Elements are not quoted.** A string inside a container renders exactly as it
  would alone, so `["x", "y"]` prints as `[x, y]`. This is deliberately *not*
  JSON — `json_encode` is the quoting-and-escaping form, and is a separate
  builtin for that reason.
- **Map pairs are ordered by the key's own textual form**, matching `keys(m)`
  (§4). The order is therefore lexicographic on the rendered key, not numeric:
  `{10: ten, 9: nine}`. Hash order must never be observable, or the same program
  prints differently on two runs of the same engine.
- **Structs render as maps**, since a struct *is* a map of string keys in the
  value model (§1); the keys are the field's original Cryo names.

> Backends that host their own value model must convert rather than lean on the
> host language's own notation. Every native form differs from this one:
> Go's `fmt.Sprint` gives `[0 1 2]`, JS `String()` gives `0,1,2` for an array and
> `[object Object]` for a map, and `console.log` gives `[ 0, 1, 2 ]`. Each of
> those was a live parity bug. `codegen_go.py` and `codegen_node.py` both emit a
> `cryoStr` helper implementing this table; `test_cli.py` asserts the three
> primary backends produce byte-identical output for a program that prints
> containers, so a fourth rendering cannot appear unnoticed.

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
| | | | | 64 | `write_file_atomic` |
| | | | | 65 | `url_decode` |
| | | | | 66 | `url_encode` |
| | | | | 67 | `asset` |
| | | | | 68 | `asset_names` |

Ids 30 and below are listed in the left three columns above; 31–68 continue here.
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

  The request map's keys are `method`, `path`, `query`, `body`, plus every
  request **header** under `header:<lowercased-name>` (`header:authorization`,
  `header:content-type`, ...). Headers share the one flat map rather than
  nesting, because the Cryo side is `map<string,string>` and a nested map
  would not survive `as map<string,string>`.

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
- **`write_file_atomic(path, content) -> bool` (11.8)** is the durable write.
  `write_file` truncates the target and *then* writes: a crash in that window
  leaves a **truncated file** — the application's whole state replaced by a
  partial one. This writes a **sibling** temp file (`path + ".tmp"`), flushes
  it, and renames it over the target, so a reader sees either the previous
  contents or the new ones and never the middle. The temp file is a sibling
  on purpose: across filesystems a rename becomes a copy, which is not atomic.
  On failure it returns false, removes the temp file and **leaves the target
  untouched**. Windows needs `MoveFileEx` rather than `rename`, which fails
  there when the target already exists. Sandbox-gated.
- **`url_decode(s)` / `url_encode(s)` (11.10)** are percent-coding for query
  strings and form bodies. `url_decode` also turns `+` into a space, and a
  **malformed escape passes through unchanged** rather than aborting — a
  server must not die on a bad request. `url_encode` leaves the unreserved
  set (`A-Za-z0-9-_.~`) alone and uppercases its hex digits.
- **`asset(name) -> string` / `asset_names() -> string[]` (11.9)** read files
  **embedded in the program**. `--assets DIR` puts every file under `DIR` into
  the `.pyro`, keyed by its path relative to that root with forward slashes;
  `pyro build --assets DIR` bakes them into a native binary, so shipping is one
  file. A missing name gives `""` rather than an error, and `asset_names()` is
  **sorted**, so every engine returns the same order. Assets are bytes, not
  text: a NUL inside one is preserved.
- `to_int`/`to_number` of a non-numeric string **abort** (fail-fast).
- **`replace(s, old, new)`** replaces all occurrences of `old` with `new`. When `old` is an empty string (`""`), `new` is inserted at every position boundary (e.g. `replace("abc", "", "-")` yields `"-a-b-c-"`, and `replace("", "", "-")` yields `"-"`).

### Loading untrusted bytecode (11.13)

The loader and the dispatch loop parse a file that may be **malformed or
hostile**, in C. They are the most exposed surface in the project, so the
contract is: **a bad `.pyro` fails with a message; it never reads out of
bounds.**

What is checked:

- **Every read is bounded** against the file size, and every length is
  validated against the bytes that actually remain *before* it drives an
  allocation — counts, string lengths, code length, debug entries, asset
  names and payloads.
- **Cross-field invariants**: the entry function index, each function's name
  index, and each function's entry offset must be inside their tables.
- **`pc` stays inside the code section**, and an instruction's operands must
  fit before the end.
- **Jump and catch targets** are computed in 64-bit and range-checked —
  `pc + rel` in `int` is undefined on overflow, and a hostile file can supply
  `rel = 0x7FFFFFFF`.
- **The four stacks are bounded** (values, frames, handlers, locals), so a
  program that pushes without popping aborts instead of running off the array.
- **`NEWARR`/`NEWMAP` counts** are checked against the current stack depth: a
  larger count made the base index negative and read *below* the stack.

**Infinite loops are out of scope.** A malformed jump can make a program loop
forever — but so can a valid one; a server's accept loop is exactly that. The
VM cannot tell them apart, so bounding execution is a quota concern, not a
memory-safety one.

`burnout/tests/test_fuzz.py` is the regression: truncations, absurd lengths
and bit flips over two seed programs, on both VMs. A crash there is a release
blocker.

### Sandbox policy

Three modes, in increasing precision:

| | |
|---|---|
| nothing set | everything allowed — the default |
| `PYRO_SANDBOX=1` | every gated native is refused |
| `PYRO_POLICY=...` | **deny by default**, grant exactly what is listed |

A policy is semicolon-separated clauses; `*` grants a whole class:

```bash
PYRO_POLICY="fs.read=./data,./config;fs.write=./data;net=api.example.com;exec=git;env=HOME"
```

| Capability | Gates |
|---|---|
| `fs.read` | `read_file`, and the directory `http_serve` publishes |
| `fs.write` | `write_bytes`, `write_file`, `write_file_atomic`, `make_dir`, `delete_file` |
| `net` | `http_get`, `http_post` (by **host**), `http_listen`, `http_serve` |
| `exec` | `exec`, by the **binary name** |
| `env` | `env`, by **variable name** |

Two properties worth stating:

- **Paths are resolved before they are compared.** A granted root of `./data`
  does not permit `./data/../secret` — the path is made absolute and `.`/`..`
  collapsed first, so a traversal cannot leave the root it was granted.
- **A refusal names what to grant**, e.g.
  `write_file() denied for data/new.txt — grant it with fs.write=data/new.txt
  in PYRO_POLICY`. A flat "blocked by sandbox policy" tells the operator
  nothing about how to proceed, which is what pushes people to disable the
  sandbox entirely.

An unknown capability is an **error**, not a silent no-op: a typo in a policy
must not quietly grant less than intended.

### Permissions declared by the program (11.12)

A program can state what it needs, and the compiler holds it to that:

```cryo
permissions {
    read  = "./data", "./config";
    write = "./data";
    net   = "api.example.com";
}
```

**Checked twice.** The compiler refuses a call to a gated builtin whose
permission was not declared — `write_file() needs the 'write' permission,
which this program does not declare` — so the mistake is caught before the
program runs. The same list is then embedded in the `.pyro` (flags bit4) and
enforced by the runtime, so it survives into the shipped artifact.

**A capability must be allowed by every active policy.** If both a declaration
and `PYRO_POLICY` are present, an operator can **narrow** what the program
asked for but never **widen** it: `PYRO_POLICY=fs.read=*` does not unlock a
path the program never declared. Without that rule the declaration would be
decoration.

The block is **opt-in** — a program without one behaves exactly as before —
and an unknown permission name is a syntax error rather than a silent no-op.

> **Not covered:** execution time and memory. A malformed or hostile program
> can still loop forever, and so can a valid one (see the note on the loader
> above), so quotas are a separate mechanism.


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

### 6.1 Resource limits

The interpreter's four stacks are **bounded, at the same sizes, in every
implementation**. The sizes come from the C VM, whose stacks are fixed arrays;
the Go VM's grow, so it must stop growing where the C VM runs out or it accepts
programs the C VM aborts on — invariant 1 broken in the direction hardest to
notice, since the engine that disagrees is the one that appears to work.

| Resource | Size | Abort message |
|---|---:|---|
| operand stack | 65536 | `malformed .pyro: value stack overflow` |
| call stack | 4096 | `malformed .pyro: call stack overflow (runaway recursion?)` |
| locals stack | 65536 | `malformed .pyro: locals stack overflow (runaway recursion?)` |
| handler stack | 4096 | `malformed .pyro: exception handler stack overflow` |

Three rules make the limits observable in the same order on both engines:

- **The call and locals stacks are checked on a call, not per instruction** —
  they only move on a call, a return and a try — and the operand stack is checked
  once per instruction, with four slots of headroom, because no instruction
  pushes more than it pops plus one.
- **The threshold is `> size - 2`, not `>= size`.** The slack matters less than
  the fact that both engines use the same expression, so they abort while pushing
  the *same* call and their stack traces are the same length.
- **Locals are checked before frames.** A recursion deep enough to trip both
  reports whichever it reaches first, and the C VM writes the callee's locals
  before it tests the frame count — so the locals limit is the earlier one.

Which limit a program reaches depends on its frame size: a function with fewer
than about 16 locals exhausts the 4096 frames first, and one with more exhausts
the 65536 local slots first. Both are reachable, and so is the operand stack,
independently of the other two — a 65535-element array literal overflows it
without exceeding 4095 frames.

> These are per-implementation limits, not language semantics: a program that
> depends on recursing deeper than 4095 is not portable across the runtimes and
> never was. Bounding the Go VM did not make such a program invalid, it made the
> two engines agree about it.

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
