/* Host unit tests for apps/app_calc/calc_engine.c. Run: sim/build/calc_test */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "calc_engine.h"

static int s_fail, s_count;

static void ok(const char *expr, double want, const calc_ctx_t *ctx)
{
    s_count++;
    calc_result_t r = calc_eval(expr, ctx);
    if (!r.ok || fabs(r.value - want) > 1e-9 * fmax(1.0, fabs(want))) {
        printf("FAIL  %-28s want %.12g got %s%.12g (%s)\n", expr, want, r.ok ? "" : "error ", r.value,
               r.ok ? "" : r.error);
        s_fail++;
    }
}

static void err(const char *expr, const char *want_msg)
{
    s_count++;
    calc_result_t r = calc_eval(expr, NULL);
    if (r.ok || strcmp(r.error, want_msg) != 0) {
        printf("FAIL  %-28s want error \"%s\" got %s\n", expr, want_msg, r.ok ? "ok" : r.error);
        s_fail++;
    }
}

static void fmt(double v, const char *want)
{
    s_count++;
    char buf[64];
    calc_format(v, buf, sizeof(buf));
    if (strcmp(buf, want) != 0) {
        printf("FAIL  format %.17g want %s got %s\n", v, want, buf);
        s_fail++;
    }
}

int main(void)
{
    calc_ctx_t rad = {.degrees = false, .ans = 42};
    calc_ctx_t deg = {.degrees = true, .ans = 0};

    /* Roadmap checkpoints */
    ok("(0xFF << 2) | 3", 1023, NULL);
    ok("2^10 / 3", 1024.0 / 3.0, NULL);

    ok("1 + 2 * 3", 7, NULL);
    ok("(1 + 2) * 3", 9, NULL);
    ok("2^3^2", 512, NULL);
    ok("-2^2", -4, NULL);
    ok("2^-1", 0.5, NULL);
    ok("10 % 4", 2, NULL);
    ok("7 \xC3\x97 6", 42, NULL);
    ok("84 \xC3\xB7 2", 42, NULL);
    ok("5 \xE2\x88\x92 3", 2, NULL);
    ok("\xE2\x88\x9A" "16", 4, NULL);
    ok("2 * \xCF\x80", 2 * M_PI, NULL);
    ok("1.5e3", 1500, NULL);
    ok(".5 + .25", 0.75, NULL);
    ok("0b1010 + 0o17", 25, NULL);
    ok("0xff & 0x0f", 15, NULL);
    ok("6 xor 3", 5, NULL);
    ok("~0", -1, NULL);
    ok("1 << 10", 1024, NULL);
    ok("1024 >> 3", 128, NULL);
    ok("5!", 120, NULL);
    ok("ans * 2", 84, &rad);
    ok("sqrt(2)^2", 2, NULL);
    ok("log(1000)", 3, NULL);
    ok("ln(e)", 1, NULL);
    ok("log2(256)", 8, NULL);
    ok("abs(-3.5) + floor(2.7) + ceil(2.1) + round(2.5)", 3.5 + 2 + 3 + 3, NULL);
    ok("sin(pi/2)", 1, &rad);
    ok("sin 90", 1, &deg);
    ok("cos(60)", 0.5, &deg);
    ok("atan(1)", 45, &deg);
    ok("  ( ( 1 ) )  ", 1, NULL);

    err("", "empty");
    err("1 +", "incomplete");
    err("(1 + 2", "missing )");
    err("1 + 2)", "unbalanced )");
    err("1 / 0", "division by zero");
    err("1.5 | 1", "bitwise ops need integers");
    err("foo(1)", "unknown function");
    err("2pi", "unexpected input");
    err("sqrt(-1)", "undefined");
    err("10^400", "overflow");
    err("171!", "factorial needs 0..170");
    err("1 << 64", "shift out of range");
    err("inf", "unknown function");

    fmt(1023, "1023");
    fmt(-0.0, "-0");
    fmt(1024.0 / 3.0, "341.333333333");
    fmt(0.1 + 0.2, "0.3");
    fmt(1e20, "1e+20");
    fmt(1.5e20, "1.5e+20");
    fmt(1e-7, "1e-07");

    char h[80], o[80], b[80];
    s_count++;
    if (!calc_format_bases(1023, h, o, b, sizeof(h)) || strcmp(h, "0x3FF") || strcmp(o, "0o1777") ||
        strcmp(b, "0011 1111 1111")) {
        printf("FAIL  bases 1023: %s %s %s\n", h, o, b);
        s_fail++;
    }
    s_count++;
    if (!calc_format_bases(-5, h, o, b, sizeof(h)) || strcmp(h, "-0x5") || strcmp(b, "-0000 0101")) {
        printf("FAIL  bases -5: %s %s %s\n", h, o, b);
        s_fail++;
    }
    s_count++;
    if (calc_format_bases(1.5, h, o, b, sizeof(h))) {
        printf("FAIL  bases 1.5 should be rejected\n");
        s_fail++;
    }

    printf("%d/%d passed\n", s_count - s_fail, s_count);
    return s_fail ? 1 : 0;
}
