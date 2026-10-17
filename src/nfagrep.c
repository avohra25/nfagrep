/* nfagrep - a small grep built on nre. */
#include "nre.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    int invert, count, number, only, whole, show_name;
} Options;

static void usage(FILE *out)
{
    fputs("usage: nfagrep [-cinovx] PATTERN [FILE...]\n"
          "  -c  print only a count of selected lines\n"
          "  -i  ignore ASCII case\n"
          "  -n  prefix each line with its line number\n"
          "  -o  print only the matched parts of each line\n"
          "  -v  select lines that do not match\n"
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

static long grep_stream(const nre *re, FILE *fp, const char *name, const Options *o)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t got;
    long lineno = 0, selected = 0;
    nre_span m;

    while ((got = getline(&line, &cap, fp)) != -1) {
        size_t len = (size_t)got;
        lineno++;
        if (len && line[len - 1] == '\n') len--;

        int hit = o->whole ? nre_fullmatch(re, line, len, NULL, 0)
                           : nre_exec(re, line, len, 0, NULL, 0);
        if (hit == o->invert) continue;
        selected++;
        if (o->count) continue;

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
            while (pos <= len && nre_exec(re, line, len, pos, &m, 1)) {
                if (m.end > m.start) {
                    prefix(o, name, lineno);
                    fwrite(line + m.start, 1, (size_t)(m.end - m.start), stdout);
                    putchar('\n');
                    pos = (size_t)m.end;
                } else {
                    pos = (size_t)m.end + 1; /* skip past an empty match */
                }
            }
            continue;
        }

        prefix(o, name, lineno);
        fwrite(line, 1, len, stdout);
        putchar('\n');
    }
    free(line);

    if (o->count) {
        if (o->show_name) printf("%s:", name);
        printf("%ld\n", selected);
    }
    return selected;
}

int main(int argc, char **argv)
{
    Options o = {0};
    int flags = 0, opt;

    while ((opt = getopt(argc, argv, "cinovxh")) != -1) {
        switch (opt) {
        case 'c': o.count = 1; break;
        case 'i': flags |= NRE_ICASE; break;
        case 'n': o.number = 1; break;
        case 'o': o.only = 1; break;
        case 'v': o.invert = 1; break;
        case 'x': o.whole = 1; break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (optind >= argc) {
        usage(stderr);
        return 2;
    }

    char err[160];
    nre *re = nre_compile(argv[optind], flags, err, sizeof err);
    if (!re) {
        fprintf(stderr, "nfagrep: bad pattern: %s\n", err);
        return 2;
    }

    int nfiles = argc - optind - 1, errors = 0;
    long selected = 0;
    o.show_name = nfiles > 1;

    if (nfiles == 0) {
        selected += grep_stream(re, stdin, "(standard input)", &o);
    }
    for (int i = optind + 1; i < argc; i++) {
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
