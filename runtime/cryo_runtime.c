/* ============================================================
   Cryo Language — C Runtime Implementation  (v0.3)
   ============================================================ */
#include "cryo_runtime.h"
#include <ctype.h>

/* Excecao global */
CryoException _cryo_exc = {.active = false};

/* ---------- Seguranca: aritmetica verificada ---------- */

static void _cryo_fatal(const char* kind, const char* detail) {
    fprintf(stderr, "[Cryo Seguranca] %s: %s\n", kind, detail);
    abort();   /* aborta com core/backtrace em vez de continuar corrompido */
}

int64_t cryo_add_ovf(int64_t a, int64_t b) {
    int64_t r;
    if (__builtin_add_overflow(a, b, &r))
        _cryo_fatal("Overflow", "estouro em adicao de inteiros");
    return r;
}

int64_t cryo_sub_ovf(int64_t a, int64_t b) {
    int64_t r;
    if (__builtin_sub_overflow(a, b, &r))
        _cryo_fatal("Overflow", "estouro em subtracao de inteiros");
    return r;
}

int64_t cryo_mul_ovf(int64_t a, int64_t b) {
    int64_t r;
    if (__builtin_mul_overflow(a, b, &r))
        _cryo_fatal("Overflow", "estouro em multiplicacao de inteiros");
    return r;
}

int64_t cryo_idiv_chk(int64_t a, int64_t b) {
    if (b == 0)
        _cryo_fatal("DivisaoPorZero", "divisao inteira por zero");
    if (a == INT64_MIN && b == -1)
        _cryo_fatal("Overflow", "INT64_MIN / -1 estoura");
    return a / b;
}

int64_t cryo_imod_chk(int64_t a, int64_t b) {
    if (b == 0)
        _cryo_fatal("DivisaoPorZero", "modulo por zero");
    if (a == INT64_MIN && b == -1)
        return 0;
    return a % b;
}

/* ---------- Seguranca: assert ---------- */

void cryo_assert(bool cond, const char* msg) {
    if (!cond) {
        fprintf(stderr, "[Cryo Assert] %s\n", msg ? msg : "condicao falsa");
        abort();
    }
}

/* ---------- Seguranca: guarda de ponteiro nulo ---------- */

void* cryo_check_null(void* p, const char* what) {
    if (!p) {
        fprintf(stderr, "[Cryo Seguranca] NullPointer: acesso a '%s' nulo\n",
                what ? what : "?");
        abort();
    }
    return p;
}

/* ---------- CryoArray ---------- */

CryoArray* cryo_array_new(void) {
    CryoArray* a = malloc(sizeof(CryoArray));
    if (!a) { fprintf(stderr, "[Cryo] malloc falhou\n"); exit(1); }
    a->capacity = 8;
    a->length   = 0;
    a->data     = malloc(a->capacity * sizeof(uint64_t));
    return a;
}

void cryo_array_push(CryoArray* a, uint64_t v) {
    if (a->length >= a->capacity) {
        a->capacity *= 2;
        a->data = realloc(a->data, a->capacity * sizeof(uint64_t));
        if (!a->data) { fprintf(stderr, "[Cryo] realloc falhou\n"); exit(1); }
    }
    a->data[a->length++] = v;
}

uint64_t cryo_array_get(CryoArray* a, int64_t i) {
    if (i < 0 || i >= a->length) {
        fprintf(stderr, "[Cryo] IndexError: indice %ld fora dos limites (length=%ld)\n", i, a->length);
        exit(1);
    }
    return a->data[i];
}

void cryo_array_set(CryoArray* a, int64_t i, uint64_t v) {
    if (i < 0 || i >= a->length) {
        fprintf(stderr, "[Cryo] IndexError: indice %ld fora dos limites\n", i);
        exit(1);
    }
    a->data[i] = v;
}

CryoArray* cryo_array_slice(CryoArray* a, int64_t start, int64_t end) {
    if (start < 0) start = 0;
    if (end > a->length) end = a->length;
    CryoArray* out = cryo_array_new();
    for (int64_t i = start; i < end; i++)
        cryo_array_push(out, a->data[i]);
    return out;
}

void cryo_array_free(CryoArray* a) {
    if (a) { free(a->data); free(a); }
}

/* ---------- Strings ---------- */

char* cryo_str_concat(const char* a, const char* b) {
    if (!a) a = ""; if (!b) b = "";
    size_t len = strlen(a) + strlen(b) + 1;
    char* r = malloc(len);
    if (!r) { fprintf(stderr, "[Cryo] malloc falhou\n"); exit(1); }
    strcpy(r, a); strcat(r, b);
    return r;
}

char* cryo_i64_to_str(int64_t n) {
    char* buf = malloc(32);
    snprintf(buf, 32, "%ld", n);
    return buf;
}

char* cryo_f64_to_str(double n) {
    char* buf = malloc(64);
    /* Remove zeros desnecessarios */
    snprintf(buf, 64, "%g", n);
    return buf;
}

char* cryo_bool_to_str(bool b) {
    /* Retorna string estatica — nao liberar */
    return b ? "true" : "false";
}

int64_t cryo_str_len(const char* s) {
    return s ? (int64_t)strlen(s) : 0;
}

bool cryo_str_eq(const char* a, const char* b) {
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

char* cryo_str_slice(const char* s, int64_t start, int64_t end) {
    int64_t slen = (int64_t)strlen(s);
    if (start < 0) start = 0;
    if (end > slen) end = slen;
    int64_t out_len = end - start;
    if (out_len < 0) out_len = 0;
    char* out = malloc(out_len + 1);
    strncpy(out, s + start, out_len);
    out[out_len] = '\0';
    return out;
}

char* cryo_str_upper(const char* s) {
    char* out = strdup(s);
    for (char* p = out; *p; p++) *p = (char)toupper((unsigned char)*p);
    return out;
}

char* cryo_str_lower(const char* s) {
    char* out = strdup(s);
    for (char* p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
    return out;
}

/* ---------- Print ---------- */

void cryo_print_str(const char* s)  { puts(s ? s : "(null)"); }
void cryo_print_i64(int64_t n)      { printf("%ld\n", n); }
void cryo_print_f64(double n)       { printf("%g\n", n); }
void cryo_print_bool(bool b)        { puts(b ? "true" : "false"); }
void cryo_print_newline(void)       { putchar('\n'); }

/* ---------- Input ---------- */

char* cryo_input(const char* prompt) {
    if (prompt && *prompt) { printf("%s", prompt); fflush(stdout); }
    char* buf = NULL; size_t cap = 0;
    ssize_t len = getline(&buf, &cap, stdin);
    if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
    return buf;
}

int64_t cryo_input_int(const char* prompt) {
    char* s = cryo_input(prompt);
    int64_t v = strtoll(s, NULL, 10);
    free(s);
    return v;
}

double cryo_input_num(const char* prompt) {
    char* s = cryo_input(prompt);
    double v = strtod(s, NULL);
    free(s);
    return v;
}
