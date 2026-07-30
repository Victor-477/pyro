// ============================================================
//  Pyro Runtime — implementation (Phase 9.2)
//  Value model, refcount, containers, conversions, I/O and
//  NATIVE builtins. Depends on host only for fatal()/pyro_sandboxed.
//  Semantics specified in PYRO_RUNTIME.md.
// ============================================================
#include "pyro_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

// strdup is POSIX, not ISO C: strict modes (-std=c11) hide it, and it would
// then be implicitly declared as returning int — which truncates the pointer
// on 64-bit hosts. Always use our own, so the build is std-level independent.
static char* pyro_strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
#define strdup pyro_strdup

#ifdef _WIN32
#include <windows.h>
#include <process.h>
// Under strict ISO mode (-std=c11 defines __STRICT_ANSI__) MinGW hides these
// MSVCRT extensions, so they would be implicitly declared as returning int and
// the FILE*/pid would be truncated on 64-bit. Declare them ourselves.
#ifdef __STRICT_ANSI__
FILE* _popen(const char* command, const char* mode);
int   _pclose(FILE* stream);
int   _getpid(void);
#endif
#define sleep_ms(ms) Sleep(ms)
#define getpid _getpid
#define popen _popen
#define pclose _pclose
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

// sandbox policy: defined here, linked by the host (engine).
bool pyro_sandboxed = false;

// program arguments, published by the host (VM main / AOT main) for args().
int    pyro_argc = 0;
char** pyro_argv = NULL;

Value val_int(int64_t i) {
    Value v = { .kind = VAL_INT };
    v.as.i = i;
    return v;
}

Value val_float(double f) {
    Value v = { .kind = VAL_FLOAT };
    v.as.f = f;
    return v;
}

Value val_bool(bool b) {
    Value v = { .kind = VAL_BOOL };
    v.as.b = b;
    return v;
}

Value val_null(void) {
    Value v = { .kind = VAL_NULL };
    return v;
}

RcString* new_rc_string(const char* chars, int64_t len) {
    RcString* s = malloc(sizeof(RcString) + len + 1);
    s->ref_count = 1;
    s->length = len;
    if (chars) {
        memcpy(s->chars, chars, len);
    }
    s->chars[len] = '\0';
    return s;
}

Value val_str(const char* chars, int64_t len) {
    Value v = { .kind = VAL_STR };
    v.as.str = new_rc_string(chars, len);
    return v;
}

Value val_str_rc(RcString* s) {
    Value v = { .kind = VAL_STR };
    v.as.str = s;
    if (s) s->ref_count++;
    return v;
}

Value val_array(RcArray* arr) {
    Value v = { .kind = VAL_ARRAY };
    v.as.arr = arr;
    if (arr) arr->ref_count++;
    return v;
}

// function value: `captured` is NULL for a plain function reference, or an
// array of captured values (taken over, not retained again) for a closure.
Value val_func(int32_t fnidx, RcArray* captured) {
    Value v = { .kind = VAL_FUNC };
    v.fnidx = fnidx;
    v.as.arr = captured;
    return v;
}

Value val_map(RcMap* map) {
    Value v = { .kind = VAL_MAP };
    v.as.map = map;
    if (map) map->ref_count++;
    return v;
}

// ── Memory Management Implementation ──────────────────────────
void retain_value(Value v) {
    if (v.kind == VAL_FUNC) {
        // a closure owns its captured values; a bare function value has none
        if (v.as.arr) v.as.arr->ref_count++;
        return;
    }
    if (v.kind == VAL_STR && v.as.str) {
        v.as.str->ref_count++;
    } else if (v.kind == VAL_ARRAY && v.as.arr) {
        v.as.arr->ref_count++;
    } else if (v.kind == VAL_MAP && v.as.map) {
        v.as.map->ref_count++;
    }
}

void release_value(Value v) {
    if (v.kind == VAL_FUNC) {
        // releasing a closure releases its captured values (via the array)
        if (v.as.arr) {
            v.as.arr->ref_count--;
            if (v.as.arr->ref_count <= 0) {
                for (int64_t i = 0; i < v.as.arr->length; i++) {
                    release_value(v.as.arr->data[i]);
                }
                free(v.as.arr->data);
                free(v.as.arr);
            }
        }
        return;
    }
    if (v.kind == VAL_STR && v.as.str) {
        v.as.str->ref_count--;
        if (v.as.str->ref_count <= 0) {
            free(v.as.str);
        }
    } else if (v.kind == VAL_ARRAY && v.as.arr) {
        v.as.arr->ref_count--;
        if (v.as.arr->ref_count <= 0) {
            for (int64_t i = 0; i < v.as.arr->length; i++) {
                release_value(v.as.arr->data[i]);
            }
            free(v.as.arr->data);
            free(v.as.arr);
        }
    } else if (v.kind == VAL_MAP && v.as.map) {
        v.as.map->ref_count--;
        if (v.as.map->ref_count <= 0) {
            for (int64_t i = 0; i < v.as.map->capacity; i++) {
                MapNode* curr = v.as.map->buckets[i];
                while (curr) {
                    MapNode* next = curr->next;
                    release_value(curr->key);
                    release_value(curr->val);
                    free(curr);
                    curr = next;
                }
            }
            free(v.as.map->buckets);
            free(v.as.map);
        }
    }
}

// ── Hashing Mechanism ────────────────────────────────────────
uint32_t hash_value(Value v) {
    switch (v.kind) {
        case VAL_INT: return (uint32_t)(v.as.i ^ (v.as.i >> 32));
        case VAL_FLOAT: {
            uint64_t u;
            memcpy(&u, &v.as.f, 8);
            return (uint32_t)(u ^ (u >> 32));
        }
        case VAL_BOOL: return v.as.b ? 1 : 0;
        case VAL_STR: {
            uint32_t hash = 2166136261u;
            if (v.as.str) {
                for (int64_t i = 0; i < v.as.str->length; i++) {
                    hash ^= (uint8_t)v.as.str->chars[i];
                    hash *= 16777619;
                }
            }
            return hash;
        }
        default: return 0;
    }
}

// ── Map Implementation ───────────────────────────────────────
RcMap* rc_map_new(void) {
    RcMap* m = malloc(sizeof(RcMap));
    m->ref_count = 1;
    m->size = 0;
    m->capacity = 16;
    m->buckets = calloc(m->capacity, sizeof(MapNode*));
    return m;
}

void rc_map_resize(RcMap* m) {
    int64_t old_cap = m->capacity;
    MapNode** old_buckets = m->buckets;
    m->capacity *= 2;
    m->buckets = calloc(m->capacity, sizeof(MapNode*));
    for (int64_t i = 0; i < old_cap; i++) {
        MapNode* curr = old_buckets[i];
        while (curr) {
            MapNode* next = curr->next;
            uint32_t hash = hash_value(curr->key);
            int64_t index = hash % m->capacity;
            curr->next = m->buckets[index];
            m->buckets[index] = curr;
            curr = next;
        }
    }
    free(old_buckets);
}

void rc_map_set(RcMap* m, Value key, Value val) {
    if (m->size >= m->capacity * 0.75) {
        rc_map_resize(m);
    }
    uint32_t hash = hash_value(key);
    int64_t index = hash % m->capacity;
    MapNode* curr = m->buckets[index];
    while (curr) {
        if (value_eq(curr->key, key)) {
            release_value(curr->val);
            curr->val = val;
            retain_value(val);
            return;
        }
        curr = curr->next;
    }
    MapNode* node = malloc(sizeof(MapNode));
    node->key = key;
    node->val = val;
    retain_value(key);
    retain_value(val);
    node->next = m->buckets[index];
    m->buckets[index] = node;
    m->size++;
}

Value rc_map_get(RcMap* m, Value key) {
    uint32_t hash = hash_value(key);
    int64_t index = hash % m->capacity;
    MapNode* curr = m->buckets[index];
    while (curr) {
        if (value_eq(curr->key, key)) {
            return curr->val;
        }
        curr = curr->next;
    }
    return val_null();
}

bool rc_map_has(RcMap* m, Value key) {
    uint32_t hash = hash_value(key);
    int64_t index = hash % m->capacity;
    MapNode* curr = m->buckets[index];
    while (curr) {
        if (value_eq(curr->key, key)) {
            return true;
        }
        curr = curr->next;
    }
    return false;
}

void rc_map_remove(RcMap* m, Value key) {
    uint32_t hash = hash_value(key);
    int64_t index = hash % m->capacity;
    MapNode* prev = NULL;
    MapNode* curr = m->buckets[index];
    while (curr) {
        if (value_eq(curr->key, key)) {
            if (prev) {
                prev->next = curr->next;
            } else {
                m->buckets[index] = curr->next;
            }
            release_value(curr->key);
            release_value(curr->val);
            free(curr);
            m->size--;
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

// ── Array Implementation ─────────────────────────────────────
RcArray* rc_array_new(void) {
    RcArray* a = malloc(sizeof(RcArray));
    a->ref_count = 1;
    a->length = 0;
    a->capacity = 8;
    a->data = malloc(a->capacity * sizeof(Value));
    return a;
}

void rc_array_push(RcArray* a, Value v) {
    if (a->length >= a->capacity) {
        a->capacity *= 2;
        a->data = realloc(a->data, a->capacity * sizeof(Value));
    }
    a->data[a->length++] = v;
    retain_value(v);
}

Value rc_array_get(RcArray* a, int64_t idx) {
    if (idx < 0 || idx >= a->length) {
        // parity with the Go VM: fail-fast (uncatchable), same message
        char err[128];
        sprintf(err, "[Cryo Security] IndexError: index %lld out of bounds (len=%lld)", (long long)idx, (long long)a->length);
        fatal(err);
        return val_null();
    }
    return a->data[idx];
}

void rc_array_set(RcArray* a, int64_t idx, Value v) {
    if (idx < 0 || idx >= a->length) {
        char err[128];
        sprintf(err, "[Cryo Security] IndexError: index %lld out of bounds", (long long)idx);
        fatal(err);
        return;
    }
    release_value(a->data[idx]);
    a->data[idx] = v;
    retain_value(v);
}

// ── String Formatting and Matching ────────────────────────────
typedef struct {
    char* key_str;
    Value key;
    Value val;
} MapPair;

int compare_map_pairs(const void* a, const void* b) {
    return strcmp(((MapPair*)a)->key_str, ((MapPair*)b)->key_str);
}

char* value_to_string(Value v) {
    char buf[128];
    if (v.kind == VAL_NULL) {
        return strdup("null");
    } else if (v.kind == VAL_BOOL) {
        return strdup(v.as.b ? "true" : "false");
    } else if (v.kind == VAL_INT) {
        sprintf(buf, "%lld", (long long)v.as.i);
        return strdup(buf);
    } else if (v.kind == VAL_FLOAT) {
        if (isinf(v.as.f)) {
            return strdup(v.as.f > 0 ? "+Inf" : "-Inf");
        } else if (isnan(v.as.f)) {
            return strdup("NaN");
        }
        sprintf(buf, "%.14g", v.as.f);
        return strdup(buf);
    } else if (v.kind == VAL_STR) {
        return strdup(v.as.str ? v.as.str->chars : "");
    } else if (v.kind == VAL_FUNC) {
        sprintf(buf, "<fn#%d>", (int)v.fnidx);
        return strdup(buf);
    } else if (v.kind == VAL_ARRAY) {
        size_t capacity = 1004;
        size_t length = 1;
        char* result = malloc(capacity);
        result[0] = '[';
        result[1] = '\0';
        if (v.as.arr) {
            for (int64_t i = 0; i < v.as.arr->length; i++) {
                if (i > 0) {
                    if (length + 3 >= capacity) {
                        capacity *= 2;
                        result = realloc(result, capacity);
                    }
                    strcat(result, ", ");
                    length += 2;
                }
                char* elem_str = value_to_string(v.as.arr->data[i]);
                size_t elem_len = strlen(elem_str);
                if (length + elem_len + 2 >= capacity) {
                    capacity = (capacity + elem_len) * 2;
                    result = realloc(result, capacity);
                }
                strcat(result, elem_str);
                length += elem_len;
                free(elem_str);
            }
        }
        result[length] = ']';
        result[length+1] = '\0';
        return result;
    } else if (v.kind == VAL_MAP) {
        if (!v.as.map || v.as.map->size == 0) {
            return strdup("{}");
        }
        int64_t count = 0;
        MapPair* pairs = malloc(sizeof(MapPair) * v.as.map->size);
        for (int64_t i = 0; i < v.as.map->capacity; i++) {
            MapNode* curr = v.as.map->buckets[i];
            while (curr) {
                pairs[count].key = curr->key;
                pairs[count].val = curr->val;
                pairs[count].key_str = value_to_string(curr->key);
                count++;
                curr = curr->next;
            }
        }
        qsort(pairs, count, sizeof(MapPair), compare_map_pairs);
        
        size_t capacity = 1004;
        size_t length = 1;
        char* result = malloc(capacity);
        result[0] = '{';
        result[1] = '\0';
        for (int64_t i = 0; i < count; i++) {
            if (i > 0) {
                if (length + 3 >= capacity) {
                    capacity *= 2;
                    result = realloc(result, capacity);
                }
                strcat(result, ", ");
                length += 2;
            }
            char* val_str = value_to_string(pairs[i].val);
            size_t klen = strlen(pairs[i].key_str);
            size_t vlen = strlen(val_str);
            if (length + klen + vlen + 5 >= capacity) {
                capacity = (capacity + klen + vlen) * 2;
                result = realloc(result, capacity);
            }
            strcat(result, pairs[i].key_str);
            strcat(result, ": ");
            strcat(result, val_str);
            length += klen + 2 + vlen;
            free(val_str);
            free(pairs[i].key_str);
        }
        free(pairs);
        result[length] = '}';
        result[length+1] = '\0';
        return result;
    }
    return strdup("");
}

bool value_eq(Value a, Value b) {
    if (a.kind == VAL_NULL || b.kind == VAL_NULL) {
        return a.kind == VAL_NULL && b.kind == VAL_NULL;
    }
    if (a.kind == VAL_STR || b.kind == VAL_STR) {
        char* sa = value_to_string(a);
        char* sb = value_to_string(b);
        bool eq = (strcmp(sa, sb) == 0);
        free(sa); free(sb);
        return eq;
    }
    if (a.kind == VAL_FLOAT || b.kind == VAL_FLOAT) {
        double fa = (a.kind == VAL_FLOAT) ? a.as.f : (double)a.as.i;
        double fb = (b.kind == VAL_FLOAT) ? b.as.f : (double)b.as.i;
        return fa == fb;
    }
    if (a.kind == VAL_BOOL || b.kind == VAL_BOOL) {
        bool ba = (a.kind == VAL_BOOL) ? a.as.b : (a.kind == VAL_INT ? a.as.i != 0 : false);
        bool bb = (b.kind == VAL_BOOL) ? b.as.b : (a.kind == VAL_INT ? b.as.i != 0 : false);
        return ba == bb;
    }
    if (a.kind != b.kind) return false;
    if (a.kind == VAL_INT) return a.as.i == b.as.i;
    if (a.kind == VAL_ARRAY) return a.as.arr == b.as.arr;
    if (a.kind == VAL_MAP) return a.as.map == b.as.map;
    if (a.kind == VAL_FUNC) return a.fnidx == b.fnidx && a.as.arr == b.as.arr;
    return false;
}

bool value_truthy(Value v) {
    switch (v.kind) {
        case VAL_BOOL: return v.as.b;
        case VAL_INT: return v.as.i != 0;
        case VAL_FLOAT: return v.as.f != 0.0;
        case VAL_STR: return v.as.str && v.as.str->length > 0;
        case VAL_ARRAY: return v.as.arr && v.as.arr->length > 0;
        case VAL_MAP: return v.as.map && v.as.map->size > 0;
        case VAL_FUNC: return true;
        default: return false;
    }
}

double value_as_float(Value v) {
    if (v.kind == VAL_FLOAT) return v.as.f;
    return (double)v.as.i;
}

int64_t value_length(Value v) {
    if (v.kind == VAL_STR && v.as.str) return v.as.str->length;
    if (v.kind == VAL_ARRAY && v.as.arr) return v.as.arr->length;
    if (v.kind == VAL_MAP && v.as.map) return v.as.map->size;
    return 0;
}

Value index_get(Value cont, Value key) {
    if (cont.kind == VAL_ARRAY) {
        int64_t idx = key.as.i;
        // rc_array_get borrows; callers of index_get own (and release) the
        // result, so retain here exactly as the map branch below does.
        // Without this the element is freed while still in the array.
        Value res = rc_array_get(cont.as.arr, idx);
        retain_value(res);
        return res;
    }
    if (cont.kind == VAL_MAP) {
        Value res = rc_map_get(cont.as.map, key);
        retain_value(res);
        return res;
    }
    if (cont.kind == VAL_STR) {
        int64_t idx = key.as.i;
        if (idx < 0 || idx >= cont.as.str->length) {
            // parity with the Go VM: fail-fast, same message
            fatal("[Cryo Security] IndexError: string index out of bounds");
            return val_null();
        }
        char ch = cont.as.str->chars[idx];
        return val_str(&ch, 1);
    }
    fatal("indexing a non-indexable value");
    return val_null();
}

void index_set(Value cont, Value key, Value val) {
    if (cont.kind == VAL_ARRAY) {
        int64_t idx = key.as.i;
        rc_array_set(cont.as.arr, idx, val);
        return;
    }
    if (cont.kind == VAL_MAP) {
        rc_map_set(cont.as.map, key, val);
        return;
    }
    fatal("indexed assignment on a non-indexable value");
}

// ── Bytecode Reader Helpers ──────────────────────────────────
uint16_t read_u16(const uint8_t* data, int* pos) {
    uint16_t v = (uint16_t)data[*pos] | ((uint16_t)data[*pos + 1] << 8);
    *pos += 2;
    return v;
}

int16_t read_i16(const uint8_t* data, int* pos) {
    return (int16_t)read_u16(data, pos);
}

uint32_t read_u32(const uint8_t* data, int* pos) {
    uint32_t v = (uint32_t)data[*pos] |
                 ((uint32_t)data[*pos + 1] << 8) |
                 ((uint32_t)data[*pos + 2] << 16) |
                 ((uint32_t)data[*pos + 3] << 24);
    *pos += 4;
    return v;
}

int32_t read_i32(const uint8_t* data, int* pos) {
    return (int32_t)read_u32(data, pos);
}

uint64_t read_u64(const uint8_t* data, int* pos) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((uint64_t)data[*pos + i] << (i * 8));
    }
    *pos += 8;
    return v;
}

void xor_decode(uint8_t* code, uint32_t len) {
    uint8_t k = 0x5A;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t enc = code[i];
        uint8_t dec = enc ^ k;
        code[i] = dec;
        k = (k * 31 + 7 + dec) & 0xFF;
    }
}

// ── Builtin JSON and HTTP Helpers ─────────────────────────────
char* escape_json_string(const char* s) {
    size_t len = strlen(s);
    char* res = malloc(len * 2 + 3);
    size_t pos = 0;
    res[pos++] = '"';
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '"') {
            res[pos++] = '\\'; res[pos++] = '"';
        } else if (c == '\\') {
            res[pos++] = '\\'; res[pos++] = '\\';
        } else if (c == '\n') {
            res[pos++] = '\\'; res[pos++] = 'n';
        } else if (c == '\r') {
            res[pos++] = '\\'; res[pos++] = 'r';
        } else if (c == '\t') {
            res[pos++] = '\\'; res[pos++] = 't';
        } else {
            res[pos++] = c;
        }
    }
    res[pos++] = '"';
    res[pos] = '\0';
    return res;
}

char* value_to_json(Value v) {
    char buf[128];
    if (v.kind == VAL_NULL) {
        return strdup("null");
    } else if (v.kind == VAL_BOOL) {
        return strdup(v.as.b ? "true" : "false");
    } else if (v.kind == VAL_INT) {
        sprintf(buf, "%lld", (long long)v.as.i);
        return strdup(buf);
    } else if (v.kind == VAL_FLOAT) {
        sprintf(buf, "%.14g", v.as.f);
        return strdup(buf);
    } else if (v.kind == VAL_STR) {
        return escape_json_string(v.as.str ? v.as.str->chars : "");
    } else if (v.kind == VAL_ARRAY) {
        size_t capacity = 1004;
        size_t length = 1;
        char* result = malloc(capacity);
        result[0] = '[';
        result[1] = '\0';
        if (v.as.arr) {
            for (int64_t i = 0; i < v.as.arr->length; i++) {
                if (i > 0) {
                    if (length + 3 >= capacity) {
                        capacity *= 2;
                        result = realloc(result, capacity);
                    }
                    strcat(result, ",");
                    length += 1;
                }
                char* elem_str = value_to_json(v.as.arr->data[i]);
                size_t elem_len = strlen(elem_str);
                if (length + elem_len + 2 >= capacity) {
                    capacity = (capacity + elem_len) * 2;
                    result = realloc(result, capacity);
                }
                strcat(result, elem_str);
                length += elem_len;
                free(elem_str);
            }
        }
        result[length] = ']';
        result[length+1] = '\0';
        return result;
    } else if (v.kind == VAL_MAP) {
        if (!v.as.map || v.as.map->size == 0) {
            return strdup("{}");
        }
        int64_t count = 0;
        MapPair* pairs = malloc(sizeof(MapPair) * v.as.map->size);
        for (int64_t i = 0; i < v.as.map->capacity; i++) {
            MapNode* curr = v.as.map->buckets[i];
            while (curr) {
                pairs[count].key = curr->key;
                pairs[count].val = curr->val;
                pairs[count].key_str = value_to_string(curr->key);
                count++;
                curr = curr->next;
            }
        }
        qsort(pairs, count, sizeof(MapPair), compare_map_pairs);
        
        size_t capacity = 1004;
        size_t length = 1;
        char* result = malloc(capacity);
        result[0] = '{';
        result[1] = '\0';
        for (int64_t i = 0; i < count; i++) {
            if (i > 0) {
                if (length + 3 >= capacity) {
                    capacity *= 2;
                    result = realloc(result, capacity);
                }
                strcat(result, ",");
                length += 1;
            }
            char* escaped_k = escape_json_string(pairs[i].key_str);
            char* val_str = value_to_json(pairs[i].val);
            size_t klen = strlen(escaped_k);
            size_t vlen = strlen(val_str);
            if (length + klen + vlen + 5 >= capacity) {
                capacity = (capacity + klen + vlen) * 2;
                result = realloc(result, capacity);
            }
            strcat(result, escaped_k);
            strcat(result, ":");
            strcat(result, val_str);
            length += klen + 1 + vlen;
            free(escaped_k);
            free(val_str);
            free(pairs[i].key_str);
        }
        free(pairs);
        result[length] = '}';
        result[length+1] = '\0';
        return result;
    }
    return strdup("null");
}

void skip_whitespace(const char** p) {
    while (**p && isspace((unsigned char)**p)) (*p)++;
}

Value parse_json_value(const char** p);

Value parse_json_string(const char** p) {
    (*p)++; // skip "
    size_t capacity = 32;
    size_t length = 0;
    char* buf = malloc(capacity);
    while (**p && **p != '"') {
        char c = **p;
        if (c == '\\') {
            (*p)++;
            char ec = **p;
            if (ec == 'n') c = '\n';
            else if (ec == 'r') c = '\r';
            else if (ec == 't') c = '\t';
            else if (ec == '"' || ec == '\\' || ec == '/') c = ec;
        }
        if (length + 1 >= capacity) {
            capacity *= 2;
            buf = realloc(buf, capacity);
        }
        buf[length++] = c;
        (*p)++;
    }
    if (**p == '"') (*p)++;
    buf[length] = '\0';
    Value v = val_str(buf, length);
    free(buf);
    return v;
}

Value parse_json_number(const char** p) {
    const char* start = *p;
    bool is_float = false;
    while (**p && ((**p >= '0' && **p <= '9') || **p == '-' || **p == '+' || **p == '.' || **p == 'e' || **p == 'E')) {
        if (**p == '.' || **p == 'e' || **p == 'E') is_float = true;
        (*p)++;
    }
    char* end;
    if (is_float) {
        double f = strtod(start, &end);
        return val_float(f);
    } else {
        long long i = strtoll(start, &end, 10);
        return val_int((int64_t)i);
    }
}

Value parse_json_array(const char** p) {
    (*p)++; // skip [
    RcArray* arr = rc_array_new();
    skip_whitespace(p);
    if (**p == ']') {
        (*p)++;
        return val_array(arr);
    }
    while (1) {
        Value val = parse_json_value(p);
        rc_array_push(arr, val);
        release_value(val);
        skip_whitespace(p);
        if (**p == ',') {
            (*p)++;
            skip_whitespace(p);
        } else if (**p == ']') {
            (*p)++;
            break;
        } else {
            break;
        }
    }
    return val_array(arr);
}

Value parse_json_object(const char** p) {
    (*p)++; // skip {
    RcMap* map = rc_map_new();
    skip_whitespace(p);
    if (**p == '}') {
        (*p)++;
        return val_map(map);
    }
    while (1) {
        skip_whitespace(p);
        if (**p != '"') break;
        Value key = parse_json_string(p);
        skip_whitespace(p);
        if (**p == ':') {
            (*p)++;
        }
        Value val = parse_json_value(p);
        rc_map_set(map, key, val);
        release_value(key);
        release_value(val);
        skip_whitespace(p);
        if (**p == ',') {
            (*p)++;
            skip_whitespace(p);
        } else if (**p == '}') {
            (*p)++;
            break;
        } else {
            break;
        }
    }
    return val_map(map);
}

Value parse_json_value(const char** p) {
    skip_whitespace(p);
    if (**p == '"') {
        return parse_json_string(p);
    } else if (**p == '[') {
        return parse_json_array(p);
    } else if (**p == '{') {
        return parse_json_object(p);
    } else if (strncmp(*p, "true", 4) == 0) {
        *p += 4;
        return val_bool(true);
    } else if (strncmp(*p, "false", 5) == 0) {
        *p += 5;
        return val_bool(false);
    } else if (strncmp(*p, "null", 4) == 0) {
        *p += 4;
        return val_null();
    } else {
        return parse_json_number(p);
    }
}

char* http_get_curl(const char* url) {
    char cmd[2048];
    #ifdef _WIN32
    sprintf(cmd, "curl -s -L \"%s\"", url);
    #else
    sprintf(cmd, "curl -s -L '%s'", url);
    #endif
    FILE* f = popen(cmd, "r");
    if (!f) return strdup("");
    size_t capacity = 4096;
    size_t length = 0;
    char* buf = malloc(capacity);
    char chunk[1024];
    while (fgets(chunk, sizeof(chunk), f)) {
        size_t clen = strlen(chunk);
        if (length + clen + 1 >= capacity) {
            capacity *= 2;
            buf = realloc(buf, capacity);
        }
        strcpy(buf + length, chunk);
        length += clen;
    }
    pclose(f);
    buf[length] = '\0';
    return buf;
}

char* http_post_curl(const char* url, const char* body) {
    char tmp_filename[256];
    sprintf(tmp_filename, "pyro_tmp_post_%d.json", (int)getpid());
    FILE* tf = fopen(tmp_filename, "w");
    if (!tf) return strdup("");
    fputs(body, tf);
    fclose(tf);
    
    char cmd[1024];
    #ifdef _WIN32
    sprintf(cmd, "curl -s -L -X POST -H \"Content-Type: application/json\" -d @%s \"%s\"", tmp_filename, url);
    #else
    sprintf(cmd, "curl -s -L -X POST -H 'Content-Type: application/json' -d @%s '%s'", tmp_filename, url);
    #endif
    
    FILE* f = popen(cmd, "r");
    if (!f) {
        remove(tmp_filename);
        return strdup("");
    }
    size_t capacity = 4096;
    size_t length = 0;
    char* buf = malloc(capacity);
    char chunk[1024];
    while (fgets(chunk, sizeof(chunk), f)) {
        size_t clen = strlen(chunk);
        if (length + clen + 1 >= capacity) {
            capacity *= 2;
            buf = realloc(buf, capacity);
        }
        strcpy(buf + length, chunk);
        length += clen;
    }
    pclose(f);
    remove(tmp_filename);
    buf[length] = '\0';
    return buf;
}

// ── Minimal static HTTP server (backs http_serve) ────────────
// Single-threaded HTTP/1.1, one connection at a time: enough to serve a demo
// (page + .wasm) and to keep the C VM at parity with the Go VM's http_serve.
#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET pyro_sock;
#define PYRO_BADSOCK  INVALID_SOCKET
#define pyro_closesock closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
typedef int pyro_sock;
#define PYRO_BADSOCK  (-1)
#define pyro_closesock close
#endif

static const char* mime_for(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".wasm")) return "application/wasm";
    if (!strcmp(dot, ".html") || !strcmp(dot, ".htm")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".js"))   return "application/javascript";
    if (!strcmp(dot, ".css"))  return "text/css";
    if (!strcmp(dot, ".json")) return "application/json";
    if (!strcmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcmp(dot, ".png"))  return "image/png";
    if (!strcmp(dot, ".txt"))  return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

// Reject anything that could escape the served root: absolute paths, drive
// letters, backslashes and any ".." segment (mirrors Go's http.Dir guard).
static bool path_is_safe(const char* p) {
    if (strchr(p, '\\') || strchr(p, ':')) return false;
    for (const char* s = p; *s; s++) {
        if (s[0] == '.' && s[1] == '.') return false;
    }
    return true;
}

static void http_send(pyro_sock c, int status, const char* reason,
                      const char* ctype, const char* body, long blen) {
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                     "Connection: close\r\n\r\n", status, reason, ctype, blen);
    send(c, head, n, 0);
    if (body && blen > 0) send(c, body, (int)blen, 0);
}

// ── capability policy (roadmap 11.11) ──────────────────────
//
//   (nothing set)   everything allowed
//   PYRO_SANDBOX=1  everything gated is refused
//   PYRO_POLICY=... deny by default, grant exactly what is listed
//
// Clauses: fs.read= fs.write= net= exec= env=  (comma-separated, * = all)
// Mirrors capPolicy in main.go, message text included.
#include <stddef.h>   // offsetof, for the policy gates
typedef struct { char** items; int n; } CapList;

// getcwd lives in <direct.h> on Windows and <unistd.h> elsewhere; wrapping it
// keeps the platform test in one place.
#ifdef _WIN32
#include <direct.h>
static bool pyro_getcwd(char* buf, size_t n) { return _getcwd(buf, (int)n) != NULL; }
#else
#include <unistd.h>
static bool pyro_getcwd(char* buf, size_t n) { return getcwd(buf, n) != NULL; }
#endif

// Two sets can be in force: what the OPERATOR set (PYRO_POLICY) and what the
// ARTIFACT declared (11.12). A capability must be allowed by both — an
// operator may narrow what a program asked for, never widen it.
typedef struct {
    bool    active;
    CapList fsread, fswrite, net, exec, env;
} CapPolicy;

static CapPolicy g_env_policy;        // PYRO_POLICY
static CapPolicy g_artifact_policy;   // embedded in the .pyro

#define g_policy_active (g_env_policy.active || g_artifact_policy.active)

static void cap_add(CapList* l, const char* item) {
    char** grown = (char**)realloc(l->items, (size_t)(l->n + 1) * sizeof(char*));
    if (!grown) return;
    l->items = grown;
    l->items[l->n++] = pyro_strdup(item);
}

static char* cap_trim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
    return s;
}

// Own case-insensitive compare: _stricmp/strcasecmp are hidden by
// -std=c11 on MinGW, and an implicit declaration would truncate the result.
static bool cap_ieq(const char* a, const char* b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

static bool cap_listed(const CapList* l, const char* want) {
    for (int i = 0; i < l->n; i++) {
        if (strcmp(l->items[i], "*") == 0) return true;
        if (cap_ieq(l->items[i], want)) return true;
    }
    return false;
}

// Absolute + normalised, so "./data/../secret" cannot slip out of a granted
// root. Separators are folded to '/' so the comparison is uniform.
// Absolute + normalised, written by hand: _fullpath and strtok_r are both
// hidden by -std=c11 on MinGW. Separators fold to '/', "." is dropped and
// ".." pops a segment — that last part is what stops "./data/../secret"
// slipping out of a granted root.
static void cap_abs(const char* path, char* out, size_t osz) {
    char joined[4096];
    bool absolute = (path[0] == '/' || path[0] == '\\') ||
                    (path[0] && path[1] == ':');
    if (absolute) {
        snprintf(joined, sizeof(joined), "%s", path);
    } else {
        char cwd[2048];
        if (!pyro_getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
        snprintf(joined, sizeof(joined), "%s/%s", cwd, path);
    }
    for (char* p = joined; *p; p++) if (*p == '\\') *p = '/';

    // keep a leading "C:" or "" prefix, then rebuild from the segments
    char prefix[8] = "";
    char* body = joined;
    if (joined[0] && joined[1] == ':') {
        prefix[0] = joined[0]; prefix[1] = ':'; prefix[2] = '\0';
        body = joined + 2;
    }

    char* seg[256];
    int nseg = 0;
    char* p = body;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char* st = p;
        while (*p && *p != '/') p++;
        char saved = *p;
        *p = '\0';
        if (strcmp(st, ".") == 0) {
            /* skip */
        } else if (strcmp(st, "..") == 0) {
            if (nseg > 0) nseg--;
        } else if (nseg < 256) {
            seg[nseg++] = st;
        }
        // Do NOT restore the separator: seg[] points into this buffer and
        // each entry must stay NUL-terminated. Restoring it made every
        // segment read to the end of the string.
        if (saved) p++;
    }

    size_t used = (size_t)snprintf(out, osz, "%s", prefix);
    for (int i = 0; i < nseg && used + 1 < osz; i++) {
        used += (size_t)snprintf(out + used, osz - used, "/%s", seg[i]);
    }
    if (used == 0 || (used == strlen(prefix) && osz > used + 1)) {
        snprintf(out + used, osz - used, "/");
    }
}

static bool cap_path_allowed(const CapList* roots, const char* target) {
    char abs[4096];
    cap_abs(target, abs, sizeof(abs));
    for (int i = 0; i < roots->n; i++) {
        if (strcmp(roots->items[i], "*") == 0) return true;
        char root[4096];
        cap_abs(roots->items[i], root, sizeof(root));
        size_t rl = strlen(root);
        if (rl && root[rl - 1] == '/') root[--rl] = '\0';
        if (strncmp(abs, root, rl) == 0 && (abs[rl] == '\0' || abs[rl] == '/')) {
            return true;
        }
    }
    return false;
}

static void cap_denied(const char* what, const char* capability, const char* subject) {
    char msg[1024];
    if (!g_policy_active) {
        snprintf(msg, sizeof(msg),
                 "[Cryo Security] Sandbox: %s blocked by sandbox policy", what);
    } else {
        snprintf(msg, sizeof(msg),
                 "[Cryo Security] Sandbox: %s denied for %s — grant it with %s=%s "
                 "in PYRO_POLICY", what, subject, capability, subject);
    }
    fatal(msg);
}

// Splits on `sep`, in place, returning the next token and advancing `*cur`.
// strtok_r is hidden by -std=c11 on MinGW and strtok is not reentrant, so the
// parser carries its own.
static char* cap_next(char** cur, char sep) {
    if (!*cur || !**cur) return NULL;
    char* start = *cur;
    char* p = start;
    while (*p && *p != sep) p++;
    if (*p == sep) { *p = '\0'; *cur = p + 1; } else { *cur = p; }
    return start;
}

static void policy_parse(CapPolicy* dst, const char* spec) {
    if (!spec || !*spec) return;
    dst->active = true;
    pyro_sandboxed = true;
    char* buf = pyro_strdup(spec);
    char* cur = buf;
    char* clause_raw;
    while ((clause_raw = cap_next(&cur, ';')) != NULL) {
        char* clause = cap_trim(clause_raw);
        if (!*clause) continue;
        char* eq = strchr(clause, '=');
        if (!eq) fatal("PYRO_POLICY: expected key=value in clause");
        *eq = '\0';
        char* key = cap_trim(clause);
        CapList* target = NULL;
        if      (strcmp(key, "fs.read")  == 0) target = &dst->fsread;
        else if (strcmp(key, "fs.write") == 0) target = &dst->fswrite;
        else if (strcmp(key, "net")      == 0) target = &dst->net;
        else if (strcmp(key, "exec")     == 0) target = &dst->exec;
        else if (strcmp(key, "env")      == 0) target = &dst->env;
        else {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "PYRO_POLICY: unknown capability '%s' "
                     "(known: fs.read, fs.write, net, exec, env)", key);
            fatal(msg);
        }
        char* items = eq + 1;
        char* it;
        while ((it = cap_next(&items, ',')) != NULL) {
            char* v = cap_trim(it);
            if (*v) cap_add(target, v);
        }
    }
    free(buf);
}

void pyro_policy_init(const char* spec)     { policy_parse(&g_env_policy, spec); }
void pyro_policy_artifact(const char* spec) { policy_parse(&g_artifact_policy, spec); }

// Every ACTIVE policy must allow — the operator's and the artifact's.
// Mirrors bothAllow() in main.go. The offset picks which CapList to consult,
// so one helper serves all five capabilities.
static bool cap_both_paths(size_t off, const char* path) {
    const CapPolicy* pols[2] = { &g_env_policy, &g_artifact_policy };
    for (int i = 0; i < 2; i++) {
        if (!pols[i]->active) continue;
        const CapList* l = (const CapList*)((const char*)pols[i] + off);
        if (!cap_path_allowed(l, path)) return false;
    }
    return g_policy_active;
}

static bool cap_both_listed(size_t off, const char* want) {
    const CapPolicy* pols[2] = { &g_env_policy, &g_artifact_policy };
    for (int i = 0; i < 2; i++) {
        if (!pols[i]->active) continue;
        const CapList* l = (const CapList*)((const char*)pols[i] + off);
        if (!cap_listed(l, want)) return false;
    }
    return g_policy_active;
}

void cap_fs(bool read, const char* path, const char* what) {
    if (!pyro_sandboxed && !g_policy_active) return;
    size_t off = read ? offsetof(CapPolicy, fsread) : offsetof(CapPolicy, fswrite);
    const char* name = read ? "fs.read" : "fs.write";
    if (!cap_both_paths(off, path)) cap_denied(what, name, path);
}

// The host out of a URL, for the net allowlist. Unparseable means refused:
// failing open here would defeat the allowlist.
static void cap_host_of(const char* rawurl, char* out, size_t osz) {
    const char* p = strstr(rawurl, "://");
    p = p ? p + 3 : rawurl;
    size_t i = 0;
    while (p[i] && p[i] != '/' && p[i] != ':' && i < osz - 1) { out[i] = p[i]; i++; }
    out[i] = '\0';
}

void cap_net(const char* target, const char* what) {
    if (!pyro_sandboxed && !g_policy_active) return;
    char host[512];
    cap_host_of(target, host, sizeof(host));
    if (!host[0]) snprintf(host, sizeof(host), "%s", target);
    if (!cap_both_listed(offsetof(CapPolicy, net), host)) cap_denied(what, "net", host);
}

void cap_exec(const char* cmd, const char* what) {
    if (!pyro_sandboxed && !g_policy_active) return;
    char bin[512];
    size_t i = 0;
    while (cmd[i] && cmd[i] != ' ' && i < sizeof(bin) - 1) { bin[i] = cmd[i]; i++; }
    bin[i] = '\0';
    const char* base = bin;
    for (const char* p = bin; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    if (!cap_both_listed(offsetof(CapPolicy, exec), base)) cap_denied(what, "exec", base);
}

void cap_env(const char* name, const char* what) {
    if (!pyro_sandboxed && !g_policy_active) return;
    if (!cap_both_listed(offsetof(CapPolicy, env), name)) cap_denied(what, "env", name);
}

// ── embedded assets (roadmap 11.9) ─────────────────────────
// Filled by the loader (C VM) or by the generated program (AOT), so both
// engines answer asset() from the same table. Names are kept sorted so
// asset_names() matches the Go VM's ordering.
typedef struct { char* name; char* data; int64_t len; } PyroAsset;
static PyroAsset* g_assets = NULL;
static int g_nassets = 0;

static int pyro_asset_cmp(const void* a, const void* b) {
    return strcmp(((const PyroAsset*)a)->name, ((const PyroAsset*)b)->name);
}

// Takes ownership of `name` and `data`.
void pyro_asset_add(char* name, char* data, int64_t len) {
    PyroAsset* grown = (PyroAsset*)realloc(g_assets,
                                           (size_t)(g_nassets + 1) * sizeof(PyroAsset));
    if (!grown) return;
    g_assets = grown;
    g_assets[g_nassets].name = name;
    g_assets[g_nassets].data = data;
    g_assets[g_nassets].len = len;
    g_nassets++;
    qsort(g_assets, (size_t)g_nassets, sizeof(PyroAsset), pyro_asset_cmp);
}

// ── filesystem helpers (roadmap 11.7) ──────────────────────
#include <dirent.h>
#include <sys/stat.h>

// list_dir sorts its names so the Go VM and this runtime return the same
// order. Go uses sort.Strings; strcmp gives the same byte ordering.
static int pyro_name_cmp(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

static int pyro_is_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (st.st_mode & S_IFMT) == S_IFDIR;
}

// Creates every missing component, like Go's os.MkdirAll. Returns 1 on
// success or if the directory already exists.
static int pyro_mkdir_all(const char* path) {
    char buf[1024];
    size_t n = strlen(path);
    if (n >= sizeof(buf)) return 0;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] == '/' || buf[i] == '\\') {
            char save = buf[i];
            buf[i] = '\0';
            if (buf[0] && !pyro_is_dir(buf)) {
#ifdef _WIN32
                _mkdir(buf);
#else
                mkdir(buf, 0755);
#endif
            }
            buf[i] = save;
        }
    }
    if (pyro_is_dir(buf)) return 1;
#ifdef _WIN32
    return _mkdir(buf) == 0 || pyro_is_dir(buf);
#else
    return mkdir(buf, 0755) == 0 || pyro_is_dir(buf);
#endif
}

// ── HTTP server (roadmap 11.6) ─────────────────────────────
// One listener, one in-flight connection: requests are served strictly one at
// a time, which is what makes this safe without any locking. Mirrors the Go
// VM's implementation in main.go, field for field.
static pyro_sock g_http_srv  = PYRO_BADSOCK;
static pyro_sock g_http_conn = PYRO_BADSOCK;

// One hex digit -> its value, or -1. Mirrors unhex() in main.go so the two
// engines agree on what counts as a valid escape.
static int pyro_unhex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char* http_status_text(int64_t code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default:  return "Status";
    }
}

// Reads one HTTP/1.1 request. Returns 0 on a malformed or closed connection.
// Kept deliberately minimal and behaviour-identical to httpParseRequest in Go.
// Headers are handed back as a single "name: value\n" blob, which the caller
// splits into the request map. A blob keeps this signature from growing a
// parallel array pair, and the map keys end up identical to the Go VM's.
static int http_read_request(pyro_sock c, char* method, size_t msz,
                             char* path, size_t psz, char* query, size_t qsz,
                             char** body_out, char** hdrs_out) {
    static char buf[65536];
    int total = 0;
    int header_end = -1;
    *body_out = NULL;
    if (hdrs_out) *hdrs_out = NULL;

    // read until the end of the headers
    while (total < (int)sizeof(buf) - 1) {
        int got = recv(c, buf + total, (int)sizeof(buf) - 1 - total, 0);
        if (got <= 0) return 0;
        total += got;
        buf[total] = '\0';
        char* p = strstr(buf, "\r\n\r\n");
        if (p) { header_end = (int)(p - buf) + 4; break; }
    }
    if (header_end < 0) return 0;

    // request line: METHOD TARGET HTTP/1.1
    const char* sp1 = strchr(buf, ' ');
    if (!sp1) return 0;
    size_t mlen = (size_t)(sp1 - buf);
    if (mlen >= msz) mlen = msz - 1;
    memcpy(method, buf, mlen); method[mlen] = '\0';

    const char* target = sp1 + 1;
    const char* sp2 = strchr(target, ' ');
    if (!sp2) return 0;
    size_t tlen = (size_t)(sp2 - target);

    const char* q = (const char*)memchr(target, '?', tlen);
    size_t plen = q ? (size_t)(q - target) : tlen;
    if (plen >= psz) plen = psz - 1;
    memcpy(path, target, plen); path[plen] = '\0';
    query[0] = '\0';
    if (q) {
        size_t qlen = tlen - plen - 1;
        if (qlen >= qsz) qlen = qsz - 1;
        memcpy(query, q + 1, qlen); query[qlen] = '\0';
    }

    // Walk the header block once: collect every header AND find
    // Content-Length. Names are lowercased so lookups match the Go VM.
    long clen = 0;
    size_t hcap = 1024, hlen = 0;
    char* hblob = (char*)malloc(hcap);
    if (hblob) hblob[0] = '\0';
    const char* first_eol = strstr(buf, "\r\n");
    for (const char* h = first_eol ? first_eol + 2 : NULL;
         h && h < buf + header_end; ) {
        const char* eol = strstr(h, "\r\n");
        if (!eol || eol > buf + header_end) break;
        size_t linelen = (size_t)(eol - h);
        if (linelen == 0) break;                     // end of headers
        const char* colon = (const char*)memchr(h, ':', linelen);
        if (colon) {
            size_t nlen = (size_t)(colon - h);
            const char* v = colon + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            size_t vlen = (size_t)(eol - v);

            char lname[128];
            size_t keep = nlen < sizeof(lname) - 1 ? nlen : sizeof(lname) - 1;
            for (size_t i = 0; i < keep; i++)
                lname[i] = (char)tolower((unsigned char)h[i]);
            lname[keep] = '\0';

            if (strcmp(lname, "content-length") == 0)
                clen = strtol(v, NULL, 10);

            if (hblob) {
                size_t need = hlen + keep + vlen + 3;
                if (need > hcap) {
                    while (need > hcap) hcap *= 2;
                    char* grown = (char*)realloc(hblob, hcap);
                    if (grown) { hblob = grown; }
                    else { free(hblob); hblob = NULL; }
                }
                if (hblob) {
                    memcpy(hblob + hlen, lname, keep); hlen += keep;
                    hblob[hlen++] = ':';
                    memcpy(hblob + hlen, v, vlen); hlen += vlen;
                    hblob[hlen++] = '\n';
                    hblob[hlen] = '\0';
                }
            }
        }
        h = eol + 2;
    }
    if (hdrs_out) *hdrs_out = hblob; else free(hblob);
    if (clen > 0) {
        char* body = (char*)malloc((size_t)clen + 1);
        if (!body) return 0;
        int have = total - header_end;
        if (have > clen) have = (int)clen;
        if (have > 0) memcpy(body, buf + header_end, (size_t)have);
        while (have < clen) {
            int got = recv(c, body + have, (int)(clen - have), 0);
            if (got <= 0) break;
            have += got;
        }
        body[have] = '\0';
        *body_out = body;
    }
    return 1;
}

void http_serve_dir(const char* dir, int port) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) fatal("http_serve: WSAStartup failed");
#endif
    pyro_sock srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == PYRO_BADSOCK) fatal("http_serve: socket() failed");
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) != 0) fatal("http_serve: bind failed");
    if (listen(srv, 16) != 0) fatal("http_serve: listen failed");

    for (;;) {
        pyro_sock c = accept(srv, NULL, NULL);
        if (c == PYRO_BADSOCK) continue;

        char req[2048];
        int got = recv(c, req, (int)sizeof(req) - 1, 0);
        if (got <= 0) { pyro_closesock(c); continue; }
        req[got] = '\0';

        // parse "GET /path HTTP/1.1"
        char path[1024] = "/";
        if (strncmp(req, "GET ", 4) == 0) {
            const char* s = req + 4;
            const char* e = strchr(s, ' ');
            size_t len = e ? (size_t)(e - s) : strlen(s);
            if (len >= sizeof(path)) len = sizeof(path) - 1;
            memcpy(path, s, len);
            path[len] = '\0';
        } else {
            const char* m = "method not allowed";
            http_send(c, 405, "Method Not Allowed", "text/plain", m, (long)strlen(m));
            pyro_closesock(c);
            continue;
        }
        char* q = strchr(path, '?');            // drop the query string
        if (q) *q = '\0';
        if (strcmp(path, "/") == 0) strcpy(path, "/index.html");

        if (!path_is_safe(path + 1)) {
            const char* m = "forbidden";
            http_send(c, 403, "Forbidden", "text/plain", m, (long)strlen(m));
            pyro_closesock(c);
            continue;
        }

        char full[2048];
        snprintf(full, sizeof(full), "%s/%s", dir, path + 1);
        FILE* f = fopen(full, "rb");
        if (!f) {
            const char* m = "404 page not found";
            http_send(c, 404, "Not Found", "text/plain", m, (long)strlen(m));
            pyro_closesock(c);
            continue;
        }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char* body = (char*)malloc((size_t)(n > 0 ? n : 1));
        size_t rd = body ? fread(body, 1, (size_t)n, f) : 0;
        fclose(f);
        if (!body) {
            const char* m = "out of memory";
            http_send(c, 500, "Internal Server Error", "text/plain", m, (long)strlen(m));
        } else {
            http_send(c, 200, "OK", mime_for(full), body, (long)rd);
            free(body);
        }
        pyro_closesock(c);
    }
}

static uint64_t g_c_prng_state = 0x853c49e6748fea9bULL;
static int64_t g_c_start_ms = 0;

static uint64_t splitmix64_next(void) {
    g_c_prng_state += 0x9e3779b97f4a7c15ULL;
    uint64_t z = g_c_prng_state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static int64_t pyro_get_now_ms(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int64_t)((t - 116444736000000000ULL) / 10000ULL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
#endif
}

static int64_t pyro_get_mono_ms(void) {
    if (g_c_start_ms == 0) {
        g_c_start_ms = pyro_get_now_ms();
    }
    return pyro_get_now_ms() - g_c_start_ms;
}

// native_pad: pad_start/pad_end with JS padStart/padEnd semantics — the pad
// string is repeated and truncated to fill exactly (width-len) bytes. Caller frees.
static char* native_pad(const char* s, int width, const char* pad, bool at_start) {
    size_t sl = strlen(s), pl = strlen(pad);
    if ((int)sl >= width || pl == 0) {
        char* out = (char*)malloc(sl + 1);
        memcpy(out, s, sl + 1);
        return out;
    }
    size_t need = (size_t)width - sl;
    char* out = (char*)malloc(need + sl + 1);
    if (at_start) {
        for (size_t i = 0; i < need; i++) out[i] = pad[i % pl];
        memcpy(out + need, s, sl);
    } else {
        memcpy(out, s, sl);
        for (size_t i = 0; i < need; i++) out[sl + i] = pad[i % pl];
    }
    out[need + sl] = '\0';
    return out;
}

// native_less: the total order used by sort() — numbers compare numerically,
// everything else by its string form. Matches the Go VM's nativeLess exactly.
static bool native_less(Value a, Value b) {
    bool an = a.kind == VAL_INT || a.kind == VAL_FLOAT;
    bool bn = b.kind == VAL_INT || b.kind == VAL_FLOAT;
    if (an && bn) return value_as_float(a) < value_as_float(b);
    char* sa = value_to_string(a);
    char* sb = value_to_string(b);
    bool r = strcmp(sa, sb) < 0;
    free(sa); free(sb);
    return r;
}

// ── Native Builtins ──────────────────────────────────────────
Value native(int id, Value* a, int argc) {
    switch (id) {
        case 0: // sqrt
            return val_float(sqrt(value_as_float(a[0])));
        case 1: // pow
            return val_float(pow(value_as_float(a[0]), value_as_float(a[1])));
        case 2: // abs
            if (a[0].kind == VAL_INT) {
                return val_int(a[0].as.i < 0 ? -a[0].as.i : a[0].as.i);
            }
            return val_float(fabs(a[0].as.f));
        case 3: // min
            if (a[0].kind == VAL_INT && a[1].kind == VAL_INT) {
                return val_int(a[0].as.i < a[1].as.i ? a[0].as.i : a[1].as.i);
            }
            return val_float(fmin(value_as_float(a[0]), value_as_float(a[1])));
        case 4: // max
            if (a[0].kind == VAL_INT && a[1].kind == VAL_INT) {
                return val_int(a[0].as.i > a[1].as.i ? a[0].as.i : a[1].as.i);
            }
            return val_float(fmax(value_as_float(a[0]), value_as_float(a[1])));
        case 5: // floor
            return val_float(floor(value_as_float(a[0])));
        case 6: // ceil
            return val_float(ceil(value_as_float(a[0])));
        case 7: // round
            return val_float(round(value_as_float(a[0])));
        case 8: // to_string
            {
                char* s = value_to_string(a[0]);
                Value res = val_str(s, strlen(s));
                free(s);
                return res;
            }
        case 9: // to_int
            switch (a[0].kind) {
                case VAL_INT: return a[0];
                case VAL_FLOAT: return val_int((int64_t)a[0].as.f);
                case VAL_BOOL: return val_int(a[0].as.b ? 1 : 0);
                case VAL_STR:
                    {
                        const char* s = a[0].as.str->chars;
                        while (*s && isspace((unsigned char)*s)) s++;
                        char* end;
                        long long val = strtoll(s, &end, 10);
                        while (*end && isspace((unsigned char)*end)) end++;
                        if (*end != '\0' || end == s) {
                            char err[1024];
                            sprintf(err, "[Cryo Security] to_int: '%s' is not a valid integer", a[0].as.str->chars);
                            fatal(err);   // parity with the Go VM: fail-fast (uncatchable)
                            return val_null();
                        }
                        return val_int(val);
                    }
                default:
                    fatal("to_int: non-convertible type");
            }
            break;
        case 10: // to_number
            switch (a[0].kind) {
                case VAL_FLOAT: return a[0];
                case VAL_INT: return val_float((double)a[0].as.i);
                case VAL_STR:
                    {
                        const char* s = a[0].as.str->chars;
                        while (*s && isspace((unsigned char)*s)) s++;
                        char* end;
                        double val = strtod(s, &end);
                        while (*end && isspace((unsigned char)*end)) end++;
                        if (*end != '\0' || end == s) {
                            char err[1024];
                            sprintf(err, "[Cryo Security] to_number: '%s' is not a valid number", a[0].as.str->chars);
                            fatal(err);   // parity with the Go VM: fail-fast (uncatchable)
                            return val_null();
                        }
                        return val_float(val);
                    }
                default:
                    fatal("to_number: non-convertible type");
            }
            break;
        case 11: // remove(map, key)
            if (a[0].kind != VAL_MAP) {
                fatal("remove() applied to a non-map value");
            }
            rc_map_remove(a[0].as.map, a[1]);
            return val_null();
        case 12: // upper
            {
                char* s = value_to_string(a[0]);
                for (int i = 0; s[i]; i++) s[i] = toupper((unsigned char)s[i]);
                Value res = val_str(s, strlen(s));
                free(s);
                return res;
            }
        case 13: // lower
            {
                char* s = value_to_string(a[0]);
                for (int i = 0; s[i]; i++) s[i] = tolower((unsigned char)s[i]);
                Value res = val_str(s, strlen(s));
                free(s);
                return res;
            }
        case 14: // trim
            {
                char* s = value_to_string(a[0]);
                char* start = s;
                while (*start && isspace((unsigned char)*start)) start++;
                size_t len = strlen(start);
                while (len > 0 && isspace((unsigned char)start[len - 1])) len--;
                Value res = val_str(start, len);
                free(s);
                return res;
            }
        case 15: // contains
            {
                char* sa = value_to_string(a[0]);
                char* sb = value_to_string(a[1]);
                bool res = strstr(sa, sb) != NULL;
                free(sa); free(sb);
                return val_bool(res);
            }
        case 16: // find
            {
                char* sa = value_to_string(a[0]);
                char* sb = value_to_string(a[1]);
                char* loc = strstr(sa, sb);
                int64_t idx = loc ? (int64_t)(loc - sa) : -1;
                free(sa); free(sb);
                return val_int(idx);
            }
        case 17: // replace(s, old, new)
            {
                char* s = value_to_string(a[0]);
                char* old = value_to_string(a[1]);
                char* new_str = value_to_string(a[2]);
                size_t old_len = strlen(old);
                size_t new_len = strlen(new_str);
                
                if (old_len == 0) {
                    size_t slen = strlen(s);
                    size_t res_cap = (slen + 1) * new_len + slen + 1;
                    char* res = malloc(res_cap);
                    size_t pos = 0;
                    for (size_t i = 0; i < slen; i++) {
                        memcpy(res + pos, new_str, new_len); pos += new_len;
                        res[pos++] = s[i];
                    }
                    memcpy(res + pos, new_str, new_len); pos += new_len;
                    res[pos] = '\0';
                    Value rval = val_str(res, pos);
                    free(s); free(old); free(new_str); free(res);
                    return rval;
                }

                size_t capacity = strlen(s) + 1;
                char* res = malloc(capacity);
                res[0] = '\0';
                size_t rlen = 0;
                
                char* curr = s;
                char* next;
                while ((next = strstr(curr, old)) != NULL) {
                    size_t diff = next - curr;
                    if (rlen + diff + new_len + 1 >= capacity) {
                        capacity = (capacity + diff + new_len) * 2;
                        res = realloc(res, capacity);
                    }
                    memcpy(res + rlen, curr, diff);
                    rlen += diff;
                    memcpy(res + rlen, new_str, new_len);
                    rlen += new_len;
                    curr = next + old_len;
                }
                size_t tail_len = strlen(curr);
                if (rlen + tail_len + 1 >= capacity) {
                    res = realloc(res, rlen + tail_len + 1);
                }
                strcpy(res + rlen, curr);
                Value rval = val_str(res, strlen(res));
                free(s); free(old); free(new_str); free(res);
                return rval;
            }
        case 18: // substr
            {
                char* s = value_to_string(a[0]);
                int64_t i = a[1].as.i;
                int64_t n = a[2].as.i;
                int64_t len = strlen(s);
                if (i < 0) i = 0;
                if (i > len) i = len;
                int64_t end = i + n;
                if (n < 0 || end > len) end = len;
                Value res = val_str(s + i, end - i);
                free(s);
                return res;
            }
        case 19: // split
            {
                char* s = value_to_string(a[0]);
                char* sep = value_to_string(a[1]);
                RcArray* arr = split_str(s, sep);
                free(s); free(sep);
                return val_array(arr);
            }
        case 20: // join
            {
                if (a[0].kind != VAL_ARRAY) {
                    fatal("join() applied to a non-array value");
                }
                char* sep = value_to_string(a[1]);
                RcString* rstr = join_arr(a[0].as.arr, sep);
                free(sep);
                return val_str_rc(rstr);
            }
        case 21: // input
            {
                char* p = value_to_string(a[0]);
                printf("%s", p);
                free(p);
                fflush(stdout);
                char buf[4096];
                if (fgets(buf, sizeof(buf), stdin)) {
                    size_t len = strlen(buf);
                    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
                        buf[len - 1] = '\0';
                        len--;
                    }
                    return val_str(buf, len);
                }
                return val_str("", 0);
            }
        case 22: // json_encode
            {
                char* js = value_to_json(a[0]);
                Value res = val_str(js, strlen(js));
                free(js);
                return res;
            }
        case 23: // json_decode
            {
                char* s = value_to_string(a[0]);
                const char* p = s;
                Value v = parse_json_value(&p);
                free(s);
                return v;
            }
        case 24: // http_get
            {
                { char* u = value_to_string(a[0]); cap_net(u, "http_get()"); free(u); }
                char* url = value_to_string(a[0]);
                char* res = http_get_curl(url);
                Value rval = val_str(res, strlen(res));
                free(url); free(res);
                return rval;
            }
        case 25: // http_post
            {
                { char* u = value_to_string(a[0]); cap_net(u, "http_post()"); free(u); }
                char* url = value_to_string(a[0]);
                char* body = value_to_string(a[1]);
                char* res = http_post_curl(url, body);
                Value rval = val_str(res, strlen(res));
                free(url); free(body); free(res);
                return rval;
            }
        case 26: // sleep
            {
                int64_t ms = (a[0].kind == VAL_FLOAT) ? (int64_t)a[0].as.f : a[0].as.i;
                if (ms > 0) sleep_ms(ms);
                return val_null();
            }
        case 27: // write_bytes(path, int[]) -> bool: writes bytes to a file
            {
                { char* wp = value_to_string(a[0]);
                  cap_fs(false, wp, "write_bytes()"); free(wp); }
                if (a[1].kind != VAL_ARRAY) return val_bool(false);
                char* path = value_to_string(a[0]);
                FILE* fp = fopen(path, "wb");
                free(path);
                if (!fp) return val_bool(false);
                RcArray* arr = a[1].as.arr;
                for (int64_t i = 0; i < arr->length; i++) {
                    Value e = arr->data[i];
                    int byte = (int)((e.kind == VAL_FLOAT ? (int64_t)e.as.f : e.as.i) & 0xFF);
                    fputc(byte, fp);
                }
                fclose(fp);
                return val_bool(true);
            }
        case 28: // read_file(path) -> string ("" on error)
            {
                { char* rp = value_to_string(a[0]);
                  cap_fs(true, rp, "read_file()"); free(rp); }
                char* path = value_to_string(a[0]);
                FILE* fp = fopen(path, "rb");
                free(path);
                if (!fp) return val_str("", 0);
                fseek(fp, 0, SEEK_END);
                long n = ftell(fp);
                fseek(fp, 0, SEEK_SET);
                if (n < 0) { fclose(fp); return val_str("", 0); }
                char* buf = (char*)malloc((size_t)n + 1);
                if (!buf) { fclose(fp); return val_str("", 0); }
                size_t got = fread(buf, 1, (size_t)n, fp);
                fclose(fp);
                buf[got] = '\0';
                Value v = val_str(buf, (int64_t)got);
                free(buf);
                return v;
            }
        case 29: // args() -> string[]: program args after the .pyro path
            {
                RcArray* arr = rc_array_new();
                for (int i = 0; i < pyro_argc; i++) {
                    Value s = val_str(pyro_argv[i], (int64_t)strlen(pyro_argv[i]));
                    rc_array_push(arr, s);
                    release_value(s);
                }
                return val_array(arr);
            }
        case 30: // http_serve(port, dir) -> serve a static directory (blocking)
            {
                // Binding a port is a net capability; the directory served
                  // is also a read capability, so both are required.
                  { char* sd = value_to_string(a[1]);
                    cap_net("listen", "http_serve()");
                    cap_fs(true, sd, "http_serve()"); free(sd); }
                char* dir = value_to_string(a[1]);
                int64_t port = (a[0].kind == VAL_FLOAT) ? (int64_t)a[0].as.f : a[0].as.i;
                printf("[pyro] serving %s on http://localhost:%lld\n", dir, (long long)port);
                fflush(stdout);
                http_serve_dir(dir, (int)port);   // only returns on fatal error
                free(dir);
                return val_null();
            }
        case 31: // clamp(x, lo, hi)
            if (a[0].kind == VAL_INT && a[1].kind == VAL_INT && a[2].kind == VAL_INT) {
                int64_t x = a[0].as.i, lo = a[1].as.i, hi = a[2].as.i;
                if (x < lo) return val_int(lo);
                if (x > hi) return val_int(hi);
                return val_int(x);
            } else {
                double x = value_as_float(a[0]), lo = value_as_float(a[1]), hi = value_as_float(a[2]);
                if (x < lo) return val_float(lo);
                if (x > hi) return val_float(hi);
                return val_float(x);
            }
        case 32: // sign(x) -> int (-1, 0, 1)
            {
                double f = value_as_float(a[0]);
                return val_int(f < 0 ? -1 : (f > 0 ? 1 : 0));
            }
        case 33: // gcd(a, b) -> int
            {
                int64_t gx = (a[0].kind == VAL_INT) ? a[0].as.i : (int64_t)value_as_float(a[0]);
                int64_t gy = (a[1].kind == VAL_INT) ? a[1].as.i : (int64_t)value_as_float(a[1]);
                if (gx < 0) gx = -gx;
                if (gy < 0) gy = -gy;
                while (gy != 0) { int64_t t = gx % gy; gx = gy; gy = t; }
                return val_int(gx);
            }
        case 34: // hypot(a, b) -> number
            return val_float(hypot(value_as_float(a[0]), value_as_float(a[1])));
        case 35: // starts_with(s, prefix) -> bool
            {
                char* s = value_to_string(a[0]);
                char* p = value_to_string(a[1]);
                size_t pl = strlen(p);
                bool r = strlen(s) >= pl && strncmp(s, p, pl) == 0;
                free(s); free(p);
                return val_bool(r);
            }
        case 36: // ends_with(s, suffix) -> bool
            {
                char* s = value_to_string(a[0]);
                char* p = value_to_string(a[1]);
                size_t sl = strlen(s), pl = strlen(p);
                bool r = sl >= pl && strcmp(s + sl - pl, p) == 0;
                free(s); free(p);
                return val_bool(r);
            }
        case 37: // repeat(s, n) -> string  (n<0 treated as 0)
            {
                char* s = value_to_string(a[0]);
                int64_t n = (a[1].kind == VAL_INT) ? a[1].as.i : (int64_t)value_as_float(a[1]);
                if (n < 0) n = 0;
                size_t sl = strlen(s);
                char* buf = (char*)malloc(sl * (size_t)n + 1);
                if (!buf) { free(s); return val_str("", 0); }
                for (int64_t i = 0; i < n; i++) memcpy(buf + i * sl, s, sl);
                buf[sl * (size_t)n] = '\0';
                Value res = val_str(buf, (int64_t)(sl * (size_t)n));
                free(buf); free(s);
                return res;
            }
        case 38: // sort(arr) -> new array sorted ascending (stable)
            {
                if (a[0].kind != VAL_ARRAY) fatal("sort() expects an array");
                RcArray* src = a[0].as.arr;
                RcArray* out = rc_array_new();
                for (int64_t i = 0; i < src->length; i++) rc_array_push(out, src->data[i]);
                // stable insertion sort (matches Go's SliceStable ordering)
                for (int64_t i = 1; i < out->length; i++) {
                    Value key = out->data[i];
                    int64_t j = i - 1;
                    while (j >= 0 && native_less(key, out->data[j])) {
                        out->data[j + 1] = out->data[j];
                        j--;
                    }
                    out->data[j + 1] = key;
                }
                return val_array(out);
            }
        case 39: // reverse(arr) -> new reversed array
            {
                if (a[0].kind != VAL_ARRAY) fatal("reverse() expects an array");
                RcArray* src = a[0].as.arr;
                RcArray* out = rc_array_new();
                for (int64_t i = src->length - 1; i >= 0; i--) rc_array_push(out, src->data[i]);
                return val_array(out);
            }
        case 40: // slice(x, start, end) -> subarray/substring [start, end), safe bounds
            {
                // Polymorphic over array|string so `xs[a..b]` and `s[a..b]` can
                // lower to the SAME call — the parser cannot know the operand's
                // type (10.9). Must mirror main.go case 40 exactly.
                if (a[0].kind == VAL_STR) {
                    char* s = value_to_string(a[0]);
                    int64_t n = (int64_t)strlen(s);
                    int64_t st = (a[1].kind == VAL_INT) ? a[1].as.i : (int64_t)value_as_float(a[1]);
                    int64_t en = (a[2].kind == VAL_INT) ? a[2].as.i : (int64_t)value_as_float(a[2]);
                    if (st < 0) st = 0;
                    if (en > n) en = n;
                    if (st > en) st = en;
                    Value res = val_str(s + st, en - st);
                    free(s);
                    return res;
                }
                if (a[0].kind != VAL_ARRAY) fatal("slice() expects an array or a string");
                RcArray* src = a[0].as.arr;
                int64_t n = src->length;
                int64_t start = (a[1].kind == VAL_INT) ? a[1].as.i : (int64_t)value_as_float(a[1]);
                int64_t end   = (a[2].kind == VAL_INT) ? a[2].as.i : (int64_t)value_as_float(a[2]);
                if (start < 0) start = 0;
                if (end > n) end = n;
                if (start > end) start = end;
                RcArray* out = rc_array_new();
                for (int64_t i = start; i < end; i++) rc_array_push(out, src->data[i]);
                return val_array(out);
            }
        case 41: // index_of(arr, x) -> first index by value equality, else -1
            {
                if (a[0].kind != VAL_ARRAY) fatal("index_of() expects an array");
                RcArray* src = a[0].as.arr;
                for (int64_t i = 0; i < src->length; i++) {
                    if (value_eq(src->data[i], a[1])) return val_int(i);
                }
                return val_int(-1);
            }
        case 42: // pad_start(s, width, pad) -> string
        case 43: // pad_end(s, width, pad) -> string
            {
                char* s = value_to_string(a[0]);
                char* pad = value_to_string(a[2]);
                int width = (a[1].kind == VAL_INT) ? (int)a[1].as.i : (int)value_as_float(a[1]);
                char* padded = native_pad(s, width, pad, id == 42);
                Value res = val_str(padded, (int64_t)strlen(padded));
                free(s); free(pad); free(padded);
                return res;
            }
        case 44: // concat(a, b) -> new array (elements of a then b)
            {
                if (a[0].kind != VAL_ARRAY || a[1].kind != VAL_ARRAY)
                    fatal("concat() expects two arrays");
                RcArray* out = rc_array_new();
                for (int64_t i = 0; i < a[0].as.arr->length; i++) rc_array_push(out, a[0].as.arr->data[i]);
                for (int64_t i = 0; i < a[1].as.arr->length; i++) rc_array_push(out, a[1].as.arr->data[i]);
                return val_array(out);
            }
        case 45: // count(arr, x) -> number of elements equal to x
            {
                if (a[0].kind != VAL_ARRAY) fatal("count() expects an array");
                RcArray* src = a[0].as.arr;
                int64_t n = 0;
                for (int64_t i = 0; i < src->length; i++)
                    if (value_eq(src->data[i], a[1])) n++;
                return val_int(n);
            }
        case 46: // sum(arr) -> int if all int, else number
            {
                if (a[0].kind != VAL_ARRAY) fatal("sum() expects an array");
                RcArray* src = a[0].as.arr;
                bool all_int = true;
                for (int64_t i = 0; i < src->length; i++)
                    if (src->data[i].kind == VAL_FLOAT) all_int = false;
                if (all_int) {
                    int64_t t = 0;
                    for (int64_t i = 0; i < src->length; i++) t += src->data[i].as.i;
                    return val_int(t);
                }
                double t = 0;
                for (int64_t i = 0; i < src->length; i++) t += value_as_float(src->data[i]);
                return val_float(t);
            }
        case 47: // now_ms() -> int
            return val_int(pyro_get_now_ms());
        case 48: // monotonic_ms() -> int
            return val_int(pyro_get_mono_ms());
        case 49: // random() -> number [0.0, 1.0)
            {
                double r = (double)(splitmix64_next() >> 11) / 9007199254740992.0;
                return val_float(r);
            }
        case 50: // random_int(lo, hi) -> int inclusive
            {
                int64_t lo = (a[0].kind == VAL_INT) ? a[0].as.i : (int64_t)value_as_float(a[0]);
                int64_t hi = (a[1].kind == VAL_INT) ? a[1].as.i : (int64_t)value_as_float(a[1]);
                if (hi < lo) { int64_t tmp = lo; lo = hi; hi = tmp; }
                uint64_t span = (uint64_t)(hi - lo + 1);
                int64_t res = lo + (int64_t)(splitmix64_next() % span);
                return val_int(res);
            }
        case 51: // seed(n) -> void/null
            {
                int64_t n = (a[0].kind == VAL_INT) ? a[0].as.i : (int64_t)value_as_float(a[0]);
                g_c_prng_state = (uint64_t)n;
                return val_null();
            }
        // ── HTTP server, roadmap 11.6 ──
        case 52: // http_listen(port) -> bool
            {
                cap_net("listen", "http_listen()");
#ifdef _WIN32
                WSADATA wsa;
                if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return val_bool(false);
#endif
                if (g_http_srv != PYRO_BADSOCK) pyro_closesock(g_http_srv);
                int64_t port = (a[0].kind == VAL_INT) ? a[0].as.i : (int64_t)value_as_float(a[0]);
                pyro_sock srv = socket(AF_INET, SOCK_STREAM, 0);
                if (srv == PYRO_BADSOCK) return val_bool(false);
                int yes = 1;
                setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
                struct sockaddr_in addr;
                memset(&addr, 0, sizeof(addr));
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = htonl(INADDR_ANY);
                addr.sin_port = htons((unsigned short)port);
                if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
                    pyro_closesock(srv); return val_bool(false);
                }
                if (listen(srv, 16) != 0) { pyro_closesock(srv); return val_bool(false); }
                g_http_srv = srv;
                return val_bool(true);
            }
        case 53: // http_accept() -> map{method,path,query,body} or null
            {
                if (g_http_srv == PYRO_BADSOCK)
                    fatal("http_accept() called before http_listen()");
                if (g_http_conn != PYRO_BADSOCK) {
                    // previous request never answered: close rather than leak
                    pyro_closesock(g_http_conn);
                    g_http_conn = PYRO_BADSOCK;
                }
                // Always returns a MAP, never null — mirrors main.go. `req == null`
                // cannot be written reliably today (equality compares containers
                // by their zero int), so a failed or malformed accept is signalled
                // by an empty method instead.
                char method[16], path[2048], query[2048];
                char* body = NULL;
                method[0] = '\0'; path[0] = '\0'; query[0] = '\0';
                pyro_sock c = accept(g_http_srv, NULL, NULL);
                char* hdrs = NULL;
                if (c != PYRO_BADSOCK &&
                    !http_read_request(c, method, sizeof(method), path, sizeof(path),
                                       query, sizeof(query), &body, &hdrs)) {
                    pyro_closesock(c);
                    c = PYRO_BADSOCK;
                    method[0] = '\0'; path[0] = '\0'; query[0] = '\0';
                    if (body) { free(body); body = NULL; }
                    if (hdrs) { free(hdrs); hdrs = NULL; }
                }
                g_http_conn = c;
                RcMap* m = rc_map_new();
                rc_map_set(m, val_str("method", 6), val_str(method, (int64_t)strlen(method)));
                rc_map_set(m, val_str("path",   4), val_str(path,   (int64_t)strlen(path)));
                rc_map_set(m, val_str("query",  5), val_str(query,  (int64_t)strlen(query)));
                rc_map_set(m, val_str("body",   4), val_str(body ? body : "",
                                                            body ? (int64_t)strlen(body) : 0));
                if (body) free(body);
                // Headers share the flat map under a `header:` prefix — same
                // keys the Go VM produces.
                if (hdrs) {
                    char* line = hdrs;
                    while (line && *line) {
                        char* nl = strchr(line, '\n');
                        if (nl) *nl = '\0';
                        char* colon = strchr(line, ':');
                        if (colon) {
                            *colon = '\0';
                            char key[160];
                            snprintf(key, sizeof(key), "header:%s", line);
                            rc_map_set(m, val_str(key, (int64_t)strlen(key)),
                                       val_str(colon + 1, (int64_t)strlen(colon + 1)));
                        }
                        line = nl ? nl + 1 : NULL;
                    }
                    free(hdrs);
                }
                return val_map(m);
            }
        case 54: // http_respond(status, content_type, body) -> bool
            {
                if (g_http_conn == PYRO_BADSOCK) return val_bool(false);
                int64_t status = (a[0].kind == VAL_INT) ? a[0].as.i : (int64_t)value_as_float(a[0]);
                char* ctype = value_to_string(a[1]);
                char* payload = value_to_string(a[2]);
                size_t plen = strlen(payload);
                size_t need = plen + strlen(ctype) + 256;
                char* resp = (char*)malloc(need);
                int ok = 0;
                if (resp) {
                    int n = snprintf(resp, need,
                        "HTTP/1.1 %lld %s\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
                        "Connection: close\r\n\r\n%s",
                        (long long)status, http_status_text(status), ctype,
                        (long long)plen, payload);
                    ok = (send(g_http_conn, resp, n, 0) == n);
                    free(resp);
                }
                free(ctype); free(payload);
                pyro_closesock(g_http_conn);
                g_http_conn = PYRO_BADSOCK;
                return val_bool(ok);
            }
        // ── filesystem & process, roadmap 11.7 ──
        // Pure queries are ungated; anything that mutates the machine or reads
        // its environment is sandbox-gated, matching read_file/write_bytes.
        case 55: // file_exists(path) -> bool
            {
                char* p = value_to_string(a[0]);
                struct stat st;
                bool ok = (stat(p, &st) == 0);
                free(p);
                return val_bool(ok);
            }
        case 56: // is_dir(path) -> bool
            {
                char* p = value_to_string(a[0]);
                bool ok = pyro_is_dir(p) != 0;
                free(p);
                return val_bool(ok);
            }
        case 57: // list_dir(path) -> string[] (names only, sorted; empty on error)
            {
                char* p = value_to_string(a[0]);
                RcArray* out = rc_array_new();
                DIR* d = opendir(p);
                free(p);
                if (!d) return val_array(out);
                char** names = NULL;
                int n = 0, cap = 0;
                struct dirent* e;
                while ((e = readdir(d)) != NULL) {
                    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                    if (n == cap) {
                        cap = cap ? cap * 2 : 16;
                        char** grown = (char**)realloc(names, (size_t)cap * sizeof(char*));
                        if (!grown) break;
                        names = grown;
                    }
                    names[n++] = pyro_strdup(e->d_name);
                }
                closedir(d);
                if (names) {
                    qsort(names, (size_t)n, sizeof(char*), pyro_name_cmp);
                    for (int i = 0; i < n; i++) {
                        rc_array_push(out, val_str(names[i], (int64_t)strlen(names[i])));
                        free(names[i]);
                    }
                    free(names);
                }
                return val_array(out);
            }
        case 58: // make_dir(path) -> bool (creates parents)
            {
                char* p = value_to_string(a[0]);
                cap_fs(false, p, "make_dir()");
                bool ok = pyro_mkdir_all(p) != 0;
                free(p);
                return val_bool(ok);
            }
        case 59: // delete_file(path) -> bool  (deliberately NOT recursive)
            {
                // FILES ONLY — see the note in main.go case 59. POSIX remove()
                // would delete an empty directory; MSVCRT's would not.
                char* p = value_to_string(a[0]);
                cap_fs(false, p, "delete_file()");
                bool ok = false;
                if (!pyro_is_dir(p)) ok = (remove(p) == 0);
                free(p);
                return val_bool(ok);
            }
        case 60: // file_size(path) -> int (-1 when it cannot be read)
            {
                char* p = value_to_string(a[0]);
                struct stat st;
                int64_t sz = (stat(p, &st) == 0) ? (int64_t)st.st_size : -1;
                free(p);
                return val_int(sz);
            }
        case 61: // write_file(path, content) -> bool
            {
                char* p = value_to_string(a[0]);
                cap_fs(false, p, "write_file()");
                char* c = value_to_string(a[1]);
                FILE* f = fopen(p, "wb");
                bool ok = false;
                if (f) {
                    size_t len = strlen(c);
                    ok = (fwrite(c, 1, len, f) == len);
                    fclose(f);
                }
                free(p); free(c);
                return val_bool(ok);
            }
        case 62: // env(name) -> string ("" when unset)
            {
                char* n = value_to_string(a[0]);
                cap_env(n, "env()");
                const char* v = getenv(n);
                free(n);
                return val_str(v ? v : "", v ? (int64_t)strlen(v) : 0);
            }
        case 63: // exec(cmd) -> string (stdout; "" on failure)
            {
                char* cmd = value_to_string(a[0]);
                cap_exec(cmd, "exec()");
#ifdef _WIN32
                FILE* pipe = _popen(cmd, "r");
#else
                FILE* pipe = popen(cmd, "r");
#endif
                free(cmd);
                if (!pipe) return val_str("", 0);
                char* buf = NULL;
                size_t len = 0, cap = 0;
                char chunk[4096];
                size_t got;
                while ((got = fread(chunk, 1, sizeof(chunk), pipe)) > 0) {
                    if (len + got + 1 > cap) {
                        size_t want = (len + got + 1) * 2;
                        char* grown = (char*)realloc(buf, want);
                        if (!grown) break;
                        buf = grown; cap = want;
                    }
                    memcpy(buf + len, chunk, got);
                    len += got;
                }
#ifdef _WIN32
                _pclose(pipe);
#else
                pclose(pipe);
#endif
                if (!buf) return val_str("", 0);
                buf[len] = '\0';
                Value res = val_str(buf, (int64_t)len);
                free(buf);
                return res;
            }
        // ── persistence, roadmap 11.8 ──
        case 64: // write_file_atomic(path, content) -> bool
            {
                // Sibling temp file + flush + rename. See the note in main.go:
                // a reader sees the old file or the new one, never the
                // truncated middle a plain write leaves after a crash.
                char* path = value_to_string(a[0]);
                cap_fs(false, path, "write_file_atomic()");
                char* data = value_to_string(a[1]);
                size_t plen = strlen(path);
                char* tmp = (char*)malloc(plen + 5);
                bool ok = false;
                if (tmp) {
                    memcpy(tmp, path, plen);
                    memcpy(tmp + plen, ".tmp", 5);
                    FILE* f = fopen(tmp, "wb");
                    if (f) {
                        size_t dlen = strlen(data);
                        ok = (fwrite(data, 1, dlen, f) == dlen);
                        if (ok) ok = (fflush(f) == 0);
                        fclose(f);
                        if (ok) {
#ifdef _WIN32
                            // rename() fails on Windows when the target exists;
                            // MoveFileEx replaces it in one step.
                            ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
                            ok = (rename(tmp, path) == 0);
#endif
                        }
                        if (!ok) remove(tmp);
                    }
                    free(tmp);
                }
                free(path); free(data);
                return val_bool(ok);
            }
        case 65: // url_decode(s) -> string ('+' is a space; bad escapes pass through)
            {
                char* in = value_to_string(a[0]);
                size_t n = strlen(in);
                char* out = (char*)malloc(n + 1);
                size_t j = 0;
                for (size_t i = 0; i < n; i++) {
                    if (in[i] == '+') {
                        out[j++] = ' ';
                    } else if (in[i] == '%' && i + 2 < n) {
                        int hi = pyro_unhex(in[i + 1]), lo = pyro_unhex(in[i + 2]);
                        if (hi >= 0 && lo >= 0) { out[j++] = (char)(hi * 16 + lo); i += 2; }
                        else                    { out[j++] = in[i]; }
                    } else {
                        out[j++] = in[i];
                    }
                }
                out[j] = '\0';
                Value res = val_str(out, (int64_t)j);
                free(in); free(out);
                return res;
            }
        case 66: // url_encode(s) -> string
            {
                static const char* HEXD = "0123456789ABCDEF";
                char* in = value_to_string(a[0]);
                size_t n = strlen(in);
                char* out = (char*)malloc(n * 3 + 1);
                size_t j = 0;
                for (size_t i = 0; i < n; i++) {
                    unsigned char ch = (unsigned char)in[i];
                    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') ||
                        ch == '-' || ch == '_' || ch == '.' || ch == '~') {
                        out[j++] = (char)ch;
                    } else {
                        out[j++] = '%';
                        out[j++] = HEXD[ch >> 4];
                        out[j++] = HEXD[ch & 0x0F];
                    }
                }
                out[j] = '\0';
                Value res = val_str(out, (int64_t)j);
                free(in); free(out);
                return res;
            }
        // ── embedded assets, roadmap 11.9 ──
        case 67: // asset(name) -> string ("" when absent)
            {
                char* want = value_to_string(a[0]);
                Value res = val_str("", 0);
                for (int i = 0; i < g_nassets; i++) {
                    if (strcmp(g_assets[i].name, want) == 0) {
                        res = val_str(g_assets[i].data, g_assets[i].len);
                        break;
                    }
                }
                free(want);
                return res;
            }
        case 68: // asset_names() -> string[] (sorted, as in the Go VM)
            {
                RcArray* out = rc_array_new();
                for (int i = 0; i < g_nassets; i++) {
                    rc_array_push(out, val_str(g_assets[i].name,
                                               (int64_t)strlen(g_assets[i].name)));
                }
                return val_array(out);
            }
    }
    fatal("unknown native builtin");
    return val_null();
}

RcArray* split_str(const char* s, const char* sep) {
    RcArray* arr = rc_array_new();
    int64_t slen = strlen(s);
    int64_t seplen = strlen(sep);
    if (seplen == 0) {
        for (int64_t i = 0; i < slen; i++) {
            Value ch = val_str(s + i, 1);
            rc_array_push(arr, ch);
            release_value(ch);
        }
        return arr;
    }
    const char* curr = s;
    const char* next;
    while ((next = strstr(curr, sep)) != NULL) {
        Value part = val_str(curr, next - curr);
        rc_array_push(arr, part);
        release_value(part);
        curr = next + seplen;
    }
    Value part = val_str(curr, strlen(curr));
    rc_array_push(arr, part);
    release_value(part);
    return arr;
}

RcString* join_arr(RcArray* arr, const char* sep) {
    size_t capacity = 1004;
    size_t length = 0;
    char* result = malloc(capacity);
    result[0] = '\0';
    size_t seplen = strlen(sep);
    for (int64_t i = 0; i < arr->length; i++) {
        if (i > 0) {
            if (length + seplen + 2 >= capacity) {
                capacity = (capacity + seplen) * 2;
                result = realloc(result, capacity);
            }
            strcat(result, sep);
            length += seplen;
        }
        char* estr = value_to_string(arr->data[i]);
        size_t elen = strlen(estr);
        if (length + elen + 2 >= capacity) {
            capacity = (capacity + elen) * 2;
            result = realloc(result, capacity);
        }
        strcat(result, estr);
        length += elen;
        free(estr);
    }
    RcString* rstr = new_rc_string(result, length);
    free(result);
    return rstr;
}

// ── Binary Operations Implementation ──────────────────────────
Value bin_op(uint8_t op, Value a, Value b) {
    if (op == opADD && (a.kind == VAL_STR || b.kind == VAL_STR)) {
        return str_concat(a, b);
    }
    if (op == opEQ || op == opNE) {
        bool eq = value_eq(a, b);
        if (op == opNE) eq = !eq;
        return val_bool(eq);
    }
    if (a.kind == VAL_STR || b.kind == VAL_STR) {
        char* sa = value_to_string(a);
        char* sb = value_to_string(b);
        int cmp = strcmp(sa, sb);
        free(sa); free(sb);
        switch (op) {
            case opLT: return val_bool(cmp < 0);
            case opGT: return val_bool(cmp > 0);
            case opLE: return val_bool(cmp <= 0);
            case opGE: return val_bool(cmp >= 0);
        }
    }
    if (a.kind == VAL_FLOAT || b.kind == VAL_FLOAT) {
        double x = value_as_float(a);
        double y = value_as_float(b);
        switch (op) {
            case opADD: return val_float(x + y);
            case opSUB: return val_float(x - y);
            case opMUL: return val_float(x * y);
            case opDIV: return val_float(x / y);
            case opMOD: return val_float(fmod(x, y));
            case opLT:  return val_bool(x < y);
            case opGT:  return val_bool(x > y);
            case opLE:  return val_bool(x <= y);
            case opGE:  return val_bool(x >= y);
        }
    }
    int64_t x = a.as.i;
    int64_t y = b.as.i;
    switch (op) {
        case opADD: return val_int(x + y);
        case opSUB: return val_int(x - y);
        case opMUL: return val_int(x * y);
        case opDIV:
            if (y == 0) {
                fatal("[Cryo Security] DivByZero: integer division");
            }
            if (x == INT64_MIN && y == -1) {
                fatal("[Cryo Security] Overflow: INT64_MIN / -1");
            }
            return val_int(x / y);
        case opMOD:
            if (y == 0) {
                fatal("[Cryo Security] DivByZero: modulo");
            }
            if (x == INT64_MIN && y == -1) {
                return val_int(0);
            }
            return val_int(x % y);
        case opBAND: return val_int(x & y);
        case opBOR:  return val_int(x | y);
        case opBXOR: return val_int(x ^ y);
        case opSHL:  return val_int(x << y);
        case opSHR:  return val_int(x >> y);
        case opLT:   return val_bool(x < y);
        case opGT:   return val_bool(x > y);
        case opLE:   return val_bool(x <= y);
        case opGE:   return val_bool(x >= y);
    }
    fatal("invalid opcode in bytecode");
    return val_null();
}

Value str_concat(Value a, Value b) {
    char* sa = value_to_string(a);
    char* sb = value_to_string(b);
    int64_t len_a = strlen(sa);
    int64_t len_b = strlen(sb);
    RcString* s = new_rc_string(NULL, len_a + len_b);
    memcpy(s->chars, sa, len_a);
    memcpy(s->chars + len_a, sb, len_b);
    free(sa); free(sb);
    Value v = { .kind = VAL_STR, .as.str = s };
    return v;
}


// VM keys(): array with map keys, sorted by their textual representation
// (deterministic, Go VM parity). Encapsulates MapPair/compare_map_pairs.
RcArray* rc_map_keys_sorted(RcMap* m) {
    RcArray* keys_arr = rc_array_new();
    int64_t count = 0;
    MapPair* pairs = malloc(sizeof(MapPair) * m->size);
    for (int64_t i = 0; i < m->capacity; i++) {
        MapNode* curr = m->buckets[i];
        while (curr) {
            pairs[count].key = curr->key;
            pairs[count].val = curr->val;
            pairs[count].key_str = value_to_string(curr->key);
            count++;
            curr = curr->next;
        }
    }
    qsort(pairs, count, sizeof(MapPair), compare_map_pairs);
    for (int64_t i = 0; i < count; i++) {
        rc_array_push(keys_arr, pairs[i].key);
        free(pairs[i].key_str);
    }
    free(pairs);
    return keys_arr;
}
