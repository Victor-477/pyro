/* ============================================================
   Sampling profiler — time per Cryo function.

   WHY SAMPLING AND NOT COUNTING

   Counting calls is easy and answers the wrong question: a function called
   twice can dominate the run and one called a million times can be free.
   What a profile has to report is where the *time* went, and the only honest
   way to get that without changing the thing being measured is to interrupt
   at a fixed rate and ask what is running.

   HOW IT AVOIDS PAYING FOR ITSELF

   11.22 measured that this VM's dispatch loop is dominated by its
   per-instruction work, so a profiler that added a check to every instruction
   would slow down the program it is reporting on — and the numbers would then
   describe the profiler. So nothing is checked per instruction. A separate
   goroutine wakes on a ticker and reads one atomic word, which the interpreter
   writes only when the active function CHANGES, i.e. on call and return. Calls
   are rare next to instructions, and the write is guarded by profOn, so the
   cost when not profiling is a predictable, never-taken branch at four sites.

   WHAT IT REPORTS

   SELF time: the sample is attributed to the function on top of the stack, the
   one actually executing. It deliberately does not report cumulative time,
   which would need the sampler to walk the frame stack while the interpreter
   mutates it — a data race, and the reason a naive version of this reports
   garbage under load rather than failing outright.
   ============================================================ */

package main

import (
	"fmt"
	"os"
	"sort"
	"sync/atomic"
	"time"
)

var (
	profOn  bool
	profHz  = 1000
	profOut string

	// The function index currently executing. Written by the interpreter on
	// call/return, read by the sampler goroutine — atomic because those are
	// different threads and this is the only state they share.
	profCur atomic.Int32

	profStop  = make(chan struct{})
	profDone  = make(chan struct{})
	profCount []int64 // samples per function index
	profTotal int64
	profStart time.Time
	profLost  int64 // samples taken while no function was active
)

// profSet records the function now executing. Called at every point the frame
// stack changes; compiles to a test and a store, and to just the test when
// profiling is off.
func profSet(fn int) {
	if profOn {
		profCur.Store(int32(fn))
	}
}

func profBegin(nfuncs int) {
	if !profOn {
		return
	}
	profCount = make([]int64, nfuncs)
	profCur.Store(-1) // "no function yet" — see profLost
	profStart = time.Now()
	// A period rather than a ticker: if a sample is late the tick is skipped,
	// not queued, so a slow moment cannot produce a burst that misattributes
	// time to whatever happens to be running when the backlog drains.
	go func() {
		defer close(profDone)
		t := time.NewTicker(time.Second / time.Duration(profHz))
		defer t.Stop()
		for {
			select {
			case <-profStop:
				return
			case <-t.C:
				fn := int(profCur.Load())
				profTotal++
				if fn >= 0 && fn < len(profCount) {
					profCount[fn]++
				} else {
					profLost++
				}
			}
		}
	}()
}

func profEnd(funcs []Func) {
	if !profOn {
		return
	}
	elapsed := time.Since(profStart)
	close(profStop)
	<-profDone

	type row struct {
		name string
		n    int64
	}
	rows := make([]row, 0, len(profCount))
	for i, n := range profCount {
		if n == 0 {
			continue
		}
		name := "?"
		if i < len(funcs) {
			name = funcs[i].name
		}
		rows = append(rows, row{name, n})
	}
	sort.Slice(rows, func(a, b int) bool {
		if rows[a].n != rows[b].n {
			return rows[a].n > rows[b].n
		}
		return rows[a].name < rows[b].name
	})

	w := os.Stderr
	if profOut != "" {
		f, err := os.Create(profOut)
		if err != nil {
			fmt.Fprintf(os.Stderr, "[pyro prof] could not write %s: %v\n", profOut, err)
		} else {
			defer f.Close()
			w = f
		}
	}

	ms := float64(elapsed) / float64(time.Millisecond)
	fmt.Fprintf(w, "\n=== Pyro profile — %.1f ms, %d samples at %d Hz ===\n",
		ms, profTotal, profHz)
	if profTotal == 0 {
		// Saying "0 samples" and printing an empty table invites the reader to
		// conclude their program spends no time anywhere.
		fmt.Fprintf(w, "  the program finished in less than one sampling period.\n"+
			"  Re-run with a higher rate, e.g. --profile-hz=10000\n")
		return
	}
	fmt.Fprintf(w, "%10s  %7s  %9s  %s\n", "samples", "self", "self ms", "function")
	for _, r := range rows {
		pct := 100 * float64(r.n) / float64(profTotal)
		fmt.Fprintf(w, "%10d  %6.2f%%  %9.1f  %s\n",
			r.n, pct, ms*float64(r.n)/float64(profTotal), r.name)
	}
	if profLost > 0 {
		// Sampled before the entry frame existed — loading and decoding the
		// program. Shown rather than dropped, so the percentages add up.
		fmt.Fprintf(w, "%10d  %6.2f%%  %9.1f  (startup, before the program ran)\n",
			profLost, 100*float64(profLost)/float64(profTotal),
			ms*float64(profLost)/float64(profTotal))
	}
	// Natives (sleep, http, file reads) do not change the active function, so
	// time inside them lands on the Cryo function that called them. That is
	// usually what you want to know — but it means a slow `http_get` shows up
	// as a slow caller, not as a slow native.
	fmt.Fprintf(w, "\nTime in native calls is charged to the Cryo function that made them.\n")
}
