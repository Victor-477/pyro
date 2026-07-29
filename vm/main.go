// ============================================================
//  Pyro VM — executes the custom target language .pyro (bytecode)
//
//  The .pyro is NOT Go/C/asm: it is Pyro's custom bytecode, with an
//  instruction set (ISA) invented in the project. This VM,
//  stack-based, loads the file and executes it on the machine.
//
//  Usage: pyrovm program.pyro
// ============================================================
package main

import (
	"bufio"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"net"
	"net/http"
	"os/exec"
	"runtime"
	"os"
	"reflect"
	"sort"
	"strconv"
	"strings"
	"time"
)

// HTTP client with timeout, shared by network natives
var httpClient = &http.Client{Timeout: 15 * time.Second}

var stdin = bufio.NewReader(os.Stdin)

// ── HTTP server (roadmap 11.6) ─────────────────────────────
// One listener and one in-flight connection: requests are served strictly one
// at a time, which is what makes this safe without locking the VM. Must mirror
// pyro_runtime.c exactly, including the request map's keys.
// 11.9 — assets embedded in the .pyro, name -> contents.
var assets = map[string]string{}

var httpListener net.Listener
var httpConn net.Conn

// httpParseRequest reads one HTTP/1.1 request and splits it into the four
// fields the request map exposes. Deliberately minimal and written to behave
// identically to the C implementation.
func httpParseRequest(c net.Conn) (method, path, query, body string, hdrs map[string]string, ok bool) {
	hdrs = map[string]string{}
	r := bufio.NewReader(c)
	line, err := r.ReadString('\n')
	if err != nil {
		return "", "", "", "", hdrs, false
	}
	parts := strings.Fields(strings.TrimSpace(line))
	if len(parts) < 2 {
		return "", "", "", "", hdrs, false
	}
	method, target := parts[0], parts[1]
	if i := strings.IndexByte(target, '?'); i >= 0 {
		path, query = target[:i], target[i+1:]
	} else {
		path = target
	}
	length := 0
	for {
		h, err := r.ReadString('\n')
		if err != nil {
			return "", "", "", "", hdrs, false
		}
		h = strings.TrimSpace(h)
		if h == "" {
			break
		}
		if k, v, found := strings.Cut(h, ":"); found {
			name := strings.ToLower(strings.TrimSpace(k))
			val := strings.TrimSpace(v)
			hdrs[name] = val
			if name == "content-length" {
				length, _ = strconv.Atoi(val)
			}
		}
	}
	if length > 0 {
		buf := make([]byte, length)
		if _, err := io.ReadFull(r, buf); err != nil {
			return "", "", "", "", hdrs, false
		}
		body = string(buf)
	}
	return method, path, query, body, hdrs, true
}

// unhex: one hex digit -> its value. Shared by url_decode so the two
// engines cannot drift on what counts as a valid escape.
func unhex(c byte) (byte, bool) {
	switch {
	case c >= '0' && c <= '9':
		return c - '0', true
	case c >= 'a' && c <= 'f':
		return c - 'a' + 10, true
	case c >= 'A' && c <= 'F':
		return c - 'A' + 10, true
	}
	return 0, false
}

func httpStatusText(code int64) string {
	switch code {
	case 200:
		return "OK"
	case 201:
		return "Created"
	case 204:
		return "No Content"
	case 400:
		return "Bad Request"
	case 401:
		return "Unauthorized"
	case 403:
		return "Forbidden"
	case 404:
		return "Not Found"
	case 405:
		return "Method Not Allowed"
	case 500:
		return "Internal Server Error"
	default:
		return "Status"
	}
}

// ── Opcodes (mirrors burnout/codegen_pyro.py) ──────────────
const (
	opHALT    = 0x00
	opCONST   = 0x01
	opTRUE    = 0x02
	opFALSE   = 0x03
	opNULL    = 0x04
	opPOP     = 0x05
	opLOAD    = 0x06
	opSTORE   = 0x07
	opADD     = 0x10
	opSUB     = 0x11
	opMUL     = 0x12
	opDIV     = 0x13
	opMOD     = 0x14
	opNEG     = 0x15
	opBAND    = 0x16
	opBOR     = 0x17
	opBXOR    = 0x18
	opSHL     = 0x19
	opSHR     = 0x1A
	opBNOT    = 0x1B
	opEQ      = 0x20
	opNE      = 0x21
	opLT      = 0x22
	opGT      = 0x23
	opLE      = 0x24
	opGE      = 0x25
	opNOT     = 0x26
	opJMP     = 0x30
	opJMPF    = 0x31
	opJMPT    = 0x32
	opCALL    = 0x40
	opRET     = 0x41
	opPUSHFN     = 0x42 // u16 funcidx -> push a function value
	opCALLVALUE  = 0x43 // u8 argc -> call the function value beneath the args
	opCLOSURE    = 0x44 // u16 funcidx, u8 ncap -> pop ncap values into a closure
	opPRINT   = 0x50
	opASSERT  = 0x51
	opPRINTLN = 0x52
	opNEWARR  = 0x60
	opNEWMAP  = 0x61
	opINDEX   = 0x62
	opSETIDX  = 0x63
	opLEN     = 0x64
	opAPPEND  = 0x65
	opHAS     = 0x66
	opKEYS    = 0x67
	opNATIVE   = 0x70 // u8 id, u8 argc — native builtin (table in native())
	opTRYPUSH  = 0x71 // i16 rel (catch), u16 slot (catch var; 0xFFFF = none)
	opTRYPOP   = 0x72 // removes the exception handler from the top
	opTHROW    = 0x73 // pop value -> unwinds to the nearest handler
	opCOALESCE = 0x74 // pop b, a -> a if a != null, else b (??)
	opUNWRAP   = 0x75 // pop a -> a if a != null, else aborts (x!)
	// Roadmap 11.1 — module state. Top-level `var` declarations live in a
	// globals array instead of being locals of main, which is what lets a
	// function read and assign them. Operand is a u16 slot index.
	opGETGLOBAL = 0x76
	opSETGLOBAL = 0x77
)

const noSlot = 0xFFFF

// value types
const (
	kInt = iota
	kFloat
	kBool
	kStr
	kNull
	kArray // *[]Value (reference: push/setidx mutate the shared array)
	kMap   // map[any]Value (structs also use this type)
	kFunc  // first-class function value (i = function index)
)

type Value struct {
	k   byte
	i   int64
	f   float64
	b   bool
	s   string
	arr *[]Value
	m   map[any]Value
}

func vInt(i int64) Value      { return Value{k: kInt, i: i} }
func vFloat(f float64) Value  { return Value{k: kFloat, f: f} }
func vBool(b bool) Value      { return Value{k: kBool, b: b} }
func vStr(s string) Value     { return Value{k: kStr, s: s} }
func vNull() Value            { return Value{k: kNull} }
func vArr(a []Value) Value    { return Value{k: kArray, arr: &a} }
func vMap(m map[any]Value) Value { return Value{k: kMap, m: m} }
func vFunc(idx int64) Value    { return Value{k: kFunc, i: idx} }

// vClosure: a function value carrying captured values (captured BY VALUE).
// They are copied into the leading locals of the callee by opCALLVALUE.
func vClosure(idx int64, captured []Value) Value {
	return Value{k: kFunc, i: idx, arr: &captured}
}

// comparable key (Go) from a Value
func keyOf(v Value) any {
	switch v.k {
	case kInt:
		return v.i
	case kStr:
		return v.s
	case kBool:
		return v.b
	case kFloat:
		return v.f
	default:
		return nil
	}
}

// rebuilds a Value from a map key
func keyToValue(k any) Value {
	switch t := k.(type) {
	case int64:
		return vInt(t)
	case string:
		return vStr(t)
	case bool:
		return vBool(t)
	case float64:
		return vFloat(t)
	default:
		return vNull()
	}
}

func (v Value) truthy() bool {
	switch v.k {
	case kBool:
		return v.b
	case kInt:
		return v.i != 0
	case kFloat:
		return v.f != 0
	case kStr:
		return v.s != ""
	case kArray:
		return len(*v.arr) > 0
	case kMap:
		return len(v.m) > 0
	case kFunc:
		return true
	default:
		return false
	}
}

func (v Value) asFloat() float64 {
	if v.k == kFloat {
		return v.f
	}
	return float64(v.i)
}

// nativeInt reads a Value as int64 for integer-only natives (gcd, repeat count).
func nativeInt(v Value) int64 {
	if v.k == kInt {
		return v.i
	}
	return int64(v.asFloat())
}

// nativePad implements pad_start/pad_end with JS padStart/padEnd semantics:
// the pad string is repeated and truncated to fill exactly (width-len) bytes.
func nativePad(s string, width int, pad string, atStart bool) string {
	if len(s) >= width || pad == "" {
		return s
	}
	need := width - len(s)
	var b strings.Builder
	for b.Len() < need {
		b.WriteString(pad)
	}
	filler := b.String()[:need]
	if atStart {
		return filler + s
	}
	return s + filler
}

// nativeLess is the total order used by sort(): numbers compare numerically,
// everything else by its string form. Deterministic and identical to the C VM.
func nativeLess(a, b Value) bool {
	an := a.k == kInt || a.k == kFloat
	bn := b.k == kInt || b.k == kFloat
	if an && bn {
		return a.asFloat() < b.asFloat()
	}
	return a.String() < b.String()
}

func (v Value) String() string {
	switch v.k {
	case kInt:
		return strconv.FormatInt(v.i, 10)
	case kFloat:
		return strconv.FormatFloat(v.f, 'g', -1, 64)
	case kBool:
		if v.b {
			return "true"
		}
		return "false"
	case kStr:
		return v.s
	case kArray:
		parts := make([]string, len(*v.arr))
		for i, e := range *v.arr {
			parts[i] = e.String()
		}
		return "[" + join(parts, ", ") + "]"
	case kMap:
		ks := mapKeysSorted(v.m)
		parts := make([]string, len(ks))
		for i, k := range ks {
			parts[i] = keyToValue(k).String() + ": " + v.m[k].String()
		}
		return "{" + join(parts, ", ") + "}"
	case kFunc:
		return "<fn#" + strconv.FormatInt(v.i, 10) + ">"
	default:
		return "null"
	}
}

func join(parts []string, sep string) string {
	out := ""
	for i, p := range parts {
		if i > 0 {
			out += sep
		}
		out += p
	}
	return out
}

// sorted map keys (deterministic output)
func mapKeysSorted(m map[any]Value) []any {
	ks := make([]any, 0, len(m))
	for k := range m {
		ks = append(ks, k)
	}
	sort.Slice(ks, func(i, j int) bool {
		return keyToValue(ks[i]).String() < keyToValue(ks[j]).String()
	})
	return ks
}

type Func struct {
	name    string
	entry   uint32
	nparams int
	nlocals int
}

type dbgEntry struct {
	pc   int
	line int
}

type Program struct {
	consts  []Value
	funcs   []Func
	entryFn int
	code    []byte
	dbg     []dbgEntry // pc -> line (ordered by pc); empty if no debug
}

type frame struct {
	retpc  int
	locals []Value
	fn     int // function index (for stack trace)
}

// ── debug state (for stack traces in fatal) ────────
var (
	dbgLines  []dbgEntry
	dbgFuncs  []Func
	dbgFrames *[]frame
	dbgPC     *int
)

// lineAt: largest entry with pc <= target (binary search).
func lineAt(pc int) int {
	lo, hi, ans := 0, len(dbgLines)-1, 0
	for lo <= hi {
		mid := (lo + hi) / 2
		if dbgLines[mid].pc <= pc {
			ans = dbgLines[mid].line
			lo = mid + 1
		} else {
			hi = mid - 1
		}
	}
	return ans
}

// stackTrace: active call stack with function name and line.
func stackTrace() string {
	if len(dbgLines) == 0 || dbgFrames == nil || dbgPC == nil {
		return ""
	}
	fr := *dbgFrames
	var b strings.Builder
	b.WriteString("  stack trace (most recent first):\n")
	for i := len(fr) - 1; i >= 0; i-- {
		// where this frame is paused: the top is at the current pc; the
		// others, at the return address of the frame they called.
		var at int
		if i == len(fr)-1 {
			at = *dbgPC
		} else {
			at = fr[i+1].retpc
		}
		name := "?"
		if fr[i].fn >= 0 && fr[i].fn < len(dbgFuncs) {
			name = dbgFuncs[fr[i].fn].name
		}
		fmt.Fprintf(&b, "    at %s (line %d)\n", name, lineAt(at))
	}
	return strings.TrimRight(b.String(), "\n")
}

func fatal(msg string) {
	fmt.Fprintln(os.Stderr, "[Pyro VM] "+msg)
	if tr := stackTrace(); tr != "" {
		fmt.Fprintln(os.Stderr, tr)
	}
	os.Exit(1)
}

// ── loading the .pyro ───────────────────────────────────

func load(data []byte) *Program {
	if len(data) < 6 || string(data[0:4]) != "PYRO" {
		fatal("invalid .pyro file (magic)")
	}
	// v3 widened the string-constant length u16 -> u32; v2 is still accepted.
	ver := data[4]
	if ver != 2 && ver != 3 {
		fatal("unsupported .pyro version (expected v2 or v3)")
	}
	flags := data[5]
	p := &Program{}
	pos := 6
	rd16 := func() int { v := int(binary.LittleEndian.Uint16(data[pos:])); pos += 2; return v }
	rd32 := func() uint32 { v := binary.LittleEndian.Uint32(data[pos:]); pos += 4; return v }

	nconsts := rd16()
	p.consts = make([]Value, nconsts)
	for i := 0; i < nconsts; i++ {
		tag := data[pos]
		pos++
		switch tag {
		case 1:
			p.consts[i] = vInt(int64(binary.LittleEndian.Uint64(data[pos:])))
			pos += 8
		case 2:
			p.consts[i] = vFloat(math.Float64frombits(binary.LittleEndian.Uint64(data[pos:])))
			pos += 8
		case 3:
			// v3 stores the length as u32; v2 as u16.
			n := 0
			if ver >= 3 {
				n = int(rd32())
			} else {
				n = rd16()
			}
			p.consts[i] = vStr(string(data[pos : pos+n]))
			pos += n
		case 4:
			p.consts[i] = vBool(data[pos] != 0)
			pos++
		default:
			fatal("unknown constant tag")
		}
	}
	nfuncs := rd16()
	p.funcs = make([]Func, nfuncs)
	for i := 0; i < nfuncs; i++ {
		nameidx := rd16()
		entry := rd32()
		nparams := int(data[pos])
		pos++
		nlocals := rd16()
		p.funcs[i] = Func{name: p.consts[nameidx].s, entry: entry, nparams: nparams, nlocals: nlocals}
	}
	p.entryFn = rd16()
	codelen := int(rd32())
	code := make([]byte, codelen)
	copy(code, data[pos:pos+codelen])
	pos += codelen
	if flags&0x01 != 0 {
		xorDecode(code)
	}
	p.code = code
	// sandbox policy recorded in the artifact (bit2): the VM refuses
	// network/machine natives. Can also be enabled at runtime
	// via PYRO_SANDBOX=1 (never disables what the artifact required).
	if flags&0x04 != 0 {
		sandboxed = true
	}
	// 11.9 — embedded assets, if present. Read AFTER the debug section, in
	// the same order the generator writes them.
	defer func() {
		if flags&0x08 == 0 {
			return
		}
		n := int(rd32())
		for i := 0; i < n; i++ {
			nl := int(rd32())
			name := string(data[pos : pos+nl])
			pos += nl
			dl := int(rd32())
			assets[name] = string(data[pos : pos+dl])
			pos += dl
		}
	}()

	// debug section (pc -> line), if present
	if flags&0x02 != 0 {
		ndbg := int(rd32())
		p.dbg = make([]dbgEntry, ndbg)
		for i := 0; i < ndbg; i++ {
			pc := int(rd32())
			line := int(rd32())
			p.dbg[i] = dbgEntry{pc: pc, line: line}
		}
	}
	return p
}

// inverse of the generator's rolling XOR (light obfuscation, not strong crypto)
func xorDecode(code []byte) {
	k := byte(0x5A)
	for i := range code {
		b := code[i] ^ k
		code[i] = b
		k = byte((int(k)*31 + 7 + int(b)) & 0xFF)
	}
}

// ── container operations ──────────────────────────────────

func lengthOf(v Value) int64 {
	switch v.k {
	case kStr:
		return int64(len(v.s))
	case kArray:
		return int64(len(*v.arr))
	case kMap:
		return int64(len(v.m))
	default:
		fatal("len() applied to a value without length")
		return 0
	}
}

func indexGet(cont, key Value) Value {
	switch cont.k {
	case kArray:
		idx := key.i
		if idx < 0 || idx >= int64(len(*cont.arr)) {
			fatal(fmt.Sprintf("[Cryo Security] IndexError: index %d out of bounds (len=%d)", idx, len(*cont.arr)))
		}
		return (*cont.arr)[idx]
	case kMap:
		if v, ok := cont.m[keyOf(key)]; ok {
			return v
		}
		return vNull()
	case kStr:
		idx := key.i
		if idx < 0 || idx >= int64(len(cont.s)) {
			fatal("[Cryo Security] IndexError: string index out of bounds")
		}
		return vStr(string(cont.s[idx]))
	default:
		fatal("indexing a non-indexable value")
		return vNull()
	}
}

func indexSet(cont, key, val Value) {
	switch cont.k {
	case kArray:
		idx := key.i
		if idx < 0 || idx >= int64(len(*cont.arr)) {
			fatal(fmt.Sprintf("[Cryo Security] IndexError: index %d out of bounds", idx))
		}
		(*cont.arr)[idx] = val
	case kMap:
		cont.m[keyOf(key)] = val
	default:
		fatal("indexed assignment on a non-indexable value")
	}
}

// ── execution ────────────────────────────────────────────────

func run(p *Program) {
	code := p.code
	var stack []Value
	push := func(v Value) { stack = append(stack, v) }
	pop := func() Value {
		n := len(stack) - 1
		v := stack[n]
		stack = stack[:n]
		return v
	}

	main := p.funcs[p.entryFn]
	frames := []frame{{retpc: -1, locals: make([]Value, main.nlocals), fn: p.entryFn}}
	pc := int(main.entry)

	// Roadmap 11.1 — module state, shared by every frame. Grown on demand by
	// opSETGLOBAL so the .pyro container needs no globals count and v3 files
	// keep loading unchanged.
	var globals []Value

	// debug state accessible by fatal() (stack trace)
	dbgLines = p.dbg
	dbgFuncs = p.funcs
	dbgFrames = &frames
	dbgPC = &pc

	// exception handlers stack (try/catch)
	type handler struct {
		catchPC int
		sp      int // operand stack depth upon entering try
		fp      int // frame stack depth
		slot    int // catch variable slot (noSlot = none)
	}
	var handlers []handler

	rd16 := func() int { v := int(binary.LittleEndian.Uint16(code[pc:])); pc += 2; return v }
	rdi32 := func() int { v := int(int32(binary.LittleEndian.Uint32(code[pc:]))); pc += 4; return v }

	// raise: unwinds to the nearest handler; returns false if none.
	raise := func(v Value) bool {
		if len(handlers) == 0 {
			return false
		}
		h := handlers[len(handlers)-1]
		handlers = handlers[:len(handlers)-1]
		if h.sp <= len(stack) {
			stack = stack[:h.sp]
		}
		frames = frames[:h.fp]
		if h.slot != noSlot {
			frames[len(frames)-1].locals[h.slot] = v
		}
		pc = h.catchPC
		return true
	}

	for {
		op := code[pc]
		pc++
		switch op {
		case opHALT:
			return
		case opCONST:
			push(p.consts[rd16()])
		case opTRUE:
			push(vBool(true))
		case opFALSE:
			push(vBool(false))
		case opNULL:
			push(vNull())
		case opPOP:
			pop()
		case opLOAD:
			push(frames[len(frames)-1].locals[rd16()])
		case opSTORE:
			frames[len(frames)-1].locals[rd16()] = pop()
		case opGETGLOBAL:
			i := int(rd16())
			if i >= len(globals) {
				// Reading before the initialiser ran. The compiler emits the
				// initialiser first, so this is defensive rather than reachable.
				push(Value{k: kNull})
			} else {
				push(globals[i])
			}
		case opSETGLOBAL:
			i := int(rd16())
			// Grown on demand: the container carries no globals count, so v3
			// files keep loading unchanged.
			for len(globals) <= i {
				globals = append(globals, Value{k: kNull})
			}
			globals[i] = pop()
		case opADD, opSUB, opMUL, opDIV, opMOD,
			opBAND, opBOR, opBXOR, opSHL, opSHR,
			opEQ, opNE, opLT, opGT, opLE, opGE:
			b := pop()
			a := pop()
			push(binOp(op, a, b))
		case opNEG:
			a := pop()
			if a.k == kFloat {
				push(vFloat(-a.f))
			} else {
				push(vInt(-a.i))
			}
		case opBNOT:
			push(vInt(^pop().i))
		case opNOT:
			push(vBool(!pop().truthy()))
		case opJMP:
			rel := rdi32()
			pc += rel
		case opJMPF:
			rel := rdi32()
			if !pop().truthy() {
				pc += rel
			}
		case opJMPT:
			rel := rdi32()
			if pop().truthy() {
				pc += rel
			}
		case opCALL:
			fi := rd16()
			argc := int(code[pc])
			pc++
			fn := p.funcs[fi]
			locals := make([]Value, fn.nlocals)
			base := len(stack) - argc
			copy(locals, stack[base:])
			stack = stack[:base]
			frames = append(frames, frame{retpc: pc, locals: locals, fn: fi})
			pc = int(fn.entry)
		case opPUSHFN:
			push(vFunc(int64(rd16())))
		case opCLOSURE:
			fi := int64(rd16())
			ncap := int(code[pc])
			pc++
			base := len(stack) - ncap
			cap := make([]Value, ncap)
			copy(cap, stack[base:])
			stack = stack[:base]
			push(vClosure(fi, cap))
		case opCALLVALUE:
			argc := int(code[pc])
			pc++
			base := len(stack) - argc
			fnval := stack[base-1]
			if fnval.k != kFunc {
				fatal("call of a non-function value")
			}
			fi := int(fnval.i)
			fn := p.funcs[fi]
			locals := make([]Value, fn.nlocals)
			// captured values occupy the leading locals, then the arguments
			ncap := 0
			if fnval.arr != nil {
				ncap = len(*fnval.arr)
				copy(locals, *fnval.arr)
			}
			copy(locals[ncap:], stack[base:])
			stack = stack[:base-1] // drop the args and the fn value beneath them
			frames = append(frames, frame{retpc: pc, locals: locals, fn: fi})
			pc = int(fn.entry)
		case opRET:
			ret := pop()
			fr := frames[len(frames)-1]
			frames = frames[:len(frames)-1]
			if fr.retpc < 0 {
				return
			}
			pc = fr.retpc
			push(ret)
		case opPRINT:
			fmt.Println(pop().String())
		case opPRINTLN:
			fmt.Println()
		case opASSERT:
			cond := pop()
			msg := pop()
			if !cond.truthy() {
				if !raise(vStr("[Cryo Assert] " + msg.String())) {
					fatal("[Cryo Assert] " + msg.String())
				}
			}
		case opTRYPUSH:
			rel := rdi32()
			slot := rd16()
			handlers = append(handlers, handler{
				catchPC: pc + rel, sp: len(stack), fp: len(frames), slot: slot})
		case opTRYPOP:
			if len(handlers) > 0 {
				handlers = handlers[:len(handlers)-1]
			}
		case opTHROW:
			v := pop()
			if !raise(v) {
				fatal("uncaught exception: " + v.String())
			}
		case opCOALESCE:
			b := pop()
			a := pop()
			if a.k == kNull {
				push(b)
			} else {
				push(a)
			}
		case opUNWRAP:
			a := pop()
			if a.k == kNull {
				if !raise(vStr("[Cryo Security] unwrap of null value")) {
					fatal("[Cryo Security] unwrap of null value")
				}
			} else {
				push(a)
			}
		case opNEWARR:
			n := rd16()
			base := len(stack) - n
			elems := make([]Value, n)
			copy(elems, stack[base:])
			stack = stack[:base]
			push(vArr(elems))
		case opNEWMAP:
			n := rd16()
			base := len(stack) - 2*n
			mm := make(map[any]Value, n)
			for j := 0; j < n; j++ {
				mm[keyOf(stack[base+2*j])] = stack[base+2*j+1]
			}
			stack = stack[:base]
			push(vMap(mm))
		case opINDEX:
			key := pop()
			cont := pop()
			push(indexGet(cont, key))
		case opSETIDX:
			val := pop()
			key := pop()
			cont := pop()
			indexSet(cont, key, val)
		case opLEN:
			push(vInt(lengthOf(pop())))
		case opAPPEND:
			val := pop()
			arr := pop()
			if arr.k != kArray {
				fatal("push on a non-array value")
			}
			*arr.arr = append(*arr.arr, val)
			push(vInt(int64(len(*arr.arr))))
		case opHAS:
			key := pop()
			mp := pop()
			if mp.k != kMap {
				push(vBool(false))
			} else {
				_, ok := mp.m[keyOf(key)]
				push(vBool(ok))
			}
		case opKEYS:
			mp := pop()
			if mp.k != kMap {
				fatal("keys() applied to a non-map value")
			}
			ks := mapKeysSorted(mp.m)
			out := make([]Value, len(ks))
			for i, k := range ks {
				out[i] = keyToValue(k)
			}
			push(vArr(out))
		case opNATIVE:
			nid := int(code[pc])
			argc := int(code[pc+1])
			pc += 2
			base := len(stack) - argc
			args := make([]Value, argc)
			copy(args, stack[base:])
			stack = stack[:base]
			push(native(nid, args))
		default:
			fatal(fmt.Sprintf("unknown opcode 0x%02X at pc=%d", op, pc-1))
		}
	}
}

// valueToGo: Pyro Value -> interface{} tree for json.Marshal.
func valueToGo(v Value) interface{} {
	switch v.k {
	case kInt:
		return v.i
	case kFloat:
		return v.f
	case kBool:
		return v.b
	case kStr:
		return v.s
	case kArray:
		out := make([]interface{}, len(*v.arr))
		for i, e := range *v.arr {
			out[i] = valueToGo(e)
		}
		return out
	case kMap:
		out := make(map[string]interface{}, len(v.m))
		for k, val := range v.m {
			out[keyToValue(k).String()] = valueToGo(val)
		}
		return out
	default:
		return nil
	}
}

// goToValue: json.Unmarshal tree -> Pyro Value. JSON integer
// numbers become int64 (Cryo structs usually have int fields); with fractional
// part, become float64.
func goToValue(x interface{}) Value {
	switch t := x.(type) {
	case nil:
		return vNull()
	case bool:
		return vBool(t)
	case float64:
		if t == math.Trunc(t) && !math.IsInf(t, 0) {
			return vInt(int64(t))
		}
		return vFloat(t)
	case string:
		return vStr(t)
	case []interface{}:
		out := make([]Value, len(t))
		for i, e := range t {
			out[i] = goToValue(e)
		}
		return vArr(out)
	case map[string]interface{}:
		m := make(map[any]Value, len(t))
		for k, val := range t {
			m[any(k)] = goToValue(val)
		}
		return vMap(m)
	default:
		return vNull()
	}
}

// httpGet: simple GET; returns the body, or "" in case of error (like in go).
func httpGet(url string) string {
	resp, err := httpClient.Get(url)
	if err != nil {
		return ""
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	return string(b)
}

// httpPost: POST with body (content-type application/json), like in the go backend.
func httpPost(url, body string) string {
	resp, err := httpClient.Post(url, "application/json", strings.NewReader(body))
	if err != nil {
		return ""
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	return string(b)
}

// native executes a VM builtin (mirrors NATIVES in codegen_pyro.py).
// sandboxed: when true, the VM refuses network/machine natives.
// Enabled by the bit2 flag of .pyro (--sandbox in the compiler) or by
// PYRO_SANDBOX=1 in the environment (runtime policy on artifacts).
var sandboxed bool
var progArgs []string   // program args after the .pyro path (for the args() native)
var prngState uint64 = 0x853c49e6748fea9b
var vmStartTime = time.Now()

func splitmix64Next() uint64 {
	prngState += 0x9e3779b97f4a7c15
	z := prngState
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9
	z = (z ^ (z >> 27)) * 0x94d049bb133111eb
	return z ^ (z >> 31)
}

func native(id int, a []Value) Value {
	switch id {
	case 0: // sqrt
		return vFloat(math.Sqrt(a[0].asFloat()))
	case 1: // pow
		return vFloat(math.Pow(a[0].asFloat(), a[1].asFloat()))
	case 2: // abs
		if a[0].k == kInt {
			if a[0].i < 0 {
				return vInt(-a[0].i)
			}
			return vInt(a[0].i)
		}
		return vFloat(math.Abs(a[0].asFloat()))
	case 3: // min
		if a[0].k == kInt && a[1].k == kInt {
			if a[0].i < a[1].i {
				return a[0]
			}
			return a[1]
		}
		return vFloat(math.Min(a[0].asFloat(), a[1].asFloat()))
	case 4: // max
		if a[0].k == kInt && a[1].k == kInt {
			if a[0].i > a[1].i {
				return a[0]
			}
			return a[1]
		}
		return vFloat(math.Max(a[0].asFloat(), a[1].asFloat()))
	case 5: // floor
		return vFloat(math.Floor(a[0].asFloat()))
	case 6: // ceil
		return vFloat(math.Ceil(a[0].asFloat()))
	case 7: // round
		return vFloat(math.Round(a[0].asFloat()))
	case 8: // to_string
		return vStr(a[0].String())
	case 9: // to_int
		switch a[0].k {
		case kInt:
			return a[0]
		case kFloat:
			return vInt(int64(a[0].f))
		case kBool:
			if a[0].b {
				return vInt(1)
			}
			return vInt(0)
		case kStr:
			n, err := strconv.ParseInt(strings.TrimSpace(a[0].s), 10, 64)
			if err != nil {
				fatal("[Cryo Security] to_int: '" + a[0].s + "' is not a valid integer")
			}
			return vInt(n)
		}
		fatal("to_int: non-convertible type")
	case 10: // to_number
		switch a[0].k {
		case kFloat:
			return a[0]
		case kInt:
			return vFloat(float64(a[0].i))
		case kStr:
			f, err := strconv.ParseFloat(strings.TrimSpace(a[0].s), 64)
			if err != nil {
				fatal("[Cryo Security] to_number: '" + a[0].s + "' is not a valid number")
			}
			return vFloat(f)
		}
		fatal("to_number: non-convertible type")
	case 11: // remove(map, key)
		if a[0].k != kMap {
			fatal("remove() applied to a non-map value")
		}
		delete(a[0].m, keyOf(a[1]))
		return vNull()
	case 12: // upper
		return vStr(strings.ToUpper(a[0].String()))
	case 13: // lower
		return vStr(strings.ToLower(a[0].String()))
	case 14: // trim
		return vStr(strings.TrimSpace(a[0].String()))
	case 15: // contains
		return vBool(strings.Contains(a[0].String(), a[1].String()))
	case 16: // find -> index of the substring (or -1)
		return vInt(int64(strings.Index(a[0].String(), a[1].String())))
	case 17: // replace(s, old, new) — all occurrences
		return vStr(strings.ReplaceAll(a[0].String(), a[1].String(), a[2].String()))
	case 18: // substr(s, start, n) — slice with safe bounds
		s := a[0].String()
		i, n := a[1].i, a[2].i
		if i < 0 {
			i = 0
		}
		if i > int64(len(s)) {
			i = int64(len(s))
		}
		end := i + n
		if n < 0 || end > int64(len(s)) {
			end = int64(len(s))
		}
		return vStr(s[i:end])
	case 19: // split(s, sep) -> array of strings
		parts := strings.Split(a[0].String(), a[1].String())
		out := make([]Value, len(parts))
		for i, p := range parts {
			out[i] = vStr(p)
		}
		return vArr(out)
	case 20: // join(arr, sep)
		if a[0].k != kArray {
			fatal("join() applied to a non-array value")
		}
		parts := make([]string, len(*a[0].arr))
		for i, v := range *a[0].arr {
			parts[i] = v.String()
		}
		return vStr(strings.Join(parts, a[1].String()))
	case 21: // input(prompt) -> reads a line from stdin
		fmt.Print(a[0].String())
		line, _ := stdin.ReadString('\n')
		return vStr(strings.TrimRight(line, "\r\n"))
	case 22: // json_encode(v) -> JSON string
		b, err := json.Marshal(valueToGo(a[0]))
		if err != nil {
			fatal("json_encode: " + err.Error())
		}
		return vStr(string(b))
	case 23: // json_decode(s) -> dynamic value (map/array/scalar)
		var raw interface{}
		if err := json.Unmarshal([]byte(a[0].String()), &raw); err != nil {
			fatal("[Cryo] json_decode: invalid JSON: " + err.Error())
		}
		return goToValue(raw)
	case 24: // http_get(url) -> body (string); "" in case of error
		if sandboxed {
			fatal("[Cryo Security] Sandbox: http_get() blocked by sandbox policy")
		}
		return vStr(httpGet(a[0].String()))
	case 25: // http_post(url, body) -> response body (string)
		if sandboxed {
			fatal("[Cryo Security] Sandbox: http_post() blocked by sandbox policy")
		}
		return vStr(httpPost(a[0].String(), a[1].String()))
	case 26: // sleep(ms) -> pause; returns null
		ms := a[0].i
		if a[0].k == kFloat {
			ms = int64(a[0].f)
		}
		if ms > 0 {
			time.Sleep(time.Duration(ms) * time.Millisecond)
		}
		return vNull()
	case 27: // write_bytes(path, int[]) -> bool: writes bytes to a file
		if sandboxed {
			fatal("[Cryo Security] Sandbox: write_bytes() blocked by sandbox policy")
		}
		if a[1].k != kArray {
			return vBool(false)
		}
		src := *a[1].arr
		buf := make([]byte, len(src))
		for i, e := range src {
			buf[i] = byte(e.i & 0xFF)
		}
		return vBool(os.WriteFile(a[0].String(), buf, 0644) == nil)
	case 28: // read_file(path) -> string ("" on error)
		if sandboxed {
			fatal("[Cryo Security] Sandbox: read_file() blocked by sandbox policy")
		}
		data, err := os.ReadFile(a[0].String())
		if err != nil {
			return vStr("")
		}
		return vStr(string(data))
	case 29: // args() -> string[]: program args after the .pyro path
		out := make([]Value, len(progArgs))
		for i, s := range progArgs {
			out[i] = vStr(s)
		}
		return vArr(out)
	case 30: // http_serve(port, dir) -> serve a static directory (blocking)
		if sandboxed {
			fatal("[Cryo Security] Sandbox: http_serve() blocked by sandbox policy")
		}
		dir := a[1].String()
		fsrv := http.FileServer(http.Dir(dir))
		h := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if strings.HasSuffix(r.URL.Path, ".wasm") {
				w.Header().Set("Content-Type", "application/wasm")
			}
			fsrv.ServeHTTP(w, r)
		})
		fmt.Printf("[pyro] serving %s on http://localhost:%d\n", dir, a[0].i)
		if err := http.ListenAndServe(fmt.Sprintf(":%d", a[0].i), h); err != nil {
			fatal("http_serve: " + err.Error())
		}
		return vNull()
	case 31: // clamp(x, lo, hi)
		if a[0].k == kInt && a[1].k == kInt && a[2].k == kInt {
			x, lo, hi := a[0].i, a[1].i, a[2].i
			if x < lo {
				return vInt(lo)
			}
			if x > hi {
				return vInt(hi)
			}
			return vInt(x)
		}
		x, lo, hi := a[0].asFloat(), a[1].asFloat(), a[2].asFloat()
		if x < lo {
			return vFloat(lo)
		}
		if x > hi {
			return vFloat(hi)
		}
		return vFloat(x)
	case 32: // sign(x) -> int (-1, 0, 1)
		f := a[0].asFloat()
		if f < 0 {
			return vInt(-1)
		}
		if f > 0 {
			return vInt(1)
		}
		return vInt(0)
	case 33: // gcd(a, b) -> int
		gx, gy := nativeInt(a[0]), nativeInt(a[1])
		if gx < 0 {
			gx = -gx
		}
		if gy < 0 {
			gy = -gy
		}
		for gy != 0 {
			gx, gy = gy, gx%gy
		}
		return vInt(gx)
	case 34: // hypot(a, b) -> number
		return vFloat(math.Hypot(a[0].asFloat(), a[1].asFloat()))
	case 35: // starts_with(s, prefix) -> bool
		return vBool(strings.HasPrefix(a[0].String(), a[1].String()))
	case 36: // ends_with(s, suffix) -> bool
		return vBool(strings.HasSuffix(a[0].String(), a[1].String()))
	case 37: // repeat(s, n) -> string  (n<0 treated as 0)
		n := nativeInt(a[1])
		if n < 0 {
			n = 0
		}
		return vStr(strings.Repeat(a[0].String(), int(n)))
	case 38: // sort(arr) -> new array sorted ascending (stable)
		if a[0].k != kArray {
			fatal("sort() expects an array")
		}
		src := *a[0].arr
		cp := make([]Value, len(src))
		copy(cp, src)
		sort.SliceStable(cp, func(i, j int) bool { return nativeLess(cp[i], cp[j]) })
		return vArr(cp)
	case 39: // reverse(arr) -> new reversed array
		if a[0].k != kArray {
			fatal("reverse() expects an array")
		}
		src := *a[0].arr
		cp := make([]Value, len(src))
		for i, e := range src {
			cp[len(src)-1-i] = e
		}
		return vArr(cp)
	case 40: // slice(x, start, end) -> subarray/substring [start, end), safe bounds
		// Polymorphic over array|string so `xs[a..b]` and `s[a..b]` can lower to
		// the SAME call — the parser cannot know the operand's type (10.9).
		if a[0].k == kStr {
			s := a[0].s
			n := int64(len(s))
			start, end := nativeInt(a[1]), nativeInt(a[2])
			if start < 0 {
				start = 0
			}
			if end > n {
				end = n
			}
			if start > end {
				start = end
			}
			return vStr(s[start:end])
		}
		if a[0].k != kArray {
			fatal("slice() expects an array or a string")
		}
		src := *a[0].arr
		n := int64(len(src))
		start, end := nativeInt(a[1]), nativeInt(a[2])
		if start < 0 {
			start = 0
		}
		if end > n {
			end = n
		}
		if start > end {
			start = end
		}
		cp := make([]Value, end-start)
		copy(cp, src[start:end])
		return vArr(cp)
	case 41: // index_of(arr, x) -> first index of x by value equality, else -1
		if a[0].k != kArray {
			fatal("index_of() expects an array")
		}
		for i, e := range *a[0].arr {
			if valueEq(e, a[1]) {
				return vInt(int64(i))
			}
		}
		return vInt(-1)
	case 42: // pad_start(s, width, pad) -> string
		return vStr(nativePad(a[0].String(), int(nativeInt(a[1])), a[2].String(), true))
	case 43: // pad_end(s, width, pad) -> string
		return vStr(nativePad(a[0].String(), int(nativeInt(a[1])), a[2].String(), false))
	case 44: // concat(a, b) -> new array (elements of a then b)
		if a[0].k != kArray || a[1].k != kArray {
			fatal("concat() expects two arrays")
		}
		s1, s2 := *a[0].arr, *a[1].arr
		cp := make([]Value, 0, len(s1)+len(s2))
		cp = append(cp, s1...)
		cp = append(cp, s2...)
		return vArr(cp)
	case 45: // count(arr, x) -> number of elements equal to x
		if a[0].k != kArray {
			fatal("count() expects an array")
		}
		var n int64
		for _, e := range *a[0].arr {
			if valueEq(e, a[1]) {
				n++
			}
		}
		return vInt(n)
	case 46: // sum(arr) -> int if all int, else number
		if a[0].k != kArray {
			fatal("sum() expects an array")
		}
		src := *a[0].arr
		allInt := true
		for _, e := range src {
			if e.k == kFloat {
				allInt = false
			}
		}
		if allInt {
			var t int64
			for _, e := range src {
				t += e.i
			}
			return vInt(t)
		}
		var t float64
		for _, e := range src {
			t += e.asFloat()
		}
		return vFloat(t)
	case 47: // now_ms() -> int
		return vInt(time.Now().UnixMilli())
	case 48: // monotonic_ms() -> int
		return vInt(time.Since(vmStartTime).Milliseconds())
	case 49: // random() -> number [0.0, 1.0)
		r := float64(splitmix64Next()>>11) / 9007199254740992.0
		return vFloat(r)
	case 50: // random_int(lo, hi) -> int inclusive
		lo, hi := nativeInt(a[0]), nativeInt(a[1])
		if hi < lo {
			lo, hi = hi, lo
		}
		span := uint64(hi - lo + 1)
		val := lo + int64(splitmix64Next()%span)
		return vInt(val)
	case 51: // seed(n) -> void/null
		prngState = uint64(nativeInt(a[0]))
		return vNull()
	// ── HTTP server, roadmap 11.6 ──
	case 52: // http_listen(port) -> bool
		if sandboxed {
			fatal("[Cryo Security] Sandbox: http_listen() blocked by sandbox policy")
		}
		if httpListener != nil {
			httpListener.Close()
		}
		ln, err := net.Listen("tcp", ":"+strconv.FormatInt(nativeInt(a[0]), 10))
		if err != nil {
			return vBool(false)
		}
		httpListener = ln
		return vBool(true)
	case 53: // http_accept() -> map{method,path,query,body}, or null
		if httpListener == nil {
			fatal("http_accept() called before http_listen()")
		}
		if httpConn != nil {
			// The program skipped http_respond for the previous request; close
			// it rather than leaking the connection.
			httpConn.Close()
			httpConn = nil
		}
		// Always returns a MAP, never null: `req == null` cannot be written
		// reliably today (valueEq compares containers by their zero int, so a
		// map tests equal to null), and an empty method is a cleaner signal
		// anyway. A failed or malformed accept yields method "".
		empty := func() Value {
			m := map[any]Value{}
			m["method"] = vStr("")
			m["path"] = vStr("")
			m["query"] = vStr("")
			m["body"] = vStr("")
			return vMap(m)
		}
		c, err := httpListener.Accept()
		if err != nil {
			return empty()
		}
		method, path, query, body, hdrs, ok := httpParseRequest(c)
		if !ok {
			c.Close()
			return empty()
		}
		httpConn = c
		m := map[any]Value{}
		m["method"] = vStr(method)
		m["path"] = vStr(path)
		m["query"] = vStr(query)
		m["body"] = vStr(body)
		// Headers share the flat map under a `header:` prefix, lowercased.
		// Flat because the Cryo side is `map<string,string>`; a nested map
		// would not survive `as map<string,string>`.
		for hk, hv := range hdrs {
			m["header:"+hk] = vStr(hv)
		}
		return vMap(m)
	case 54: // http_respond(status, content_type, body) -> bool
		if httpConn == nil {
			return vBool(false)
		}
		status := nativeInt(a[0])
		ctype := a[1].String()
		payload := a[2].String()
		resp := "HTTP/1.1 " + strconv.FormatInt(status, 10) + " " + httpStatusText(status) + "\r\n" +
			"Content-Type: " + ctype + "\r\n" +
			"Content-Length: " + strconv.Itoa(len(payload)) + "\r\n" +
			"Connection: close\r\n\r\n" + payload
		_, werr := httpConn.Write([]byte(resp))
		httpConn.Close()
		httpConn = nil
		return vBool(werr == nil)

	// ── filesystem & process, roadmap 11.7 ──
	// Pure queries (exists/is_dir/size/list) are ungated: they reveal only what
	// a path lookup reveals. Everything that MUTATES the machine or reads its
	// environment is sandbox-gated, matching read_file/write_bytes.
	case 55: // file_exists(path) -> bool
		_, err := os.Stat(a[0].String())
		return vBool(err == nil)
	case 56: // is_dir(path) -> bool
		st, err := os.Stat(a[0].String())
		return vBool(err == nil && st.IsDir())
	case 57: // list_dir(path) -> string[] (names only, sorted; empty on error)
		ents, err := os.ReadDir(a[0].String())
		if err != nil {
			return vArr([]Value{})
		}
		names := make([]string, 0, len(ents))
		for _, e := range ents {
			names = append(names, e.Name())
		}
		sort.Strings(names) // deterministic: the C runtime sorts too
		out := make([]Value, len(names))
		for i, n := range names {
			out[i] = vStr(n)
		}
		return vArr(out)
	case 58: // make_dir(path) -> bool (creates parents; true if it already exists)
		if sandboxed {
			fatal("[Cryo Security] Sandbox: make_dir() blocked by sandbox policy")
		}
		return vBool(os.MkdirAll(a[0].String(), 0o755) == nil)
	case 59: // delete_file(path) -> bool
		if sandboxed {
			fatal("[Cryo Security] Sandbox: delete_file() blocked by sandbox policy")
		}
		// FILES ONLY, and deliberately not recursive. Go's os.Remove would
		// also drop an empty directory, but MSVCRT's remove() will not and
		// POSIX's will — so left alone this one call would mean three
		// different things. Directory removal is simply not offered yet.
		if st, err := os.Stat(a[0].String()); err == nil && st.IsDir() {
			return vBool(false)
		}
		return vBool(os.Remove(a[0].String()) == nil)
	case 60: // file_size(path) -> int (-1 when it cannot be read)
		st, err := os.Stat(a[0].String())
		if err != nil {
			return vInt(-1)
		}
		return vInt(st.Size())
	case 61: // write_file(path, content) -> bool
		if sandboxed {
			fatal("[Cryo Security] Sandbox: write_file() blocked by sandbox policy")
		}
		return vBool(os.WriteFile(a[0].String(), []byte(a[1].String()), 0o644) == nil)
	case 62: // env(name) -> string ("" when unset)
		if sandboxed {
			fatal("[Cryo Security] Sandbox: env() blocked by sandbox policy")
		}
		return vStr(os.Getenv(a[0].String()))
	case 63: // exec(cmd) -> string (stdout; "" on failure)
		if sandboxed {
			fatal("[Cryo Security] Sandbox: exec() blocked by sandbox policy")
		}
		var cmd *exec.Cmd
		if runtime.GOOS == "windows" {
			cmd = exec.Command("cmd", "/C", a[0].String())
		} else {
			cmd = exec.Command("sh", "-c", a[0].String())
		}
		outBytes, err := cmd.Output()
		if err != nil && len(outBytes) == 0 {
			return vStr("")
		}
		return vStr(string(outBytes))

	// ── persistence, roadmap 11.8 ──
	case 64: // write_file_atomic(path, content) -> bool
		if sandboxed {
			fatal("[Cryo Security] Sandbox: write_file_atomic() blocked by sandbox policy")
		}
		// Write a sibling temp file, flush it to disk, then rename over the
		// target. A reader then sees either the old file or the new one, never
		// the truncated middle that a plain write leaves behind on a crash.
		// The temp file is a SIBLING so the rename stays on one filesystem —
		// across devices it would be a copy, which is not atomic.
		{
			path := a[0].String()
			tmp := path + ".tmp"
			f, err := os.Create(tmp)
			if err != nil {
				return vBool(false)
			}
			if _, err = f.WriteString(a[1].String()); err != nil {
				f.Close()
				os.Remove(tmp)
				return vBool(false)
			}
			if err = f.Sync(); err != nil {
				f.Close()
				os.Remove(tmp)
				return vBool(false)
			}
			if err = f.Close(); err != nil {
				os.Remove(tmp)
				return vBool(false)
			}
			// os.Rename replaces an existing target on Windows too.
			if err = os.Rename(tmp, path); err != nil {
				os.Remove(tmp)
				return vBool(false)
			}
			return vBool(true)
		}
	case 65: // url_decode(s) -> string  (percent-decoding, '+' is a space)
		{
			in := a[0].String()
			var b strings.Builder
			for i := 0; i < len(in); i++ {
				switch {
				case in[i] == '+':
					b.WriteByte(' ')
				case in[i] == '%' && i+2 < len(in):
					hi, ok1 := unhex(in[i+1])
					lo, ok2 := unhex(in[i+2])
					if ok1 && ok2 {
						b.WriteByte(hi<<4 | lo)
						i += 2
					} else {
						b.WriteByte(in[i]) // malformed: pass through
					}
				default:
					b.WriteByte(in[i])
				}
			}
			return vStr(b.String())
		}
	case 66: // url_encode(s) -> string
		{
			const hexd = "0123456789ABCDEF"
			in := a[0].String()
			var b strings.Builder
			for i := 0; i < len(in); i++ {
				ch := in[i]
				if (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
					(ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
					ch == '.' || ch == '~' {
					b.WriteByte(ch)
				} else {
					b.WriteByte('%')
					b.WriteByte(hexd[ch>>4])
					b.WriteByte(hexd[ch&0x0F])
				}
			}
			return vStr(b.String())
		}
	// ── embedded assets, roadmap 11.9 ──
	case 67: // asset(name) -> string ("" when absent)
		return vStr(assets[a[0].String()])
	case 68: // asset_names() -> string[] (sorted, so every engine agrees)
		names := make([]string, 0, len(assets))
		for k := range assets {
			names = append(names, k)
		}
		sort.Strings(names)
		out := make([]Value, len(names))
		for i, n := range names {
			out[i] = vStr(n)
		}
		return vArr(out)

	}
	fatal(fmt.Sprintf("unknown native builtin: id=%d", id))
	return vNull()
}

func binOp(op byte, a, b Value) Value {
	if op == opADD && (a.k == kStr || b.k == kStr) {
		return vStr(a.String() + b.String())
	}
	if op == opEQ || op == opNE {
		eq := valueEq(a, b)
		if op == opNE {
			eq = !eq
		}
		return vBool(eq)
	}
	if a.k == kStr || b.k == kStr {
		switch op {
		case opLT:
			return vBool(a.String() < b.String())
		case opGT:
			return vBool(a.String() > b.String())
		case opLE:
			return vBool(a.String() <= b.String())
		case opGE:
			return vBool(a.String() >= b.String())
		}
	}
	if a.k == kFloat || b.k == kFloat {
		x, y := a.asFloat(), b.asFloat()
		switch op {
		case opADD:
			return vFloat(x + y)
		case opSUB:
			return vFloat(x - y)
		case opMUL:
			return vFloat(x * y)
		case opDIV:
			return vFloat(x / y)
		case opMOD:
			return vFloat(math.Mod(x, y))
		case opLT:
			return vBool(x < y)
		case opGT:
			return vBool(x > y)
		case opLE:
			return vBool(x <= y)
		case opGE:
			return vBool(x >= y)
		}
	}
	x, y := a.i, b.i
	switch op {
	case opADD:
		return vInt(x + y)
	case opSUB:
		return vInt(x - y)
	case opMUL:
		return vInt(x * y)
	case opDIV:
		if y == 0 {
			fatal("[Cryo Security] DivByZero: integer division")
		}
		if x == -1<<63 && y == -1 {
			fatal("[Cryo Security] Overflow: INT64_MIN / -1")
		}
		return vInt(x / y)
	case opMOD:
		if y == 0 {
			fatal("[Cryo Security] DivByZero: modulo")
		}
		if x == -1<<63 && y == -1 {
			return vInt(0) // INT64_MIN % -1 = 0 (well-defined); only division overflows
		}
		return vInt(x % y)
	case opBAND:
		return vInt(x & y)
	case opBOR:
		return vInt(x | y)
	case opBXOR:
		return vInt(x ^ y)
	case opSHL:
		return vInt(x << uint(y))
	case opSHR:
		return vInt(x >> uint(y))
	case opLT:
		return vBool(x < y)
	case opGT:
		return vBool(x > y)
	case opLE:
		return vBool(x <= y)
	case opGE:
		return vBool(x >= y)
	}
	fatal("invalid opcode in bytecode")
	return vNull()
}

func sameMap(m1, m2 map[any]Value) bool {
	if m1 == nil && m2 == nil {
		return true
	}
	if m1 == nil || m2 == nil {
		return false
	}
	return reflect.ValueOf(m1).Pointer() == reflect.ValueOf(m2).Pointer()
}

func valueEq(a, b Value) bool {
	if a.k == kNull || b.k == kNull {
		return a.k == kNull && b.k == kNull
	}
	if a.k == kStr || b.k == kStr {
		return a.String() == b.String()
	}
	if a.k == kFloat || b.k == kFloat {
		return a.asFloat() == b.asFloat()
	}
	if a.k == kBool || b.k == kBool {
		return a.truthy() == b.truthy()
	}
	if a.k == kArray || b.k == kArray {
		return a.k == b.k && a.arr == b.arr
	}
	if a.k == kMap || b.k == kMap {
		return a.k == b.k && sameMap(a.m, b.m)
	}
	if a.k == kFunc || b.k == kFunc {
		return a.k == b.k && a.i == b.i && a.arr == b.arr
	}
	return a.k == b.k && a.i == b.i
}

func main() {
	if len(os.Args) < 2 {
		fatal("usage: pyrovm program.pyro")
	}
	data, err := os.ReadFile(os.Args[1])
	if err != nil {
		fatal("could not read: " + err.Error())
	}
	if os.Getenv("PYRO_SANDBOX") == "1" {
		sandboxed = true
	}
	progArgs = os.Args[2:]   // exposed to the program via the args() native
	run(load(data))
}
