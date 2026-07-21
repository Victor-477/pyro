# Pyro — the native machine layer

**Cryo** is the high-level language. **Pyro** is the native layer that Cryo is
compiled into — the part that talks *directly to the machine*: native code,
operating-system access, and the LLM *skills* baked into the binary itself.

> Mental rule: you **write Cryo**, the compiler **generates Pyro** (native code),
> and Pyro **runs on the machine**. Cryo is the ice (ergonomic, safe); Pyro is the
> fire (fast, close to the metal).

## How the pipeline works

```
  app.cryo ──(lexer→parser→AST)──►  code generator  ──►  Pyro  ──►  binary
                                          │
                    ┌─────────────────────┼─────────────────────┐
                    ▼                     ▼                      ▼
              native Go (.go)       native C (.pyro)      x86-64 (.s)
              current base          C runtime             win64/sysv ABIs
```

The generated C artifact is named `.pyro` precisely because it is the "hot"/native
form of the program. Today the **base is the Go backend**: Cryo lowers to idiomatic
Go and uses the mature stdlib (`os`, `os/exec`, `encoding/json`, goroutines)
underneath — with no external runtime. The safety instrumentation (overflow,
division by zero, `assert`, null-safety) is inlined into the generated code itself.

## Native LLM skills (no `.md` files)

LLM tools usually describe *skills* in `SKILL.md` files (markdown with
frontmatter). For a machine language that is the opposite of optimal: it needs file
I/O and text parsing at runtime. In Pyro, a **skill is a language construct**,
compiled as a native struct inside the binary:

```cryo
skill resumir {
    desc:        "Summarize a text into objective bullets";
    model:       "gpt-x";
    temperature: 0.2;
    max_tokens:  512;
    tools:       ["contar_palavras"];
}
```

In the binary this becomes an entry in a global `map[string]Skill` registry — zero
markdown, zero file reads, O(1) introspection. Known fields (`desc`, `model`,
`tools`) become typed fields; the rest (`temperature`, `max_tokens`, …) go into a
compact `Config map[string]string`. The `Skill` type is natively JSON-serializable
(`json` tags).

### Native introspection

| Function | Returns | What it does |
|---|---|---|
| `skills()` | `string[]` | Names of all skills (sorted) |
| `skill_get(name)` | `Skill` | The skill and its configuration |
| `skill_has(name)` | `bool` | Whether the skill exists |
| `skills_json()` | `string` | Full catalog in JSON (for interop/LLM) |

```cryo
Skill s = skill_get("resumir");
print(s.desc);
print(s.model);
print(s.config["temperature"]);     // "0.2"
string catalogo = skills_json();      // export everything as JSON, no .md
```

An agent/LLM reads the catalog with `skills_json()` — a single compact value,
generated in memory from data that already lives in the binary.

## Direct machine access

Pyro exposes *builtins* that talk to the operating system (lowering to Go's
`os`/`os/exec`/`time`):

| Function | Returns | Description |
|---|---|---|
| `pyro_exec(cmd)` | `string` | Runs a shell command and returns its output (stdout+stderr). Cross-platform (`cmd /c` on Windows, `sh -c` elsewhere) |
| `pyro_env(name)` | `string` | Reads an environment variable |
| `pyro_args()` | `string[]` | Command-line arguments |
| `pyro_time()` | `int` | Current timestamp (Unix milliseconds) |
| `pyro_write(s)` | — | Writes to stdout without a newline |
| `pyro_read()` | `string` | Reads a line from stdin |
| `pyro_exit(code)` | — | Ends the process with the given code |

```cryo
string usuario = pyro_env("USERNAME");
string saida   = pyro_exec("go version");
int    agora   = pyro_time();
```

For an AI agent, `pyro_exec` is the key primitive: the "tool" a skill declares can
be a command the agent runs directly on the machine.

## Availability by backend

`skill`, the `skills*`/`skill_*` functions and the `pyro_*` builtins are features of
the **Pyro-over-Go** layer (the default backend). The `c` and `asm` backends emit a
clear error pointing to `--backend go` when they hit these constructs.

## Full example

See [`examples/example_pyro.cryo`](../examples/example_pyro.cryo): it declares two
skills, does native introspection and accesses the machine — all compiled into a
single binary, with no `.md` file at all.
