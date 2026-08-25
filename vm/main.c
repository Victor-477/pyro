// ============================================================
//  Pyro VM — execution engine (.pyro)
//
//  Loads/decodes the bytecode and runs the stack machine.
//  The runtime (values, containers, natives) lives in pyro_runtime.c;
//  this file depends only on pyro_runtime.h. (Phase 9.2)
// ============================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "pyro_runtime.h"

// ── Program and VM State Structures ─────────────────────────
typedef struct {
    char* name;
    uint32_t entry;
    uint8_t nparams;
    uint16_t nlocals;
} FuncInfo;

typedef struct {
    uint32_t pc;
    uint32_t line;
} DebugEntry;

typedef struct {
    Value* consts;
    int nconsts;
    FuncInfo* funcs;
    int nfuncs;
    uint16_t entryFn;
    uint8_t* code;
    uint32_t codelen;
    DebugEntry* dbg;
    uint32_t ndebug;
    bool sandboxed;
} Program;

typedef struct {
    int retpc;
    int locals_base;
    int nlocals;
    int fn;
} Frame;

typedef struct {
    int catchPC;
    int sp;
    int fp;
    int slot;
} Handler;

// ── Global VM State (for stack traces and abort) ────────
static Program* current_program = NULL;
static Frame frames[4096];
static int fp = 0;
static Handler handlers[4096];
static int hp = 0;
static Value stack[65536];
static int sp = 0;
static Value locals_stack[65536];
static int pc = 0;

// ── Fatal Errors and Exception Raising ─────────────────────────
int get_line_number(uint32_t target_pc, Program* program) {
    if (!program->dbg || program->ndebug == 0) return 0;
    int line = 0;
    for (uint32_t i = 0; i < program->ndebug; i++) {
        if (program->dbg[i].pc <= target_pc) {
            line = program->dbg[i].line;
        } else {
            break;
        }
    }
    return line;
}

void print_stack_trace(Program* program) {
    // parity with the Go VM: no debug section -> no stack trace.
    if (!program->dbg || program->ndebug == 0) return;
    fprintf(stderr, "  stack trace (most recent first):\n");
    for (int i = fp - 1; i >= 0; i--) {
        Frame fr = frames[i];
        const char* name = (fr.fn >= 0 && fr.fn < (int)program->nfuncs)
                           ? program->funcs[fr.fn].name : "?";
        int line = get_line_number(i == fp - 1 ? (uint32_t)pc : (uint32_t)fr.retpc, program);
        fprintf(stderr, "    at %s (line %d)\n", name, line);
    }
}

void fatal(const char* msg) {
    fprintf(stderr, "[Pyro VM] %s\n", msg);
    if (current_program) {
        print_stack_trace(current_program);
    }
    exit(1);
}

bool raise_exception(Value v) {
    if (hp == 0) return false;
    Handler h = handlers[--hp];
    while (sp > h.sp) {
        release_value(stack[--sp]);
    }
    while (fp > h.fp) {
        Frame fr = frames[--fp];
        for (int i = 0; i < fr.nlocals; i++) {
            release_value(locals_stack[fr.locals_base + i]);
        }
    }
    if (h.slot != 0xFFFF) {
        int loc_idx = frames[fp - 1].locals_base + h.slot;
        release_value(locals_stack[loc_idx]);
        locals_stack[loc_idx] = v;
    } else {
        release_value(v);
    }
    pc = h.catchPC;
    return true;
}

// ── Bytecode Loader ──────────────────────────────────────────
// ── bounds-checked reading (roadmap 11.13) ─────────────────
// The loader consumes a file that may be malformed or hostile. Every read
// goes through these, so a truncated or lying length fails with a message
// instead of walking off the buffer.
static size_t g_pyro_size = 0;

static void need_bytes(int pos, size_t want, const char* what) {
    if (pos < 0 || (size_t)pos > g_pyro_size || want > g_pyro_size - (size_t)pos) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "malformed .pyro: %s needs %llu byte(s) at offset %d, "
                 "but the file is only %llu bytes",
                 what, (unsigned long long)want, pos,
                 (unsigned long long)g_pyro_size);
        fatal(msg);
    }
}

static uint8_t rd_u8(const uint8_t* data, int* pos, const char* what) {
    need_bytes(*pos, 1, what);
    return data[(*pos)++];
}

static uint16_t rd_u16(const uint8_t* data, int* pos, const char* what) {
    need_bytes(*pos, 2, what);
    return read_u16(data, pos);
}

static uint32_t rd_u32(const uint8_t* data, int* pos, const char* what) {
    need_bytes(*pos, 4, what);
    return read_u32(data, pos);
}

Program* load_program(const uint8_t* data, size_t size) {
    if (size < 6 || memcmp(data, "PYRO", 4) != 0) {
        fatal("invalid .pyro file (magic)");
    }
    // v3 widened the string-constant length u16 -> u32; v2 is still accepted.
    uint8_t ver = data[4];
    if (ver != 2 && ver != 3) {
        fatal("unsupported .pyro version (expected v2 or v3)");
    }
    uint8_t flags = data[5];
    g_pyro_size = size;
    Program* p = malloc(sizeof(Program));
    if (!p) fatal("out of memory loading .pyro");
    int pos = 6;
    
    p->sandboxed = (flags & 0x04) != 0;
    
    uint16_t nconsts = rd_u16(data, &pos, "constant count");
    p->nconsts = nconsts;
    // Each constant costs at least 1 byte (its tag), so a count larger than
    // the bytes remaining is a lie — caught before allocating for it.
    need_bytes(pos, nconsts, "constant table");
    p->consts = malloc(sizeof(Value) * (nconsts ? nconsts : 1));
    if (!p->consts) fatal("out of memory loading .pyro constants");
    for (int i = 0; i < nconsts; i++) {
        uint8_t tag = rd_u8(data, &pos, "constant tag");
        switch (tag) {
            case TAG_INT:
                need_bytes(pos, 8, "int constant");
                p->consts[i] = val_int((int64_t)read_u64(data, &pos));
                break;
            case TAG_FLT:
                {
                    need_bytes(pos, 8, "float constant");
                    uint64_t u = read_u64(data, &pos);
                    double f;
                    memcpy(&f, &u, 8);
                    p->consts[i] = val_float(f);
                }
                break;
            case TAG_STR:
                {
                    // The length is attacker-controlled; check it against the
                    // bytes that remain BEFORE val_str copies from data + pos.
                    uint32_t len = (ver >= 3) ? rd_u32(data, &pos, "string length")
                                              : (uint32_t)rd_u16(data, &pos, "string length");
                    need_bytes(pos, len, "string constant");
                    p->consts[i] = val_str((const char*)(data + pos), (int64_t)len);
                    pos += (int)len;
                }
                break;
            case TAG_BOOL:
                p->consts[i] = val_bool(rd_u8(data, &pos, "bool constant") != 0);
                break;
            default:
                fatal("unknown constant tag");
        }
    }
    
    uint16_t nfuncs = rd_u16(data, &pos, "function count");
    // 9 bytes per entry: nameidx(2) + entry(4) + nparams(1) + nlocals(2)
    need_bytes(pos, (size_t)nfuncs * 9, "function table");
    p->nfuncs = nfuncs;
    p->funcs = malloc(sizeof(FuncInfo) * nfuncs);
    for (int i = 0; i < nfuncs; i++) {
        uint16_t nameidx = rd_u16(data, &pos, "function name index");
        if (nameidx >= nconsts) fatal("malformed .pyro: function name index out of range");
        uint32_t entry = rd_u32(data, &pos, "function entry");
        uint8_t nparams = data[pos++];
        uint16_t nlocals = rd_u16(data, &pos, "function locals");
        
        char* fname = value_to_string(p->consts[nameidx]);
        p->funcs[i] = (FuncInfo){ .name = fname, .entry = entry, .nparams = nparams, .nlocals = nlocals };
    }
    
    p->entryFn = rd_u16(data, &pos, "entry function");
    if (p->entryFn >= nfuncs) fatal("malformed .pyro: entry function index out of range");
    uint32_t codelen = rd_u32(data, &pos, "code length");
    need_bytes(pos, codelen, "code section");
    p->codelen = codelen;
    p->code = malloc(codelen ? codelen : 1);
    if (!p->code) fatal("out of memory loading .pyro code");
    memcpy(p->code, data + pos, codelen);
    pos += (int)codelen;
    
    if (flags & 0x01) {
        xor_decode(p->code, codelen);
    }
    
    p->dbg = NULL;
    p->ndebug = 0;
    if (flags & 0x02) {
        uint32_t ndbg = rd_u32(data, &pos, "debug entry count");
        need_bytes(pos, (size_t)ndbg * 8, "debug section");   // 2 x u32 each
        p->ndebug = ndbg;
        p->dbg = malloc(sizeof(DebugEntry) * (ndbg ? ndbg : 1));
        if (!p->dbg) fatal("out of memory loading .pyro debug section");
        for (uint32_t i = 0; i < ndbg; i++) {
            p->dbg[i].pc = rd_u32(data, &pos, "debug pc");
            p->dbg[i].line = rd_u32(data, &pos, "debug line");
        }
    }
    

    // 11.9 — embedded assets, LAST section. Guarded by a flag bit rather
    // than a version bump, so a .pyro without assets loads exactly as
    // before and an engine that predates the flag never reads this far.
    if (flags & 0x08) {
        uint32_t n = rd_u32(data, &pos, "asset count");
        // 8 bytes minimum per asset (two lengths), so an inflated count is
        // rejected before it drives a single allocation.
        need_bytes(pos, (size_t)n * 8, "asset table");
        for (uint32_t i = 0; i < n; i++) {
            uint32_t nl = rd_u32(data, &pos, "asset name length");
            need_bytes(pos, nl, "asset name");
            char* name = (char*)malloc((size_t)nl + 1);
            if (!name) fatal("out of memory loading .pyro assets");
            memcpy(name, data + pos, nl); name[nl] = 0;
            pos += (int)nl;
            uint32_t dl = rd_u32(data, &pos, "asset data length");
            need_bytes(pos, dl, "asset data");
            char* blob = (char*)malloc((size_t)dl + 1);
            if (!blob) { free(name); fatal("out of memory loading .pyro assets"); }
            memcpy(blob, data + pos, dl); blob[dl] = 0;
            pos += (int)dl;
            pyro_asset_add(name, blob, (int64_t)dl);
        }
    }
    // 11.12 — permissions declared by the program, after the assets.
    if (flags & 0x10) {
        uint32_t n = rd_u32(data, &pos, "permissions length");
        need_bytes(pos, n, "permissions section");
        char* spec = (char*)malloc((size_t)n + 1);
        if (!spec) fatal("out of memory loading .pyro permissions");
        memcpy(spec, data + pos, n); spec[n] = 0;
        pos += (int)n;
        pyro_policy_artifact(spec);
        free(spec);
    }

    // Cross-field invariants. A function whose entry points outside the code
    // section would send the dispatch loop off the end on its first call —
    // the length checks above cannot catch that on their own.
    for (int i = 0; i < p->nfuncs; i++) {
        if (p->funcs[i].entry > p->codelen) {
            fatal("malformed .pyro: function entry point past the end of the code");
        }
    }

    return p;
}

// Operand width per opcode, mirroring _OPERAND in burnout/codegen_pyro.py.
// Used only to reject a truncated final instruction (11.13).
static uint32_t pyro_operand_width(uint8_t op) {
    switch (op) {
        case opCONST: case opLOAD: case opSTORE: case opNEWARR: case opNEWMAP:
        case opPUSHFN: case opGETGLOBAL: case opSETGLOBAL:
            return 2;
        case opJMP: case opJMPF: case opJMPT:
            return 4;
        case opCALL:
            return 3;
        case opNATIVE:
            return 2;
        case opTRYPUSH:
            return 6;
        case opCLOSURE:
            return 3;
        case opCALLVALUE:
            return 1;
        default:
            return 0;
    }
}

// A relative jump, validated. `pc + rel` in int is undefined on overflow and
// a hostile .pyro can supply rel = 0x7FFFFFFF, so the sum is computed in
// int64 and the destination checked against the code section (11.13).
static int pyro_jump_to(int pc, int32_t rel, uint32_t codelen) {
    int64_t dest = (int64_t)pc + (int64_t)rel;
    if (dest < 0 || dest > (int64_t)codelen) {
        fatal("malformed .pyro: jump target outside the code section");
    }
    return (int)dest;
}

// ── Virtual Machine Execution Loop ───────────────────────────
void run_program(Program* p) {
    current_program = p;
    if (p->sandboxed) {
        pyro_sandboxed = true;
    }
    
    FuncInfo main_fn = p->funcs[p->entryFn];
    for (int i = 0; i < main_fn.nlocals; i++) {
        locals_stack[i] = val_null();
    }
    frames[0] = (Frame){ .retpc = -1, .locals_base = 0, .nlocals = main_fn.nlocals, .fn = p->entryFn };
    fp = 1;
    pc = main_fn.entry;
    hp = 0;
    sp = 0;
    
    const uint8_t* code = p->code;
    
    // Roadmap 11.1 — module state, shared by every frame. Grown on demand
    // by opSETGLOBAL, so the .pyro container carries no globals count and
    // v3 files keep loading unchanged. Must mirror main.go exactly.
    Value*  globals   = NULL;
    int     nglobals  = 0;

    while (1) {
        // 11.13 — the code section is untrusted input too. A malformed .pyro
        // can hold a jump past the end or an instruction whose operand runs
        // off the buffer; without this the loop reads whatever follows in
        // memory.
        //
        // 11.22 — ONE unsigned comparison, not two. Converting a negative int
        // to uint32_t is well defined and wraps to something huge, so the
        // `>= codelen` test rejects a negative pc on its own; same for sp.
        if ((uint32_t)pc >= p->codelen) {
            fatal("malformed .pyro: execution ran past the end of the code");
        }
        // The value stack moves on almost every instruction, so it is checked
        // on every instruction. The margin covers the widest single one:
        // NEWMAP touches 2*n slots.
        if ((unsigned)sp > (unsigned)(int)(sizeof(stack) / sizeof(stack[0])) - 4u) {
            fatal("malformed .pyro: value stack overflow");
        }
        // fp and hp used to be checked here too, and that was the waste: the
        // frame stack only moves on CALL/CALLVALUE/RET and the handler stack
        // only on TRYPUSH/TRYPOP, so four comparisons and two loads were being
        // paid on every ADD and LOAD to re-verify something that had not
        // changed. They are now checked at those opcodes instead: the same
        // guarantee, ~3% faster on a guard-bound loop (BENCHMARKS.md), and the
        // same malformed files rejected — test_fuzz.py is the arbiter, with
        // 888 malformed inputs across both VMs.
        uint8_t op = code[pc++];
        // Every instruction below reads at most 6 operand bytes (TRYPUSH).
        // Verifying the widest case once is simpler than threading a length
        // through each read, and still rejects a truncated final instruction.
        if ((uint32_t)pc + 6 > p->codelen) {
            switch (op) {
                // operand-less opcodes are always safe at the tail
                case opHALT: case opRET: case opPOP: case opTRUE: case opFALSE:
                case opNULL: case opPRINT: case opPRINTLN: case opASSERT:
                case opNOT: case opNEG: case opBNOT: case opLEN: case opINDEX:
                case opSETIDX: case opAPPEND: case opHAS: case opKEYS:
                case opTRYPOP: case opTHROW: case opCOALESCE: case opUNWRAP:
                case opADD: case opSUB: case opMUL: case opDIV: case opMOD:
                case opBAND: case opBOR: case opBXOR: case opSHL: case opSHR:
                case opEQ: case opNE: case opLT: case opGT: case opLE: case opGE:
                    break;
                default:
                    if ((uint32_t)pc + pyro_operand_width(op) > p->codelen) {
                        fatal("malformed .pyro: instruction operand runs past "
                              "the end of the code");
                    }
            }
        }
        switch (op) {
            case opHALT:
                return;
            case opCONST:
                {
                    uint16_t idx = read_u16(code, &pc);
                    if (idx >= p->nconsts) fatal("malformed .pyro: constant index out of range");
                    Value v = p->consts[idx];
                    retain_value(v);
                    stack[sp++] = v;
                }
                break;
            case opTRUE:
                stack[sp++] = val_bool(true);
                break;
            case opFALSE:
                stack[sp++] = val_bool(false);
                break;
            case opNULL:
                stack[sp++] = val_null();
                break;
            case opPOP:
                release_value(stack[--sp]);
                break;
            case opLOAD:
                {
                    uint16_t slot = read_u16(code, &pc);
                    Value v = locals_stack[frames[fp - 1].locals_base + slot];
                    retain_value(v);
                    stack[sp++] = v;
                }
                break;
            case opSTORE:
                {
                    uint16_t slot = read_u16(code, &pc);
                    int loc_idx = frames[fp - 1].locals_base + slot;
                    release_value(locals_stack[loc_idx]);
                    locals_stack[loc_idx] = stack[--sp];
                }
                break;
            case opGETGLOBAL:
                {
                    uint16_t slot = read_u16(code, &pc);
                    // Reading before the initialiser ran: the compiler emits the
                    // initialiser first, so this is defensive, not reachable.
                    Value v = (slot < nglobals) ? globals[slot] : val_null();
                    retain_value(v);
                    stack[sp++] = v;
                }
                break;
            case opSETGLOBAL:
                {
                    uint16_t slot = read_u16(code, &pc);
                    if (slot >= nglobals) {
                        int want = slot + 1;
                        Value* grown = (Value*)realloc(globals, (size_t)want * sizeof(Value));
                        if (!grown) fatal("out of memory growing globals");
                        for (int i = nglobals; i < want; i++) grown[i] = val_null();
                        globals  = grown;
                        nglobals = want;
                    }
                    release_value(globals[slot]);
                    globals[slot] = stack[--sp];   // ownership moves off the stack
                }
                break;
            case opADD:
            case opSUB:
            case opMUL:
            case opDIV:
            case opMOD:
            case opBAND:
            case opBOR:
            case opBXOR:
            case opSHL:
            case opSHR:
            case opEQ:
            case opNE:
            case opLT:
            case opGT:
            case opLE:
            case opGE:
                {
                    Value b = stack[--sp];
                    Value a = stack[--sp];
                    Value res = bin_op(op, a, b);
                    stack[sp++] = res;
                    release_value(a);
                    release_value(b);
                }
                break;
            case opNEG:
                {
                    Value a = stack[sp - 1];
                    if (a.kind == VAL_FLOAT) {
                        stack[sp - 1] = val_float(-a.as.f);
                    } else {
                        stack[sp - 1] = val_int(-a.as.i);
                    }
                }
                break;
            case opBNOT:
                {
                    Value a = stack[sp - 1];
                    stack[sp - 1] = val_int(~a.as.i);
                }
                break;
            case opNOT:
                {
                    Value a = stack[sp - 1];
                    bool t = value_truthy(a);
                    release_value(a);
                    stack[sp - 1] = val_bool(!t);
                }
                break;
            case opJMP:
                {
                    int32_t rel = read_i32(code, &pc);
                    pc = pyro_jump_to(pc, rel, p->codelen);
                }
                break;
            case opJMPF:
                {
                    int32_t rel = read_i32(code, &pc);
                    Value a = stack[--sp];
                    bool t = value_truthy(a);
                    release_value(a);
                    if (!t) pc = pyro_jump_to(pc, rel, p->codelen);
                }
                break;
            case opJMPT:
                {
                    int32_t rel = read_i32(code, &pc);
                    Value a = stack[--sp];
                    bool t = value_truthy(a);
                    release_value(a);
                    if (t) pc = pyro_jump_to(pc, rel, p->codelen);
                }
                break;
            case opCALL:
                {
                    uint16_t fi = read_u16(code, &pc);
                    uint8_t argc = code[pc++];
                    if (fi >= p->nfuncs) fatal("malformed .pyro: function index out of range");
                    FuncInfo fn = p->funcs[fi];
                    int next_base = frames[fp - 1].locals_base + frames[fp - 1].nlocals;
                    // 14.1 — bound the LOCALS stack. Nothing did.
                    // Every local of the callee is written straight into
                    // locals_stack just below, and with the frame stack capped
                    // at 4095 a function with 31 locals recursing ~2114 deep
                    // puts next_base past 65536 — this writes off the end of a
                    // static array. A valid program, silent memory corruption,
                    // no diagnostic. Found by the 14.1 audit after the frame
                    // guard landed, since capping frames is what makes the
                    // per-frame locals the binding constraint.
                    //
                    // Checked HERE, before the writes, and therefore before the
                    // frame guard further down: whichever limit a deep
                    // recursion reaches first is the one that reports, and the
                    // Go VM checks in the same order for the same reason.
                    if ((unsigned)(next_base + fn.nlocals) >
                        (unsigned)(int)(sizeof(locals_stack) / sizeof(locals_stack[0])) - 2u) {
                        fatal("malformed .pyro: locals stack overflow (runaway recursion?)");
                    }
                    for (int i = 0; i < fn.nlocals; i++) {
                        locals_stack[next_base + i] = val_null();
                    }
                    int base = sp - argc;
                    for (int i = 0; i < argc; i++) {
                        locals_stack[next_base + i] = stack[base + i];
                    }
                    sp = base;
                    // 11.22 — bounded HERE, not on every instruction: the
                    // frame stack only moves on a call and a return.
                    if ((unsigned)fp >
                        (unsigned)(int)(sizeof(frames) / sizeof(frames[0])) - 2u) {
                        fatal("malformed .pyro: call stack overflow (runaway recursion?)");
                    }
                    frames[fp++] = (Frame){ .retpc = pc, .locals_base = next_base, .nlocals = fn.nlocals, .fn = fi };
                    pc = fn.entry;
                }
                break;
            case opPUSHFN:
                {
                    uint16_t fi = read_u16(code, &pc);
                    stack[sp++] = val_func((int32_t)fi, NULL);
                }
                break;
            case opCLOSURE:
                {
                    uint16_t fi = read_u16(code, &pc);
                    uint8_t ncap = code[pc++];
                    RcArray* cap = rc_array_new();
                    for (int i = 0; i < ncap; i++) {
                        rc_array_push(cap, stack[sp - ncap + i]);
                    }
                    for (int i = 0; i < ncap; i++) {
                        release_value(stack[sp - ncap + i]);
                    }
                    sp -= ncap;
                    stack[sp++] = val_func((int32_t)fi, cap);
                }
                break;
            case opCALLVALUE:
                {
                    uint8_t argc = code[pc++];
                    int base = sp - argc;
                    Value fnval = stack[base - 1];
                    if (fnval.kind != VAL_FUNC) {
                        fatal("call of a non-function value");
                    }
                    int fi = (int)fnval.fnidx;
                    if (fi >= p->nfuncs) fatal("malformed .pyro: function index out of range");
                    FuncInfo fn = p->funcs[fi];
                    int next_base = frames[fp - 1].locals_base + frames[fp - 1].nlocals;
                    // 14.1 — bound the LOCALS stack. Nothing did.
                    // Every local of the callee is written straight into
                    // locals_stack just below, and with the frame stack capped
                    // at 4095 a function with 31 locals recursing ~2114 deep
                    // puts next_base past 65536 — this writes off the end of a
                    // static array. A valid program, silent memory corruption,
                    // no diagnostic. Found by the 14.1 audit after the frame
                    // guard landed, since capping frames is what makes the
                    // per-frame locals the binding constraint.
                    //
                    // Checked HERE, before the writes, and therefore before the
                    // frame guard further down: whichever limit a deep
                    // recursion reaches first is the one that reports, and the
                    // Go VM checks in the same order for the same reason.
                    if ((unsigned)(next_base + fn.nlocals) >
                        (unsigned)(int)(sizeof(locals_stack) / sizeof(locals_stack[0])) - 2u) {
                        fatal("malformed .pyro: locals stack overflow (runaway recursion?)");
                    }
                    for (int i = 0; i < fn.nlocals; i++) {
                        locals_stack[next_base + i] = val_null();
                    }
                    // captured values fill the leading locals, then the arguments
                    int ncap = fnval.as.arr ? (int)fnval.as.arr->length : 0;
                    for (int i = 0; i < ncap; i++) {
                        Value c = fnval.as.arr->data[i];
                        retain_value(c);
                        locals_stack[next_base + i] = c;
                    }
                    for (int i = 0; i < argc; i++) {
                        locals_stack[next_base + ncap + i] = stack[base + i];
                    }
                    release_value(fnval);   // the stack slot's reference is gone
                    sp = base - 1;          // drop the args and the fn value beneath
                    // 11.22 — bounded HERE, not on every instruction: the
                    // frame stack only moves on a call and a return.
                    if ((unsigned)fp >
                        (unsigned)(int)(sizeof(frames) / sizeof(frames[0])) - 2u) {
                        fatal("malformed .pyro: call stack overflow (runaway recursion?)");
                    }
                    frames[fp++] = (Frame){ .retpc = pc, .locals_base = next_base, .nlocals = fn.nlocals, .fn = fi };
                    pc = fn.entry;
                }
                break;
            case opRET:
                {
                    Value ret = stack[--sp];
                    // The pop needs a LOWER bound, and needs it BEFORE the
                    // read. The old per-instruction check could not give
                    // that: `frames[--fp]` with fp == 0 read frames[-1] and
                    // only failed on the next iteration, after the fact.
                    if (fp <= 0) {
                        fatal("malformed .pyro: return with no frame to return to");
                    }
                    Frame fr = frames[--fp];
                    for (int i = 0; i < fr.nlocals; i++) {
                        release_value(locals_stack[fr.locals_base + i]);
                    }
                    if (fr.retpc < 0) {
                        release_value(ret);
                        return;
                    }
                    pc = fr.retpc;
                    stack[sp++] = ret;
                }
                break;
            case opPRINT:
                {
                    Value v = stack[--sp];
                    char* s = value_to_string(v);
                    printf("%s\n", s);
                    free(s);
                    release_value(v);
                }
                break;
            case opASSERT:
                {
                    Value cond = stack[--sp];
                    Value msg = stack[--sp];
                    if (!value_truthy(cond)) {
                        char* mstr = value_to_string(msg);
                        char err[1024];
                        sprintf(err, "[Cryo Assert] %s", mstr);
                        free(mstr);
                        if (!raise_exception(val_str(err, strlen(err)))) {
                            fatal(err);
                        }
                    }
                    release_value(cond);
                    release_value(msg);
                }
                break;
            case opPRINTLN:
                printf("\n");
                break;
            case opNEWARR:
                {
                    uint16_t n = read_u16(code, &pc);
                    // 11.13 — a count larger than the stack depth makes `base`
                    // negative and indexes BELOW the stack array.
                    if ((int)n > sp) fatal("malformed .pyro: array literal larger than the stack");
                    RcArray* arr = rc_array_new();
                    int base = sp - n;
                    for (int i = 0; i < n; i++) {
                        rc_array_push(arr, stack[base + i]);
                        release_value(stack[base + i]);
                    }
                    sp = base;
                    stack[sp++] = val_array(arr);
                }
                break;
            case opNEWMAP:
                {
                    uint16_t n = read_u16(code, &pc);
                    // 11.13 — same trap as opNEWARR, doubled: each pair is two
                    // stack slots, so 2*n must fit in the current depth.
                    if (2 * (int)n > sp) fatal("malformed .pyro: map literal larger than the stack");
                    RcMap* mp = rc_map_new();
                    int base = sp - 2 * n;
                    for (int i = 0; i < n; i++) {
                        Value k = stack[base + 2 * i];
                        Value v = stack[base + 2 * i + 1];
                        rc_map_set(mp, k, v);
                        release_value(k);
                        release_value(v);
                    }
                    sp = base;
                    stack[sp++] = val_map(mp);
                }
                break;
            case opINDEX:
                {
                    Value key = stack[--sp];
                    Value cont = stack[--sp];
                    stack[sp++] = index_get(cont, key);
                    release_value(key);
                    release_value(cont);
                }
                break;
            case opSETIDX:
                {
                    Value val = stack[--sp];
                    Value key = stack[--sp];
                    Value cont = stack[--sp];
                    index_set(cont, key, val);
                    release_value(val);
                    release_value(key);
                    release_value(cont);
                }
                break;
            case opLEN:
                {
                    Value v = stack[sp - 1];
                    int64_t len = value_length(v);
                    release_value(v);
                    stack[sp - 1] = val_int(len);
                }
                break;
            case opAPPEND:
                {
                    // contract: pop val, pop arr -> push new size (net -1).
                    // Peeking arr here would leave a stranded slot, and paths
                    // that merge after a conditional push would then disagree
                    // on the stack depth.
                    Value val = stack[--sp];
                    Value arr = stack[--sp];
                    if (arr.kind != VAL_ARRAY) {
                        fatal("push on a non-array value");
                    }
                    rc_array_push(arr.as.arr, val);
                    release_value(val);
                    int64_t new_len = arr.as.arr->length;
                    release_value(arr);
                    stack[sp++] = val_int(new_len);
                }
                break;
            case opHAS:
                {
                    Value key = stack[--sp];
                    Value mp = stack[sp - 1];
                    bool has_key = false;
                    if (mp.kind == VAL_MAP) {
                        has_key = rc_map_has(mp.as.map, key);
                    }
                    release_value(key);
                    release_value(mp);
                    stack[sp - 1] = val_bool(has_key);
                }
                break;
            case opKEYS:
                {
                    Value mp = stack[sp - 1];
                    if (mp.kind != VAL_MAP) {
                        fatal("keys() applied to a non-map value");
                    }
                    RcArray* keys_arr = rc_map_keys_sorted(mp.as.map);
                    release_value(mp);
                    stack[sp - 1] = val_array(keys_arr);
                }
                break;
            case opNATIVE:
                {
                    uint8_t nid = code[pc++];
                    uint8_t argc = code[pc++];
                    int base = sp - argc;
                    Value res = native(nid, stack + base, argc);
                    for (int i = 0; i < argc; i++) {
                        release_value(stack[base + i]);
                    }
                    sp = base;
                    stack[sp++] = res;
                }
                break;
            case opTRYPUSH:
                {
                    int32_t rel = read_i32(code, &pc);
                    uint16_t slot = read_u16(code, &pc);
                    // the catch target is attacker-controlled like any jump
                    int catch_pc = pyro_jump_to(pc, rel, p->codelen);
                    // 11.22 — likewise: the handler stack only moves here
                    // and at TRYPOP.
                    if ((unsigned)hp >
                        (unsigned)(int)(sizeof(handlers) / sizeof(handlers[0])) - 2u) {
                        fatal("malformed .pyro: exception handler stack overflow");
                    }
                    handlers[hp++] = (Handler){ .catchPC = catch_pc, .sp = sp, .fp = fp, .slot = slot };
                }
                break;
            case opTRYPOP:
                if (hp > 0) hp--;
                break;
            case opTHROW:
                {
                    Value v = stack[--sp];
                    if (!raise_exception(v)) {
                        char* s = value_to_string(v);
                        char err[1024];
                        sprintf(err, "uncaught exception: %s", s);
                        free(s);
                        fatal(err);
                    }
                }
                break;
            case opCOALESCE:
                {
                    Value b = stack[--sp];
                    Value a = stack[--sp];
                    if (a.kind == VAL_NULL) {
                        stack[sp++] = b;
                        release_value(a);
                    } else {
                        stack[sp++] = a;
                        release_value(b);
                    }
                }
                break;
            case opUNWRAP:
                {
                    Value a = stack[sp - 1];
                    if (a.kind == VAL_NULL) {
                        // Pop BEFORE raising. raise_exception unwinds the stack
                        // to the handler's recorded sp, so decrementing
                        // afterwards took it one slot too far — with a handler
                        // at depth 0 that left sp NEGATIVE, and the next
                        // instruction read stack[-1]. The Go VM pops first;
                        // this now matches. (Found by the 11.13 stack guard,
                        // which fired on a valid program.)
                        sp--;
                        const char* um = "[Cryo Security] unwrap of null value";
                        Value err_msg = val_str(um, (int64_t)strlen(um));
                        if (!raise_exception(err_msg)) {
                            fatal(um);
                        }
                    }
                }
                break;
            case opSPAWN:
            case opAWAIT:
                // 12.5 — the scheduler is implemented in the Go VM. Refusing by
                // name rather than falling through to "invalid opcode": the
                // bytecode is perfectly valid, this VM just cannot run it, and
                // an opcode number tells the reader nothing about which.
                fatal("[Cryo Concurrency] spawn/await need the Go VM — "
                      "this C VM has no scheduler (roadmap 12.5)");
                break;
            default:
                fatal("invalid opcode in bytecode");
        }
    }
}

// ── Main Entry Point ─────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "[Pyro VM] usage: pyrovm program.pyro\n");
        return 1;
    }

    // program args seen by args(): everything after the .pyro path
    pyro_argc = argc - 2;
    pyro_argv = argv + 2;

    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "[Pyro VM] could not read: %s\n", argv[1]);
        return 1;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    uint8_t* data = malloc(size);
    if (fread(data, 1, size, f) != size) {
        fprintf(stderr, "Fatal error: error reading file\n");
        fclose(f);
        free(data);
        return 1;
    }
    fclose(f);
    
    // 11.11 — a policy implies the sandbox: deny by default, grant what
    // is listed. Parsed before PYRO_SANDBOX so a flat deny still wins.
    pyro_policy_init(getenv("PYRO_POLICY"));
    const char* env_sandbox = getenv("PYRO_SANDBOX");
    if (env_sandbox && strcmp(env_sandbox, "1") == 0) {
        pyro_sandboxed = true;
    }
    
    Program* p = load_program(data, size);
    free(data);
    
    run_program(p);
    
    for (int i = 0; i < p->nconsts; i++) {
        release_value(p->consts[i]);
    }
    free(p->consts);
    
    for (int i = 0; i < p->nfuncs; i++) {
        free(p->funcs[i].name);
    }
    free(p->funcs);
    free(p->code);
    if (p->dbg) free(p->dbg);
    free(p);
    
    return 0;
}
