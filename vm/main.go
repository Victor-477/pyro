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
)

// tipos de valor
const (
	kInt = iota
	kFloat
	kBool
	kStr
	kNull
)

type Value struct {
	k byte
	i int64
	f float64
	b bool
	s string
}

func vInt(i int64) Value    { return Value{k: kInt, i: i} }
func vFloat(f float64) Value { return Value{k: kFloat, f: f} }
func vBool(b bool) Value    { return Value{k: kBool, b: b} }
func vStr(s string) Value   { return Value{k: kStr, s: s} }
func vNull() Value          { return Value{k: kNull} }

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
	default:
		return "null"
	}
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
		case 1: // int64
			p.consts[i] = vInt(int64(binary.LittleEndian.Uint64(data[pos:])))
			pos += 8
		case 2: // float64
			p.consts[i] = vFloat(math.Float64frombits(binary.LittleEndian.Uint64(data[pos:])))
			pos += 8
		case 3: // string
			n := rd16()
			p.consts[i] = vStr(string(data[pos : pos+n]))
			pos += n
		case 4: // bool
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
				return // retorno da main
			}
			pc = fr.retpc
			push(ret)
		case opPRINT:
			fmt.Println(pop().String())
			// valor de expressão fica a cargo do bytecode (NULL emitido depois)
		case opPRINTLN:
			fmt.Println()
		case opASSERT:
			cond := pop()
			msg := pop()
			if !cond.truthy() {
				fatal("[Cryo Assert] " + msg.String())
			}
		default:
			fatal(fmt.Sprintf("opcode desconhecido 0x%02X em pc=%d", op, pc-1))
		}
	}
}

func binOp(op byte, a, b Value) Value {
	// concatenação de string
	if op == opADD && (a.k == kStr || b.k == kStr) {
		return vStr(a.String() + b.String())
	}
	// comparações de igualdade genéricas
	if op == opEQ || op == opNE {
		eq := valueEq(a, b)
		if op == opNE {
			eq = !eq
		}
		return vBool(eq)
	}
	// operandos de string em <, >, etc.
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
	// float se algum operando for float
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
	// inteiros
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
