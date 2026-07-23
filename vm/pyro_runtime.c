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

Value val_map(RcMap* map) {
    Value v = { .kind = VAL_MAP };
    v.as.map = map;
    if (map) map->ref_count++;
    return v;
}

// ── Memory Management Implementation ──────────────────────────
void retain_value(Value v) {
    if (v.kind == VAL_STR && v.as.str) {
        v.as.str->ref_count++;
    } else if (v.kind == VAL_ARRAY && v.as.arr) {
        v.as.arr->ref_count++;
    } else if (v.kind == VAL_MAP && v.as.map) {
        v.as.map->ref_count++;
    }
}

void release_value(Value v) {
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
        bool bb = (b.kind == VAL_BOOL) ? b.as.b : (b.kind == VAL_INT ? b.as.i != 0 : false);
        return ba == bb;
    }
    if (a.kind == VAL_NULL && b.kind == VAL_NULL) return true;
    if (a.kind != b.kind) return false;
    if (a.kind == VAL_INT) return a.as.i == b.as.i;
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
        return rc_array_get(cont.as.arr, idx);
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
                
                size_t capacity = strlen(s) + 1;
                char* res = malloc(capacity);
                res[0] = '\0';
                size_t rlen = 0;
                
                char* curr = s;
                char* next;
                if (old_len == 0) {
                    Value rval = val_str(s, strlen(s));
                    free(s); free(old); free(new_str); free(res);
                    return rval;
                }
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
            if (pyro_sandboxed) {
                fatal("[Cryo Security] Sandbox: http_get() blocked by sandbox policy");
            }
            {
                char* url = value_to_string(a[0]);
                char* res = http_get_curl(url);
                Value rval = val_str(res, strlen(res));
                free(url); free(res);
                return rval;
            }
        case 25: // http_post
            if (pyro_sandboxed) {
                fatal("[Cryo Security] Sandbox: http_post() blocked by sandbox policy");
            }
            {
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
                if (pyro_sandboxed) {
                    fatal("[Cryo Security] Sandbox: write_bytes() blocked by sandbox policy");
                }
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
                if (pyro_sandboxed) {
                    fatal("[Cryo Security] Sandbox: read_file() blocked by sandbox policy");
                }
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
                if (pyro_sandboxed) {
                    fatal("[Cryo Security] Sandbox: http_serve() blocked by sandbox policy");
                }
                char* dir = value_to_string(a[1]);
                int64_t port = (a[0].kind == VAL_FLOAT) ? (int64_t)a[0].as.f : a[0].as.i;
                printf("[pyro] serving %s on http://localhost:%lld\n", dir, (long long)port);
                fflush(stdout);
                http_serve_dir(dir, (int)port);   // only returns on fatal error
                free(dir);
                return val_null();
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
            case opSUB: return val_float(x * y);
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
