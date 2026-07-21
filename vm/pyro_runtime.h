// ============================================================
//  Pyro Runtime — contrato de runtime compartilhado da VM Pyro
//
//  Isola o "runtime mínimo" do motor de execução (Fase 9.2):
//  modelo de valores, contagem de referências (retain/release),
//  strings/arrays/maps, conversões, I/O e os builtins NATIVE.
//  O motor (main.c) — e futuros alvos — dependem SÓ desta API,
//  garantindo semântica idêntica. A especificação em prosa está
//  em PYRO_RUNTIME.md.
//
//  Fronteira com o host (motor):
//    - void fatal(const char*)  : aborta com mensagem + stack trace
//    - bool pyro_sandboxed       : política; o runtime recusa rede se true
// ============================================================
#ifndef PYRO_RUNTIME_H
#define PYRO_RUNTIME_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ── Opcodes ──────────────────────────────────────────────────
#define opHALT       0x00
#define opCONST      0x01
#define opTRUE       0x02
#define opFALSE      0x03
#define opNULL       0x04
#define opPOP        0x05
#define opLOAD       0x06
#define opSTORE      0x07
#define opADD        0x10
#define opSUB        0x11
#define opMUL        0x12
#define opDIV        0x13
#define opMOD        0x14
#define opNEG        0x15
#define opBAND       0x16
#define opBOR        0x17
#define opBXOR       0x18
#define opSHL        0x19
#define opSHR        0x1A
#define opBNOT       0x1B
#define opEQ         0x20
#define opNE         0x21
#define opLT         0x22
#define opGT         0x23
#define opLE         0x24
#define opGE         0x25
#define opNOT        0x26
#define opJMP        0x30
#define opJMPF       0x31
#define opJMPT       0x32
#define opCALL       0x40
#define opRET        0x41
#define opPRINT      0x50
#define opASSERT     0x51
#define opPRINTLN    0x52
#define opNEWARR     0x60
#define opNEWMAP     0x61
#define opINDEX      0x62
#define opSETIDX     0x63
#define opLEN        0x64
#define opAPPEND     0x65
#define opHAS        0x66
#define opKEYS       0x67
#define opNATIVE     0x70
#define opTRYPUSH    0x71
#define opTRYPOP     0x72
#define opTHROW      0x73
#define opCOALESCE   0x74
#define opUNWRAP     0x75

// ── Constant Tags ───────────────────────────────────────────
#define TAG_INT      1
#define TAG_FLT      2
#define TAG_STR      3
#define TAG_BOOL     4

// 1. Forward typedefs/struct declarations
typedef struct RcString RcString;
typedef struct RcArray RcArray;
typedef struct RcMap RcMap;
typedef struct Value Value;

// 2. Define ValueKind and struct Value
typedef enum {
    VAL_INT,
    VAL_FLOAT,
    VAL_BOOL,
    VAL_STR,
    VAL_NULL,
    VAL_ARRAY,
    VAL_MAP
} ValueKind;

struct Value {
    ValueKind kind;
    union {
        int64_t i;
        double f;
        bool b;
        RcString* str;
        RcArray* arr;
        RcMap* map;
    } as;
};

// 3. Define the actual structs matching the tags
struct RcString {
    int ref_count;
    int64_t length;
    char chars[];
};

struct RcArray {
    int ref_count;
    Value* data;
    int64_t length;
    int64_t capacity;
};

// 4. Define MapNode
typedef struct MapNode {
    Value key;
    Value val;
    struct MapNode* next;
} MapNode;

// 5. Define struct RcMap matching the tag
struct RcMap {
    int ref_count;
    MapNode** buckets;
    int64_t size;
    int64_t capacity;
};

// ── fronteira com o host ─────────────────────────────────────
void fatal(const char* msg);      // aborto fail-fast (definido no motor)
extern bool pyro_sandboxed;       // política de sandbox (rede desligada)

// ── API do runtime ───────────────────────────────────────────
// criadores de Value
Value val_int(int64_t i);
Value val_float(double f);
Value val_bool(bool b);
Value val_null(void);
RcString* new_rc_string(const char* chars, int64_t len);
Value val_str(const char* chars, int64_t len);
Value val_str_rc(RcString* s);
Value val_array(RcArray* arr);
Value val_map(RcMap* map);
// contagem de referências
void retain_value(Value v);
void release_value(Value v);
uint32_t hash_value(Value v);
// maps
RcMap* rc_map_new(void);
void rc_map_resize(RcMap* m);
void rc_map_set(RcMap* m, Value key, Value val);
Value rc_map_get(RcMap* m, Value key);
bool rc_map_has(RcMap* m, Value key);
void rc_map_remove(RcMap* m, Value key);
// arrays
RcArray* rc_array_new(void);
void rc_array_push(RcArray* a, Value v);
Value rc_array_get(RcArray* a, int64_t idx);
void rc_array_set(RcArray* a, int64_t idx, Value v);
RcArray* rc_map_keys_sorted(RcMap* m);   // keys() ordenadas (paridade Go)
// conversões / consultas
char* value_to_string(Value v);
bool value_eq(Value a, Value b);
bool value_truthy(Value v);
double value_as_float(Value v);
int64_t value_length(Value v);
// indexação, aritmética e concatenação
Value index_get(Value cont, Value key);
void index_set(Value cont, Value key, Value val);
Value bin_op(uint8_t op, Value a, Value b);
Value str_concat(Value a, Value b);
RcArray* split_str(const char* s, const char* sep);
RcString* join_arr(RcArray* arr, const char* sep);
// builtins nativos (ids 0..26; ver NATIVES no gerador e PYRO_RUNTIME.md)
Value native(int id, Value* a, int argc);
// leitura do bytecode (little-endian) + decodificação da seção de código
uint16_t read_u16(const uint8_t* data, int* pos);
uint32_t read_u32(const uint8_t* data, int* pos);
uint64_t read_u64(const uint8_t* data, int* pos);
void xor_decode(uint8_t* code, uint32_t len);

#endif // PYRO_RUNTIME_H
