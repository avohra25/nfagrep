/* nfagrep - a small grep built on nre. */
#include "nre.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_SPANS 33 /* group 0 plus nre's 32 capture groups */

typedef struct {
    int invert, count, number, only, whole, show_name;
    const char *repl; /* -r template, or NULL */
} Options;

static void usage(FILE *out)
{
    fputs("usage: nfagrep [-cinovwx] [-r REPLACEMENT] PATTERN [FILE...]\n"
          "       nfagrep [-cinovwx] [-r REPLACEMENT] -e PATTERN [-e PATTERN]... [FILE...]\n"
          "  -c  print only a count of selected lines\n"
          "  -e  use PATTERN; repeat to select lines matching any of them\n"
          "  -i  ignore ASCII case\n"
          "  -n  prefix each line with its line number\n"
          "  -o  print only the matched parts of each line\n"
          "  -r  print selected lines with every match replaced by REPLACEMENT\n"
          "      ($1 or ${1} for a group, ${name} for a named group, $& for the\n"
          "      whole match, $$ for a dollar sign); with -o, print just the\n"
          "      replacement for each match\n"
          "  -v  select lines that do not match\n"
          "  -w  matches must not have a word character right before or after them\n"
          "  -x  the pattern must match the whole line\n"
          "With no FILE, or when FILE is -, read standard input.\n"
          "Exit status: 0 if a line was selected, 1 if none, 2 on error.\n",
          out);
}

static void prefix(const Options *o, const char *name, long lineno)
{
    if (o->show_name) printf("%s:", name);
    if (o->number) printf("%ld:", lineno);
}

/* Print the -r expansion for one match. */
static void put_expansion(const nre *re, const Options *o, const char *line, const nre_span *m)
{
    size_t n;
    char *s = nre_expand(re, o->repl, line, m, MAX_SPANS, &n, NULL, 0);
    if (!s) {
        fputs("nfagrep: out of memory\n", stderr);
        exit(2);
    }
    fwrite(s, 1, n, stdout);
    free(s);
}

static long grep_stream(const nre *re, FILE *fp, const char *name, const Options *o)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t got;
    long lineno = 0, selected = 0;
    nre_span m[MAX_SPANS];

    while ((got = getline(&line, &cap, fp)) != -1) {
        size_t len = (size_t)got;
        lineno++;
        if (len && line[len - 1] == '\n') len--;

        int hit = o->whole ? nre_fullmatch(re, line, len, m, MAX_SPANS)
                           : nre_exec(re, line, len, 0, m, MAX_SPANS);
        if (hit == o->invert) continue;
        selected++;
        if (o->count) continue;

        if (o->whole && o->repl) {
            /* The match is the whole line, so -o and plain -r agree. */
            prefix(o, name, lineno);
            put_expansion(re, o, line, m);
            putchar('\n');
            continue;
        }
        if (o->only) {
            if (o->invert) continue; /* inverted lines have no matched parts */
            if (o->whole) {
                if (len) {
                    prefix(o, name, lineno);
                    fwrite(line, 1, len, stdout);
                    putchar('\n');
                }
                continue;
            }
            size_t pos = 0;
            while (pos <= len && nre_exec(re, line, len, pos, m, MAX_SPANS)) {
                if (m[0].end > m[0].start) {
                    prefix(o, name, lineno);
                    if (o->repl) {
                        put_expansion(re, o, line, m);
                    } else {
                        fwrite(line + m[0].start, 1, (size_t)(m[0].end - m[0].start), stdout);
                    }
                    putchar('\n');
                    pos = (size_t)m[0].end;
                } else {
                    pos = (size_t)m[0].end + 1; /* skip past an empty match */
                }
            }
            continue;
        }

        prefix(o, name, lineno);
        if (o->repl) {
            size_t n;
            char *s = nre_replace(re, line, len, o->repl, 1, &n, NULL, NULL, 0);
            if (!s) {
                fputs("nfagrep: out of memory\n", stderr);
                exit(2);
            }
            fwrite(s, 1, n, stdout);
            free(s);
        } else {
            fwrite(line, 1, len, stdout);
        }
        putchar('\n');
    }
    free(line);

    if (o->count) {
        if (o->show_name) printf("%s:", name);
        printf("%ld\n", selected);
    }
    return selected;
}

/* Join several patterns into one alternation, (?:p1)|(?:p2)|... */
static char *join_patterns(char **pats, int n)
{
    if (n == 1) return strdup(pats[0]);
    size_t total = 1;
    for (int i = 0; i < n; i++) total += strlen(pats[i]) + 6;
    char *out = malloc(total), *w = out;
    if (!out) return NULL;
    for (int i = 0; i < n; i++)
        w += sprintf(w, "%s(?:%s)", i ? "|" : "", pats[i]);
    return out;
}

int main(int argc, char **argv)
{
    Options o = {0};
    int flags = 0, opt, npats = 0;
    char **pats = calloc((size_t)argc, sizeof *pats);
    if (!pats) return 2;

    while ((opt = getopt(argc, argv, "ce:inor:vwxh")) != -1) {
        switch (opt) {
        case 'c': o.count = 1; break;
        case 'e': pats[npats++] = optarg; break;
        case 'i': flags |= NRE_ICASE; break;
        case 'n': o.number = 1; break;
        case 'o': o.only = 1; break;
        case 'r': o.repl = optarg; break;
        case 'v': o.invert = 1; break;
        case 'w': flags |= NRE_WORD; break;
        case 'x': o.whole = 1; break;
        case 'h': usage(stdout); free(pats); return 0;
        default: usage(stderr); free(pats); return 2;
        }
    }
    if (npats == 0) {
        if (optind >= argc) {
            usage(stderr);
            free(pats);
            return 2;
        }
        pats[npats++] = argv[optind++];
    }
    if (o.repl && o.invert) {
        fputs("nfagrep: -r can't be combined with -v\n", stderr);
        free(pats);
        return 2;
    }

    /* Compile each pattern on its own first so errors point at the right one. */
    char err[160];
    for (int i = 0; i < npats; i++) {
        nre *one = nre_compile(pats[i], flags, err, sizeof err);
        if (!one) {
            fprintf(stderr, "nfagrep: bad pattern '%s': %s\n", pats[i], err);
            free(pats);
            return 2;
        }
        nre_free(one);
    }
    char *joined = join_patterns(pats, npats);
    if (!joined) snprintf(err, sizeof err, "out of memory");
    nre *re = joined ? nre_compile(joined, flags, err, sizeof err) : NULL;
    free(joined);
    free(pats);
    if (!re) {
        fprintf(stderr, "nfagrep: bad pattern: %s\n", err);
        return 2;
    }
    if (o.repl) {
        nre_span probe = { 0, 0 };
        char *s = nre_expand(re, o.repl, "", &probe, 1, NULL, err, sizeof err);
        if (!s) {
            fprintf(stderr, "nfagrep: bad replacement: %s\n", err);
            nre_free(re);
            return 2;
        }
        free(s);
    }

    int nfiles = argc - optind, errors = 0;
    long selected = 0;
    o.show_name = nfiles > 1;

    if (nfiles == 0) {
        selected += grep_stream(re, stdin, "(standard input)", &o);
    }
    for (int i = optind; i < argc; i++) {
        const char *name = argv[i];
        if (strcmp(name, "-") == 0) {
            selected += grep_stream(re, stdin, "(standard input)", &o);
            continue;
        }
        FILE *fp = fopen(name, "rb");
        if (!fp) {
            fprintf(stderr, "nfagrep: %s: %s\n", name, strerror(errno));
            errors = 1;
            continue;
        }
        selected += grep_stream(re, fp, name, &o);
        fclose(fp);
    }
    fflush(stdout);
    nre_free(re);
    if (errors) return 2;
    return selected > 0 ? 0 : 1;
}
