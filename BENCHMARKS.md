# Pyro VM benchmarks

Produced by `python Burnout/tests/bench_vm.py --save`. Times are the
**minimum** of several runs with process start-up subtracted; the
minimum is used because benchmark noise on a desktop is all upward.

These numbers exist so a change to the VM can be **measured** rather
than asserted (roadmap 11.22). They are not a test and never fail —
correctness is `test_c_vm.py` and `test_fuzz.py`, and a dispatch
change means nothing until both of those pass.

- machine: Windows 11, Intel64 Family 6 Model 186 Stepping 3, GenuineIntel
- python: 3.12.3
- C VM built with `-O2 -std=c11`; Go VM with `go build`

| Program | .pyro | Go VM | C VM | go/c |
|---|---:|---:|---:|---:|
| `arith` | 182 B | 232 ms | — | — |
| `calls` | 199 B | 40 ms | — | — |
| `fib` | 148 B | 24 ms | — | — |
| `array` | 211 B | 57 ms | — | — |
| `branch` | 257 B | 147 ms | — | — |
| `strings` | 156 B | 32 ms | — | — |

> **`--save` only writes the table above.** Everything below it is written by
> hand and `bench_vm.py --save` **deletes it** — the generator emits the header,
> the table and "What each program isolates", nothing else. Copy the prose out
> before running `--save` and paste it back, which is what was done for this
> revision. Fixing the generator to append rather than truncate is worth doing;
> until then, this warning is the safeguard.

### The C VM column is not from this run

There is no C toolchain on the machine that produced the table above, so
`bench_vm.py` skipped the C VM entirely. The previous record, which had both,
is kept here so the comparison is not silently lost:

| Program | .pyro | Go VM | C VM | go/c |
|---|---:|---:|---:|---:|
| `arith` | 158 B | 297 ms | 272 ms | 1.09× |
| `calls` | 175 B | 55 ms | 48 ms | 1.13× |
| `fib` | 140 B | 31 ms | 19 ms | 1.63× |
| `array` | 179 B | 66 ms | 40 ms | 1.65× |
| `branch` | 217 B | 187 ms | 178 ms | 1.05× |
| `strings` | 140 B | 33 ms | 44 ms | 0.76× |

Two reasons not to read the Go VM columns as a before/after of the 13.4 change:

- **The `.pyro` files are not the same programs.** Every one grew (158 → 182 B
  on `arith`) because the front end moved between the two records. Different
  bytecode, different work.
- **`bench_vm.py` does not warm the binaries or alternate their order**, and
  11.22 measured both of those as worth up to 15% on their own. `array` reading
  66 ms then 102 ms across the two records is that, not a regression — the A/B
  harness below, which does warm and alternate, has it at 58–66 ms on both VMs.

The tracked table is a **record of the machine at a point in time**. A change to
the VM is measured by the A/B procedure below, on one set of `.pyro` files and
two binaries built from one source tree, never by diffing two `--save` runs.

## What each program isolates

| Program | Stresses |
|---|---|
| `arith` | a tight integer loop — the densest opcode mix, where dispatch cost is most visible |
| `calls` | call overhead: frames, arguments, returns |
| `fib` | recursion — call-heavy and branch-heavy together |
| `array` | indexed reads and writes, with their bounds checks |
| `branch` | unpredictable branching, where indirect-branch prediction shows |
| `strings` | library-dominated work, as a control for how much dispatch matters at all |

## Computed-goto threading: measured, and not adopted

Roadmap 11.22 asked for threaded dispatch in the C VM. It was implemented —
`switch` kept, each case given a label beside it, `break` replaced by a
`goto *table[op]` carrying the 11.13 guards — and it passed every gate:
`test_c_vm` 57, `test_fuzz` 6, `test_aot` 22, on both the threaded and the
portable (`-DPYRO_NO_COMPUTED_GOTO`) build.

Then it was measured, and the numbers said no.

The first comparison looked like a 7–10% win, until the **Go VM — which was not
touched — appeared to improve by the same amount**. That was the machine, not
the change. Building both C VMs from one source and interleaving their runs
gives the real answer:

| Program | switch | threaded | change |
|---|---:|---:|---:|
| `arith` | 262 ms | 256 ms | −2.5% |
| `calls` | 46 ms | 45 ms | −3.0% |
| `fib` | 17 ms | 17 ms | −1.8% |
| `array` | 38 ms | 36 ms | −6.4% |
| `branch` | 170 ms | 171 ms | +0.4% |
| `strings` | 40 ms | 69 ms | **+70.9%** |
| **total** | **574 ms** | **593 ms** | **+3.3%** |

Reproduced across runs, including the `strings` regression. Inlining the
dispatch at 38 sites grows the interpreter by ~2.5 KB of text, and the
allocation-heavy string loop pays for the lost locality far more than the
arithmetic loops gain from better branch prediction.

So the change was reverted. Two things it suggests, for anyone tempted to try
again:

- The per-instruction **guards** (bounds on `pc` and the four stacks) cost more
  than the dispatch does. Making dispatch cheaper cannot help much while those
  dominate, and they are not negotiable — they are what took the fuzzer from
  249 crashes to zero.
- The folklore figure for threading comes from processors with far weaker
  indirect-branch prediction than a current one. On this machine the shared
  branch in a `switch` is predicted well enough that replicating it buys
  almost nothing.

The benchmark harness stays. It is what turned "threading is faster" from a
belief into a measurement, and the measurement was the opposite.

---

## 11.25 — what the debugger hook costs when it is off

The debugger (`--debug`) needs the interpreter to be able to stop before any
instruction, which means a check inside the dispatch loop:

```go
for {
    if dbgOn {                      // false in every normal run
        if !dbg.before(pc, frames) { return }
    }
    op := code[pc]
    ...
```

11.22 established that this loop is sensitive enough that a change intended to
speed it up made it 3.3% slower, so "one predictable branch, it's free" is a
claim that needed a number rather than an argument.

### Isolating the branch

The first comparison — the VM at `HEAD` against the VM with the debugger — said
the new one was **12% faster**, reproducibly. That is not a thing a added
branch does, and the explanation was not the branch: building the same modified
file with the hook compiled out (`if false && dbgOn`) measured *just as fast*.
The ~12% comes from code layout shifting under the other edits in `main.go`
(the option parsing, the `defer`), not from anything done on purpose. It is
noted here so nobody reads it as an optimisation and tries to keep it.

So the number below is from two binaries built from **one file in one
directory**, differing only in whether that branch is compiled in:

| Program | hook out | hook in | change |
|---|---:|---:|---:|
| `arith` | 252 ms | 257 ms | +2.0% |
| `calls` | 43 ms | 44 ms | +2.3% |
| `fib` | 29 ms | 28 ms | −2.1% |
| `array` | 72 ms | 68 ms | −4.4% |
| `branch` | 159 ms | 172 ms | +8.1% |
| `strings` | 31 ms | 30 ms | −4.7% |
| **total** | **585 ms** | **599 ms** | **+2.3%** |

Run again with the two binaries in the opposite slots, the total came out at
**−0.4%** — the sign flipped. So the honest reading is **no cost measurable
above the noise**, not "+2.3%".

### The noise floor, measured rather than assumed

Two checks were needed before any of the above meant anything:

- **Order bias.** Timing A then B every repetition gives B a warmed file cache;
  that alone was worth ~15%, enough to manufacture a speedup from nothing. The
  harness alternates which binary runs first.
- **A/A control.** The same binary against itself, alternating, still reports
  **−3.1%** in favour of the second slot. That is the floor: any result inside
  ±3% on this machine is not a result. It is why +2.3% is reported as "no
  measurable cost" instead of a regression.

`continue` with no breakpoints set clears `dbgOn`, so even a debugged program
runs at full speed once it is past the part being looked at.

The sampling profiler (`--profile`) adds nothing to this loop at all: the
sampler is a separate goroutine reading one atomic word, and the interpreter
writes that word only on call and return.

---

## 11.22, second attempt — the guards, not the dispatch

The first attempt made *dispatch* cheaper (computed goto) and measured 3.3%
slower. The note it left behind was that the per-instruction **guards** cost
more than the dispatch they protect, so that is where this attempt went.

### First: how much is even available?

Before optimising anything, all three per-instruction guards were removed —
unsafe, purely diagnostic — to put a ceiling on the exercise:

| | min | median |
|---|---:|---:|
| every guard removed vs `HEAD` | −7.5% | −7.3% |

So the guards cost about 7%, and no amount of reorganising them can beat that.
Worth knowing before starting rather than after.

### What shipped

`fp` and `hp` were checked on **every instruction**, but the frame stack only
moves on `CALL`/`CALLVALUE`/`RET` and the handler stack only on
`TRYPUSH`/`TRYPOP`. Four comparisons and two loads were being paid on every
`ADD` and `LOAD` to re-verify something that had not changed. They are now
checked at those opcodes. The `pc` and `sp` checks each became one unsigned
comparison instead of two — a negative int cast to unsigned wraps to something
huge, so the upper-bound test rejects it on its own.

| Program | `HEAD` | guards moved | change |
|---|---:|---:|---:|
| `arith` | 289 ms | 269 ms | −6.8% |
| `calls` | 55 ms | 52 ms | −5.4% |
| `fib` | 24 ms | 25 ms | +4.1% |
| `array` | 45 ms | 44 ms | −2.6% |
| `branch` | 195 ms | 189 ms | −3.0% |
| `strings` | 66 ms | 64 ms | −2.6% |
| **total** | **674 ms** | **643 ms** | **−4.5%** |

On a single 12M-iteration arithmetic loop, where fixed costs are negligible:
**−3.6% min, −4.2% median**, and **+3.5% / +4.6% with the binaries swapped** —
the sign follows the change, which is what separates a result from noise.

`fib` going the other way is the noise floor talking: it is a 24 ms program and
the A/A control swings ±8% on the short ones.

### Rejected on measurement: removing the `pc` guard entirely

It looked like the best idea in the file. Every write to `pc` is *already*
validated — a function entry against `codelen` at load, a jump through
`pyro_jump_to`, a return to a pc that was valid when saved — so `pc` can only
leave the code section by advancing off the end, which 8 bytes of sentinel
padding absorbs. Implemented, fuzzed clean, parity clean… and **+1.7% min,
+2.3% median. Slower.** It was reverted.

Measured alone, back to back, against a 0.1% floor:

| variant | min | median |
|---|---:|---:|
| A/A control | −0.02% | +0.11% |
| `pc` padding only | +1.72% | +2.30% |
| `fp`/`hp` moved only | **−2.70%** | **−3.88%** |
| both together | −1.97% | −2.82% |

Both together is *worse* than the guard move alone — the padding gives back
part of the win. Two changes that each look right can fight.

### The methodology cost more than the change

An early A/A control between two copies of one binary reported **−7.5%**, and
the same binary against its own path reported **−0.7%**. The difference is
entirely first-touch cost — page cache, and on Windows an on-access scan —
which lands on whichever file is timed first and which `min()` cannot remove,
because it is paid once before the first sample rather than spread across them.
The harness now warms both binaries on each program before timing.

Under that broken floor, the guard move measured "no change" and the padding
measured "a win". Both readings were wrong, and both were reversed once the
floor was fixed. **The first thing to measure is the measurement.**

Correctness is unchanged and was checked before speed: `test_fuzz.py` 888
malformed inputs across both VMs with no crash, `test_c_vm.py` 57 parity cases
including abort messages, `test_aot.py` 22.

## Where the VM's time actually goes (roadmap 13.4)

11.22 built computed-goto threading, measured it **3% slower** and reverted it.
That was a guess at the dispatch loop. This is a measurement of it.

Capture a CPU profile of the interpreter itself — not of the Cryo program,
which is what `--profile` (11.25) samples:

```bash
pyrovm --cpuprofile=arith.prof arith.pyro
go tool pprof -top pyrovm.exe arith.prof
```

On `arith`, the densest opcode mix in the suite:

| Function | flat | what it is |
|---|---|---|
| `main.run` | 65% | the dispatch loop and the opcode bodies |
| `main.run.func2` | 14% | `pop` |
| `main.run.func1` | 7% | `push` |
| `main.binOp` | 7% | the arithmetic itself |

**One fifth of the time is operand-stack traffic**, and only 7% is the
arithmetic those instructions exist to perform. The stack is a Go slice and
every `push` is an `append` with a capacity check, every `pop` a re-slice.

That is the answer to why 11.22's result was negative: threading attacks the
*selection* of the next opcode, and selection is not where the time is. A
pre-sized stack with index arithmetic — no append, no capacity check — targets
the 20%, and can be measured against these numbers rather than assumed.

Note `fib` is too short to sample at the default rate (30ms, zero samples).
Profiling it needs a larger iteration count; the numbers above are `arith`.

## 13.4 — the operand stack: measured, and adopted

The profile above said one fifth of the VM's time was operand-stack traffic and
7% was the arithmetic. This is the change that acts on it, and the numbers that
decided its shape.

### What changed

`run()`'s operand stack was a `[]Value` grown with `append`, behind two
closures:

```go
var stack []Value
push := func(v Value) { stack = append(stack, v) }
pop  := func() Value  { n := len(stack) - 1; v := stack[n]; stack = stack[:n]; return v }
```

Every push was a capacity test, a possible `growslice`, and a slice-header
write-back; every pop a re-slice. Both went through an indirect call the
compiler cannot inline — `save`/`load` capture them, so the closures and the
stack live on the heap, which is also why they showed up in the profile as
`main.run.func1` and `main.run.func2` rather than being folded into `main.run`.

It is now a slice indexed by an integer `sp`, written out at all 31 use sites:
`stack[sp] = v; sp++` and `sp--; v := stack[sp]`. No closures, no append, no
slice-header traffic.

### The first version was wrong, and the control caught it

Pre-sizing to the full 65536 slots up front measured **−12.4%** overall — and
**+10% on `strings`**, the allocation-heavy program, reproducibly and well
outside its noise band.

`Value` holds a string, a slice pointer and a map, so 65536 of them is 4 MB that
the garbage collector must scan on every cycle whether the program uses two
slots or twenty thousand. `GODEBUG=gctrace=1` on `strings`:

| | GC cycles | GC share of runtime | live heap |
|---|---:|---:|---:|
| append (before) | 69 | 4% | ~1 MB |
| pre-sized 65536 | 35 | **13%** | ~5–6 MB |

Fewer collections, each far more expensive. So the stack starts at 256 slots and
doubles, capped at 65536, with one headroom check per instruction — the same
place and shape the C VM has always had it. `strings` returns to its noise band
and nothing else regresses.

### The floor, measured first

Two copies of one binary, alternating, both warmed on each program before any
sample is taken:

| A/A control | total, min | total, median |
|---|---:|---:|
| `new.exe` vs an identical copy | +0.73% | +1.03% |

Per program the floor is wider than the total — ±2.7% on `array`, ±5% on
`strings` — because the short programs swing more. Anything inside those bands
is not a result.

### The result

Same six `.pyro` files, two binaries built from one source tree each, 11
alternating reps, start-up subtracted:

| Program | append | pre-sized + `sp` | min | median |
|---|---:|---:|---:|---:|
| `arith` | 291 ms | 255 ms | **−12.2%** | −3.1% |
| `calls` | 51 ms | 44 ms | **−13.7%** | −16.3% |
| `fib` | 27 ms | 26 ms | −4.4% | −6.6% |
| `array` | 65 ms | 58 ms | **−10.7%** | −6.1% |
| `branch` | 178 ms | 156 ms | **−12.5%** | −15.6% |
| `strings` | 36 ms | 37 ms | +3.3% | +7.2% |
| **total** | **647 ms** | **576 ms** | **−11.0%** | −7.4% |

And with the binaries in the opposite slots, which is what separates a result
from an artefact of position:

| | total, min | total, median |
|---|---:|---:|
| old in slot A, new in slot B | −11.0% | −7.4% |
| new in slot A, old in slot B | **+11.7%** | +9.2% |

The sign follows the change, the magnitude is stable across orientations, and
both are an order of magnitude outside the ±1% floor. `strings` is the one
program that does not move consistently: +3.3% in one orientation and −7.3% in
the other, which is noise rather than a small regression.

**Roughly 11% faster overall**, and it is the first of the three attempts on
this loop to survive its own control. 11.22's threading attacked instruction
*selection* and lost 3%; the guard reshuffle won 4.5%; this attacks the operand
traffic the profile actually pointed at.

### One behaviour change, deliberate

The Go VM used to grow its operand stack without limit. It now aborts at 65532
slots with the C VM's own wording:

```
[Pyro VM] malformed .pyro: value stack overflow
```

That is a **parity fix, not a new limit**. `static Value stack[65536]` and the
guard against it have been in `Pyro/vm/main.c` all along, so a program deep
enough to hit this already aborted on the C VM and the AOT while the Go VM
carried on — invariant 1 broken in the direction that is hardest to notice,
since the engine that disagrees is the one that appears to work. A
200,000-deep recursion is the shape that reaches it.

**This has not been run against the C VM.** There is no C toolchain on this
machine, so `test_c_vm.py` exits early and the matching abort text is verified
by reading `main.c`, not by executing it. It needs a machine with gcc before the
parity claim is more than an argument.

### Correctness, checked before speed

`test_smoke` 557, `test_parity` 200, `test_selfhost` 101, `test_bootstrap` 6,
`test_examples` 43, `test_concurrency` 33, `test_fuzz` 6, `test_aot` 11, and
`difftest.py --runs 60` at 60/60 agreeing across pyro/node/go.

`test_bootstrap` is the load-bearing one: the self-hosted compiler's fixed point
runs entirely on this interpreter, so a stack-indexing error that only appeared
under one push/pop sequence would break it. `test_concurrency` matters for a
second reason — each task owns its own operand stack, and `save`/`load` now
carry `sp` alongside it.

`test_c_vm.py` is the gate this change most wants and the one that could not
run.

### Reproducing

`bench_vm.py` cannot answer this question — it neither warms the binaries nor
alternates their order, and diffing two `--save` runs compares two
machines-in-time rather than two versions of the code. The A/B harness is
`Burnout/tests/bench_ab.py`, added with this change:

```bash
python Burnout/tests/bench_ab.py                    # HEAD vs the working tree
python Burnout/tests/bench_ab.py --only arith --reps 21
```

It builds both sides from their own directory, compiles one set of `.pyro`
files that both then run, warms each binary on each program, alternates which
side goes first, subtracts start-up, and reports the A/A control plus **both**
A/B orientations. It ends by saying which of the three readings you have:
inside the floor (not a result), sign did not flip (position, not code), or a
real change.

Run the A/A control first and believe it. An A/A that does not come back near
zero means the measurement is broken and every number taken under it is
worthless — which is exactly what happened in 11.22, twice.

### Re-verified 2026-08-11 — the floor, and what could not be re-run

The change was re-checked on the same machine. **The A/A control reproduces**,
which is the reading that licenses every other number here:

| A/A control, 11 reps | total, min | total, median |
|---|---:|---:|
| working tree vs an identical copy | **−1.02%** | −0.78% |

Same ~1% band as the original run (+0.73% / +1.03%); the sign of a floor is
meaningless, its magnitude is the point. Per program it is again wider than the
total — −4.7% `calls`, −4.1% `fib`, −3.6% `strings`, under ±1% on the three long
ones — so the short programs still cannot carry a result on their own. The
tracked **−11.0%** is an order of magnitude outside this band and stands.

The table at the top of this file was regenerated the same day. Every program
is at or below its previous record (`arith` 261→232 ms, `array` 102→57,
`branch` 172→147, `fib` 28→24, `calls` 44→40, `strings` 38→32) — but that
comparison is worth **nothing** on its own and is recorded only because the
table is dated: two `--save` runs are two machines-in-time, which is the exact
mistake `bench_ab.py` exists to prevent. It is consistent with the optimisation
being in place; it is not evidence of it.

**What could not be re-run:** the A/B legs. `bench_ab.py` builds side A with
`git show <ref>:vm/main.go`, and this working copy is not a git repository, so
the pre-13.4 `main.go` is not reachable and the −11.0% / +11.7% pair was not
re-derived. Reconstructing the append-and-closure version by hand to stand in
for it was rejected: a stand-in that is not the historical code gives a number
that looks like a measurement and is not one, which is worse than the gap.

Correctness was re-run in full: `test_smoke` 557 pass, `test_fuzz` 6 pass,
`test_concurrency` 33 pass. **`test_c_vm` still skips** — no C toolchain on this
machine — so the 65532-slot overflow abort that this change added to match
`main.c` remains unexecuted against the C VM, exactly as flagged above.
