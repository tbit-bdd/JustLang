# Just Language

*A lightweight embeddable scripting language written in pure C.*

---

## What is Just?

**Just** is a small scripting language built from scratch in C.

- **Single binary** — no dependencies, no package manager, no runtime
- **Cross-platform** — Windows and Linux from the same code
- **Familiar syntax** — if you know JavaScript or Python, you'll feel at home
- **Tracing GC** — mark-and-sweep garbage collector
- **Modules** — split code across files with `import`
- **C plugins** — extend the language with native code
- **50+ builtins** — JSON, HTTP, files, strings, math, and more

It compiles in seconds and runs anywhere. Good for automation scripts, learning how languages work, or embedding into larger C/C++ projects.

---

## Quick Start

### Build

**Windows (MinGW):**
```bash
gcc -o just.exe main.c -lws2_32 -lm
```

**Linux:**
```bash
gcc -o just main.c -lm -ldl
```

### Run a script
```bash
just hello.just
```

### Or try the REPL
```bash
just
```

**hello.just:**
```javascript
print("Hello from Just!")
```

---

## Syntax at a Glance

```javascript
// Variables
let name = "Just"
const PI = 3.14

// Objects
let user = { name: "Alex", age: 25 }

// Arrays
let items = [1, 2, 3]

// Functions
func greet(name) {
    return "Hello, " + name
}

// Conditions and loops
if (user.age >= 18) {
    print("Adult")
}

for (let i = 0; i < 5; i = i + 1) {
    print(i)
}
```

---

## Modules (import)

Split your code across multiple files with `import`:

**math.just:**
```javascript
func add(a, b) {
    return a + b
}

func multiply(a, b) {
    return a * b
}
```

**main.just:**
```javascript
import "math.just"

print(add(10, 20))       // 30
print(multiply(5, 6))    // 30
```

Imports are loaded once and share the global scope. Good for organizing larger projects.

---

## Built-in Functions

| Category | Functions |
|----------|-----------|
| **Output** | `print` |
| **Types** | `type`, `int`, `str`, `bool` |
| **Input** | `input` |
| **Strings** | `upper`, `lower`, `trim`, `split`, `join`, `replace`, `contains`, `len` |
| **Math** | `sqrt`, `pow`, `abs`, `min`, `max`, `floor`, `ceil`, `round`, `random` |
| **JSON** | `json`, `json_parse`, `read_json`, `write_json`, `export` |
| **HTTP** | `http_get`, `http_post` |
| **Files** | `read`, `write`, `exists`, `import_json` |
| **Time** | `now`, `sleep` |
| **System** | `exec`, `env`, `load_plugin` |
| **Modules** | `import` |
| **Collections** | `len`, `range`, `filter`, `map`, `keys`, `values`, `has`, `array_push`, `array_pop` |
| **Colors** | `red`, `green`, `blue`, `yellow`, `magenta`, `cyan`, `bold` |
| **Debug** | `error`, `task`, `watch` |

---

## C Plugins

Write native functions in C and call them from Just:

**myplugin.c:**
```c
#include "just_api.h"

Value* double_number(Value **args, int count) {
    double x = args[0]->data.number;
    return api.create_number(x * 2);
}

void init_plugin(void (*register_func)(const char*, void*)) {
    register_func("double", double_number);
}
```

**In Just:**
```javascript
load_plugin("myplugin")
print(double(21))  // 42
```

---

## Building

```bash
# Windows (MinGW)
gcc -o just.exe main.c -lws2_32 -lm

# Linux
gcc -o just main.c -lm -ldl
```

That's it. No CMake, no Makefile, no build system. One command.

---

## Project Status

Just is under active development. It already handles real tasks — HTTP requests, JSON processing, file I/O, C plugins — and keeps improving. The entire language can be learned in a couple of evenings. Fully open to contributions: libraries, plugins, documentation, ideas.

Bugs, suggestions, and contributions are welcome.

---

## License

MIT © 2026 T-bit/BDD — Beyond Digital Dominance
