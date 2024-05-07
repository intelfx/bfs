Using `bfs`
===========

`bfs` has the same command line syntax as `find`, and almost any `find` command that works with a major `find` implementation will also work with `bfs`.
When invoked with no arguments, `bfs` will list everything under the current directory recursively, breadth-first:

```console
$ bfs
.
./LICENSE
./Makefile
./README.md
./completions
./docs
./src
./tests
./completions/bfs.bash
./completions/bfs.zsh
./docs/BUILDING.md
./docs/CHANGELOG.md
./docs/CONTRIBUTING.md
./docs/USAGE.md
./docs/bfs.1
...
```


Paths
-----

Arguments that don't begin with `-` are treated as paths to search.
If one or more paths are specified, they are used instead of the current directory:

```console
$ bfs /usr/bin /usr/lib
/usr/bin
/usr/lib
/usr/bin/bfs
...
/usr/lib/libc.so
...
```


Expressions
-----------

Arguments that start with `-` form an *expression* which `bfs` evaluates to filter the matched files, and to do things with the files that match.
The most common expression is probably `-name`, which matches filenames against a glob pattern:

```console
$ bfs -name '*.md'
./README.md
./docs/BUILDING.md
./docs/CHANGELOG.md
./docs/CONTRIBUTING.md
./docs/USAGE.md
```

### Operators

When you put multiple expressions next to each other, both of them must match:

```console
$ bfs -name '*.md' -name '*ING*'
./docs/BUILDING.md
./docs/CONTRIBUTING.md
```

This works because the expressions are implicitly combined with *logical and*.
You could be explicit by writing

```console
$ bfs -name '*.md' -and -name '*ING'`
```

There are other operators like `-or`:

```console
$ bfs -name '*.md' -or -name 'bfs.*'
./README.md
./completions/bfs.bash
./completions/bfs.fish
./completions/bfs.zsh
./docs/BUILDING.md
./docs/CHANGELOG.md
./docs/CONTRIBUTING.md
./docs/USAGE.md
./docs/bfs.1
```

and `-not`:

```console
$ bfs -name '*.md' -and -not -name '*ING*'
./README.md
./docs/CHANGELOG.md
./docs/USAGE.md
```

### Actions

Every `bfs` expression returns either `true` or `false`.
For expressions like `-name`, that's all they do.
But some expressions, called *actions*, have other side effects.

If no actions are included in the expression, `bfs` adds the `-print` action automatically, which is why the above examples actually print any output.
The default `-print` is supressed if any actions are given explicitly.
Available actions include printing with alternate formats (`-ls`, `-printf`, etc.), executing commands (`-exec`, `-execdir`, etc.), deleting files (`-delete`), and stopping the search (`-quit`, `-exit`).


Extensions
----------

`bfs` implements a few extensions not found in other `find` implementations.

### `-exclude`

The `-exclude` operator skips an entire subtree whenever an expression matches.
For example, `-exclude -name .git` will exclude any files or directories named `.git` from the search results.
`-exclude` is easier to use than the standard `-prune` action; compare

    bfs -name config -exclude -name .git

to the equivalent

    find ! \( -name .git -prune \) -name config

Unlike `-prune`, `-exclude` even works in combination with `-depth`/`-delete`.

---

### `-limit`

The `-limit N` action makes `bfs` quit once it gets evaluated `N` times.
Placing it after an action like `-print` limits the number of results that get printed, for example:

```console
$ bfs -s -type f -name '*.txt'
./1.txt
./2.txt
./3.txt
./4.txt
$ bfs -s -type f -name '*.txt' -print -limit 2
./1.txt
./2.txt
```

This is similar to

```console
$ bfs -s -type f -name '*.txt' | head -n2
```

but more powerful because you can apply separate limits to different expressions:

```console
$ bfs \( -name '*.txt' -print -limit 3 -o -name '*.log' -print -limit 4 \) -limit 5
[At most 3 .txt files, at most 4 .log files, and at most 5 in total]
```

and more efficient because it will quit immediately.
When piping to `head`, `bfs` will only quit *after* it tries to output too many results.

---

### `-hidden`/`-nohidden`

`-hidden` matches "hidden" files (dotfiles).
`bfs -hidden` is effectively shorthand for

    find \( -name '.*' -not -name . -not -name .. \)

`-nohidden` is equivalent to `-exclude -hidden`.

---

### `-unique`

This option ensures that `bfs` only visits each file once, even if it's reachable through multiple hard or symbolic links.
It's particularly useful when following symbolic links (`-L`).

---

### `-color`/`-nocolor`

When printing to a terminal, `bfs` automatically colors paths like GNU `ls`, according to the `LS_COLORS` environment variable.
The `-color` and `-nocolor` options override the automatic behavior, which may be handy when you want to preserve colors through a pipe:

    bfs -color | less -R

If the [`NO_COLOR`](https://no-color.org/) environment variable is set, colors will be disabled by default.
