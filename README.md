# nfagrep

A small regular expression engine in C99 that compiles patterns to a Thompson
NFA and matches them with a Pike VM, so matching runs in linear time with no
backtracking. `nfagrep` is a small grep built on it.

Build and run the tests with `make test`. MIT licensed.
