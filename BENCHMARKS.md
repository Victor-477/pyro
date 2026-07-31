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
