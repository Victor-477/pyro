/* ============================================================
   Interactive debugger — breakpoints and single-step.

   This is built on the pc->line table the .pyro already carries (the debug
   section, flags bit1), the same one stack traces are printed from. Nothing
   about the file format changes, so any program compiled before this existed
   can be debugged.

   WHERE A BREAKPOINT IS ALLOWED TO BE

   Not at any pc. The debug section holds one entry per *statement boundary* —
   that is what the code generator emits — so those entries are exactly the
   places where stopping shows the program in a state a reader recognises.
   Stopping between the two halves of an expression would show a half-built
   operand stack and a line number that has already moved on. So `break 12`
   resolves to the debug entry for line 12 and refuses when there is none,
   naming the nearest line that does work, rather than silently arming a
   breakpoint that can never fire.

   WHAT `step` MEANS

   Line stepping, not instruction stepping — one Cryo line is many opcodes and
   stepping through them is not how anyone reads their own program. `stepi`
   is there for when the bytecode itself is the thing under suspicion.

   `next` and `finish` are defined against the FRAME DEPTH captured when the
   command was issued, not against a function identity: a recursive call to the
   function you are standing in is a deeper frame and must be stepped over like
   any other, which comparing names would get wrong.

   THE COST WHEN IT IS OFF

   One test of a package-level bool at the top of the dispatch loop, always
   false in a normal run and therefore predicted. 11.22 established that this
   loop is sensitive, so that claim is measured in Pyro/BENCHMARKS.md rather
   than asserted here.
   ============================================================ */

package main

import (
	"bufio"
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"
)

// dbgOn is read once per instruction. Keep it a plain bool: the interpreter
// is single-threaded and only the interpreter writes it.
var dbgOn bool

type stepMode int

const (
	modeRun    stepMode = iota // only breakpoints stop us
	modeLine                   // stop on the next line, entering calls
	modeOver                   // ... but not in frames deeper than stepDepth
	modeInstr                  // stop on the very next instruction
	modeOut                    // stop when the frame stack gets shallower
)

type debugger struct {
	p      *Program
	src    []string       // source lines, if --source was given
	srcName string
	lineBP map[int]bool   // breakpoints, by source line
	stops  map[int]int    // pc -> line, only for pcs that are debug entries
	byLine map[int][]int  // line -> the pcs that start it
	mode   stepMode
	depth  int // frame depth when the step command was issued
	lastLine int
	in     *bufio.Scanner
	started bool
	quit    bool
}

var dbg *debugger

func newDebugger(p *Program, source string) *debugger {
	d := &debugger{
		p:      p,
		lineBP: map[int]bool{},
		stops:  map[int]int{},
		byLine: map[int][]int{},
		mode:   modeInstr, // stop before the first instruction
		in:     bufio.NewScanner(os.Stdin),
	}
	for _, e := range p.dbg {
		if _, seen := d.stops[e.pc]; !seen {
			d.stops[e.pc] = e.line
			d.byLine[e.line] = append(d.byLine[e.line], e.pc)
		}
	}
	if source != "" {
		if b, err := os.ReadFile(source); err == nil {
			d.src = strings.Split(strings.ReplaceAll(string(b), "\r\n", "\n"), "\n")
			d.srcName = source
		} else {
			fmt.Fprintf(os.Stderr, "[pyro dbg] could not read %s: %v\n", source, err)
		}
	}
	return d
}

func (d *debugger) fnNameAt(fn int) string {
	if fn >= 0 && fn < len(d.p.funcs) {
		return d.p.funcs[fn].name
	}
	return "?"
}

// srcLine returns the text of a source line, or "" when we have no source.
func (d *debugger) srcLine(n int) string {
	if n >= 1 && n <= len(d.src) {
		return strings.TrimRight(d.src[n-1], " \t")
	}
	return ""
}

func (d *debugger) where(pc int, frames []frame) {
	line := lineAt(pc)
	fn := "?"
	if len(frames) > 0 {
		fn = d.fnNameAt(frames[len(frames)-1].fn)
	}
	if t := d.srcLine(line); t != "" {
		fmt.Printf("%s:%d  in %s\n%5d | %s\n", d.srcName, line, fn, line, t)
	} else {
		fmt.Printf("line %d  in %s  (pc %d)\n", line, fn, pc)
	}
}

func (d *debugger) list(center int) {
	if len(d.src) == 0 {
		fmt.Println("no source loaded — re-run with --source <file.cryo>")
		return
	}
	lo, hi := center-5, center+5
	if lo < 1 {
		lo = 1
	}
	if hi > len(d.src) {
		hi = len(d.src)
	}
	for n := lo; n <= hi; n++ {
		mark := "  "
		if n == center {
			mark = "->"
		}
		bp := " "
		if d.lineBP[n] {
			bp = "*"
		}
		fmt.Printf("%s%s%4d | %s\n", bp, mark, n, d.srcLine(n))
	}
}

func (d *debugger) backtrace(pc int, frames []frame) {
	for i := len(frames) - 1; i >= 0; i-- {
		at := pc
		if i != len(frames)-1 {
			at = frames[i+1].retpc
		}
		tag := "  "
		if i == len(frames)-1 {
			tag = "->"
		}
		fmt.Printf("%s #%d  %s (line %d)\n", tag, len(frames)-1-i,
			d.fnNameAt(frames[i].fn), lineAt(at))
	}
}

func (d *debugger) locals(frames []frame) {
	if len(frames) == 0 {
		return
	}
	f := frames[len(frames)-1]
	fn := d.p.funcs[f.fn]
	if len(f.locals) == 0 {
		fmt.Println("(no locals)")
		return
	}
	// The debug section carries lines, not names — see the note printed by
	// `help`. Slots are shown in declaration order, parameters first, which is
	// how the code generator assigns them.
	for i, v := range f.locals {
		kind := "local"
		if i < fn.nparams {
			kind = "param"
		}
		fmt.Printf("  [%d] %-5s = %s\n", i, kind, v.String())
	}
}

// firstStopAtOrAfter: the line of the first debug entry at or after pc. p.dbg
// is ordered by pc, so the first match is the nearest one forward.
func (d *debugger) firstStopAtOrAfter(pc int) (int, bool) {
	for _, e := range d.p.dbg {
		if e.pc >= pc {
			return e.line, true
		}
	}
	return 0, false
}

// resolveLine maps a requested source line to the pcs that begin it.
func (d *debugger) resolveLine(line int) bool {
	if len(d.byLine[line]) > 0 {
		return true
	}
	near := make([]int, 0, len(d.byLine))
	for l := range d.byLine {
		near = append(near, l)
	}
	sort.Ints(near)
	best, bestd := 0, 1<<30
	for _, l := range near {
		delta := l - line
		if delta < 0 {
			delta = -delta
		}
		if delta < bestd {
			best, bestd = l, delta
		}
	}
	if best == 0 {
		fmt.Printf("no breakpoint at line %d: this program has no debug information\n", line)
	} else {
		fmt.Printf("no statement begins at line %d — nearest is line %d\n", line, best)
	}
	return false
}

func (d *debugger) help() {
	fmt.Print(`
  break <line>      stop at a source line          delete <line>   remove it
  break <function>  stop when it is entered        delete all      remove all
  info break        list breakpoints               info locals     slots in this frame
  step   / s        run one source line, into calls
  next   / n        run one source line, over calls
  stepi  / si       run one bytecode instruction
  finish / fin      run until this function returns
  continue / c      run until a breakpoint
  backtrace / bt    the call stack                 list / l        source around here
  quit   / q        stop the program

  Locals are shown by SLOT, not by name: the .pyro debug section records
  pc->line and nothing else, so the names are not in the file to show.

`)
}

// command reads and executes commands until one of them resumes execution.
func (d *debugger) command(pc int, frames []frame) {
	for {
		fmt.Print("(pyro) ")
		if !d.in.Scan() {
			// stdin closed (piped input ran out): let the program finish
			// rather than spinning on EOF forever.
			fmt.Println("continue")
			d.mode = modeRun
			dbgOn = len(d.lineBP) > 0
			return
		}
		f := strings.Fields(strings.TrimSpace(d.in.Text()))
		if len(f) == 0 {
			continue
		}
		arg := ""
		if len(f) > 1 {
			arg = f[1]
		}
		switch f[0] {
		case "break", "b":
			d.setBreak(arg)
		case "delete", "d":
			d.delBreak(arg)
		case "info", "i":
			switch arg {
			case "locals", "l":
				d.locals(frames)
			default:
				if len(d.lineBP) == 0 {
					fmt.Println("no breakpoints")
				}
				ls := make([]int, 0, len(d.lineBP))
				for l := range d.lineBP {
					ls = append(ls, l)
				}
				sort.Ints(ls)
				for _, l := range ls {
					fmt.Printf("  breakpoint at line %d  %s\n", l, d.srcLine(l))
				}
			}
		case "step", "s":
			d.mode, d.lastLine = modeLine, lineAt(pc)
			return
		case "next", "n":
			d.mode, d.lastLine, d.depth = modeOver, lineAt(pc), len(frames)
			return
		case "stepi", "si":
			d.mode = modeInstr
			return
		case "finish", "fin":
			d.mode, d.depth = modeOut, len(frames)
			return
		case "continue", "cont", "c":
			d.mode = modeRun
			// Nothing to stop for and nothing to step: turn the per-instruction
			// check back off entirely rather than paying for it to do nothing.
			dbgOn = len(d.lineBP) > 0
			return
		case "backtrace", "bt", "where":
			d.backtrace(pc, frames)
		case "list", "l":
			d.list(lineAt(pc))
		case "quit", "q":
			d.quit = true
			return
		case "help", "h", "?":
			d.help()
		default:
			fmt.Printf("unknown command %q — try `help`\n", f[0])
		}
	}
}

func (d *debugger) setBreak(arg string) {
	if arg == "" {
		fmt.Println("usage: break <line> | break <function>")
		return
	}
	if n, err := strconv.Atoi(arg); err == nil {
		if !d.resolveLine(n) {
			return
		}
		d.lineBP[n] = true
		dbgOn = true
		fmt.Printf("breakpoint at line %d\n", n)
		return
	}
	for _, fn := range d.p.funcs {
		if fn.name != arg {
			continue
		}
		// NOT lineAt(fn.entry): lineAt walks BACKWARDS to the last debug entry
		// at or before a pc, and a function's entry pc is the start of its
		// prologue, before any statement. It therefore reports the line of
		// whatever was emitted before the function — line 0 for the first one.
		// The first statement is the first debug entry at or after the entry.
		line, ok := d.firstStopAtOrAfter(int(fn.entry))
		if !ok {
			fmt.Printf("%q has no debug information to stop at\n", arg)
			return
		}
		d.lineBP[line] = true
		dbgOn = true
		fmt.Printf("breakpoint at line %d (%s)\n", line, arg)
		return
	}
	fmt.Printf("no function named %q\n", arg)
}

func (d *debugger) delBreak(arg string) {
	if arg == "all" {
		d.lineBP = map[int]bool{}
		fmt.Println("all breakpoints removed")
	} else if n, err := strconv.Atoi(arg); err == nil {
		if d.lineBP[n] {
			delete(d.lineBP, n)
			fmt.Printf("breakpoint at line %d removed\n", n)
		} else {
			fmt.Printf("no breakpoint at line %d\n", n)
		}
	} else {
		fmt.Println("usage: delete <line> | delete all")
		return
	}
	if len(d.lineBP) == 0 && d.mode == modeRun {
		dbgOn = false
	}
}

// before is called with the pc of the instruction about to execute. It returns
// false when the user asked to quit.
func (d *debugger) before(pc int, frames []frame) bool {
	line, atStop := d.stops[pc]
	stop := false

	switch d.mode {
	case modeInstr:
		stop = true
	case modeLine:
		stop = atStop && line != d.lastLine
	case modeOver:
		stop = atStop && line != d.lastLine && len(frames) <= d.depth
	case modeOut:
		stop = atStop && len(frames) < d.depth
	}
	// A breakpoint fires regardless of the stepping mode, including in a frame
	// `next` is stepping over — otherwise stepping past a call would silently
	// skip a breakpoint inside it, which is the one thing a breakpoint must
	// never do.
	if !stop && atStop && d.lineBP[line] {
		stop = true
		fmt.Printf("\nbreakpoint hit — ")
	}
	if !stop {
		return true
	}
	if !d.started {
		d.started = true
		fmt.Printf("Pyro debugger. `help` for commands.\n")
	}
	d.where(pc, frames)
	d.command(pc, frames)
	return !d.quit
}
