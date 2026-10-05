#include "calc_engine.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_DEPTH 64

typedef struct {
    const char *s;
    const char *p;
    const calc_ctx_t *ctx;
    const char *err;
    const char *err_at;
    int depth;
} parser_t;

static double parse_or(parser_t *ps);

static void fail(parser_t *ps, const char *msg)
{
    if (ps->err == NULL) {
        ps->err    = msg;
        ps->err_at = ps->p;
    }
}

static void skip_ws(parser_t *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t') ps->p++;
}

/* Match a literal (ASCII or UTF-8) after whitespace and consume it. */
static bool accept(parser_t *ps, const char *tok)
{
    skip_ws(ps);
    size_t n = strlen(tok);
    if (strncmp(ps->p, tok, n) == 0) {
        ps->p += n;
        return true;
    }
    return false;
}

/* Like accept, but the word must not continue as an identifier ("xor"). */
static bool accept_word(parser_t *ps, const char *word)
{
    skip_ws(ps);
    size_t n = strlen(word);
    if (strncmp(ps->p, word, n) == 0 && !isalnum((unsigned char)ps->p[n]) && ps->p[n] != '_') {
        ps->p += n;
        return true;
    }
    return false;
}

static bool to_int(parser_t *ps, double v, int64_t *out)
{
    if (!isfinite(v) || v != floor(v) || fabs(v) > 9.2e18) {
        fail(ps, "bitwise ops need integers");
        return false;
    }
    *out = (int64_t)v;
    return true;
}

/* ---- Primary ------------------------------------------------------------- */

static double parse_number(parser_t *ps)
{
    const char *p = ps->p;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X' || p[1] == 'b' || p[1] == 'B' || p[1] == 'o' || p[1] == 'O')) {
        int base = (p[1] == 'x' || p[1] == 'X') ? 16 : (p[1] == 'b' || p[1] == 'B') ? 2 : 8;
        char *end;
        unsigned long long v = strtoull(p + 2, &end, base);
        if (end == p + 2) {
            fail(ps, "bad number");
            return 0;
        }
        ps->p = end;
        return (double)v;
    }
    char *end;
    double v = strtod(p, &end);
    if (end == p) {
        fail(ps, "bad number");
        return 0;
    }
    /* strtod also eats "inf"/"nan" and hex floats; only allow plain decimals. */
    for (const char *q = p; q < end; q++) {
        if (!(isdigit((unsigned char)*q) || *q == '.' || *q == 'e' || *q == 'E' || *q == '+' || *q == '-')) {
            fail(ps, "bad number");
            return 0;
        }
    }
    ps->p = end;
    return v;
}

typedef double (*fn1_t)(double);

static double fn_log10(double x) { return log10(x); }
static double fn_log2(double x) { return log2(x); }

static const char *const s_fn_names[] = {"sin",  "cos", "tan", "asin", "acos", "atan", "sqrt", "cbrt",
                                         "log",  "ln",  "log2", "exp", "abs", "floor", "ceil", "round"};

static bool is_fn(const char *name)
{
    for (size_t i = 0; i < sizeof(s_fn_names) / sizeof(s_fn_names[0]); i++) {
        if (!strcmp(name, s_fn_names[i])) return true;
    }
    return false;
}

static double call_fn(parser_t *ps, const char *name, double x)
{
    bool deg      = ps->ctx && ps->ctx->degrees;
    double to_rad = deg ? M_PI / 180.0 : 1.0;
    if (!strcmp(name, "sin")) return sin(x * to_rad);
    if (!strcmp(name, "cos")) return cos(x * to_rad);
    if (!strcmp(name, "tan")) return tan(x * to_rad);
    if (!strcmp(name, "asin")) return asin(x) / to_rad;
    if (!strcmp(name, "acos")) return acos(x) / to_rad;
    if (!strcmp(name, "atan")) return atan(x) / to_rad;
    static const struct {
        const char *name;
        fn1_t fn;
    } plain[] = {{"sqrt", sqrt}, {"cbrt", cbrt}, {"log", fn_log10}, {"ln", log},     {"log2", fn_log2},
                 {"exp", exp},   {"abs", fabs},  {"floor", floor},  {"ceil", ceil}, {"round", round}};
    for (size_t i = 0; i < sizeof(plain) / sizeof(plain[0]); i++) {
        if (!strcmp(name, plain[i].name)) return plain[i].fn(x);
    }
    fail(ps, "unknown name");
    return 0;
}

static double parse_unary(parser_t *ps);

static double parse_primary(parser_t *ps)
{
    skip_ws(ps);
    if (accept(ps, "(")) {
        double v = parse_or(ps);
        if (!accept(ps, ")")) fail(ps, "missing )");
        return v;
    }
    if (accept(ps, "\xE2\x88\x9A")) { /* √ */
        return sqrt(parse_unary(ps));
    }
    if (accept(ps, "\xCF\x80")) return M_PI; /* π */

    if (isdigit((unsigned char)*ps->p) || (*ps->p == '.' && isdigit((unsigned char)ps->p[1]))) {
        return parse_number(ps);
    }
    if (isalpha((unsigned char)*ps->p)) {
        char name[12];
        size_t n = 0;
        while ((isalnum((unsigned char)*ps->p) || *ps->p == '_') && n < sizeof(name) - 1) name[n++] = *ps->p++;
        name[n] = '\0';
        if (!strcmp(name, "pi")) return M_PI;
        if (!strcmp(name, "e")) return M_E;
        if (!strcmp(name, "ans")) return ps->ctx ? ps->ctx->ans : 0;
        if (!is_fn(name)) {
            ps->p -= n;
            fail(ps, "unknown name");
            return 0;
        }
        if (!accept(ps, "(")) {
            /* Allow "sin 30" style without parentheses. */
            return call_fn(ps, name, parse_unary(ps));
        }
        double arg = parse_or(ps);
        if (!accept(ps, ")")) fail(ps, "missing )");
        return call_fn(ps, name, arg);
    }
    fail(ps, *ps->p ? "unexpected input" : "incomplete");
    return 0;
}

static double parse_postfix(parser_t *ps)
{
    double v = parse_primary(ps);
    while (accept(ps, "!")) {
        if (v < 0 || v != floor(v) || v > 170) {
            fail(ps, "factorial needs 0..170");
            return 0;
        }
        double f = 1;
        for (int i = 2; i <= (int)v; i++) f *= i;
        v = f;
    }
    return v;
}

static double parse_power(parser_t *ps)
{
    double base = parse_postfix(ps);
    if (accept(ps, "^")) {
        double ex = parse_unary(ps); /* right associative, binds tighter than unary minus on the left */
        return pow(base, ex);
    }
    return base;
}

static double parse_unary(parser_t *ps)
{
    if (++ps->depth > MAX_DEPTH) {
        fail(ps, "too deeply nested");
        return 0;
    }
    double v;
    if (accept(ps, "-") || accept(ps, "\xE2\x88\x92")) {
        v = -parse_unary(ps);
    } else if (accept(ps, "+")) {
        v = parse_unary(ps);
    } else if (accept(ps, "~")) {
        int64_t i;
        double x = parse_unary(ps);
        v        = to_int(ps, x, &i) ? (double)~i : 0;
    } else {
        v = parse_power(ps);
    }
    ps->depth--;
    return v;
}

static double parse_mul(parser_t *ps)
{
    double v = parse_unary(ps);
    for (;;) {
        if (accept(ps, "*") || accept(ps, "\xC3\x97")) {
            v *= parse_unary(ps);
        } else if (accept(ps, "/") || accept(ps, "\xC3\xB7")) {
            double d = parse_unary(ps);
            if (d == 0) fail(ps, "division by zero");
            v = d == 0 ? 0 : v / d;
        } else if (accept(ps, "%")) {
            double d = parse_unary(ps);
            if (d == 0) fail(ps, "division by zero");
            v = d == 0 ? 0 : fmod(v, d);
        } else {
            return v;
        }
    }
}

static double parse_add(parser_t *ps)
{
    double v = parse_mul(ps);
    for (;;) {
        if (accept(ps, "+")) {
            v += parse_mul(ps);
        } else if (accept(ps, "-") || accept(ps, "\xE2\x88\x92")) {
            v -= parse_mul(ps);
        } else {
            return v;
        }
    }
}

static double parse_shift(parser_t *ps)
{
    double v = parse_add(ps);
    for (;;) {
        bool left = accept(ps, "<<");
        if (!left && !accept(ps, ">>")) return v;
        double r = parse_add(ps);
        int64_t a, b;
        if (!to_int(ps, v, &a) || !to_int(ps, r, &b)) return 0;
        if (b < 0 || b > 63) {
            fail(ps, "shift out of range");
            return 0;
        }
        v = left ? (double)(int64_t)((uint64_t)a << b) : (double)(a >> b);
    }
}

static double parse_and(parser_t *ps)
{
    double v = parse_shift(ps);
    while (accept(ps, "&")) {
        int64_t a, b;
        double r = parse_shift(ps);
        if (!to_int(ps, v, &a) || !to_int(ps, r, &b)) return 0;
        v = (double)(a & b);
    }
    return v;
}

static double parse_xor(parser_t *ps)
{
    double v = parse_and(ps);
    while (accept_word(ps, "xor")) {
        int64_t a, b;
        double r = parse_and(ps);
        if (!to_int(ps, v, &a) || !to_int(ps, r, &b)) return 0;
        v = (double)(a ^ b);
    }
    return v;
}

static double parse_or(parser_t *ps)
{
    double v = parse_xor(ps);
    while (accept(ps, "|")) {
        int64_t a, b;
        double r = parse_xor(ps);
        if (!to_int(ps, v, &a) || !to_int(ps, r, &b)) return 0;
        v = (double)(a | b);
    }
    return v;
}

calc_result_t calc_eval(const char *expr, const calc_ctx_t *ctx)
{
    parser_t ps = {.s = expr, .p = expr, .ctx = ctx};
    calc_result_t r = {0};
    skip_ws(&ps);
    if (*ps.p == '\0') {
        r.error = "empty";
        return r;
    }
    double v = parse_or(&ps);
    skip_ws(&ps);
    if (ps.err == NULL && *ps.p != '\0') fail(&ps, *ps.p == ')' ? "unbalanced )" : "unexpected input");
    if (ps.err == NULL && !isfinite(v)) fail(&ps, isnan(v) ? "undefined" : "overflow");
    if (ps.err) {
        r.error = ps.err;
        r.pos   = (int)(ps.err_at - expr);
        return r;
    }
    r.ok    = true;
    r.value = v == 0 ? 0 : v; /* no -0 */
    return r;
}

/* ---- Formatting ---------------------------------------------------------- */

void calc_format(double v, char *out, size_t n)
{
    if (v == floor(v) && fabs(v) < 1e15) {
        snprintf(out, n, "%.0f", v);
        return;
    }
    snprintf(out, n, "%.12g", v);
    /* %g already trims zeros unless it chose exponent form with a mantissa
     * like 1.500000000000e+20; tidy that up. */
    char *e = strchr(out, 'e');
    if (e && strchr(out, '.')) {
        char *z = e - 1;
        while (*z == '0') z--;
        if (*z == '.') z--;
        memmove(z + 1, e, strlen(e) + 1);
    }
}

bool calc_format_bases(double v, char *hex, char *oct, char *bin, size_t n)
{
    if (!isfinite(v) || v != floor(v) || fabs(v) > 9.2e18) return false;
    int64_t i   = (int64_t)v;
    uint64_t u  = (uint64_t)i;
    bool neg    = i < 0;
    uint64_t mag = neg ? (uint64_t)(-(i + 1)) + 1 : u;

    snprintf(hex, n, "%s0x%llX", neg ? "-" : "", (unsigned long long)mag);
    snprintf(oct, n, "%s0o%llo", neg ? "-" : "", (unsigned long long)mag);

    /* Binary in nibble groups, at least 8 bits. */
    int bits = 1;
    while (bits < 64 && (mag >> bits)) bits++;
    if (bits < 8) bits = 8;
    bits = (bits + 3) / 4 * 4;
    size_t k = 0;
    if (neg && k < n - 1) bin[k++] = '-';
    for (int b = bits - 1; b >= 0 && k < n - 2; b--) {
        bin[k++] = (mag >> b) & 1 ? '1' : '0';
        if (b && b % 4 == 0) bin[k++] = ' ';
    }
    bin[k] = '\0';
    return true;
}
