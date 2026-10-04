# vida — vda

![vda icon](assets/vda-icon-64.png)

**vda** is a tiny, fast, compute-first scripting language written in portable C11.
One static library, one CLI, no dependencies. It runs `.vda` scripts through a
bytecode VM that beats Python on tight numeric loops while staying small enough
to embed anywhere.

```js
fn fact(n) { if n <= 1 { return 1 } return n * fact(n - 1) }
print(fact(10))   // 3628800

let r = 5
print(3.14 * pow(r, 2))   // 78.5
```

## Build & test

```sh
make          # builds build/vda
make test     # runs the test suite
```

Requires only a C11 compiler. Math builtins link `-lm` automatically.

## Run

```sh
./build/vda run hello.vda     # run a script
./build/vda eval 'print(2+3)' # one-liner
./build/vda version           # vida 0.2.0
./build/vda help              # usage + builtin list
```

## The language

**Numbers first.** 64-bit ints and doubles with mixed-type arithmetic, integer
division for ints (`7 / 2` is `3`, `7 / 2.0` is `3.5`), modulo, and scientific
literals (`1e10`, `2.5e-3`).

```js
let x = 42
let pi = 3.14
print(x + y * 2 % 5)
print(sqrt(3*3 + 4*4))   // 5
```

**Math builtins.** `sqrt sin cos tan asin acos atan atan2 pow exp log log10
abs floor ceil round min max rand srand`, plus `str int float type len`.

**Functions.** First-class, recursive, with arity checking:

```js
fn add(a, b) { return a + b }
let sq = fn(x) { return x * x }
print(add(sq(3), sq(4)))   // 25
```

**Control flow.** `if`/`else` (plus `else if` by nesting), `while`, blocks.
`for` loops and closures are on the roadmap — nested `fn` binds globally for now.

**Strings & booleans.** Concatenation with `+`, `and`/`or`/`not`, `true`/`false`/`nil`.

**Strict by design — use it or remove it.** Declaring something you never read
is a compile **error** for locals and a **warning** for globals. Name it `_` to
opt out explicitly.

```sh
$ vda eval '{ let q = 1; print("x") }'
[line 2] Error at 'q': declared 'q' and never used (use it or remove it)
```

## Speed

Same machine, same algorithms (200k-iteration loops):

| bench | vda | python3 | sb |
|---|---|---|---|
| sum 1M | ~0.06s | ~0.20s | ~3.7s |
| sqrt+sin 200k | 0.038s | 0.141s | — |
| fib(20) recursion | 0.003s | 0.051s | — |

Tricks inside: slot-indexed globals, threaded dispatch, int fast-paths, fused
`i = i + 1` / `s = s + x` superinstructions, exponential string append,
compile-time constant folding, zero-copy `print`.

## Layout

```
include/   public headers (vida_vm.h, vida_compiler.h, vida_value.h, ...)
src/       lexer, parser/compiler, VM, native builtins, CLI
assets/    icon (SVG + PNG)
examples/  hello.vda tour
tests/     test programs
```

## See also

* **[ShimbaBomb (sb)](https://github.com/Shimba-crypto/shimbabomb)** —
  English-like scripting that compiles to native C (`set x to 10.` / `say x.`).
  The friendly, readable counterpart to vda's compute-first minimalism.

## Embed

```c
#include "vida_vm.h"
#include "vida_compiler.h"

VidaChunk chunk; vida_chunk_init(&chunk);
if (vida_compile("print(sqrt(16))", &chunk)) {
    VidaVM vm; vida_vm_init(&vm);
    vida_vm_run(&vm, &chunk);
    vida_vm_free(&vm);
}
vida_chunk_free(&chunk);
```
