/* Unit tests for nre. Build and run with `make test`. */
#include "nre.h"

#include <stdio.h>
#include <string.h>

static int checks, failures;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        checks++;                                                     \
        if (!(cond)) {                                                \
            failures++;                                               \
            fprintf(stderr, "%s:%d: FAIL: ", __FILE__, __LINE__);     \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
        }                                                             \
    } while (0)

/* Expect the leftmost match of pat in text to be [s, e); s == -1 means none. */
static void match_at(int line, const char *pat, int flags, const char *text, long s, long e)
{
    char err[160];
    nre *re = nre_compile(pat, flags, err, sizeof err);
    checks++;
    if (!re) {
        failures++;
        fprintf(stderr, "line %d: /%s/ failed to compile: %s\n", line, pat, err);
        return;
    }
    nre_span m = { -2, -2 };
    int hit = nre_exec(re, text, strlen(text), 0, &m, 1);
    if (s < 0) {
        CHECK(!hit, "line %d: /%s/ on \"%s\": expected no match, got [%ld,%ld)", line, pat, text, m.start, m.end);
    } else {
        CHECK(hit && m.start == s && m.end == e,
              "line %d: /%s/ on \"%s\": expected [%ld,%ld), got %s [%ld,%ld)", line, pat, text, s, e,
              hit ? "match" : "no match", m.start, m.end);
    }
    nre_free(re);
}

#define M(pat, text, s, e) match_at(__LINE__, pat, 0, text, s, e)
#define MI(pat, text, s, e) match_at(__LINE__, pat, NRE_ICASE, text, s, e)
#define NOMATCH(pat, text) match_at(__LINE__, pat, 0, text, -1, -1)

/* Expect capture group g of the first match to be [s, e). */
static void group_at(int line, const char *pat, const char *text, int g, long s, long e)
{
    nre *re = nre_compile(pat, 0, NULL, 0);
    checks++;
    if (!re) {
        failures++;
        fprintf(stderr, "line %d: /%s/ failed to compile\n", line, pat);
        return;
    }
    nre_span m[8];
    int hit = nre_exec(re, text, strlen(text), 0, m, 8);
    CHECK(hit && m[g].start == s && m[g].end == e, "line %d: /%s/ on \"%s\" group %d: expected [%ld,%ld), got [%ld,%ld)",
          line, pat, text, g, s, e, hit ? m[g].start : -9, hit ? m[g].end : -9);
    nre_free(re);
}
#define G(pat, text, g, s, e) group_at(__LINE__, pat, text, g, s, e)

static void test_literals(void)
{
    M("abc", "abc", 0, 3);
    M("abc", "xxabcxx", 2, 5);
    M("", "", 0, 0);
    M("", "abc", 0, 0);
    NOMATCH("abd", "abcabc");
    M("a.c", "abc", 0, 3);
    NOMATCH("a.c", "a\nc");
    M("needle", "haystack with a needle in it", 16, 22);
}

static void test_alternation(void)
{
    M("cat|dog", "hotdog", 3, 6);
    M("cat|dog", "catdog", 0, 3);
    M("a|ab", "ab", 0, 1); /* leftmost-first, like Perl and Python */
    M("ab|a", "ab", 0, 2);
    M("x(a|b|c)y", "xcy", 0, 3);
    M("|a", "a", 0, 0);
    NOMATCH("cat|dog", "cow");
}

static void test_quantifiers(void)
{
    M("a*", "aaa", 0, 3);
    M("a*", "baaa", 0, 0);
    M("a+", "baaa", 1, 4);
    M("ba?c", "bc", 0, 2);
    M("ba?c", "bac", 0, 3);
    NOMATCH("ba?c", "baac");
    M("(ab)+", "xababab", 1, 7);
    M("a*b", "aaab", 0, 4);
    M("(a*)*b", "aab", 0, 3);  /* nested empty loops terminate */
    M("(a|)*b", "aab", 0, 3);
}

static void test_groups(void)
{
    G("(a+)(b+)", "xaabbb", 1, 1, 3);
    G("(a+)(b+)", "xaabbb", 2, 3, 6);
    G("(a)|(b)", "b", 1, -1, -1);
    G("(a)|(b)", "b", 2, 0, 1);
    G("(?:ab)+(c)", "ababc", 1, 4, 5);
    G("(a)*", "aaa", 1, 2, 3); /* last iteration wins */
    G("((a)(b))", "ab", 3, 1, 2);
}

int main(void)
{
    test_literals();
    test_alternation();
    test_quantifiers();
    test_groups();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
