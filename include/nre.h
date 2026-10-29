/*
 * nre - a small regular expression engine built on Thompson's construction
 * and a Pike VM. Matching runs in O(len(text) * len(pattern)) time with no
 * backtracking, so no pattern can make it go exponential.
 */
#ifndef NRE_H
#define NRE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nre nre;

/* A matched region as byte offsets [start, end). Unset groups are -1. */
typedef struct {
    long start;
    long end;
} nre_span;

enum {
    NRE_ICASE = 1, /* ASCII case-insensitive matching */
    NRE_WORD = 2   /* matches must not touch a word character on either side */
};

/*
 * Compile a pattern. Returns NULL on failure and, if err is non-NULL,
 * writes a human-readable message (including the offending offset).
 */
nre *nre_compile(const char *pattern, int flags, char *err, size_t errlen);

/* Number of capturing groups in the pattern, not counting group 0. */
size_t nre_groups(const nre *re);

/*
 * Find the leftmost match in text[0..len) that starts at or after `start`.
 * Anchors and \b still see the whole text, so searching from an offset
 * behaves like continuing a scan. On success returns 1 and fills up to nm
 * spans (m[0] is the whole match, m[i] is group i). Returns 0 on no match.
 */
/*
 * Look up a named group, written (?<name>x) or (?P<name>x). Returns its
 * index, or -1 if the pattern has no group with that name.
 */
int nre_group_index(const nre *re, const char *name);

/* The name of group `group`, or NULL if it is unnamed or out of range. */
const char *nre_group_name(const nre *re, size_t group);

int nre_exec(const nre *re, const char *text, size_t len, size_t start,
             nre_span *m, size_t nm);

/* Like nre_exec, but the match must cover all of text[0..len). */
int nre_fullmatch(const nre *re, const char *text, size_t len,
                  nre_span *m, size_t nm);

/*
 * Replace the first match (or every match, if `all` is non-zero) of re in
 * text[0..len) with the template repl, and return the result as a new
 * NUL-terminated buffer the caller must free(). In the template, $1 or ${1}
 * is a group by number, ${name} a named group, $0 or $& the whole match and
 * $$ a literal dollar sign; groups that did not take part expand to nothing.
 * An empty match directly after the previous match is skipped. The result's
 * length goes in *outlen and the number of replacements in *count (either
 * may be NULL). Returns NULL and fills err on a bad template or no memory.
 */
char *nre_replace(const nre *re, const char *text, size_t len, const char *repl, int all,
                  size_t *outlen, size_t *count, char *err, size_t errlen);

/*
 * Expand the template repl (same syntax as nre_replace) for one match, where
 * m holds nm spans as filled in by nre_exec. Groups past nm count as unset.
 * Returns a new NUL-terminated buffer to free(), or NULL with err filled in.
 */
char *nre_expand(const nre *re, const char *repl, const char *text, const nre_span *m, size_t nm,
                 size_t *outlen, char *err, size_t errlen);

void nre_free(nre *re);

#ifdef __cplusplus
}
#endif

#endif /* NRE_H */
