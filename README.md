# nfagrep

A small regular expression engine in C99, plus a grep-like command-line tool
built on it. Patterns compile to a Thompson NFA and run on a Pike VM, so
matching takes time proportional to `len(pattern) × len(text)` no matter
what the pattern is. There is no backtracking, so patterns like `(a*)*b` or
`(a|aa)+$` can't blow up.

It has no dependencies beyond the C standard library (the CLI also uses
POSIX `getline` and `getopt`).

## Build and test

```sh
make            # build/libnre.a and build/nfagrep
make test       # unit tests and CLI end-to-end tests
make sanitize   # unit tests under AddressSanitizer and UBSan
make bench      # nre against a naive backtracking matcher
make install PREFIX=$HOME/.local
```

## Using the CLI

```
usage: nfagrep [-cinovx] PATTERN [FILE...]
  -c  print only a count of selected lines
  -i  ignore ASCII case
  -n  prefix each line with its line number
  -o  print only the matched parts of each line
  -v  select lines that do not match
  -x  the pattern must match the whole line
```

```sh
$ nfagrep -n '\bTODO\b' src/*.c
$ printf 'call 555-1234\nno number\n' | nfagrep -o '\d{3}-\d{4}'
555-1234
$ nfagrep -ci 'error|warn' build.log
```

The exit status is 0 if any line was selected, 1 if none were, and 2 on a bad
pattern or an unreadable file, the same as grep.

## Using the library

```c
#include "nre.h"

char err[128];
nre *re = nre_compile("(\\w+)@(\\w+)\\.com", 0, err, sizeof err);
if (!re) { fprintf(stderr, "%s\n", err); return 1; }

const char *text = "mail ada@example.com today";
nre_span m[3];
if (nre_exec(re, text, strlen(text), 0, m, 3))
    printf("user is %.*s\n", (int)(m[1].end - m[1].start), text + m[1].start);
nre_free(re);
```

| Function | What it does |
|---|---|
| `nre_compile(pattern, flags, err, errlen)` | Compiles a pattern. Pass `NRE_ICASE` for ASCII case-insensitive matching. On error it returns `NULL` and writes a message with the offset into `err`. |
| `nre_exec(re, text, len, start, m, nm)` | Finds the leftmost match starting at or after `start`. `m[0]` is the whole match and `m[i]` is group `i`; groups that did not take part are `-1`. Anchors and `\b` still see the text before `start`, so you can loop over matches. |
| `nre_fullmatch(re, text, len, m, nm)` | Like `nre_exec`, but the match has to cover the whole text. |
| `nre_groups(re)` | The number of capture groups. |
| `nre_free(re)` | Frees a compiled pattern. |

Text is a byte buffer with an explicit length, so embedded NUL bytes are fine.
A compiled `nre` is never modified by matching and can be shared between
threads.

## Supported syntax

| Syntax | Meaning |
|---|---|
| `abc` | literal bytes |
| `.` | any byte except newline |
| `[abc]`, `[a-z]`, `[^0-9]` | character classes; `]` first or `-` at either end is literal |
| `\d \w \s` and `\D \W \S` | digits, word characters, whitespace (ASCII), and their complements; these work inside classes too |
| `\n \t \r \f \v \xHH` | control characters and a hex byte |
| `\.` `\*` `\\` and so on | escaped punctuation is literal |
| `^` `$` | start and end of the text |
| `\b` `\B` | word boundary and non-boundary |
| `x*` `x+` `x?` | zero or more, one or more, zero or one |
| `x{n}` `x{n,}` `x{n,m}` | counted repetition, with counts up to 1000 |
| `x*?` `x+?` `x??` `x{n,m}?` | lazy versions of the above |
| `a\|b` | alternation |
| `(x)` | capture group (up to 32) |
| `(?:x)` | group without capturing |

A `{` that does not start a well-formed count, as in `a{,2}` or `x{`, is a
literal brace. Lookaround and backreferences aren't supported, because neither
can be matched in linear time.

## How it works

1. **Parsing.** A recursive-descent parser turns the pattern into a small
   AST. Character classes become 256-bit sets, and with `NRE_ICASE` letters
   are folded into classes at this point, so matching never has to think
   about case.
2. **Compiling.** The AST is compiled to a program for a tiny virtual
   machine with these instructions: `CHAR`, `ANY`, `CLASS`, `ASSERT`, `SPLIT`
   (try two paths, the first one preferred), `JMP`, `SAVE` (record a capture
   position) and `MATCH`. Counted repetition is unrolled, which is why counts
   are capped and the whole program is limited to 10,000 instructions. Each
   program starts with a lazy `.*?` loop so a search can begin anywhere.
3. **Matching.** The Pike VM keeps a list of threads, each one a program
   counter plus its capture positions. For each input byte it advances all
   threads in priority order and builds the next list. A program counter is
   added at most once per step, which keeps the work per byte bounded by the
   program size. When a thread reaches `MATCH`, every lower-priority thread
   is dropped, and that gives leftmost-first results like Perl, Python and
   RE2: `a|ab` matches `a` in `ab`, `a+` is greedy and `a+?` is lazy.

Results match Python's `re` module, with one exception that RE2 and Go share:
when a loop body can match the empty string (for example `(a?)*` or
`(a*|b)+`), the two can disagree about which empty iteration a capture group
records, and now and then about where the match ends. A quick differential
check over 4,000 random patterns found about a dozen such cases and no other
differences.

## Benchmark

`make bench` runs the textbook worst case `a?ⁿaⁿ` against `aⁿ`. On the
machine this was written on:

```
   n      nre (ms)  backtrack (ms)
  10         0.004         0.012
  16         0.004         0.687
  20         0.007        13.789
  24         0.016       257.589
  28         0.010      4693.045
```

The backtracking time roughly quadruples every time `n` goes up by 2, while
nre stays in the microseconds.

## Layout

```
include/nre.h      public API
src/nre.c          parser, compiler and Pike VM
src/nfagrep.c      the command-line tool
tests/test_nre.c   unit tests
tests/cli_test.sh  end-to-end tests for nfagrep
bench/bench.c      pathological-pattern benchmark
```

## License

MIT, see [LICENSE](LICENSE).
