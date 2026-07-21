// ============================================================
//  Pyro VM — executa a linguagem-alvo própria .pyro (bytecode)
//
//  O .pyro NÃO é Go/C/asm: é o bytecode próprio do Pyro, com um
//  conjunto de instruções (ISA) inventado no projeto. Esta VM,
//  baseada em pilha, carrega o arquivo e o executa na máquina.
//
//  Uso:  pyrovm programa.pyro
// ============================================================
package main

import (
	"bufio"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"net/http"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"
)

// cliente HTTP com timeout, compartilhado pelos natives de rede
var httpClient = &http.Client{Timeout: 15 * time.Second}

var stdin = bufio.NewReader(os.Stdin)

// ── Opcodes (espelham burnout/codegen_pyro.py) ──────────────
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
	opNATIVE   = 0x70 // u8 id, u8 argc — builtin nativo (tabela em native())
	opTRYPUSH  = 0x71 // i16 rel (catch), u16 slot (var do catch; 0xFFFF = nenhuma)
	opTRYPOP   = 0x72 // remove o handler de exceção do topo
	opTHROW    = 0x73 // pop valor -> desenrola até o handler mais próximo
	opCOALESCE = 0x74 // pop b, a -> a se a != null, senão b  (??)
	opUNWRAP   = 0x75 // pop a -> a se a != null, senão aborta (x!)
)

const noSlot = 0xFFFF

// tipos de valor
const (
	kInt = iota
	kFloat
	kBool
	kStr
	kNull
	kArray // *[]Value  (referência: push/setidx mutam o compartilhado)
	kMap   // map[any]Value (structs também usam este tipo)
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

// chave comparável (Go) a partir de um Value
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

// reconstrói um Value a partir de uma chave de map
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

// chaves de map ordenadas (saída determinística)
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
	dbg     []dbgEntry // pc -> linha (ordenado por pc); vazio se sem depuração
}

type frame struct {
	retpc  int
	locals []Value
	fn     int // índice da função (para stack trace)
}

// ── estado de depuração (para stack traces em fatal) ────────
var (
	dbgLines  []dbgEntry
	dbgFuncs  []Func
	dbgFrames *[]frame
	dbgPC     *int
)

// lineAt: maior entrada com pc <= alvo (busca binária).
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

// stackTrace: pilha de chamadas ativas com nome da função e linha.
func stackTrace() string {
	if len(dbgLines) == 0 || dbgFrames == nil || dbgPC == nil {
		return ""
	}
	fr := *dbgFrames
	var b strings.Builder
	b.WriteString("  stack trace (most recent first):\n")
	for i := len(fr) - 1; i >= 0; i-- {
		// onde este quadro está pausado: o topo está no pc atual; os
		// demais, no endereço de retorno do quadro que eles chamaram.
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

// ── carregamento do .pyro ───────────────────────────────────

func load(data []byte) *Program {
	if len(data) < 6 || string(data[0:4]) != "PYRO" {
		fatal("invalid .pyro file (magic)")
	}
	if data[4] != 2 {
		fatal("unsupported .pyro version (expected v2)")
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
			n := rd16()
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
	// política de sandbox gravada no artefato (bit2): a VM recusa
	// natives de rede/máquina. Também pode ser ligada em runtime
	// por PYRO_SANDBOX=1 (nunca desliga o que o artefato exigiu).
	if flags&0x04 != 0 {
		sandboxed = true
	}
	// seção de depuração (pc -> linha), se presente
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

// inverso do XOR rolling do gerador (ofuscação leve, não é cripto forte)
func xorDecode(code []byte) {
	k := byte(0x5A)
	for i := range code {
		b := code[i] ^ k
		code[i] = b
		k = byte((int(k)*31 + 7 + int(b)) & 0xFF)
	}
}

// ── operações de container ──────────────────────────────────

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

// ── execução ────────────────────────────────────────────────

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

	// estado de depuração acessível pelo fatal() (stack trace)
	dbgLines = p.dbg
	dbgFuncs = p.funcs
	dbgFrames = &frames
	dbgPC = &pc

	// pilha de handlers de exceção (try/catch)
	type handler struct {
		catchPC int
		sp      int // profundidade da pilha de operandos ao entrar no try
		fp      int // profundidade da pilha de quadros
		slot    int // slot da variável de catch (noSlot = nenhuma)
	}
	var handlers []handler

	rd16 := func() int { v := int(binary.LittleEndian.Uint16(code[pc:])); pc += 2; return v }
	rdi32 := func() int { v := int(int32(binary.LittleEndian.Uint32(code[pc:]))); pc += 4; return v }

	// raise: desenrola até o handler mais próximo; devolve false se não há.
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

// valueToGo: Value do Pyro -> árvore interface{} p/ json.Marshal.
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

// goToValue: árvore de json.Unmarshal -> Value do Pyro. Números JSON
// inteiros viram int64 (structs Cryo costumam ter campos int); com parte
// fracionária, viram float64.
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

// httpGet: GET simples; devolve o corpo, ou "" em caso de erro (como no go).
func httpGet(url string) string {
	resp, err := httpClient.Get(url)
	if err != nil {
		return ""
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	return string(b)
}

// httpPost: POST com corpo (content-type application/json), como no backend go.
func httpPost(url, body string) string {
	resp, err := httpClient.Post(url, "application/json", strings.NewReader(body))
	if err != nil {
		return ""
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	return string(b)
}

// native executa um builtin da VM (espelha NATIVES em codegen_pyro.py).
// sandboxed: quando true, a VM recusa natives de rede/máquina.
// Ligado pela flag bit2 do .pyro (--sandbox no compilador) ou por
// PYRO_SANDBOX=1 no ambiente (política de runtime sobre artefatos).
var sandboxed bool

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
	case 16: // find -> índice do substring (ou -1)
		return vInt(int64(strings.Index(a[0].String(), a[1].String())))
	case 17: // replace(s, velho, novo) — todas as ocorrências
		return vStr(strings.ReplaceAll(a[0].String(), a[1].String(), a[2].String()))
	case 18: // substr(s, inicio, n) — recorta com limites seguros
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
	case 19: // split(s, sep) -> array de strings
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
	case 21: // input(prompt) -> lê uma linha do stdin
		fmt.Print(a[0].String())
		line, _ := stdin.ReadString('\n')
		return vStr(strings.TrimRight(line, "\r\n"))
	case 22: // json_encode(v) -> string JSON
		b, err := json.Marshal(valueToGo(a[0]))
		if err != nil {
			fatal("json_encode: " + err.Error())
		}
		return vStr(string(b))
	case 23: // json_decode(s) -> valor dinâmico (map/array/escalar)
		var raw interface{}
		if err := json.Unmarshal([]byte(a[0].String()), &raw); err != nil {
			fatal("[Cryo] json_decode: invalid JSON: " + err.Error())
		}
		return goToValue(raw)
	case 24: // http_get(url) -> corpo (string); "" em caso de erro
		if sandboxed {
			fatal("[Cryo Security] Sandbox: http_get() blocked by sandbox policy")
		}
		return vStr(httpGet(a[0].String()))
	case 25: // http_post(url, body) -> corpo da resposta (string)
		if sandboxed {
			fatal("[Cryo Security] Sandbox: http_post() blocked by sandbox policy")
		}
		return vStr(httpPost(a[0].String(), a[1].String()))
	case 26: // sleep(ms) -> pausa; devolve null
		ms := a[0].i
		if a[0].k == kFloat {
			ms = int64(a[0].f)
		}
		if ms > 0 {
			time.Sleep(time.Duration(ms) * time.Millisecond)
		}
		return vNull()
	case 27: // write_bytes(path, int[]) -> bool: grava os bytes num arquivo
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
			return vInt(0) // INT64_MIN % -1 = 0 (bem-definido); só a divisão estoura
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

func valueEq(a, b Value) bool {
	if a.k == kStr || b.k == kStr {
		return a.String() == b.String()
	}
	if a.k == kFloat || b.k == kFloat {
		return a.asFloat() == b.asFloat()
	}
	if a.k == kBool || b.k == kBool {
		return a.truthy() == b.truthy()
	}
	return a.i == b.i
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
	run(load(data))
}
