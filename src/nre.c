/*
 * nre.c - regex parser, Thompson-NFA compiler and Pike VM.
 *
 * Pipeline: pattern --(recursive descent)--> AST --(codegen)--> program of
 * VM instructions --(Pike VM)--> match. The VM advances every live thread in
 * lock step over the input, one byte at a time, and never revisits a byte,
 * which is what gives the linear-time guarantee.
 */
#include "nre.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_REPEAT 1000  /* largest count allowed in {m,n} */
#define MAX_INST 10000   /* program size limit, bounds memory and recursion */
#define MAX_GROUPS 32    /* capture groups per pattern */

/* ---------- ASCII helpers (locale independent) ---------- */

static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_word(int c) { return is_alpha(c) || is_digit(c) || c == '_'; }
static int is_space(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static int to_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int to_upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static int hex_val(int c)
{
    if (is_digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

typedef uint8_t ByteSet[32];

static void set_add(uint8_t *s, int c) { s[c >> 3] |= (uint8_t)(1u << (c & 7)); }
static int set_has(const uint8_t *s, int c) { return (s[c >> 3] >> (c & 7)) & 1; }

/* ---------- AST ---------- */

typedef enum { N_EMPTY, N_CHAR, N_ANY, N_CLASS, N_ASSERT, N_CAT, N_ALT, N_REPEAT, N_GROUP } NodeType;
enum { A_BOL, A_EOL, A_WORDB, A_NWORDB };

typedef struct Node {
    NodeType type;
    int c;        /* char (N_CHAR), class index (N_CLASS), kind (N_ASSERT), group (N_GROUP) */
    int min, max; /* N_REPEAT bounds; max == -1 means unbounded */
    int greedy;   /* N_REPEAT */
    struct Node *a, *b;
} Node;

typedef struct {
    const char *p;
    const char *start;
    int flags;
    int ngroups;
    Node **nodes;
    size_t nnodes, capnodes;
    ByteSet *classes;
    int nclasses, capclasses;
    char *err;
    size_t errlen;
    int failed;
} Parser;

static void *fail(Parser *ps, const char *fmt, ...)
{
    if (ps->failed) return NULL;
    ps->failed = 1;
    if (ps->err && ps->errlen) {
        char msg[128];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
        snprintf(ps->err, ps->errlen, "%s at offset %ld", msg, (long)(ps->p - ps->start));
    }
    return NULL;
}

static Node *new_node(Parser *ps, NodeType type)
{
    if (ps->nnodes == ps->capnodes) {
        size_t cap = ps->capnodes ? ps->capnodes * 2 : 32;
        Node **grown = realloc(ps->nodes, cap * sizeof *grown);
        if (!grown) return fail(ps, "out of memory");
        ps->nodes = grown;
        ps->capnodes = cap;
    }
    Node *n = calloc(1, sizeof *n);
    if (!n) return fail(ps, "out of memory");
    n->type = type;
    ps->nodes[ps->nnodes++] = n;
    return n;
}

static Node *class_node(Parser *ps, const uint8_t *bits)
{
    if (ps->nclasses == ps->capclasses) {
        int cap = ps->capclasses ? ps->capclasses * 2 : 8;
        ByteSet *grown = realloc(ps->classes, (size_t)cap * sizeof *grown);
        if (!grown) return fail(ps, "out of memory");
        ps->classes = grown;
        ps->capclasses = cap;
    }
    Node *n = new_node(ps, N_CLASS);
    if (!n) return NULL;
    memcpy(ps->classes[ps->nclasses], bits, sizeof(ByteSet));
    n->c = ps->nclasses++;
    return n;
}

static Node *char_node(Parser *ps, int c)
{
    if ((ps->flags & NRE_ICASE) && is_alpha(c)) {
        ByteSet bits = {0};
        set_add(bits, to_lower(c));
        set_add(bits, to_upper(c));
        return class_node(ps, bits);
    }
    Node *n = new_node(ps, N_CHAR);
    if (n) n->c = c;
    return n;
}

static Node *binary(Parser *ps, NodeType type, Node *a, Node *b)
{
    Node *n = new_node(ps, type);
    if (n) {
        n->a = a;
        n->b = b;
    }
    return n;
}

/* \d \D \w \W \s \S: add the named set (or its complement) to bits. */
static int add_named_class(uint8_t *bits, int e)
{
    int kind = to_lower(e);
    if (kind != 'd' && kind != 'w' && kind != 's') return 0;
    int negate = e != kind;
    for (int c = 0; c < 256; c++) {
        int in = kind == 'd' ? is_digit(c) : kind == 'w' ? is_word(c) : is_space(c);
        if (in != negate) set_add(bits, c);
    }
    return 1;
}

/* Single-character escapes shared by atoms and classes. ps->p is just past e. */
static int escape_char(Parser *ps, int e, int *out)
{
    switch (e) {
    case 'n': *out = '\n'; return 1;
    case 't': *out = '\t'; return 1;
    case 'r': *out = '\r'; return 1;
    case 'f': *out = '\f'; return 1;
    case 'v': *out = '\v'; return 1;
    case 'x': {
        int hi = hex_val((unsigned char)ps->p[0]);
        int lo = hi < 0 ? -1 : hex_val((unsigned char)ps->p[1]);
        if (lo < 0) {
            fail(ps, "\\x needs two hex digits");
            return 0;
        }
        ps->p += 2;
        *out = hi * 16 + lo;
        return 1;
    }
    default:
        if (is_word(e)) {
            fail(ps, "unknown escape \\%c", e);
            return 0;
        }
        *out = e;
        return 1;
    }
}

static Node *parse_alt(Parser *ps);

static Node *parse_class(Parser *ps)
{
    ByteSet bits = {0};
    int negate = 0, first = 1;
    ps->p++; /* '[' */
    if (*ps->p == '^') {
        negate = 1;
        ps->p++;
    }
    for (;;) {
        int ch = (unsigned char)*ps->p;
        if (!ch) return fail(ps, "missing ']'");
        if (ch == ']' && !first) {
            ps->p++;
            break;
        }
        first = 0;
        int lo, hi;
        if (ch == '\\') {
            int e = (unsigned char)ps->p[1];
            if (!e) return fail(ps, "trailing backslash");
            ps->p += 2;
            if (add_named_class(bits, e)) continue;
            if (!escape_char(ps, e, &lo)) return NULL;
        } else {
            lo = ch;
            ps->p++;
        }
        hi = lo;
        if (ps->p[0] == '-' && ps->p[1] && ps->p[1] != ']') {
            ps->p++;
            int h = (unsigned char)*ps->p;
            if (h == '\\') {
                int e = (unsigned char)ps->p[1];
                if (!e) return fail(ps, "trailing backslash");
                ps->p += 2;
                if (to_lower(e) == 'd' || to_lower(e) == 'w' || to_lower(e) == 's')
                    return fail(ps, "bad character range");
                if (!escape_char(ps, e, &hi)) return NULL;
            } else {
                hi = h;
                ps->p++;
            }
            if (hi < lo) return fail(ps, "bad character range");
        }
        for (int c = lo; c <= hi; c++) set_add(bits, c);
    }
    if (ps->flags & NRE_ICASE) {
        for (int c = 'a'; c <= 'z'; c++) {
            if (set_has(bits, c) || set_has(bits, to_upper(c))) {
                set_add(bits, c);
                set_add(bits, to_upper(c));
            }
        }
    }
    if (negate)
        for (int i = 0; i < 32; i++) bits[i] = (uint8_t)~bits[i];
    return class_node(ps, bits);
}

static Node *parse_escape(Parser *ps)
{
    int e = (unsigned char)ps->p[1];
    if (!e) return fail(ps, "trailing backslash");
    ps->p += 2;
    if (e == 'b' || e == 'B') {
        Node *n = new_node(ps, N_ASSERT);
        if (n) n->c = e == 'b' ? A_WORDB : A_NWORDB;
        return n;
    }
    ByteSet bits = {0};
    if (add_named_class(bits, e)) return class_node(ps, bits);
    int c;
    if (!escape_char(ps, e, &c)) return NULL;
    return char_node(ps, c);
}

static Node *parse_atom(Parser *ps)
{
    int ch = (unsigned char)*ps->p;
    Node *n;
    switch (ch) {
    case '(': {
        int capture = 1;
        ps->p++;
        if (ps->p[0] == '?') {
            if (ps->p[1] != ':') return fail(ps, "unsupported group syntax");
            capture = 0;
            ps->p += 2;
        }
        int index = capture ? ++ps->ngroups : 0;
        if (index > MAX_GROUPS) return fail(ps, "too many capture groups (max %d)", MAX_GROUPS);
        Node *inner = parse_alt(ps);
        if (!inner) return NULL;
        if (*ps->p != ')') return fail(ps, "missing ')'");
        ps->p++;
        if (!capture) return inner;
        n = new_node(ps, N_GROUP);
        if (n) {
            n->c = index;
            n->a = inner;
        }
        return n;
    }
    case '*': case '+': case '?':
        return fail(ps, "nothing to repeat");
    case '.':
        ps->p++;
        return new_node(ps, N_ANY);
    case '^': case '$':
        ps->p++;
        n = new_node(ps, N_ASSERT);
        if (n) n->c = ch == '^' ? A_BOL : A_EOL;
        return n;
    case '[':
        return parse_class(ps);
    case '\\':
        return parse_escape(ps);
    default:
        ps->p++;
        return char_node(ps, ch);
    }
}

/* Parse {m}, {m,} or {m,n} at ps->p. Returns 0 (without consuming) if the
 * brace is not a well-formed quantifier, in which case it is a literal. */
static int parse_braces(Parser *ps, int *min, int *max)
{
    const char *q = ps->p + 1;
    long m = 0, n;
    if (!is_digit((unsigned char)*q)) return 0;
    while (is_digit((unsigned char)*q)) {
        if (m <= MAX_REPEAT) m = m * 10 + (*q - '0');
        q++;
    }
    n = m;
    if (*q == ',') {
        q++;
        if (*q == '}') {
            n = -1;
        } else if (is_digit((unsigned char)*q)) {
            n = 0;
            while (is_digit((unsigned char)*q)) {
                if (n <= MAX_REPEAT) n = n * 10 + (*q - '0');
                q++;
            }
        } else {
            return 0;
        }
    }
    if (*q != '}') return 0;
    if (m > MAX_REPEAT || n > MAX_REPEAT) {
        fail(ps, "repetition count too large (max %d)", MAX_REPEAT);
        return 1;
    }
    if (n != -1 && n < m) {
        fail(ps, "bad repetition range");
        return 1;
    }
    ps->p = q + 1;
    *min = (int)m;
    *max = (int)n;
    return 1;
}

static Node *parse_repeat(Parser *ps)
{
    Node *atom = parse_atom(ps);
    int repeated = 0;
    while (atom) {
        int min = 0, max = 0;
        char ch = *ps->p;
        if (ch == '*') { min = 0; max = -1; ps->p++; }
        else if (ch == '+') { min = 1; max = -1; ps->p++; }
        else if (ch == '?') { min = 0; max = 1; ps->p++; }
        else if (ch == '{' && parse_braces(ps, &min, &max)) { if (ps->failed) return NULL; }
        else break;
        if (repeated) return fail(ps, "multiple repeat");
        if (atom->type == N_ASSERT) return fail(ps, "nothing to repeat");
        repeated = 1;
        int greedy = 1;
        if (*ps->p == '?') {
            greedy = 0;
            ps->p++;
        }
        Node *n = new_node(ps, N_REPEAT);
        if (!n) return NULL;
        n->a = atom;
        n->min = min;
        n->max = max;
        n->greedy = greedy;
        atom = n;
    }
    return atom;
}

static Node *parse_concat(Parser *ps)
{
    Node *result = NULL;
    while (*ps->p && *ps->p != '|' && *ps->p != ')') {
        Node *next = parse_repeat(ps);
        if (!next) return NULL;
        result = result ? binary(ps, N_CAT, result, next) : next;
        if (!result) return NULL;
    }
    return result ? result : new_node(ps, N_EMPTY);
}

static Node *parse_alt(Parser *ps)
{
    Node *left = parse_concat(ps);
    if (!left || *ps->p != '|') return left;
    ps->p++;
    Node *right = parse_alt(ps);
    return right ? binary(ps, N_ALT, left, right) : NULL;
}

/* ---------- code generation ---------- */

enum { I_CHAR, I_ANY, I_ANYBYTE, I_CLASS, I_ASSERT, I_SPLIT, I_JMP, I_SAVE, I_MATCH };

typedef struct {
    int op;
    int x, y; /* SPLIT prefers x over y */
} Inst;

typedef struct {
    Inst *code;
    int n, cap;
    int failed;
} Prog;

static int emit(Prog *pg, int op, int x, int y)
{
    if (pg->failed) return -1;
    if (pg->n >= MAX_INST) {
        pg->failed = 1;
        return -1;
    }
    if (pg->n == pg->cap) {
        int cap = pg->cap ? pg->cap * 2 : 64;
        Inst *grown = realloc(pg->code, (size_t)cap * sizeof *grown);
        if (!grown) {
            pg->failed = 1;
            return -1;
        }
        pg->code = grown;
        pg->cap = cap;
    }
    pg->code[pg->n].op = op;
    pg->code[pg->n].x = x;
    pg->code[pg->n].y = y;
    return pg->n++;
}

static void patch_split(Prog *pg, int at, int prefer, int other, int greedy)
{
    if (pg->failed) return;
    pg->code[at].x = greedy ? prefer : other;
    pg->code[at].y = greedy ? other : prefer;
}

static void gen(Prog *pg, const Node *n);

/* body* : L: SPLIT body, out; body; JMP L; out: */
static void gen_star(Prog *pg, const Node *body, int greedy)
{
    int split = emit(pg, I_SPLIT, 0, 0);
    gen(pg, body);
    emit(pg, I_JMP, split, 0);
    patch_split(pg, split, split + 1, pg->n, greedy);
}

/* body+ : L: body; SPLIT L, out; out: */
static void gen_plus(Prog *pg, const Node *body, int greedy)
{
    int loop = pg->n;
    gen(pg, body);
    int split = emit(pg, I_SPLIT, 0, 0);
    patch_split(pg, split, loop, pg->n, greedy);
}

/* (body(body(...)?)?)? nested `count` deep, used for x? and x{m,n}. */
static void gen_optional(Prog *pg, const Node *body, int count, int greedy)
{
    if (count == 0 || pg->failed) return;
    int split = emit(pg, I_SPLIT, 0, 0);
    gen(pg, body);
    gen_optional(pg, body, count - 1, greedy);
    patch_split(pg, split, split + 1, pg->n, greedy);
}

static void gen_repeat(Prog *pg, const Node *n)
{
    if (n->max == -1) {
        if (n->min == 0) {
            gen_star(pg, n->a, n->greedy);
        } else {
            for (int i = 0; i < n->min - 1 && !pg->failed; i++) gen(pg, n->a);
            gen_plus(pg, n->a, n->greedy);
        }
    } else {
        for (int i = 0; i < n->min && !pg->failed; i++) gen(pg, n->a);
        gen_optional(pg, n->a, n->max - n->min, n->greedy);
    }
}

static void gen(Prog *pg, const Node *n)
{
    if (pg->failed) return;
    switch (n->type) {
    case N_EMPTY: break;
    case N_CHAR: emit(pg, I_CHAR, n->c, 0); break;
    case N_ANY: emit(pg, I_ANY, 0, 0); break;
    case N_CLASS: emit(pg, I_CLASS, n->c, 0); break;
    case N_ASSERT: emit(pg, I_ASSERT, n->c, 0); break;
    case N_CAT:
        gen(pg, n->a);
        gen(pg, n->b);
        break;
    case N_ALT: {
        int split = emit(pg, I_SPLIT, 0, 0);
        gen(pg, n->a);
        int jmp = emit(pg, I_JMP, 0, 0);
        int right = pg->n;
        gen(pg, n->b);
        if (pg->failed) return;
        pg->code[split].x = split + 1;
        pg->code[split].y = right;
        pg->code[jmp].x = pg->n;
        break;
    }
    case N_GROUP:
        emit(pg, I_SAVE, 2 * n->c, 0);
        gen(pg, n->a);
        emit(pg, I_SAVE, 2 * n->c + 1, 0);
        break;
    case N_REPEAT: gen_repeat(pg, n); break;
    }
}

struct nre {
    Inst *code;
    int ninst;
    int main_pc; /* entry point that skips the unanchored-search prefix */
    int ngroups;
    ByteSet *classes;
};

static void free_parser(Parser *ps)
{
    for (size_t i = 0; i < ps->nnodes; i++) free(ps->nodes[i]);
    free(ps->nodes);
    free(ps->classes);
}

nre *nre_compile(const char *pattern, int flags, char *err, size_t errlen)
{
    Parser ps;
    memset(&ps, 0, sizeof ps);
    ps.p = ps.start = pattern ? pattern : "";
    ps.flags = flags;
    ps.err = err;
    ps.errlen = errlen;
    if (err && errlen) err[0] = '\0';
    if (!pattern) {
        fail(&ps, "null pattern");
        return NULL;
    }

    Node *root = parse_alt(&ps);
    if (root && *ps.p == ')') fail(&ps, "unmatched ')'");
    if (ps.failed) {
        free_parser(&ps);
        return NULL;
    }

    /* Program layout:
     *   0: SPLIT 3, 1   lazy .*? prefix so search can start anywhere
     *   1: ANYBYTE
     *   2: JMP 0
     *   3: SAVE 0       main_pc
     *      ...pattern...
     *      SAVE 1
     *      MATCH
     */
    Prog pg;
    memset(&pg, 0, sizeof pg);
    emit(&pg, I_SPLIT, 3, 1);
    emit(&pg, I_ANYBYTE, 0, 0);
    emit(&pg, I_JMP, 0, 0);
    emit(&pg, I_SAVE, 0, 0);
    gen(&pg, root);
    emit(&pg, I_SAVE, 1, 0);
    emit(&pg, I_MATCH, 0, 0);

    nre *re = NULL;
    if (pg.failed) {
        ps.p = ps.start + strlen(ps.start);
        fail(&ps, "pattern too large");
    } else if ((re = calloc(1, sizeof *re)) == NULL) {
        fail(&ps, "out of memory");
    } else {
        re->code = pg.code;
        re->ninst = pg.n;
        re->main_pc = 3;
        re->ngroups = ps.ngroups;
        re->classes = ps.classes;
        ps.classes = NULL; /* ownership moved */
        pg.code = NULL;
    }
    free(pg.code);
    free_parser(&ps);
    return re;
}

size_t nre_groups(const nre *re) { return re ? (size_t)re->ngroups : 0; }

void nre_free(nre *re)
{
    if (!re) return;
    free(re->code);
    free(re->classes);
    free(re);
}

/* ---------- Pike VM ---------- */

typedef struct {
    int *pc;
    long *caps; /* n * nslots capture slots, one row per thread */
    int n;
} ThreadList;

typedef struct {
    const nre *re;
    const unsigned char *text;
    long len;
    int nslots;
    unsigned *mark; /* mark[pc] == gen means pc is already in the list being built */
    unsigned gen;
} VM;

static int assertion_holds(const VM *vm, int kind, long pos)
{
    switch (kind) {
    case A_BOL: return pos == 0;
    case A_EOL: return pos == vm->len;
    default: {
        int before = pos > 0 && is_word(vm->text[pos - 1]);
        int after = pos < vm->len && is_word(vm->text[pos]);
        return (before != after) == (kind == A_WORDB);
    }
    }
}

/* Follow empty transitions from pc and add every reachable consuming
 * instruction to the list in priority order. Each pc is added at most once
 * per step, which is what keeps the simulation linear. */
static void add_thread(VM *vm, ThreadList *l, int pc, long *caps, long pos)
{
    if (vm->mark[pc] == vm->gen) return;
    vm->mark[pc] = vm->gen;
    const Inst *in = &vm->re->code[pc];
    switch (in->op) {
    case I_JMP:
        add_thread(vm, l, in->x, caps, pos);
        return;
    case I_SPLIT:
        add_thread(vm, l, in->x, caps, pos);
        add_thread(vm, l, in->y, caps, pos);
        return;
    case I_SAVE: {
        long old = caps[in->x];
        caps[in->x] = pos;
        add_thread(vm, l, pc + 1, caps, pos);
        caps[in->x] = old;
        return;
    }
    case I_ASSERT:
        if (assertion_holds(vm, in->x, pos)) add_thread(vm, l, pc + 1, caps, pos);
        return;
    default:
        l->pc[l->n] = pc;
        memcpy(l->caps + (size_t)l->n * vm->nslots, caps, (size_t)vm->nslots * sizeof *caps);
        l->n++;
    }
}

static int run(const nre *re, const char *text, size_t len, size_t start, int anchored,
               nre_span *m, size_t nm)
{
    if (!re || (!text && len) || start > len) return 0;

    VM vm;
    vm.re = re;
    vm.text = (const unsigned char *)text;
    vm.len = (long)len;
    vm.nslots = 2 * (re->ngroups + 1);
    vm.gen = 0;

    size_t ninst = (size_t)re->ninst, nslots = (size_t)vm.nslots;
    vm.mark = calloc(ninst, sizeof *vm.mark);
    int *pcs = malloc(2 * ninst * sizeof *pcs);
    long *caps = malloc((2 * ninst + 2) * nslots * sizeof *caps);
    if (!vm.mark || !pcs || !caps) {
        free(vm.mark);
        free(pcs);
        free(caps);
        return 0;
    }
    ThreadList a = { pcs, caps, 0 }, b = { pcs + ninst, caps + ninst * nslots, 0 };
    ThreadList *clist = &a, *nlist = &b;
    long *scratch = caps + 2 * ninst * nslots, *best = scratch + nslots;
    for (size_t i = 0; i < nslots; i++) scratch[i] = -1;

    int matched = 0;
    vm.gen++;
    add_thread(&vm, clist, anchored ? re->main_pc : 0, scratch, (long)start);

    for (long sp = (long)start; clist->n > 0; sp++) {
        int c = sp < vm.len ? vm.text[sp] : -1;
        vm.gen++;
        nlist->n = 0;
        for (int i = 0; i < clist->n; i++) {
            const Inst *in = &re->code[clist->pc[i]];
            long *tcaps = clist->caps + (size_t)i * nslots;
            int ok = 0, stop = 0;
            switch (in->op) {
            case I_CHAR: ok = c == in->x; break;
            case I_ANY: ok = c >= 0 && c != '\n'; break;
            case I_ANYBYTE: ok = c >= 0; break;
            case I_CLASS: ok = c >= 0 && set_has(re->classes[in->x], c); break;
            case I_MATCH:
                if (anchored && sp != vm.len) break;
                matched = 1;
                memcpy(best, tcaps, nslots * sizeof *best);
                stop = 1; /* lower-priority threads can only lose */
                break;
            }
            if (stop) break;
            if (ok) add_thread(&vm, nlist, clist->pc[i] + 1, tcaps, sp + 1);
        }
        ThreadList *t = clist;
        clist = nlist;
        nlist = t;
        if (c < 0) break;
    }

    if (matched && m) {
        for (size_t i = 0; i < nm; i++) {
            if (i <= (size_t)re->ngroups) {
                m[i].start = best[2 * i];
                m[i].end = best[2 * i + 1];
            } else {
                m[i].start = m[i].end = -1;
            }
        }
    }
    free(vm.mark);
    free(pcs);
    free(caps);
    return matched;
}

int nre_exec(const nre *re, const char *text, size_t len, size_t start, nre_span *m, size_t nm)
{
    return run(re, text, len, start, 0, m, nm);
}

int nre_fullmatch(const nre *re, const char *text, size_t len, nre_span *m, size_t nm)
{
    return run(re, text, len, 0, 1, m, nm);
}
