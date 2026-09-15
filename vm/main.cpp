// ============================================================
//  Pyro C++ VM — Execution Engine for .pyro Bytecode
//
//  C++ implementation of the Pyro VM stack machine.
//  Loads and executes .pyro bytecode containers with parity
//  to the Go and C VM engines.
// ============================================================
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <memory>
#include <cstring>
#include <cstdint>
#include <cstdlib>

extern "C" {
#include "pyro_runtime.h"
}

namespace pyro {

struct FuncInfo {
    std::string name;
    uint32_t entry;
    uint8_t nparams;
    uint16_t nlocals;
};

struct DebugEntry {
    uint32_t pc;
    uint32_t line;
};

struct Frame {
    int retpc;
    int locals_base;
    int nlocals;
    int fn;
};

struct Handler {
    int catchPC;
    int sp;
    int fp;
    int slot;
};

struct Program {
    std::vector<Value> consts;
    std::vector<FuncInfo> funcs;
    uint16_t entryFn = 0;
    std::vector<uint8_t> code;
    std::vector<DebugEntry> dbg;
    bool sandboxed = false;

    ~Program() {
        for (auto& v : consts) {
            release_value(v);
        }
    }
};

static Program* g_current_program = nullptr;
static Frame g_frames[4096];
static int g_fp = 0;
static Handler g_handlers[4096];
static int g_hp = 0;
static Value g_stack[65536];
static int g_sp = 0;
static Value g_locals_stack[65536];
static int g_pc = 0;
static size_t g_pyro_size = 0;

static int get_line_number(uint32_t target_pc, const Program* program) {
    if (!program || program->dbg.empty()) return 0;
    int line = 0;
    for (const auto& entry : program->dbg) {
        if (entry.pc <= target_pc) {
            line = (int)entry.line;
        } else {
            break;
        }
    }
    return line;
}

static void print_stack_trace(const Program* program) {
    if (!program || program->dbg.empty()) return;
    std::cerr << "  stack trace (most recent first):\n";
    for (int i = g_fp - 1; i >= 0; i--) {
        Frame fr = g_frames[i];
        const char* name = (fr.fn >= 0 && fr.fn < (int)program->funcs.size())
                           ? program->funcs[fr.fn].name.c_str() : "?";
        int line = get_line_number(i == g_fp - 1 ? (uint32_t)g_pc : (uint32_t)fr.retpc, program);
        std::cerr << "    at " << name << " (line " << line << ")\n";
    }
}

} // namespace pyro

extern "C" void fatal(const char* msg) {
    std::cerr << "[Pyro VM] " << msg << "\n";
    if (pyro::g_current_program) {
        pyro::print_stack_trace(pyro::g_current_program);
    }
    std::exit(1);
}

namespace pyro {

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

static bool raise_exception(Value v) {
    if (g_hp == 0) return false;
    Handler h = g_handlers[--g_hp];
    while (g_sp > h.sp) {
        release_value(g_stack[--g_sp]);
    }
    while (g_fp > h.fp) {
        Frame fr = g_frames[--g_fp];
        for (int i = 0; i < fr.nlocals; i++) {
            release_value(g_locals_stack[fr.locals_base + i]);
        }
    }
    if (h.slot != 0xFFFF) {
        int loc_idx = g_frames[g_fp - 1].locals_base + h.slot;
        release_value(g_locals_stack[loc_idx]);
        g_locals_stack[loc_idx] = v;
    } else {
        release_value(v);
    }
    g_pc = h.catchPC;
    return true;
}

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

static int pyro_jump_to(int pc, int32_t rel, uint32_t codelen) {
    int64_t dest = (int64_t)pc + (int64_t)rel;
    if (dest < 0 || dest > (int64_t)codelen) {
        fatal("malformed .pyro: jump target outside the code section");
    }
    return (int)dest;
}

std::unique_ptr<Program> load_program(const uint8_t* data, size_t size) {
    if (size < 6 || std::memcmp(data, "PYRO", 4) != 0) {
        fatal("invalid .pyro file (magic)");
    }
    uint8_t ver = data[4];
    if (ver != 2 && ver != 3) {
        fatal("unsupported .pyro version (expected v2 or v3)");
    }
    uint8_t flags = data[5];
    g_pyro_size = size;
    auto p = std::unique_ptr<Program>(new Program());
    int pos = 6;

    p->sandboxed = (flags & 0x04) != 0;

    uint16_t nconsts = rd_u16(data, &pos, "constant count");
    need_bytes(pos, nconsts, "constant table");
    p->consts.resize(nconsts);
    for (int i = 0; i < nconsts; i++) {
        uint8_t tag = rd_u8(data, &pos, "constant tag");
        switch (tag) {
            case TAG_INT:
                need_bytes(pos, 8, "int constant");
                p->consts[i] = val_int((int64_t)read_u64(data, &pos));
                break;
            case TAG_FLT: {
                need_bytes(pos, 8, "float constant");
                uint64_t u = read_u64(data, &pos);
                double f;
                std::memcpy(&f, &u, 8);
                p->consts[i] = val_float(f);
                break;
            }
            case TAG_STR: {
                uint32_t len = (ver >= 3) ? rd_u32(data, &pos, "string length")
                                          : (uint32_t)rd_u16(data, &pos, "string length");
                need_bytes(pos, len, "string constant");
                p->consts[i] = val_str((const char*)(data + pos), (int64_t)len);
                pos += (int)len;
                break;
            }
            case TAG_BOOL:
                p->consts[i] = val_bool(rd_u8(data, &pos, "bool constant") != 0);
                break;
            default:
                fatal("unknown constant tag");
        }
    }

    uint16_t nfuncs = rd_u16(data, &pos, "function count");
    need_bytes(pos, (size_t)nfuncs * 9, "function table");
    p->funcs.resize(nfuncs);
    for (int i = 0; i < nfuncs; i++) {
        uint16_t nameidx = rd_u16(data, &pos, "function name index");
        if (nameidx >= nconsts) fatal("malformed .pyro: function name index out of range");
        uint32_t entry = rd_u32(data, &pos, "function entry");
        uint8_t nparams = data[pos++];
        uint16_t nlocals = rd_u16(data, &pos, "function locals");

        char* fname = value_to_string(p->consts[nameidx]);
        p->funcs[i] = { std::string(fname), entry, nparams, nlocals };
        std::free(fname);
    }

    p->entryFn = rd_u16(data, &pos, "entry function");
    if (p->entryFn >= nfuncs) fatal("malformed .pyro: entry function index out of range");
    uint32_t codelen = rd_u32(data, &pos, "code length");
    need_bytes(pos, codelen, "code section");
    p->code.resize(codelen);
    if (codelen > 0) {
        std::memcpy(p->code.data(), data + pos, codelen);
    }
    pos += (int)codelen;

    if (flags & 0x01) {
        xor_decode(p->code.data(), codelen);
    }

    if (flags & 0x02) {
        uint32_t ndbg = rd_u32(data, &pos, "debug entry count");
        need_bytes(pos, (size_t)ndbg * 8, "debug section");
        p->dbg.resize(ndbg);
        for (uint32_t i = 0; i < ndbg; i++) {
            p->dbg[i].pc = rd_u32(data, &pos, "debug pc");
            p->dbg[i].line = rd_u32(data, &pos, "debug line");
        }
    }

    if (flags & 0x08) {
        uint32_t n = rd_u32(data, &pos, "asset count");
        need_bytes(pos, (size_t)n * 8, "asset table");
        for (uint32_t i = 0; i < n; i++) {
            uint32_t nl = rd_u32(data, &pos, "asset name length");
            need_bytes(pos, nl, "asset name");
            char* name = (char*)std::malloc((size_t)nl + 1);
            if (!name) fatal("out of memory loading .pyro assets");
            std::memcpy(name, data + pos, nl); name[nl] = 0;
            pos += (int)nl;
            uint32_t dl = rd_u32(data, &pos, "asset data length");
            need_bytes(pos, dl, "asset data");
            char* blob = (char*)std::malloc((size_t)dl + 1);
            if (!blob) { std::free(name); fatal("out of memory loading .pyro assets"); }
            std::memcpy(blob, data + pos, dl); blob[dl] = 0;
            pos += (int)dl;
            pyro_asset_add(name, blob, (int64_t)dl);
        }
    }

    if (flags & 0x10) {
        uint32_t n = rd_u32(data, &pos, "permissions length");
        need_bytes(pos, n, "permissions section");
        char* spec = (char*)std::malloc((size_t)n + 1);
        if (!spec) fatal("out of memory loading .pyro permissions");
        std::memcpy(spec, data + pos, n); spec[n] = 0;
        pos += (int)n;
        pyro_policy_artifact(spec);
        std::free(spec);
    }

    for (const auto& fn : p->funcs) {
        if (fn.entry > p->code.size()) {
            fatal("malformed .pyro: function entry point past the end of the code");
        }
    }

    return p;
}

void run_program(Program* p) {
    g_current_program = p;
    if (p->sandboxed) {
        pyro_sandboxed = true;
    }

    const FuncInfo& main_fn = p->funcs[p->entryFn];
    for (int i = 0; i < main_fn.nlocals; i++) {
        g_locals_stack[i] = val_null();
    }
    g_frames[0] = { -1, 0, (int)main_fn.nlocals, (int)p->entryFn };
    g_fp = 1;
    g_pc = (int)main_fn.entry;
    g_hp = 0;
    g_sp = 0;

    const uint8_t* code = p->code.data();
    uint32_t codelen = (uint32_t)p->code.size();

    std::vector<Value> globals;

    while (1) {
        if ((uint32_t)g_pc >= codelen) {
            fatal("malformed .pyro: execution ran past the end of the code");
        }
        if ((unsigned)g_sp > (unsigned)(int)(sizeof(g_stack) / sizeof(g_stack[0])) - 4u) {
            fatal("malformed .pyro: value stack overflow");
        }
        uint8_t op = code[g_pc++];
        if ((uint32_t)g_pc + 6 > codelen) {
            switch (op) {
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
                    if ((uint32_t)g_pc + pyro_operand_width(op) > codelen) {
                        fatal("malformed .pyro: instruction operand runs past the end of the code");
                    }
            }
        }
        switch (op) {
            case opHALT:
                for (auto& g : globals) release_value(g);
                return;
            case opCONST: {
                uint16_t idx = read_u16(code, &g_pc);
                if (idx >= p->consts.size()) fatal("malformed .pyro: constant index out of range");
                Value v = p->consts[idx];
                retain_value(v);
                g_stack[g_sp++] = v;
                break;
            }
            case opTRUE:
                g_stack[g_sp++] = val_bool(true);
                break;
            case opFALSE:
                g_stack[g_sp++] = val_bool(false);
                break;
            case opNULL:
                g_stack[g_sp++] = val_null();
                break;
            case opPOP:
                release_value(g_stack[--g_sp]);
                break;
            case opLOAD: {
                uint16_t slot = read_u16(code, &g_pc);
                Value v = g_locals_stack[g_frames[g_fp - 1].locals_base + slot];
                retain_value(v);
                g_stack[g_sp++] = v;
                break;
            }
            case opSTORE: {
                uint16_t slot = read_u16(code, &g_pc);
                int loc_idx = g_frames[g_fp - 1].locals_base + slot;
                release_value(g_locals_stack[loc_idx]);
                g_locals_stack[loc_idx] = g_stack[--g_sp];
                break;
            }
            case opGETGLOBAL: {
                uint16_t slot = read_u16(code, &g_pc);
                Value v = (slot < globals.size()) ? globals[slot] : val_null();
                retain_value(v);
                g_stack[g_sp++] = v;
                break;
            }
            case opSETGLOBAL: {
                uint16_t slot = read_u16(code, &g_pc);
                if (slot >= globals.size()) {
                    globals.resize(slot + 1, val_null());
                }
                release_value(globals[slot]);
                globals[slot] = g_stack[--g_sp];
                break;
            }
            case opADD: case opSUB: case opMUL: case opDIV: case opMOD:
            case opBAND: case opBOR: case opBXOR: case opSHL: case opSHR:
            case opEQ: case opNE: case opLT: case opGT: case opLE: case opGE: {
                Value b = g_stack[--g_sp];
                Value a = g_stack[--g_sp];
                Value res = bin_op(op, a, b);
                g_stack[g_sp++] = res;
                release_value(a);
                release_value(b);
                break;
            }
            case opNEG: {
                Value a = g_stack[g_sp - 1];
                if (a.kind == VAL_FLOAT) {
                    g_stack[g_sp - 1] = val_float(-a.as.f);
                } else {
                    g_stack[g_sp - 1] = val_int(-a.as.i);
                }
                break;
            }
            case opBNOT: {
                Value a = g_stack[g_sp - 1];
                g_stack[g_sp - 1] = val_int(~a.as.i);
                break;
            }
            case opNOT: {
                Value a = g_stack[g_sp - 1];
                bool t = value_truthy(a);
                release_value(a);
                g_stack[g_sp - 1] = val_bool(!t);
                break;
            }
            case opJMP: {
                int32_t rel = read_i32(code, &g_pc);
                g_pc = pyro_jump_to(g_pc, rel, codelen);
                break;
            }
            case opJMPF: {
                int32_t rel = read_i32(code, &g_pc);
                Value a = g_stack[--g_sp];
                bool t = value_truthy(a);
                release_value(a);
                if (!t) g_pc = pyro_jump_to(g_pc, rel, codelen);
                break;
            }
            case opJMPT: {
                int32_t rel = read_i32(code, &g_pc);
                Value a = g_stack[--g_sp];
                bool t = value_truthy(a);
                release_value(a);
                if (t) g_pc = pyro_jump_to(g_pc, rel, codelen);
                break;
            }
            case opCALL: {
                uint16_t fi = read_u16(code, &g_pc);
                uint8_t argc = code[g_pc++];
                if (fi >= p->funcs.size()) fatal("malformed .pyro: function index out of range");
                const FuncInfo& fn = p->funcs[fi];
                int next_base = g_frames[g_fp - 1].locals_base + g_frames[g_fp - 1].nlocals;
                if ((unsigned)(next_base + fn.nlocals) >
                    (unsigned)(int)(sizeof(g_locals_stack) / sizeof(g_locals_stack[0])) - 2u) {
                    fatal("malformed .pyro: locals stack overflow (runaway recursion?)");
                }
                for (int i = 0; i < fn.nlocals; i++) {
                    g_locals_stack[next_base + i] = val_null();
                }
                int base = g_sp - argc;
                for (int i = 0; i < argc; i++) {
                    g_locals_stack[next_base + i] = g_stack[base + i];
                }
                g_sp = base;
                if ((unsigned)g_fp > (unsigned)(int)(sizeof(g_frames) / sizeof(g_frames[0])) - 2u) {
                    fatal("malformed .pyro: call stack overflow (runaway recursion?)");
                }
                g_frames[g_fp++] = { g_pc, next_base, (int)fn.nlocals, (int)fi };
                g_pc = (int)fn.entry;
                break;
            }
            case opPUSHFN: {
                uint16_t fi = read_u16(code, &g_pc);
                g_stack[g_sp++] = val_func((int32_t)fi, NULL);
                break;
            }
            case opCLOSURE: {
                uint16_t fi = read_u16(code, &g_pc);
                uint8_t ncap = code[g_pc++];
                RcArray* cap = rc_array_new();
                for (int i = 0; i < ncap; i++) {
                    rc_array_push(cap, g_stack[g_sp - ncap + i]);
                }
                for (int i = 0; i < ncap; i++) {
                    release_value(g_stack[g_sp - ncap + i]);
                }
                g_sp -= ncap;
                g_stack[g_sp++] = val_func((int32_t)fi, cap);
                break;
            }
            case opCALLVALUE: {
                uint8_t argc = code[g_pc++];
                int base = g_sp - argc;
                Value fnval = g_stack[base - 1];
                if (fnval.kind != VAL_FUNC) {
                    fatal("call of a non-function value");
                }
                int fi = (int)fnval.fnidx;
                if (fi >= (int)p->funcs.size()) fatal("malformed .pyro: function index out of range");
                const FuncInfo& fn = p->funcs[fi];
                int next_base = g_frames[g_fp - 1].locals_base + g_frames[g_fp - 1].nlocals;
                if ((unsigned)(next_base + fn.nlocals) >
                    (unsigned)(int)(sizeof(g_locals_stack) / sizeof(g_locals_stack[0])) - 2u) {
                    fatal("malformed .pyro: locals stack overflow (runaway recursion?)");
                }
                for (int i = 0; i < fn.nlocals; i++) {
                    g_locals_stack[next_base + i] = val_null();
                }
                int ncap = fnval.as.arr ? (int)fnval.as.arr->length : 0;
                for (int i = 0; i < ncap; i++) {
                    Value c = fnval.as.arr->data[i];
                    retain_value(c);
                    g_locals_stack[next_base + i] = c;
                }
                for (int i = 0; i < argc; i++) {
                    g_locals_stack[next_base + ncap + i] = g_stack[base + i];
                }
                release_value(fnval);
                g_sp = base - 1;
                if ((unsigned)g_fp > (unsigned)(int)(sizeof(g_frames) / sizeof(g_frames[0])) - 2u) {
                    fatal("malformed .pyro: call stack overflow (runaway recursion?)");
                }
                g_frames[g_fp++] = { g_pc, next_base, (int)fn.nlocals, fi };
                g_pc = (int)fn.entry;
                break;
            }
            case opRET: {
                Value ret = g_stack[--g_sp];
                if (g_fp <= 0) {
                    fatal("malformed .pyro: return with no frame to return to");
                }
                Frame fr = g_frames[--g_fp];
                for (int i = 0; i < fr.nlocals; i++) {
                    release_value(g_locals_stack[fr.locals_base + i]);
                }
                if (fr.retpc < 0) {
                    release_value(ret);
                    for (auto& g : globals) release_value(g);
                    return;
                }
                g_pc = fr.retpc;
                g_stack[g_sp++] = ret;
                break;
            }
            case opPRINT: {
                Value v = g_stack[--g_sp];
                char* s = value_to_string(v);
                std::cout << s << "\n";
                std::free(s);
                release_value(v);
                break;
            }
            case opASSERT: {
                Value cond = g_stack[--g_sp];
                Value msg = g_stack[--g_sp];
                if (!value_truthy(cond)) {
                    char* mstr = value_to_string(msg);
                    char err[1024];
                    std::snprintf(err, sizeof(err), "[Cryo Assert] %s", mstr);
                    std::free(mstr);
                    if (!raise_exception(val_str(err, (int64_t)std::strlen(err)))) {
                        fatal(err);
                    }
                }
                release_value(cond);
                release_value(msg);
                break;
            }
            case opPRINTLN:
                std::cout << "\n";
                break;
            case opNEWARR: {
                uint16_t n = read_u16(code, &g_pc);
                if ((int)n > g_sp) fatal("malformed .pyro: array literal larger than the stack");
                RcArray* arr = rc_array_new();
                int base = g_sp - n;
                for (int i = 0; i < n; i++) {
                    rc_array_push(arr, g_stack[base + i]);
                    release_value(g_stack[base + i]);
                }
                g_sp = base;
                g_stack[g_sp++] = val_array(arr);
                break;
            }
            case opNEWMAP: {
                uint16_t n = read_u16(code, &g_pc);
                if (2 * (int)n > g_sp) fatal("malformed .pyro: map literal larger than the stack");
                RcMap* mp = rc_map_new();
                int base = g_sp - 2 * n;
                for (int i = 0; i < n; i++) {
                    Value k = g_stack[base + 2 * i];
                    Value v = g_stack[base + 2 * i + 1];
                    rc_map_set(mp, k, v);
                    release_value(k);
                    release_value(v);
                }
                g_sp = base;
                g_stack[g_sp++] = val_map(mp);
                break;
            }
            case opINDEX: {
                Value key = g_stack[--g_sp];
                Value cont = g_stack[--g_sp];
                g_stack[g_sp++] = index_get(cont, key);
                release_value(key);
                release_value(cont);
                break;
            }
            case opSETIDX: {
                Value val = g_stack[--g_sp];
                Value key = g_stack[--g_sp];
                Value cont = g_stack[--g_sp];
                index_set(cont, key, val);
                release_value(val);
                release_value(key);
                release_value(cont);
                break;
            }
            case opLEN: {
                Value v = g_stack[g_sp - 1];
                int64_t len = value_length(v);
                release_value(v);
                g_stack[g_sp - 1] = val_int(len);
                break;
            }
            case opAPPEND: {
                Value val = g_stack[--g_sp];
                Value arr = g_stack[--g_sp];
                if (arr.kind != VAL_ARRAY) {
                    fatal("push on a non-array value");
                }
                rc_array_push(arr.as.arr, val);
                release_value(val);
                int64_t new_len = arr.as.arr->length;
                release_value(arr);
                g_stack[g_sp++] = val_int(new_len);
                break;
            }
            case opHAS: {
                Value key = g_stack[--g_sp];
                Value mp = g_stack[g_sp - 1];
                bool has_key = false;
                if (mp.kind == VAL_MAP) {
                    has_key = rc_map_has(mp.as.map, key);
                }
                release_value(key);
                release_value(mp);
                g_stack[g_sp - 1] = val_bool(has_key);
                break;
            }
            case opKEYS: {
                Value mp = g_stack[g_sp - 1];
                if (mp.kind != VAL_MAP) {
                    fatal("keys() applied to a non-map value");
                }
                RcArray* keys_arr = rc_map_keys_sorted(mp.as.map);
                release_value(mp);
                g_stack[g_sp - 1] = val_array(keys_arr);
                break;
            }
            case opNATIVE: {
                uint8_t nid = code[g_pc++];
                uint8_t argc = code[g_pc++];
                int base = g_sp - argc;
                Value res = native(nid, g_stack + base, argc);
                for (int i = 0; i < argc; i++) {
                    release_value(g_stack[base + i]);
                }
                g_sp = base;
                g_stack[g_sp++] = res;
                break;
            }
            case opTRYPUSH: {
                int32_t rel = read_i32(code, &g_pc);
                uint16_t slot = read_u16(code, &g_pc);
                int catch_pc = pyro_jump_to(g_pc, rel, codelen);
                if ((unsigned)g_hp > (unsigned)(int)(sizeof(g_handlers) / sizeof(g_handlers[0])) - 2u) {
                    fatal("malformed .pyro: exception handler stack overflow");
                }
                g_handlers[g_hp++] = { catch_pc, g_sp, g_fp, (int)slot };
                break;
            }
            case opTRYPOP:
                if (g_hp > 0) g_hp--;
                break;
            case opTHROW: {
                Value v = g_stack[--g_sp];
                if (!raise_exception(v)) {
                    char* s = value_to_string(v);
                    char err[1024];
                    std::snprintf(err, sizeof(err), "uncaught exception: %s", s);
                    std::free(s);
                    fatal(err);
                }
                break;
            }
            case opCOALESCE: {
                Value b = g_stack[--g_sp];
                Value a = g_stack[--g_sp];
                if (a.kind == VAL_NULL) {
                    g_stack[g_sp++] = b;
                    release_value(a);
                } else {
                    g_stack[g_sp++] = a;
                    release_value(b);
                }
                break;
            }
            case opUNWRAP: {
                Value a = g_stack[g_sp - 1];
                if (a.kind == VAL_NULL) {
                    g_sp--;
                    const char* um = "[Cryo Security] unwrap of null value";
                    Value err_msg = val_str(um, (int64_t)std::strlen(um));
                    if (!raise_exception(err_msg)) {
                        fatal(um);
                    }
                }
                break;
            }
            case opSPAWN:
            case opAWAIT:
                fatal("[Cryo Concurrency] spawn/await need the Go VM — this C++ VM has no scheduler (roadmap 12.5)");
                break;
            default:
                fatal("invalid opcode in bytecode");
        }
    }
}

} // namespace pyro

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "[Pyro VM C++] usage: pyrovm_cpp program.pyro\n";
        return 1;
    }

    pyro_argc = argc - 2;
    pyro_argv = argv + 2;

    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "[Pyro VM C++] could not read: " << argv[1] << "\n";
        return 1;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer((size_t)size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        std::cerr << "Fatal error: error reading file\n";
        return 1;
    }

    pyro_policy_init(std::getenv("PYRO_POLICY"));
    const char* env_sandbox = std::getenv("PYRO_SANDBOX");
    if (env_sandbox && std::strcmp(env_sandbox, "1") == 0) {
        pyro_sandboxed = true;
    }

    auto prog = pyro::load_program(buffer.data(), (size_t)size);
    pyro::run_program(prog.get());

    return 0;
}
