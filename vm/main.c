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
Program* load_program(const uint8_t* data, size_t size) {
    if (size < 6 || memcmp(data, "PYRO", 4) != 0) {
        fatal("invalid .pyro file (magic)");
    }
    if (data[4] != 2) {
        fatal("unsupported .pyro version (expected v2)");
    }
    uint8_t flags = data[5];
    Program* p = malloc(sizeof(Program));
    int pos = 6;
    
    p->sandboxed = (flags & 0x04) != 0;
    
    uint16_t nconsts = read_u16(data, &pos);
    p->nconsts = nconsts;
    p->consts = malloc(sizeof(Value) * nconsts);
    for (int i = 0; i < nconsts; i++) {
        uint8_t tag = data[pos++];
        switch (tag) {
            case TAG_INT:
                p->consts[i] = val_int((int64_t)read_u64(data, &pos));
                break;
            case TAG_FLT:
                {
                    uint64_t u = read_u64(data, &pos);
                    double f;
                    memcpy(&f, &u, 8);
                    p->consts[i] = val_float(f);
                }
                break;
            case TAG_STR:
                {
                    uint16_t len = read_u16(data, &pos);
                    p->consts[i] = val_str((const char*)(data + pos), len);
                    pos += len;
                }
                break;
            case TAG_BOOL:
                p->consts[i] = val_bool(data[pos++] != 0);
                break;
            default:
                fatal("unknown constant tag");
        }
    }
    
    uint16_t nfuncs = read_u16(data, &pos);
    p->nfuncs = nfuncs;
    p->funcs = malloc(sizeof(FuncInfo) * nfuncs);
    for (int i = 0; i < nfuncs; i++) {
        uint16_t nameidx = read_u16(data, &pos);
        uint32_t entry = read_u32(data, &pos);
        uint8_t nparams = data[pos++];
        uint16_t nlocals = read_u16(data, &pos);
        
        char* fname = value_to_string(p->consts[nameidx]);
        p->funcs[i] = (FuncInfo){ .name = fname, .entry = entry, .nparams = nparams, .nlocals = nlocals };
    }
    
    p->entryFn = read_u16(data, &pos);
    uint32_t codelen = read_u32(data, &pos);
    p->codelen = codelen;
    p->code = malloc(codelen);
    memcpy(p->code, data + pos, codelen);
    pos += codelen;
    
    if (flags & 0x01) {
        xor_decode(p->code, codelen);
    }
    
    p->dbg = NULL;
    p->ndebug = 0;
    if (flags & 0x02) {
        uint32_t ndbg = read_u32(data, &pos);
        p->ndebug = ndbg;
        p->dbg = malloc(sizeof(DebugEntry) * ndbg);
        for (uint32_t i = 0; i < ndbg; i++) {
            p->dbg[i].pc = read_u32(data, &pos);
            p->dbg[i].line = read_u32(data, &pos);
        }
    }
    
    return p;
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
    
    while (1) {
        uint8_t op = code[pc++];
        switch (op) {
            case opHALT:
                return;
            case opCONST:
                {
                    uint16_t idx = read_u16(code, &pc);
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
                    pc += rel;
                }
                break;
            case opJMPF:
                {
                    int32_t rel = read_i32(code, &pc);
                    Value a = stack[--sp];
                    bool t = value_truthy(a);
                    release_value(a);
                    if (!t) pc += rel;
                }
                break;
            case opJMPT:
                {
                    int32_t rel = read_i32(code, &pc);
                    Value a = stack[--sp];
                    bool t = value_truthy(a);
                    release_value(a);
                    if (t) pc += rel;
                }
                break;
            case opCALL:
                {
                    uint16_t fi = read_u16(code, &pc);
                    uint8_t argc = code[pc++];
                    FuncInfo fn = p->funcs[fi];
                    int next_base = frames[fp - 1].locals_base + frames[fp - 1].nlocals;
                    for (int i = 0; i < fn.nlocals; i++) {
                        locals_stack[next_base + i] = val_null();
                    }
                    int base = sp - argc;
                    for (int i = 0; i < argc; i++) {
                        locals_stack[next_base + i] = stack[base + i];
                    }
                    sp = base;
                    frames[fp++] = (Frame){ .retpc = pc, .locals_base = next_base, .nlocals = fn.nlocals, .fn = fi };
                    pc = fn.entry;
                }
                break;
            case opRET:
                {
                    Value ret = stack[--sp];
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
                    handlers[hp++] = (Handler){ .catchPC = pc + rel, .sp = sp, .fp = fp, .slot = slot };
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
                        const char* um = "[Cryo Security] unwrap of null value";
                        Value err_msg = val_str(um, (int64_t)strlen(um));
                        if (!raise_exception(err_msg)) {
                            fatal(um);
                        }
                        sp--;
                    }
                }
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
