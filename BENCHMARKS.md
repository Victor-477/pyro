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
| `arith` | 158 B | 297 ms | 272 ms | 1.09× |
| `calls` | 175 B | 55 ms | 48 ms | 1.13× |
| `fib` | 140 B | 31 ms | 19 ms | 1.63× |
| `array` | 179 B | 66 ms | 40 ms | 1.65× |
| `branch` | 217 B | 187 ms | 178 ms | 1.05× |
| `strings` | 140 B | 33 ms | 44 ms | 0.76× |

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
