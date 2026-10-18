/*
 * bench: time the classic pathological pattern a?^n a^n against a^n, on
 * nre and on a deliberately naive backtracking matcher (single characters
 * with an optional '?' suffix). The backtracker takes about 2^n steps;
 * nre's work grows only with n^2 (pattern length times text length).
 */
#include "nre.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Backtracking full match for patterns made of literal chars and "c?". */
static int bt_match(const char *p, const char *s)
{
    if (*p == '\0') return *s == '\0';
    if (p[1] == '?') {
        if (*s == *p && bt_match(p + 2, s + 1)) return 1;
        return bt_match(p + 2, s);
    }
    return *s == *p && bt_match(p + 1, s + 1);
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void)
{
    char pat[256], text[128];
    printf("%4s  %12s  %12s\n", "n", "nre (ms)", "backtrack (ms)");
    for (int n = 10; n <= 28; n += 2) {
        pat[0] = '\0';
        for (int i = 0; i < n; i++) strcat(pat, "a?");
        for (int i = 0; i < n; i++) strcat(pat, "a");
        memset(text, 'a', (size_t)n);
        text[n] = '\0';

        nre *re = nre_compile(pat, 0, NULL, 0);
        double t0 = now();
        int a = nre_fullmatch(re, text, (size_t)n, NULL, 0);
        double t1 = now();
        int b = bt_match(pat, text);
        double t2 = now();
        nre_free(re);

        printf("%4d  %12.3f  %12.3f%s\n", n, (t1 - t0) * 1e3, (t2 - t1) * 1e3,
               a == b ? "" : "  (results differ!)");
    }
    return 0;
}
