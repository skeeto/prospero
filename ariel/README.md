# ariel

Terminal emulation, xterm-256color class, in one C11 header and source.
ariel parses a child program's output into screens and history, answers
its queries, and encodes keys, mouse events and pastes for it. Drawing,
the PTY and the clipboard stay with the host.

- No allocation: the host hands each terminal one block, sized by
  `ariel_size`. Internal references are offsets, so a block can be
  copied (a fork) or moved.
- Nothing from libc but `memcpy`, `memmove`, `memset` and `memcmp`,
  which compilers emit. No I/O, globals, callbacks or floating point.
- Events, including replies for the child, come back from `ariel_feed`
  one at a time, in order, so the host never re-enters ariel.
- UTF-8 only. Grapheme clusters (UAX #29) by default, with mode 2027.

See `ariel.h` for the API. Status: in development inside Prospero, its
first user; the API isn't frozen.

## Building

    $ cc -std=c11 -c -O2 ariel.c

Or with CMake, on its own (tests and tools included) or through
`add_subdirectory` (the `ariel` object library):

    $ cmake -B build && cmake --build build && ctest --test-dir build

Define `ARIEL_TRAP` to make assertions trap in release builds too.

## Layout

    ariel.h, ariel.c      the library
    tests/test.c          unit tests and the *.test script runner
    tests/*.test          scripts in a libvterm-like language
    tests/fuzz.c          AFL++ target with an invariant checker
    tools/replay.c        ariel-replay: dump or benchmark a recording
    tools/gentables.py    Unicode tables, spliced into ariel.c (uv run)

## License

ariel is in the public domain (UNLICENSE). Test scripts ported from
libvterm keep its MIT notice in their headers.

The Unicode tables in `ariel.c` and the data in `tests/grapheme.txt`
and `tests/emoji.txt` derive from the Unicode Character Database:

> UNICODE LICENSE V3
>
> COPYRIGHT AND PERMISSION NOTICE
>
> Copyright © 1991-2026 Unicode, Inc.
>
> NOTICE TO USER: Carefully read the following legal agreement. BY
> DOWNLOADING, INSTALLING, COPYING OR OTHERWISE USING DATA FILES, AND/OR
> SOFTWARE, YOU UNEQUIVOCALLY ACCEPT, AND AGREE TO BE BOUND BY, ALL OF THE
> TERMS AND CONDITIONS OF THIS AGREEMENT. IF YOU DO NOT AGREE, DO NOT
> DOWNLOAD, INSTALL, COPY, DISTRIBUTE OR USE THE DATA FILES OR SOFTWARE.
>
> Permission is hereby granted, free of charge, to any person obtaining a
> copy of data files and any associated documentation (the "Data Files") or
> software and any associated documentation (the "Software") to deal in the
> Data Files or Software without restriction, including without limitation
> the rights to use, copy, modify, merge, publish, distribute, and/or sell
> copies of the Data Files or Software, and to permit persons to whom the
> Data Files or Software are furnished to do so, provided that either (a)
> this copyright and permission notice appear with all copies of the Data
> Files or Software, or (b) this copyright and permission notice appear in
> associated Documentation.
>
> THE DATA FILES AND SOFTWARE ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
> KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
> MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
> THIRD PARTY RIGHTS.
>
> IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS INCLUDED IN THIS NOTICE
> BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES,
> OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
> WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
> ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THE DATA
> FILES OR SOFTWARE.
>
> Except as contained in this notice, the name of a copyright holder shall
> not be used in advertising or otherwise to promote the sale, use or other
> dealings in these Data Files or Software without prior written
> authorization of the copyright holder.
