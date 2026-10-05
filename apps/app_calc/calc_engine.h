/*
 * Expression evaluator for the CALC module. Pure C, no LVGL, so it is unit
 * tested on the host (sim/calc_test.c).
 *
 * Grammar, lowest precedence first:
 *   |        bitwise or          (integers)
 *   xor      bitwise xor
 *   &        bitwise and
 *   << >>    shifts
 *   + -
 *   * / %    (also × ÷); % is remainder
 *   - + ~    unary
 *   ^        power, right associative (-2^2 = -4)
 *   !        factorial (postfix)
 *   primary  numbers (12, 1.5e3, 0xFF, 0b1010, 0o17), ( expr ), constants
 *            pi π e ans, functions sin cos tan asin acos atan sqrt √ cbrt
 *            log (base 10) ln log2 exp abs floor ceil round
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool degrees;   /* trig functions take and return degrees */
    double ans;     /* value of `ans` */
} calc_ctx_t;

typedef struct {
    bool ok;
    double value;
    const char *error;  /* static message when !ok */
    int pos;            /* byte offset of the error */
} calc_result_t;

calc_result_t calc_eval(const char *expr, const calc_ctx_t *ctx);

/* Shortest faithful decimal: integers without a fraction, otherwise up to
 * 12 significant digits with trailing zeros trimmed. */
void calc_format(double v, char *out, size_t n);

/* For integral values: hex, octal and binary (binary grouped in nibbles).
 * Returns false if `v` is not an integer that fits in 64 bits. */
bool calc_format_bases(double v, char *hex, char *oct, char *bin, size_t n);
