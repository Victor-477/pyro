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
	"encoding/binary"
	"fmt"
	"math"
	"os"
	"sort"
	"strconv"
)

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
)

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

type Program struct {
	consts  []Value
	funcs   []Func
	entryFn int
	code    []byte
}

type frame struct {
	retpc  int
	locals []Value
}

func fatal(msg string) {
	fmt.Fprintln(os.Stderr, "[Pyro VM] "+msg)
	os.Exit(1)
}

// ── carregamento do .pyro ───────────────────────────────────

func load(data []byte) *Program {
	if len(data) < 6 || string(data[0:4]) != "PYRO" {
		fatal("arquivo .pyro inválido (magic)")
	}
	if data[4] != 1 {
		fatal("versão de .pyro não suportada")
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
			fatal("tag de constante desconhecida")
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
	if flags&0x01 != 0 {
		xorDecode(code)
	}
	p.code = code
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
		fatal("len() aplicado a valor sem tamanho")
		return 0
	}
}

func indexGet(cont, key Value) Value {
	switch cont.k {
	case kArray:
		idx := key.i
		if idx < 0 || idx >= int64(len(*cont.arr)) {
			fatal(fmt.Sprintf("[Cryo Seguranca] IndexError: índice %d fora dos limites (len=%d)", idx, len(*cont.arr)))
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
			fatal("[Cryo Seguranca] IndexError: índice de string fora dos limites")
		}
		return vStr(string(cont.s[idx]))
	default:
		fatal("indexação de valor não indexável")
		return vNull()
	}
}

func indexSet(cont, key, val Value) {
	switch cont.k {
	case kArray:
		idx := key.i
		if idx < 0 || idx >= int64(len(*cont.arr)) {
			fatal(fmt.Sprintf("[Cryo Seguranca] IndexError: índice %d fora dos limites", idx))
		}
		(*cont.arr)[idx] = val
	case kMap:
		cont.m[keyOf(key)] = val
	default:
		fatal("atribuição indexada em valor não indexável")
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
	frames := []frame{{retpc: -1, locals: make([]Value, main.nlocals)}}
	pc := int(main.entry)

	rd16 := func() int { v := int(binary.LittleEndian.Uint16(code[pc:])); pc += 2; return v }
	rdi16 := func() int { v := int(int16(binary.LittleEndian.Uint16(code[pc:]))); pc += 2; return v }

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
			rel := rdi16()
			pc += rel
		case opJMPF:
			rel := rdi16()
			if !pop().truthy() {
				pc += rel
			}
		case opJMPT:
			rel := rdi16()
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
			frames = append(frames, frame{retpc: pc, locals: locals})
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
				fatal("[Cryo Assert] " + msg.String())
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
				fatal("push em valor que não é array")
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
				fatal("keys() aplicado a valor que não é map")
			}
			ks := mapKeysSorted(mp.m)
			out := make([]Value, len(ks))
			for i, k := range ks {
				out[i] = keyToValue(k)
			}
			push(vArr(out))
		default:
			fatal(fmt.Sprintf("opcode desconhecido 0x%02X em pc=%d", op, pc-1))
		}
	}
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
			fatal("[Cryo Seguranca] DivisaoPorZero: divisão inteira")
		}
		return vInt(x / y)
	case opMOD:
		if y == 0 {
			fatal("[Cryo Seguranca] DivisaoPorZero: módulo")
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
	fatal("operador inválido no bytecode")
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
		fatal("uso: pyrovm programa.pyro")
	}
	data, err := os.ReadFile(os.Args[1])
	if err != nil {
		fatal("não foi possível ler: " + err.Error())
	}
	run(load(data))
}
