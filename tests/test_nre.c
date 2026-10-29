/* Unit tests for nre. Build and run with `make test`. */
#include "nre.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static void test_classes(void)
{
    M("[abc]+", "xxcabz", 2, 5);
    M("[a-z]+", "ABCdefGHI", 3, 6);
    M("[^a-z]+", "abcDEF1", 3, 7);
    M("[]a]+", "x]a]", 1, 4);   /* ']' first is literal */
    M("[^]a]", "]ab", 2, 3);
    M("[a-]+", "-a-", 0, 3);     /* trailing '-' is literal */
    M("[-a]+", "a-", 0, 2);
    M("[\\d.]+", "v1.25!", 1, 5);
    M("[\\]]", "a]", 1, 2);
    M("[\\x41-\\x43]+", "zABCD", 1, 4);
    NOMATCH("[^\\s]", " \t\n");
}

static void test_escapes(void)
{
    M("\\d+", "abc123def", 3, 6);
    M("\\D+", "123abc", 3, 6);
    M("\\w+", "  hello_42 ", 2, 10);
    M("\\W+", "ab, cd", 2, 4);
    M("\\s+", "a \t b", 1, 4);
    M("\\S+", "   xy ", 3, 5);
    M("a\\.b", "axb a.b", 4, 7);
    M("\\(\\)", "f()", 1, 3);
    M("\\\\", "a\\b", 1, 2);
    M("\\t", "a\tb", 1, 2);
    M("\\x41", "zA", 1, 2);
    M("\\*", "2*3", 1, 2);
}

static void test_anchors(void)
{
    M("^abc", "abcabc", 0, 3);
    NOMATCH("^abc", "xabc");
    M("abc$", "abcabc", 3, 6);
    NOMATCH("abc$", "abcx");
    M("^$", "", 0, 0);
    NOMATCH("^$", "x");
    M("\\bcat\\b", "concat cat", 7, 10);
    NOMATCH("\\bcat\\b", "concatenate");
    M("\\Bcat", "concat", 3, 6);
    M("a|^b", "xb", -1, -1);
    M("(^a|b)", "ab", 0, 1);
}

static void test_lazy(void)
{
    M("a+?", "aaa", 0, 1);
    M("a*?", "aaa", 0, 0);
    M("a??b", "ab", 0, 2);
    M("<.+?>", "<a><b>", 0, 3);
    M("<.+>", "<a><b>", 0, 6);
    G("(a+?)(a*)", "aaaa", 1, 0, 1);
    G("(a{2,3}?)", "aaaa", 1, 0, 2);
}

static void test_counted(void)
{
    M("a{3}", "aaaa", 0, 3);
    NOMATCH("a{3}", "aa");
    M("a{2,}", "aaaaa", 0, 5);
    M("a{2,3}", "aaaaa", 0, 3);
    M("a{0,2}b", "aab", 0, 3);
    M("(ab){2}", "abababab", 0, 4);
    M("\\d{3}-\\d{4}", "call 555-1234 now", 5, 13);
    M("a{,2}", "a{,2}", 0, 5);  /* not a quantifier: literal braces */
    M("x{", "x{", 0, 2);
    M("{2}", "{2}", 0, 3);
    M("a{1000}", "a", -1, -1);
}

static void test_icase(void)
{
    MI("hello", "Say HeLLo", 4, 9);
    MI("[a-c]+", "xABCd", 1, 4);
    MI("[^a]", "Ab", 1, 2);
    NOMATCH("hello", "HELLO");
}

static void test_offsets_and_fullmatch(void)
{
    nre *re = nre_compile("\\bab", 0, NULL, 0);
    nre_span m;
    CHECK(re != NULL, "compile");
    /* starting mid-word: \b still sees the preceding 'x' */
    CHECK(nre_exec(re, "xab ab", 6, 1, &m, 1) && m.start == 4, "\\b honours text before start, got %ld", m.start);
    CHECK(nre_exec(re, "ab ab", 5, 1, &m, 1) && m.start == 3, "found second at 3, got %ld", m.start);
    CHECK(!nre_exec(re, "ab", 2, 3, &m, 1), "start past end");
    nre_free(re);

    re = nre_compile("a|ab", 0, NULL, 0);
    CHECK(nre_fullmatch(re, "ab", 2, &m, 1) && m.start == 0 && m.end == 2, "fullmatch tries all alternatives");
    CHECK(!nre_fullmatch(re, "abc", 3, NULL, 0), "fullmatch rejects partial");
    nre_free(re);

    re = nre_compile("(\\d+)-(\\d+)", 0, NULL, 0);
    nre_span g[4];
    CHECK(nre_groups(re) == 2, "group count");
    CHECK(nre_fullmatch(re, "10-20", 5, g, 4), "fullmatch with groups");
    CHECK(g[2].start == 3 && g[2].end == 5, "group 2");
    CHECK(g[3].start == -1 && g[3].end == -1, "extra span reset to -1");
    nre_free(re);

    /* embedded NUL bytes are ordinary input */
    re = nre_compile("b\\x00c", 0, NULL, 0);
    CHECK(nre_exec(re, "ab\0cd", 5, 0, &m, 1) && m.start == 1 && m.end == 4, "NUL in text");
    nre_free(re);
}

static void expect_error(int line, const char *pat, const char *needle)
{
    char err[160];
    nre *re = nre_compile(pat, 0, err, sizeof err);
    CHECK(re == NULL, "line %d: /%s/ should not compile", line, pat);
    CHECK(re != NULL || strstr(err, needle) != NULL, "line %d: /%s/: wanted \"%s\" in \"%s\"", line, pat, needle, err);
    nre_free(re);
}
#define ERR(pat, needle) expect_error(__LINE__, pat, needle)

static void test_errors(void)
{
    ERR("(abc", "missing ')'");
    ERR("abc)", "unmatched ')'");
    ERR("*a", "nothing to repeat");
    ERR("a|+", "nothing to repeat");
    ERR("^*", "nothing to repeat");
    ERR("a**", "multiple repeat");
    ERR("[abc", "missing ']'");
    ERR("[z-a]", "bad character range");
    ERR("a\\", "trailing backslash");
    ERR("\\q", "unknown escape");
    ERR("\\x4", "two hex digits");
    ERR("a{3,2}", "bad repetition range");
    ERR("a{1001}", "too large");
    ERR("(?=a)", "unsupported group");
    ERR("(a{1000}){1000}", "pattern too large");
    ERR("(a{1000}){1000}", "at offset");
    CHECK(nre_compile(NULL, 0, NULL, 0) == NULL, "NULL pattern");
}

static void test_named_groups(void)
{
    nre *re = nre_compile("(?<year>\\d{4})-(?P<month>\\d\\d)-(\\d\\d)", 0, NULL, 0);
    CHECK(re != NULL, "named groups compile");
    if (re) {
        CHECK(nre_groups(re) == 3, "named groups count: %zu", nre_groups(re));
        CHECK(nre_group_index(re, "year") == 1, "year index");
        CHECK(nre_group_index(re, "month") == 2, "month index");
        CHECK(nre_group_index(re, "day") == -1, "missing name");
        CHECK(nre_group_index(re, NULL) == -1, "NULL name");
        CHECK(nre_group_name(re, 1) && strcmp(nre_group_name(re, 1), "year") == 0, "name of 1");
        CHECK(nre_group_name(re, 2) && strcmp(nre_group_name(re, 2), "month") == 0, "name of 2");
        CHECK(nre_group_name(re, 3) == NULL, "unnamed group has no name");
        CHECK(nre_group_name(re, 0) == NULL, "group 0 has no name");
        CHECK(nre_group_name(re, 4) == NULL, "out of range");
        const char *text = "on 2026-10-28 at noon";
        nre_span m[4];
        CHECK(nre_exec(re, text, strlen(text), 0, m, 4), "date matches");
        CHECK(m[1].start == 3 && m[1].end == 7, "year span");
        CHECK(m[2].start == 8 && m[2].end == 10, "month span");
        CHECK(m[3].start == 11 && m[3].end == 13, "day span");
        nre_free(re);
    }
    /* Named groups number in order with unnamed ones and nest normally. */
    G("(a)(?<x>b(?<y>c))", "abc", 2, 1, 3);
    G("(a)(?<x>b(?<y>c))", "abc", 3, 2, 3);
    G("(?:(?<w>\\w+) )+", "ab cd ", 1, 3, 5);
    M("(?<_under9>x+)", "yxxy", 1, 3);
    CHECK(nre_group_index(NULL, "x") == -1 && nre_group_name(NULL, 1) == NULL, "NULL re");

    ERR("(?<>a)", "bad group name");
    ERR("(?<1a>a)", "bad group name");
    ERR("(?<a-b>x)", "missing '>'");
    ERR("(?<ab", "missing '>'");
    ERR("(?<a>x)(?P<a>y)", "duplicate group name 'a'");
    ERR("(?<=a)b", "unsupported group");
    ERR("(?<!a)b", "unsupported group");
    ERR("(?P=a)", "unsupported group");
    ERR("(?<abcdefghijabcdefghijabcdefghijabc>x)", "too long");
}

/* Expect nre_replace(pat, text, repl, all) to give want with n replacements. */
static void replace_is(int line, const char *pat, const char *text, const char *repl, int all,
                       const char *want, size_t want_n)
{
    char err[160];
    nre *re = nre_compile(pat, 0, err, sizeof err);
    checks++;
    if (!re) {
        failures++;
        fprintf(stderr, "line %d: /%s/ failed to compile: %s\n", line, pat, err);
        return;
    }
    size_t n = 99, outlen = 0;
    char *got = nre_replace(re, text, strlen(text), repl, all, &outlen, &n, err, sizeof err);
    CHECK(got && strcmp(got, want) == 0 && outlen == strlen(want) && n == want_n,
          "line %d: s/%s/%s/ on \"%s\": want \"%s\" (%zu), got \"%s\" (%zu) %s", line, pat, repl, text,
          want, want_n, got ? got : "(null)", n, got ? "" : err);
    free(got);
    nre_free(re);
}
#define R1(pat, text, repl, want, n) replace_is(__LINE__, pat, text, repl, 0, want, n)
#define RA(pat, text, repl, want, n) replace_is(__LINE__, pat, text, repl, 1, want, n)

static void bad_template(int line, const char *pat, const char *repl, const char *needle)
{
    char err[160];
    nre *re = nre_compile(pat, 0, NULL, 0);
    char *got = nre_replace(re, "abc", 3, repl, 1, NULL, NULL, err, sizeof err);
    CHECK(got == NULL && strstr(err, needle) != NULL, "line %d: template \"%s\": wanted \"%s\" in \"%s\"",
          line, repl, needle, got ? "(no error)" : err);
    free(got);
    nre_free(re);
}
#define BADT(pat, repl, needle) bad_template(__LINE__, pat, repl, needle)

static void test_replace(void)
{
    R1("o", "foo boo", "0", "f0o boo", 1);
    RA("o", "foo boo", "0", "f00 b00", 4);
    RA("q", "foo", "x", "foo", 0);
    RA("o", "", "x", "", 0);
    RA("(\\w+)@(\\w+)", "ada@lovelace, alan@turing", "$2.$1", "lovelace.ada, turing.alan", 2);
    RA("(?<user>\\w+)@(?<host>\\w+)", "ada@lovelace", "${host}/${user}", "lovelace/ada", 1);
    RA("(\\d+)", "a1b22", "[$0|$&|${1}]", "a[1|1|1]b[22|22|22]", 2);
    RA("x", "axb", "$$", "a$b", 1);
    RA("(a)|(b)", "ab", "<$1$2>", "<a><b>", 2);       /* unset groups are empty */
    RA("(a)", "aa", "${1}0", "a0a0", 2);              /* braces end a number */
    RA("(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)", "abcdefghij", "$10$1", "ja", 1);
    RA("x*", "abxd", "-", "-a-b-d-", 4);              /* empty match after x is skipped */
    RA("", "abc", "-", "-a-b-c-", 4);
    RA("\\b", "hi yo", "|", "|hi| |yo|", 4);
    RA("^", "abc", ">", ">abc", 1);
    RA("$", "abc", "<", "abc<", 1);
    RA("a|", "baaac", "-", "-b---c-", 5);
    RA("a+?", "aaa", "<$0>", "<a><a><a>", 3);
    R1("\\s+", "  a  b", "", "a  b", 1);

    /* Embedded NUL bytes pass through. */
    nre *re = nre_compile("b", 0, NULL, 0);
    size_t outlen = 0, n = 0;
    char *got = nre_replace(re, "a\0b\0b", 5, "X", 1, &outlen, &n, NULL, 0);
    CHECK(got && outlen == 5 && n == 2 && memcmp(got, "a\0X\0X", 5) == 0, "NUL bytes in replace");
    free(got);
    nre_free(re);

    BADT("(a)", "$2", "no group 2");
    BADT("(a)", "${2}", "no group 2");
    BADT("(a)", "$999999999999", "no group");
    BADT("(?<x>a)", "${y}", "no group named 'y'");
    BADT("a", "x$", "must be followed");
    BADT("a", "$z", "must be followed");
    BADT("a", "${", "bad ${...}");
    BADT("a", "${}", "bad ${...}");
    BADT("a", "${1x}", "bad ${...}");
    BADT("a", "ab${cd", "at offset 2");
    CHECK(nre_replace(NULL, "a", 1, "b", 1, NULL, NULL, NULL, 0) == NULL, "NULL re");
}

static double seconds_for(const char *pat, const char *text)
{
    nre *re = nre_compile(pat, 0, NULL, 0);
    if (!re) return 1e9;
    clock_t t0 = clock();
    nre_span m;
    nre_fullmatch(re, text, strlen(text), &m, 1);
    double s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    nre_free(re);
    return s;
}

/* Patterns that take exponential time in backtracking engines. */
static void test_pathological(void)
{
    char pat[512] = "", text[64] = "";
    for (int i = 0; i < 30; i++) strcat(pat, "a?");
    for (int i = 0; i < 30; i++) strcat(pat, "a");
    memset(text, 'a', 30);
    double t = seconds_for(pat, text);
    CHECK(t < 0.5, "(a?){30}a{30} took %.3fs", t);

    static char big[20001];
    memset(big, 'a', 20000);
    t = seconds_for("(a*)*b", big);
    CHECK(t < 0.5, "(a*)*b on 20k a's took %.3fs", t);
    t = seconds_for("(a|aa)+$", big);
    CHECK(t < 0.5, "(a|aa)+$ on 20k a's took %.3fs", t);
}

int main(void)
{
    test_literals();
    test_alternation();
    test_quantifiers();
    test_groups();
    test_classes();
    test_escapes();
    test_anchors();
    test_lazy();
    test_counted();
    test_icase();
    test_offsets_and_fullmatch();
    test_errors();
    test_named_groups();
    test_replace();
    test_pathological();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
