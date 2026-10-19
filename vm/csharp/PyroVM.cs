// ============================================================
//  Pyro C# VM — Execution Engine for .pyro Bytecode
//
//  C# / .NET implementation of the Pyro VM stack machine.
//  Loads and executes .pyro bytecode containers with parity
//  to the Go, C and C++ VM engines.
// ============================================================
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace PyroVM;

public enum ValueKind {
    Int,
    Float,
    Bool,
    Str,
    Null,
    Array,
    Map,
    Func
}

public class Value {
    public ValueKind Kind;
    public long I;
    public double F;
    public bool B;
    public string S;
    public List<Value> Arr;
    public Dictionary<string, (Value Key, Value Val)> Map;
    public int FnIdx;
    public List<Value> Captures;

    public static readonly Value Null = new Value { Kind = ValueKind.Null };

    public static Value FromInt(long val) => new Value { Kind = ValueKind.Int, I = val };
    public static Value FromFloat(double val) => new Value { Kind = ValueKind.Float, F = val };
    public static Value FromBool(bool val) => new Value { Kind = ValueKind.Bool, B = val };
    public static Value FromStr(string val) => new Value { Kind = ValueKind.Str, S = val ?? "" };
    public static Value FromArray(List<Value> arr) => new Value { Kind = ValueKind.Array, Arr = arr ?? new List<Value>() };
    public static Value FromMap(Dictionary<string, (Value Key, Value Val)> map) =>
        new Value { Kind = ValueKind.Map, Map = map ?? new Dictionary<string, (Value, Value)>() };
    public static Value FromFunc(int fnIdx, List<Value> cap = null) =>
        new Value { Kind = ValueKind.Func, FnIdx = fnIdx, Captures = cap };

    public bool IsTruthy() {
        return Kind switch {
            ValueKind.Null => false,
            ValueKind.Bool => B,
            ValueKind.Int => I != 0,
            ValueKind.Float => F != 0.0,
            ValueKind.Str => !string.IsNullOrEmpty(S),
            ValueKind.Array => true,
            ValueKind.Map => true,
            ValueKind.Func => true,
            _ => false
        };
    }

    public static string FormatFloat(double d) {
        if (double.IsNaN(d)) return "NaN";
        if (double.IsPositiveInfinity(d)) return "+Inf";
        if (double.IsNegativeInfinity(d)) return "-Inf";
        if (d == Math.Floor(d) && Math.Abs(d) < 1e15)
            return ((long)d).ToString(CultureInfo.InvariantCulture);
        return d.ToString("R", CultureInfo.InvariantCulture);
    }

    public override string ToString() {
        switch (Kind) {
            case ValueKind.Null: return "null";
            case ValueKind.Bool: return B ? "true" : "false";
            case ValueKind.Int: return I.ToString(CultureInfo.InvariantCulture);
            case ValueKind.Float: return FormatFloat(F);
            case ValueKind.Str: return S;
            case ValueKind.Array: {
                var parts = new List<string>();
                foreach (var v in Arr) parts.Add(v.ToString());
                return "[" + string.Join(", ", parts) + "]";
            }
            case ValueKind.Map: {
                var keys = new List<string>(Map.Keys);
                keys.Sort(StringComparer.Ordinal);
                var parts = new List<string>();
                foreach (var k in keys) parts.Add(Map[k].Key.ToString() + ": " + Map[k].Val.ToString());
                return "{" + string.Join(", ", parts) + "}";
            }
            case ValueKind.Func: return $"<fn {FnIdx}>";
            default: return "null";
        }
    }

    public bool IsEqual(Value other) {
        if (other == null) return Kind == ValueKind.Null;
        if (Kind == ValueKind.Int && other.Kind == ValueKind.Int) return I == other.I;
        if (Kind == ValueKind.Float && other.Kind == ValueKind.Float) return F == other.F;
        if (Kind == ValueKind.Int && other.Kind == ValueKind.Float) return (double)I == other.F;
        if (Kind == ValueKind.Float && other.Kind == ValueKind.Int) return F == (double)other.I;
        if (Kind != other.Kind) return false;
        switch (Kind) {
            case ValueKind.Null: return true;
            case ValueKind.Bool: return B == other.B;
            case ValueKind.Str: return S == other.S;
            case ValueKind.Array: return ReferenceEquals(Arr, other.Arr);
            case ValueKind.Map: return ReferenceEquals(Map, other.Map);
            case ValueKind.Func: return FnIdx == other.FnIdx;
            default: return false;
        }
    }
}

public class FuncInfo {
    public string Name;
    public uint Entry;
    public byte NParams;
    public ushort NLocals;
}

public class DebugEntry {
    public uint PC;
    public uint Line;
}

public struct Frame {
    public int RetPC;
    public int LocalsBase;
    public int NLocals;
    public int Fn;
}

public struct Handler {
    public int CatchPC;
    public int SP;
    public int FP;
    public int Slot;
}

public class Program {
    public List<Value> Consts = new();
    public List<FuncInfo> Funcs = new();
    public ushort EntryFn;
    public byte[] Code;
    public List<DebugEntry> Dbg = new();
    public Dictionary<string, byte[]> Assets = new();
    public bool Sandboxed;
}

public class VM {
    private const int STACK_MAX = 65536;
    private const int FRAME_MAX = 4096;
    private const int HANDLER_MAX = 4096;

    private readonly Program _prog;
    private readonly Value[] _stack = new Value[STACK_MAX];
    private int _sp = 0;
    private readonly Value[] _locals = new Value[STACK_MAX];
    private readonly Frame[] _frames = new Frame[FRAME_MAX];
    private int _fp = 0;
    private readonly Handler[] _handlers = new Handler[HANDLER_MAX];
    private int _hp = 0;
    private readonly List<Value> _globals = new();
    private int _pc = 0;

    private static readonly Random _rng = new();

    public VM(Program prog) {
        _prog = prog;
    }

    private void Fatal(string msg) {
        Console.Error.WriteLine("[Pyro VM] " + msg);
        PrintStackTrace();
        Environment.Exit(1);
    }

    private void PrintStackTrace() {
        if (_prog.Dbg == null || _prog.Dbg.Count == 0) return;
        Console.Error.WriteLine("  stack trace (most recent first):");
        for (int i = _fp - 1; i >= 0; i--) {
            var fr = _frames[i];
            string name = (fr.Fn >= 0 && fr.Fn < _prog.Funcs.Count) ? _prog.Funcs[fr.Fn].Name : "?";
            uint currentPC = (uint)((i == _fp - 1) ? _pc : fr.RetPC);
            int line = GetLineNumber(currentPC);
            Console.Error.WriteLine($"    at {name} (line {line})");
        }
    }

    private int GetLineNumber(uint targetPC) {
        int line = 0;
        foreach (var entry in _prog.Dbg) {
            if (entry.PC <= targetPC) line = (int)entry.Line;
            else break;
        }
        return line;
    }

    private bool RaiseException(Value v) {
        if (_hp == 0) return false;
        var h = _handlers[--_hp];
        _sp = h.SP;
        _fp = h.FP;
        if (h.Slot != 0xFFFF) {
            int locIdx = _frames[_fp - 1].LocalsBase + h.Slot;
            _locals[locIdx] = v;
        }
        _pc = h.CatchPC;
        return true;
    }

    private static ushort ReadU16(byte[] code, ref int pc) {
        ushort val = (ushort)(code[pc] | (code[pc + 1] << 8));
        pc += 2;
        return val;
    }

    private static int ReadI32(byte[] code, ref int pc) {
        int val = (int)(code[pc] | (code[pc + 1] << 8) | (code[pc + 2] << 16) | (code[pc + 3] << 24));
        pc += 4;
        return val;
    }

    private static uint ReadU32(byte[] code, ref int pc) {
        uint val = (uint)(code[pc] | (code[pc + 1] << 8) | (code[pc + 2] << 16) | (code[pc + 3] << 24));
        pc += 4;
        return val;
    }

    private int JumpTo(int currentPC, int rel, int codeLen) {
        long dest = (long)currentPC + rel;
        if (dest < 0 || dest > codeLen) {
            Fatal("malformed .pyro: jump target outside the code section");
        }
        return (int)dest;
    }

    public void Run() {
        if (_prog.Funcs.Count == 0) return;
        var mainFn = _prog.Funcs[_prog.EntryFn];
        for (int i = 0; i < mainFn.NLocals; i++) {
            _locals[i] = Value.Null;
        }
        _frames[0] = new Frame { RetPC = -1, LocalsBase = 0, NLocals = mainFn.NLocals, Fn = _prog.EntryFn };
        _fp = 1;
        _pc = (int)mainFn.Entry;
        _hp = 0;
        _sp = 0;

        byte[] code = _prog.Code;
        int codelen = code.Length;

        while (true) {
            if ((uint)_pc >= (uint)codelen) {
                Fatal("malformed .pyro: execution ran past the end of the code");
            }
            if (_sp > STACK_MAX - 4) {
                Fatal("malformed .pyro: value stack overflow");
            }

            byte op = code[_pc++];

            switch (op) {
                case 0x00: // opHALT
                    return;

                case 0x01: { // opCONST
                    ushort idx = ReadU16(code, ref _pc);
                    if (idx >= _prog.Consts.Count) Fatal("malformed .pyro: constant index out of range");
                    _stack[_sp++] = _prog.Consts[idx];
                    break;
                }

                case 0x02: // opTRUE
                    _stack[_sp++] = Value.FromBool(true);
                    break;

                case 0x03: // opFALSE
                    _stack[_sp++] = Value.FromBool(false);
                    break;

                case 0x04: // opNULL
                    _stack[_sp++] = Value.Null;
                    break;

                case 0x05: // opPOP
                    _sp--;
                    break;

                case 0x06: { // opLOAD
                    ushort slot = ReadU16(code, ref _pc);
                    _stack[_sp++] = _locals[_frames[_fp - 1].LocalsBase + slot];
                    break;
                }

                case 0x07: { // opSTORE
                    ushort slot = ReadU16(code, ref _pc);
                    _locals[_frames[_fp - 1].LocalsBase + slot] = _stack[--_sp];
                    break;
                }

                case 0x76: { // opGETGLOBAL
                    ushort slot = ReadU16(code, ref _pc);
                    _stack[_sp++] = (slot < _globals.Count) ? _globals[slot] : Value.Null;
                    break;
                }

                case 0x77: { // opSETGLOBAL
                    ushort slot = ReadU16(code, ref _pc);
                    while (_globals.Count <= slot) _globals.Add(Value.Null);
                    _globals[slot] = _stack[--_sp];
                    break;
                }

                case 0x10: { // opADD
                    var b = _stack[--_sp];
                    var a = _stack[--_sp];
                    if (a.Kind == ValueKind.Str || b.Kind == ValueKind.Str) {
                        _stack[_sp++] = Value.FromStr(a.ToString() + b.ToString());
                    } else if (a.Kind == ValueKind.Float || b.Kind == ValueKind.Float) {
                        double fa = (a.Kind == ValueKind.Float) ? a.F : (double)a.I;
                        double fb = (b.Kind == ValueKind.Float) ? b.F : (double)b.I;
                        _stack[_sp++] = Value.FromFloat(fa + fb);
                    } else {
                        long ires;
                        try { ires = checked(a.I + b.I); }
                        catch (OverflowException) { Fatal("[Cryo Security] Overflow: integer addition"); ires = 0; }
                        _stack[_sp++] = Value.FromInt(ires);
                    }
                    break;
                }

                case 0x11: { // opSUB
                    var b = _stack[--_sp];
                    var a = _stack[--_sp];
                    if (a.Kind == ValueKind.Float || b.Kind == ValueKind.Float) {
                        double fa = (a.Kind == ValueKind.Float) ? a.F : (double)a.I;
                        double fb = (b.Kind == ValueKind.Float) ? b.F : (double)b.I;
                        _stack[_sp++] = Value.FromFloat(fa - fb);
                    } else {
                        long ires;
                        try { ires = checked(a.I - b.I); }
                        catch (OverflowException) { Fatal("[Cryo Security] Overflow: integer subtraction"); ires = 0; }
                        _stack[_sp++] = Value.FromInt(ires);
                    }
                    break;
                }

                case 0x12: { // opMUL
                    var b = _stack[--_sp];
                    var a = _stack[--_sp];
                    if (a.Kind == ValueKind.Float || b.Kind == ValueKind.Float) {
                        double fa = (a.Kind == ValueKind.Float) ? a.F : (double)a.I;
                        double fb = (b.Kind == ValueKind.Float) ? b.F : (double)b.I;
                        _stack[_sp++] = Value.FromFloat(fa * fb);
                    } else {
                        long ires;
                        try { ires = checked(a.I * b.I); }
                        catch (OverflowException) { Fatal("[Cryo Security] Overflow: integer multiplication"); ires = 0; }
                        _stack[_sp++] = Value.FromInt(ires);
                    }
                    break;
                }

                case 0x13: { // opDIV
                    var b = _stack[--_sp];
                    var a = _stack[--_sp];
                    if (a.Kind == ValueKind.Float || b.Kind == ValueKind.Float) {
                        double fa = (a.Kind == ValueKind.Float) ? a.F : (double)a.I;
                        double fb = (b.Kind == ValueKind.Float) ? b.F : (double)b.I;
                        if (fb == 0.0) {
                            string msg = "[Cryo Security] DivisionByZeroError: division by zero";
                            if (!RaiseException(Value.FromStr(msg))) Fatal(msg);
                            break;
                        }
                        _stack[_sp++] = Value.FromFloat(fa / fb);
                    } else {
                        if (b.I == 0) {
                            string msg = "[Cryo Security] DivisionByZeroError: division by zero";
                            if (!RaiseException(Value.FromStr(msg))) Fatal(msg);
                            break;
                        }
                        _stack[_sp++] = Value.FromInt(a.I / b.I);
                    }
                    break;
                }

                case 0x14: { // opMOD
                    var b = _stack[--_sp];
                    var a = _stack[--_sp];
                    if (b.I == 0) {
                        string msg = "[Cryo Security] DivByZero: integer division";
                        if (!RaiseException(Value.FromStr(msg))) Fatal(msg);
                        break;
                    }
                    _stack[_sp++] = Value.FromInt(a.I % b.I);
                    break;
                }

                case 0x15: { // opNEG
                    var a = _stack[_sp - 1];
                    if (a.Kind == ValueKind.Float) _stack[_sp - 1] = Value.FromFloat(-a.F);
                    else _stack[_sp - 1] = Value.FromInt(-a.I);
                    break;
                }

                case 0x16: { // opBAND
                    var b = _stack[--_sp];
                    var a = _stack[--_sp];
                    _stack[_sp++] = Value.FromInt(a.I & b.I);
                    break;
                }

                case 0x17: { // opBOR
                    var b = _stack[--_sp];
                    var a = _stack[--_sp];
                    _stack[_sp++] = Value.FromInt(a.I | b.I);
                    break;
                }

                case 0x18: { // opBXOR
                    var b = _stack[--_sp];
                    var a = _stack[--_sp];
                    _stack[_sp++] = Value.FromInt(a.I ^ b.I);
                    break;
                }

                case 0x19: { // opSHL
                    var b = _stack[--_sp];
                    var a = _stack[--_sp];
                    _stack[_sp++] = Value.FromInt(a.I << (int)b.I);
                    break;
                }

                case 0x1A: { // opSHR
                    var b = _stack[--_sp];
                    var a = _stack[--_sp];
                    _stack[_sp++] = Value.FromInt(a.I >> (int)b.I);
                    break;
                }

                case 0x1B: { // opBNOT
                    var a = _stack[_sp - 1];
                    _stack[_sp - 1] = Value.FromInt(~a.I);
                    break;
                }

                case 0x20: { // opEQ
                    var b = _stack[--_sp];
                    var a = _stack[--_sp];
                    _stack[_sp++] = Value.FromBool(a.IsEqual(b));
                    break;
                }

                case 0x21: { // opNE
                    var b = _stack[--_sp];
                    var a = _stack[--_sp];
                    _stack[_sp++] = Value.FromBool(!a.IsEqual(b));
                    break;
                }

                case 0x22: { // opLT
                    var b = _stack[--_sp];
                    var a = _stack[--_sp];
                    if (a.Kind == ValueKind.Float || b.Kind == ValueKind.Float) {
                        double fa = (a.Kind == ValueKind.Float) ? a.F : (double)a.I;
                        double fb = (b.Kind == ValueKind.Float) ? b.F : (double)b.I;
                        _stack[_sp++] = Value.FromBool(fa < fb);
                    } else if (a.Kind == ValueKind.Str && b.Kind == ValueKind.Str) {
                        _stack[_sp++] = Value.FromBool(string.CompareOrdinal(a.S, b.S) < 0);
                    } else {
                        _stack[_sp++] = Value.FromBool(a.I < b.I);
                    }
                    break;
                }

                case 0x23: { // opGT
                    var b = _stack[--_sp];
                    var a = _stack[--_sp];
                    if (a.Kind == ValueKind.Float || b.Kind == ValueKind.Float) {
                        double fa = (a.Kind == ValueKind.Float) ? a.F : (double)a.I;
                        double fb = (b.Kind == ValueKind.Float) ? b.F : (double)b.I;
                        _stack[_sp++] = Value.FromBool(fa > fb);
                    } else if (a.Kind == ValueKind.Str && b.Kind == ValueKind.Str) {
                        _stack[_sp++] = Value.FromBool(string.CompareOrdinal(a.S, b.S) > 0);
                    } else {
                        _stack[_sp++] = Value.FromBool(a.I > b.I);
                    }
                    break;
                }

                case 0x24: { // opLE
                    var b = _stack[--_sp];
                    var a = _stack[--_sp];
                    if (a.Kind == ValueKind.Float || b.Kind == ValueKind.Float) {
                        double fa = (a.Kind == ValueKind.Float) ? a.F : (double)a.I;
                        double fb = (b.Kind == ValueKind.Float) ? b.F : (double)b.I;
                        _stack[_sp++] = Value.FromBool(fa <= fb);
                    } else if (a.Kind == ValueKind.Str && b.Kind == ValueKind.Str) {
                        _stack[_sp++] = Value.FromBool(string.CompareOrdinal(a.S, b.S) <= 0);
                    } else {
                        _stack[_sp++] = Value.FromBool(a.I <= b.I);
                    }
                    break;
                }

                case 0x25: { // opGE
                    var b = _stack[--_sp];
                    var a = _stack[--_sp];
                    if (a.Kind == ValueKind.Float || b.Kind == ValueKind.Float) {
                        double fa = (a.Kind == ValueKind.Float) ? a.F : (double)a.I;
                        double fb = (b.Kind == ValueKind.Float) ? b.F : (double)b.I;
                        _stack[_sp++] = Value.FromBool(fa >= fb);
                    } else if (a.Kind == ValueKind.Str && b.Kind == ValueKind.Str) {
                        _stack[_sp++] = Value.FromBool(string.CompareOrdinal(a.S, b.S) >= 0);
                    } else {
                        _stack[_sp++] = Value.FromBool(a.I >= b.I);
                    }
                    break;
                }

                case 0x26: { // opNOT
                    var a = _stack[_sp - 1];
                    _stack[_sp - 1] = Value.FromBool(!a.IsTruthy());
                    break;
                }

                case 0x30: { // opJMP
                    int rel = ReadI32(code, ref _pc);
                    _pc = JumpTo(_pc, rel, codelen);
                    break;
                }

                case 0x31: { // opJMPF
                    int rel = ReadI32(code, ref _pc);
                    var a = _stack[--_sp];
                    if (!a.IsTruthy()) _pc = JumpTo(_pc, rel, codelen);
                    break;
                }

                case 0x32: { // opJMPT
                    int rel = ReadI32(code, ref _pc);
                    var a = _stack[--_sp];
                    if (a.IsTruthy()) _pc = JumpTo(_pc, rel, codelen);
                    break;
                }

                case 0x40: { // opCALL
                    ushort fi = ReadU16(code, ref _pc);
                    byte argc = code[_pc++];
                    if (fi >= _prog.Funcs.Count) Fatal("malformed .pyro: function index out of range");
                    var fn = _prog.Funcs[fi];
                    int nextBase = _frames[_fp - 1].LocalsBase + _frames[_fp - 1].NLocals;
                    if (nextBase + fn.NLocals > STACK_MAX - 2) Fatal("malformed .pyro: locals stack overflow");
                    for (int i = 0; i < fn.NLocals; i++) _locals[nextBase + i] = Value.Null;
                    int baseIdx = _sp - argc;
                    for (int i = 0; i < argc; i++) _locals[nextBase + i] = _stack[baseIdx + i];
                    _sp = baseIdx;
                    if (_fp > FRAME_MAX - 2) Fatal("malformed .pyro: call stack overflow");
                    _frames[_fp++] = new Frame { RetPC = _pc, LocalsBase = nextBase, NLocals = fn.NLocals, Fn = fi };
                    _pc = (int)fn.Entry;
                    break;
                }

                case 0x41: { // opRET
                    var ret = _stack[--_sp];
                    if (_fp <= 0) Fatal("malformed .pyro: return with no frame to return to");
                    var fr = _frames[--_fp];
                    if (fr.RetPC < 0) return;
                    _pc = fr.RetPC;
                    _stack[_sp++] = ret;
                    break;
                }

                case 0x42: { // opPUSHFN
                    ushort fi = ReadU16(code, ref _pc);
                    _stack[_sp++] = Value.FromFunc(fi);
                    break;
                }

                case 0x44: { // opCLOSURE
                    ushort fi = ReadU16(code, ref _pc);
                    byte ncap = code[_pc++];
                    var cap = new List<Value>();
                    for (int i = 0; i < ncap; i++) cap.Add(_stack[_sp - ncap + i]);
                    _sp -= ncap;
                    _stack[_sp++] = Value.FromFunc(fi, cap);
                    break;
                }

                case 0x43: { // opCALLVALUE
                    byte argc = code[_pc++];
                    int baseIdx = _sp - argc;
                    var fnval = _stack[baseIdx - 1];
                    if (fnval.Kind != ValueKind.Func) Fatal("call of a non-function value");
                    int fi = fnval.FnIdx;
                    if (fi >= _prog.Funcs.Count) Fatal("malformed .pyro: function index out of range");
                    var fn = _prog.Funcs[fi];
                    int nextBase = _frames[_fp - 1].LocalsBase + _frames[_fp - 1].NLocals;
                    if (nextBase + fn.NLocals > STACK_MAX - 2) Fatal("malformed .pyro: locals stack overflow");
                    for (int i = 0; i < fn.NLocals; i++) _locals[nextBase + i] = Value.Null;
                    int ncap = fnval.Captures != null ? fnval.Captures.Count : 0;
                    for (int i = 0; i < ncap; i++) _locals[nextBase + i] = fnval.Captures[i];
                    for (int i = 0; i < argc; i++) _locals[nextBase + ncap + i] = _stack[baseIdx + i];
                    _sp = baseIdx - 1;
                    if (_fp > FRAME_MAX - 2) Fatal("malformed .pyro: call stack overflow");
                    _frames[_fp++] = new Frame { RetPC = _pc, LocalsBase = nextBase, NLocals = fn.NLocals, Fn = fi };
                    _pc = (int)fn.Entry;
                    break;
                }

                case 0x50: { // opPRINT
                    var v = _stack[--_sp];
                    Console.WriteLine(v.ToString());
                    break;
                }

                case 0x51: { // opASSERT
                    var cond = _stack[--_sp];
                    var msg = _stack[--_sp];
                    if (!cond.IsTruthy()) {
                        string err = "[Cryo Assert] " + msg.ToString();
                        if (!RaiseException(Value.FromStr(err))) Fatal(err);
                    }
                    break;
                }

                case 0x52: // opPRINTLN
                    Console.WriteLine();
                    break;

                case 0x60: { // opNEWARR
                    ushort n = ReadU16(code, ref _pc);
                    if (n > _sp) Fatal("malformed .pyro: array literal larger than the stack");
                    var arr = new List<Value>();
                    int baseIdx = _sp - n;
                    for (int i = 0; i < n; i++) arr.Add(_stack[baseIdx + i]);
                    _sp = baseIdx;
                    _stack[_sp++] = Value.FromArray(arr);
                    break;
                }

                case 0x61: { // opNEWMAP
                    ushort n = ReadU16(code, ref _pc);
                    if (2 * n > _sp) Fatal("malformed .pyro: map literal larger than the stack");
                    var map = new Dictionary<string, (Value Key, Value Val)>();
                    int baseIdx = _sp - 2 * n;
                    for (int i = 0; i < n; i++) {
                        var k = _stack[baseIdx + 2 * i];
                        var v = _stack[baseIdx + 2 * i + 1];
                        map[k.ToString()] = (k, v);
                    }
                    _sp = baseIdx;
                    _stack[_sp++] = Value.FromMap(map);
                    break;
                }

                case 0x62: { // opINDEX
                    var key = _stack[--_sp];
                    var cont = _stack[--_sp];
                    if (cont.Kind == ValueKind.Array) {
                        long idx = key.I;
                        if (idx < 0 || idx >= cont.Arr.Count) {
                            string msg = $"[Cryo Security] IndexError: index {idx} out of bounds (len={cont.Arr.Count})";
                            if (!RaiseException(Value.FromStr(msg))) Fatal(msg);
                            break;
                        }
                        _stack[_sp++] = cont.Arr[(int)idx];
                    } else if (cont.Kind == ValueKind.Map) {
                        string k = key.ToString();
                        _stack[_sp++] = cont.Map.TryGetValue(k, out var pair) ? pair.Val : Value.Null;
                    } else if (cont.Kind == ValueKind.Str) {
                        long idx = key.I;
                        if (idx < 0 || idx >= cont.S.Length) {
                            string msg = "[Cryo Security] IndexError: string index out of bounds";
                            if (!RaiseException(Value.FromStr(msg))) Fatal(msg);
                            break;
                        }
                        _stack[_sp++] = Value.FromStr(cont.S[(int)idx].ToString());
                    } else {
                        Fatal("indexing on non-collection");
                    }
                    break;
                }

                case 0x63: { // opSETIDX
                    var val = _stack[--_sp];
                    var key = _stack[--_sp];
                    var cont = _stack[--_sp];
                    if (cont.Kind == ValueKind.Array) {
                        long idx = key.I;
                        if (idx < 0 || idx >= cont.Arr.Count) {
                            string msg = $"[Cryo Security] IndexError: index {idx} out of bounds";
                            if (!RaiseException(Value.FromStr(msg))) Fatal(msg);
                            break;
                        }
                        cont.Arr[(int)idx] = val;
                    } else if (cont.Kind == ValueKind.Map) {
                        cont.Map[key.ToString()] = (key, val);
                    } else {
                        Fatal("index assignment on non-collection");
                    }
                    break;
                }

                case 0x64: { // opLEN
                    var v = _stack[_sp - 1];
                    long len = v.Kind switch {
                        ValueKind.Str => v.S.Length,
                        ValueKind.Array => v.Arr.Count,
                        ValueKind.Map => v.Map.Count,
                        _ => 0
                    };
                    _stack[_sp - 1] = Value.FromInt(len);
                    break;
                }

                case 0x65: { // opAPPEND
                    var val = _stack[--_sp];
                    var arr = _stack[--_sp];
                    if (arr.Kind != ValueKind.Array) Fatal("push on a non-array value");
                    arr.Arr.Add(val);
                    _stack[_sp++] = Value.FromInt(arr.Arr.Count);
                    break;
                }

                case 0x66: { // opHAS
                    var key = _stack[--_sp];
                    var mp = _stack[_sp - 1];
                    bool has = (mp.Kind == ValueKind.Map) && mp.Map.ContainsKey(key.ToString());
                    _stack[_sp - 1] = Value.FromBool(has);
                    break;
                }

                case 0x67: { // opKEYS
                    var mp = _stack[_sp - 1];
                    if (mp.Kind != ValueKind.Map) Fatal("keys() applied to a non-map value");
                    var keys = new List<string>(mp.Map.Keys);
                    keys.Sort(StringComparer.Ordinal);
                    var karr = new List<Value>();
                    foreach (var k in keys) karr.Add(mp.Map[k].Key);
                    _stack[_sp - 1] = Value.FromArray(karr);
                    break;
                }

                case 0x70: { // opNATIVE
                    byte nid = code[_pc++];
                    byte argc = code[_pc++];
                    int baseIdx = _sp - argc;
                    var args = new Value[argc];
                    for (int i = 0; i < argc; i++) args[i] = _stack[baseIdx + i];
                    _sp = baseIdx;
                    var res = CallNative(nid, args);
                    _stack[_sp++] = res;
                    break;
                }

                case 0x71: { // opTRYPUSH
                    int rel = ReadI32(code, ref _pc);
                    ushort slot = ReadU16(code, ref _pc);
                    int catchPC = JumpTo(_pc, rel, codelen);
                    if (_hp > HANDLER_MAX - 2) Fatal("malformed .pyro: exception handler stack overflow");
                    _handlers[_hp++] = new Handler { CatchPC = catchPC, SP = _sp, FP = _fp, Slot = (int)slot };
                    break;
                }

                case 0x72: { // opTRYPOP
                    if (_hp > 0) _hp--;
                    break;
                }

                case 0x73: { // opTHROW
                    var v = _stack[--_sp];
                    if (!RaiseException(v)) {
                        Fatal("uncaught exception: " + v.ToString());
                    }
                    break;
                }

                case 0x74: { // opCOALESCE
                    var b = _stack[--_sp];
                    var a = _stack[--_sp];
                    _stack[_sp++] = (a.Kind == ValueKind.Null) ? b : a;
                    break;
                }

                case 0x75: { // opUNWRAP
                    var a = _stack[_sp - 1];
                    if (a.Kind == ValueKind.Null) {
                        _sp--;
                        string um = "[Cryo Security] unwrap of null value";
                        if (!RaiseException(Value.FromStr(um))) Fatal(um);
                    }
                    break;
                }

                case 0x78:
                case 0x79:
                    Fatal("[Cryo Concurrency] spawn/await need the Go VM — this C# VM has no scheduler (roadmap 12.5)");
                    break;

                default:
                    Fatal("invalid opcode in bytecode: 0x" + op.ToString("X2"));
                    break;
            }
        }
    }

    private Value CallNative(byte id, Value[] a) {
        switch (id) {
            case 0: // sqrt
                return Value.FromFloat(Math.Sqrt(a[0].Kind == ValueKind.Float ? a[0].F : a[0].I));
            case 1: { // pow
                double baseVal = a[0].Kind == ValueKind.Float ? a[0].F : a[0].I;
                double expVal = a[1].Kind == ValueKind.Float ? a[1].F : a[1].I;
                return Value.FromFloat(Math.Pow(baseVal, expVal));
            }
            case 2: // abs
                return (a[0].Kind == ValueKind.Float) ? Value.FromFloat(Math.Abs(a[0].F)) : Value.FromInt(Math.Abs(a[0].I));
            case 3: // min
                if (a[0].Kind == ValueKind.Float || a[1].Kind == ValueKind.Float) {
                    double d0 = a[0].Kind == ValueKind.Float ? a[0].F : a[0].I;
                    double d1 = a[1].Kind == ValueKind.Float ? a[1].F : a[1].I;
                    return Value.FromFloat(Math.Min(d0, d1));
                }
                return Value.FromInt(Math.Min(a[0].I, a[1].I));
            case 4: // max
                if (a[0].Kind == ValueKind.Float || a[1].Kind == ValueKind.Float) {
                    double d0 = a[0].Kind == ValueKind.Float ? a[0].F : a[0].I;
                    double d1 = a[1].Kind == ValueKind.Float ? a[1].F : a[1].I;
                    return Value.FromFloat(Math.Max(d0, d1));
                }
                return Value.FromInt(Math.Max(a[0].I, a[1].I));
            case 5: // floor
                return Value.FromFloat(Math.Floor(a[0].Kind == ValueKind.Float ? a[0].F : a[0].I));
            case 6: // ceil
                return Value.FromFloat(Math.Ceiling(a[0].Kind == ValueKind.Float ? a[0].F : a[0].I));
            case 7: { // round
                double d = a[0].Kind == ValueKind.Float ? a[0].F : a[0].I;
                return Value.FromFloat(Math.Round(d, MidpointRounding.AwayFromZero));
            }
            case 8: // to_string
                return Value.FromStr(a[0].ToString());
            case 9: { // to_int
                if (a[0].Kind == ValueKind.Str && long.TryParse(a[0].S, NumberStyles.Integer, CultureInfo.InvariantCulture, out long l))
                    return Value.FromInt(l);
                return (a[0].Kind == ValueKind.Float) ? Value.FromInt((long)a[0].F) : a[0];
            }
            case 10: { // to_number
                if (a[0].Kind == ValueKind.Str && double.TryParse(a[0].S, NumberStyles.Float, CultureInfo.InvariantCulture, out double d))
                    return Value.FromFloat(d);
                return (a[0].Kind == ValueKind.Int) ? Value.FromFloat((double)a[0].I) : a[0];
            }
            case 11: { // remove (map)
                if (a[0].Kind == ValueKind.Map) {
                    a[0].Map.Remove(a[1].ToString());
                }
                return Value.Null;
            }
            case 12: // upper
                return Value.FromStr(a[0].ToString().ToUpperInvariant());
            case 13: // lower
                return Value.FromStr(a[0].ToString().ToLowerInvariant());
            case 14: // trim
                return Value.FromStr(a[0].ToString().Trim());
            case 15: { // contains
                string s = a[0].ToString();
                string sub = a[1].ToString();
                return Value.FromBool(s.Contains(sub));
            }
            case 16: { // find
                string s = a[0].ToString();
                string sub = a[1].ToString();
                return Value.FromInt(s.IndexOf(sub, StringComparison.Ordinal));
            }
            case 17: { // replace
                string s = a[0].ToString();
                string oldV = a[1].ToString();
                string newV = a[2].ToString();
                return Value.FromStr(s.Replace(oldV, newV));
            }
            case 18: { // substr
                string s = a[0].ToString();
                int start = (int)a[1].I;
                int len = (int)a[2].I;
                if (start < 0 || start > s.Length || len < 0) return Value.FromStr("");
                if (start + len > s.Length) len = s.Length - start;
                return Value.FromStr(s.Substring(start, len));
            }
            case 19: { // split
                string s = a[0].ToString();
                string sep = a[1].ToString();
                string[] parts = s.Split(new[] { sep }, StringSplitOptions.None);
                var arr = new List<Value>();
                foreach (var p in parts) arr.Add(Value.FromStr(p));
                return Value.FromArray(arr);
            }
            case 20: { // join
                if (a[0].Kind != ValueKind.Array) return Value.FromStr("");
                string sep = a[1].ToString();
                var parts = new List<string>();
                foreach (var x in a[0].Arr) parts.Add(x.ToString());
                return Value.FromStr(string.Join(sep, parts));
            }
            case 21: { // input
                return Value.FromStr(Console.ReadLine() ?? "");
            }
            case 22: { // json_encode
                return Value.FromStr(JsonEncode(a[0]));
            }
            case 26: { // sleep
                long ms = a[0].I;
                if (ms > 0) System.Threading.Thread.Sleep((int)ms);
                return Value.Null;
            }
            case 28: { // read_file
                string path = a[0].ToString();
                if (!File.Exists(path)) return Value.Null;
                return Value.FromStr(File.ReadAllText(path));
            }
            case 29: { // args
                var arr = new List<Value>();
                string[] cliArgs = Environment.GetCommandLineArgs();
                for (int i = 2; i < cliArgs.Length; i++) arr.Add(Value.FromStr(cliArgs[i]));
                return Value.FromArray(arr);
            }
            case 31: { // clamp
                if (a[0].Kind == ValueKind.Float || a[1].Kind == ValueKind.Float || a[2].Kind == ValueKind.Float) {
                    double v = a[0].Kind == ValueKind.Float ? a[0].F : a[0].I;
                    double min = a[1].Kind == ValueKind.Float ? a[1].F : a[1].I;
                    double max = a[2].Kind == ValueKind.Float ? a[2].F : a[2].I;
                    return Value.FromFloat(Math.Clamp(v, min, max));
                }
                return Value.FromInt(Math.Clamp(a[0].I, a[1].I, a[2].I));
            }
            case 32: { // sign
                long v = a[0].I;
                return Value.FromInt(v > 0 ? 1 : (v < 0 ? -1 : 0));
            }
            case 33: { // gcd
                long u = Math.Abs(a[0].I);
                long v = Math.Abs(a[1].I);
                while (v != 0) { long t = v; v = u % v; u = t; }
                return Value.FromInt(u);
            }
            case 34: { // hypot
                double x = a[0].Kind == ValueKind.Float ? a[0].F : a[0].I;
                double y = a[1].Kind == ValueKind.Float ? a[1].F : a[1].I;
                return Value.FromFloat(Math.Sqrt(x * x + y * y));
            }
            case 35: // starts_with
                return Value.FromBool(a[0].ToString().StartsWith(a[1].ToString(), StringComparison.Ordinal));
            case 36: // ends_with
                return Value.FromBool(a[0].ToString().EndsWith(a[1].ToString(), StringComparison.Ordinal));
            case 37: { // repeat
                string s = a[0].ToString();
                int count = (int)a[1].I;
                var sb = new StringBuilder();
                for (int i = 0; i < count; i++) sb.Append(s);
                return Value.FromStr(sb.ToString());
            }
            case 38: { // sort
                if (a[0].Kind == ValueKind.Array) {
                    var copy = new List<Value>(a[0].Arr);
                    copy.Sort((x, y) => {
                        if (x.Kind == ValueKind.Int && y.Kind == ValueKind.Int) return x.I.CompareTo(y.I);
                        if (x.Kind == ValueKind.Float || y.Kind == ValueKind.Float) {
                            double dx = x.Kind == ValueKind.Float ? x.F : x.I;
                            double dy = y.Kind == ValueKind.Float ? y.F : y.I;
                            return dx.CompareTo(dy);
                        }
                        return string.CompareOrdinal(x.ToString(), y.ToString());
                    });
                    return Value.FromArray(copy);
                }
                return a[0];
            }
            case 39: { // reverse
                if (a[0].Kind == ValueKind.Array) {
                    var copy = new List<Value>(a[0].Arr);
                    copy.Reverse();
                    return Value.FromArray(copy);
                }
                return a[0];
            }
            case 40: { // slice
                if (a[0].Kind == ValueKind.Array) {
                    int start = (int)a[1].I;
                    int end = (int)a[2].I;
                    int count = a[0].Arr.Count;
                    if (start < 0) start = 0;
                    if (end > count) end = count;
                    if (start >= end) return Value.FromArray(new List<Value>());
                    return Value.FromArray(a[0].Arr.GetRange(start, end - start));
                }
                if (a[0].Kind == ValueKind.Str) {
                    string s = a[0].S;
                    int start = (int)a[1].I;
                    int end = (int)a[2].I;
                    if (start < 0) start = 0;
                    if (end > s.Length) end = s.Length;
                    if (start >= end) return Value.FromStr("");
                    return Value.FromStr(s.Substring(start, end - start));
                }
                return a[0];
            }
            case 41: { // index_of
                if (a[0].Kind == ValueKind.Array) {
                    for (int i = 0; i < a[0].Arr.Count; i++) {
                        if (a[0].Arr[i].IsEqual(a[1])) return Value.FromInt(i);
                    }
                    return Value.FromInt(-1);
                }
                return Value.FromInt(-1);
            }
            case 42: { // pad_start
                string s = a[0].ToString();
                int width = (int)a[1].I;
                string pad = a[2].ToString();
                return Value.FromStr(NativePad(s, width, pad, true));
            }
            case 43: { // pad_end
                string s = a[0].ToString();
                int width = (int)a[1].I;
                string pad = a[2].ToString();
                return Value.FromStr(NativePad(s, width, pad, false));
            }
            case 44: { // concat
                if (a[0].Kind == ValueKind.Array && a[1].Kind == ValueKind.Array) {
                    var combined = new List<Value>(a[0].Arr);
                    combined.AddRange(a[1].Arr);
                    return Value.FromArray(combined);
                }
                return a[0];
            }
            case 45: { // count
                if (a[0].Kind == ValueKind.Array) {
                    int cnt = 0;
                    foreach (var x in a[0].Arr) if (x.IsEqual(a[1])) cnt++;
                    return Value.FromInt(cnt);
                }
                return Value.FromInt(0);
            }
            case 46: { // sum
                if (a[0].Kind == ValueKind.Array) {
                    bool isFloat = false;
                    double sumF = 0;
                    long sumI = 0;
                    foreach (var x in a[0].Arr) {
                        if (x.Kind == ValueKind.Float) { isFloat = true; sumF += x.F; }
                        else { sumI += x.I; sumF += x.I; }
                    }
                    return isFloat ? Value.FromFloat(sumF) : Value.FromInt(sumI);
                }
                return Value.FromInt(0);
            }
            case 47: // now_ms
                return Value.FromInt(DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
            case 48: // monotonic_ms
                return Value.FromInt(Environment.TickCount64);
            case 49: // random
                return Value.FromFloat(_rng.NextDouble());
            case 50: { // random_int
                long min = a[0].I;
                long max = a[1].I;
                return Value.FromInt((long)(min + _rng.NextDouble() * (max - min)));
            }
            case 55: // file_exists
                return Value.FromBool(File.Exists(a[0].ToString()));
            case 56: // is_dir
                return Value.FromBool(Directory.Exists(a[0].ToString()));
            case 60: { // file_size
                string path = a[0].ToString();
                return Value.FromInt(File.Exists(path) ? new FileInfo(path).Length : 0);
            }
            case 61: { // write_file
                string path = a[0].ToString();
                string content = a[1].ToString();
                File.WriteAllText(path, content);
                return Value.FromBool(true);
            }
            case 62: { // env
                string varName = a[0].ToString();
                return Value.FromStr(Environment.GetEnvironmentVariable(varName) ?? "");
            }
            default:
                return Value.Null;
        }
    }

    private static string NativePad(string s, int width, string pad, bool atStart) {
        if (s.Length >= width || string.IsNullOrEmpty(pad)) return s;
        int need = width - s.Length;
        var sb = new StringBuilder();
        while (sb.Length < need) sb.Append(pad);
        string filler = sb.ToString().Substring(0, need);
        return atStart ? (filler + s) : (s + filler);
    }

    private static string JsonQuote(string s) {
        if (s == null) return "null";
        var sb = new StringBuilder("\"");
        foreach (char c in s) {
            if (c == '"') sb.Append("\\\"");
            else if (c == '\\') sb.Append("\\\\");
            else if (c == '\b') sb.Append("\\b");
            else if (c == '\f') sb.Append("\\f");
            else if (c == '\n') sb.Append("\\n");
            else if (c == '\r') sb.Append("\\r");
            else if (c == '\t') sb.Append("\\t");
            else sb.Append(c);
        }
        sb.Append("\"");
        return sb.ToString();
    }

    private static string JsonEncode(Value v) {
        if (v == null || v.Kind == ValueKind.Null) return "null";
        if (v.Kind == ValueKind.Bool) return v.B ? "true" : "false";
        if (v.Kind == ValueKind.Int) return v.I.ToString(CultureInfo.InvariantCulture);
        if (v.Kind == ValueKind.Float) return Value.FormatFloat(v.F);
        if (v.Kind == ValueKind.Str) return JsonQuote(v.S);
        if (v.Kind == ValueKind.Array) {
            var parts = new List<string>();
            foreach (var x in v.Arr) parts.Add(JsonEncode(x));
            return "[" + string.Join(",", parts) + "]";
        }
        if (v.Kind == ValueKind.Map) {
            var keys = new List<string>(v.Map.Keys);
            keys.Sort(StringComparer.Ordinal);
            var parts = new List<string>();
            foreach (var k in keys) {
                parts.Add(JsonQuote(v.Map[k].Key.ToString()) + ":" + JsonEncode(v.Map[k].Val));
            }
            return "{" + string.Join(",", parts) + "}";
        }
        return JsonQuote(v.ToString());
    }

    public static Program Load(byte[] data) {
        if (data == null || data.Length < 6 ||
            data[0] != 'P' || data[1] != 'Y' || data[2] != 'R' || data[3] != 'O') {
            Console.Error.WriteLine("[Pyro VM] invalid .pyro file (magic)");
            Environment.Exit(1);
        }

        byte ver = data[4];
        if (ver != 2 && ver != 3) {
            Console.Error.WriteLine("[Pyro VM] unsupported .pyro version (expected v2 or v3)");
            Environment.Exit(1);
        }
        byte flags = data[5];
        var p = new Program { Sandboxed = (flags & 0x04) != 0 };
        int pos = 6;

        ushort nconsts = (ushort)(data[pos] | (data[pos + 1] << 8));
        pos += 2;
        for (int i = 0; i < nconsts; i++) {
            byte tag = data[pos++];
            switch (tag) {
                case 1: { // INT
                    long val = BitConverter.ToInt64(data, pos);
                    pos += 8;
                    p.Consts.Add(Value.FromInt(val));
                    break;
                }
                case 2: { // FLT
                    double val = BitConverter.ToDouble(data, pos);
                    pos += 8;
                    p.Consts.Add(Value.FromFloat(val));
                    break;
                }
                case 3: { // STR
                    uint len = (ver >= 3)
                        ? (uint)(data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24))
                        : (uint)(data[pos] | (data[pos + 1] << 8));
                    pos += (ver >= 3) ? 4 : 2;
                    string str = Encoding.UTF8.GetString(data, pos, (int)len);
                    pos += (int)len;
                    p.Consts.Add(Value.FromStr(str));
                    break;
                }
                case 4: { // BOOL
                    p.Consts.Add(Value.FromBool(data[pos++] != 0));
                    break;
                }
                default:
                    Console.Error.WriteLine("[Pyro VM] unknown constant tag");
                    Environment.Exit(1);
                    break;
            }
        }

        ushort nfuncs = (ushort)(data[pos] | (data[pos + 1] << 8));
        pos += 2;
        for (int i = 0; i < nfuncs; i++) {
            ushort nameidx = (ushort)(data[pos] | (data[pos + 1] << 8));
            pos += 2;
            uint entry = (uint)(data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24));
            pos += 4;
            byte nparams = data[pos++];
            ushort nlocals = (ushort)(data[pos] | (data[pos + 1] << 8));
            pos += 2;
            p.Funcs.Add(new FuncInfo {
                Name = p.Consts[nameidx].ToString(),
                Entry = entry,
                NParams = nparams,
                NLocals = nlocals
            });
        }

        p.EntryFn = (ushort)(data[pos] | (data[pos + 1] << 8));
        pos += 2;

        uint codelen = (uint)(data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24));
        pos += 4;
        p.Code = new byte[codelen];
        Buffer.BlockCopy(data, pos, p.Code, 0, (int)codelen);
        pos += (int)codelen;

        if ((flags & 0x01) != 0) {
            byte k = 0x5A;
            for (int i = 0; i < p.Code.Length; i++) {
                byte b = (byte)(p.Code[i] ^ k);
                p.Code[i] = b;
                k = (byte)((k * 31 + 7 + b) & 0xFF);
            }
        }

        if ((flags & 0x02) != 0 && pos < data.Length) {
            uint ndbg = (uint)(data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24));
            pos += 4;
            for (int i = 0; i < ndbg; i++) {
                uint dpc = (uint)(data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24));
                pos += 4;
                uint dline = (uint)(data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24));
                pos += 4;
                p.Dbg.Add(new DebugEntry { PC = dpc, Line = dline });
            }
        }

        if ((flags & 0x08) != 0 && pos < data.Length) {
            uint nassets = (uint)(data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24));
            pos += 4;
            for (int i = 0; i < nassets; i++) {
                uint nl = (uint)(data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24));
                pos += 4;
                string aname = Encoding.UTF8.GetString(data, pos, (int)nl);
                pos += (int)nl;
                uint dl = (uint)(data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24));
                pos += 4;
                byte[] blob = new byte[dl];
                Buffer.BlockCopy(data, pos, blob, 0, (int)dl);
                pos += (int)dl;
                p.Assets[aname] = blob;
            }
        }

        return p;
    }
}

public static class EntryPoint {
    public static int Main(string[] args) {
        if (args.Length < 1) {
            Console.Error.WriteLine("[Pyro VM C#] usage: pyrovm_cs program.pyro");
            return 1;
        }

        string path = args[0];
        if (!File.Exists(path)) {
            Console.Error.WriteLine("[Pyro VM C#] could not read: " + path);
            return 1;
        }

        byte[] bytes = File.ReadAllBytes(path);
        var program = VM.Load(bytes);
        var vm = new VM(program);
        vm.Run();

        return 0;
    }
}
