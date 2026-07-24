# Just Language

*A lightweight embeddable scripting language written in pure C.*

**Current version: 1.0.0**

---

## What is Just?

**Just** is a small scripting language built from scratch in C.

- **Single binary** — no runtime, no package manager. Optional libraries (SQLite, HTTPS) are baked in at link time, not loaded from separate DLLs/.so files at runtime
- **Cross-platform** — Windows and Linux from the same code
- **Familiar syntax** — if you know JavaScript or Python, you'll feel at home
- **Tracing GC** — mark-and-sweep garbage collector with a configurable threshold
- **Thread-safe** — a single interpreter instance can be called from multiple threads safely (calls are serialized internally); separate instances run fully in parallel
- **Modules** — split code across files with `import`
- **C plugins** — extend the language with native code (graphics/audio, custom I/O, anything)
- **Structured error handling** — `try` / `catch` / `finally` / `throw`, with catchable runtime errors instead of crashes (division by zero, stack overflow, etc.)
- **Closures & lambdas** — `func(x) { ... }` expressions with capture-by-value
- **Capability-based sandboxing** — turn off file, network, exec, plugin, or database access per-interpreter
- **Resource limits** — cap iterations, call depth, and token count to safely run untrusted scripts
- **Regex** — built in, always available, zero extra setup
- **100+ builtins** — JSON, HTTP(S), files, strings, math, collections, regex, SQLite, and more
- **SQLite built-in** — full database support out of the box (optional at build time)
- **HTTPS built-in** — TLS-encrypted requests via mbedTLS (optional at build time)

It compiles in seconds and runs anywhere. Good for automation scripts, configuration with logic, learning how languages work, or embedding into larger C/C++ projects — including ones that need to run scripts from untrusted sources, or call into the same interpreter from multiple threads.

---

## Quick Start

### Build

**Windows (MinGW), with the interactive build script:**

```
build.bat
```

It asks whether to include SQLite and/or HTTPS and produces `just.exe` either way. See [Building](#building) below for what files need to be present for each.

**One command, no script, any platform — see [Building](#building) for the full breakdown of flags.**

### Run a script

```
just hello.just
```

### Or try the REPL

```
just
```

**hello.just:**

```just
print("Hello from Just!")
```

---

## Syntax at a Glance

```just
// Variables
let name = "Just"
const version = 1

// Objects
let user = { name: "Alex", age: 25 }

// Arrays (support negative indices)
let items = [1, 2, 3]
print(items[-1])   // 3

// Functions
func greet(name) {
    return "Hello, " + name
}

// Closures / lambdas
let square = func(x) { return x * x }
print(square(5))   // 25

// Logical operators — both keyword and symbol forms work
if (user.age >= 18 and user.age < 65) {
    print("Adult")
}
if (user.age >= 18 && !false) {
    print("Also adult")
}

// Loops
for (let i = 0; i < 5; i = i + 1) {
    print(i)
}
```

> **🤖 For LLMs/AI assistants:** See [`LLM-reference.md`](LLM-reference.md) — a concise technical reference optimized for AI code generation.

---

## Error Handling

Runtime failures that used to crash the interpreter now raise **catchable** errors: division/modulo by zero, exceeding the call-depth limit, and explicit `error()`/`throw` calls.

```just
try {
    let x = 5 / 0
} catch (e) {
    print("Caught:", e)
}

// throw supports both strings and structured objects
try {
    throw ({ code: 404, message: "not found" })
} catch (e) {
    print(e.code, e.message)   // 404 not found
}

// finally always runs, even when the try block returns
func withCleanup() {
    try {
        return "ok"
    } finally {
        print("cleanup ran")
    }
}
withCleanup()
```

---

## Closures & Lambdas

Anonymous functions capture the enclosing scope **by value** at the moment they're created:

```just
func make_adder(n) {
    return func(x) { return x + n }
}

let add5 = make_adder(5)
let add10 = make_adder(10)

print(add5(1))    // 6
print(add10(1))   // 11
```

Lambdas work anywhere a value is expected — as arguments to `filter`, `map`, `reduce`, stored in variables, or called directly. Because capture is by value, a lambda assigned to a `let` can't call itself recursively — use a named `func` for recursion instead.

---

## Regex

Built directly into the core (via [tiny-regex-c](https://github.com/kokke/tiny-regex-c), public domain) — no build flag, no setup, always available.

```just
print(regex_match("hello world", "world"))          // true
print(regex_find("id=42", "\\d+"))                   // "42"
print(regex_find_all("a1 b22 c333", "\\d+"))         // ["1", "22", "333"]
print(regex_replace("x1y2z3", "\\d", "#"))           // "x#y#z#"
```

Supports: `. ^ $ * + ? [abc] [^abc] [a-z] \s \S \w \W \d \D`. Does **not** support capture groups, alternation (`|`), or `{n,m}` repetition — it's intentionally minimal, good for validation/extraction, not a full PCRE replacement. Like any naive backtracking regex engine, avoid running untrusted patterns against untrusted, attacker-sized input.

---

## Modules (import)

Split your code across multiple files with `import`:

**math.just:**

```just
func add(a, b) {
    return a + b
}

func multiply(a, b) {
    return a * b
}
```

**main.just:**

```just
import "math.just"

print(add(10, 20))       // 30
print(multiply(5, 6))    // 30
```

Imports are loaded once and share the global scope. Good for organizing larger projects.

---

## Built-in Functions

| Category        | Functions                                                                                                                                                                              |
| --------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Output**      | `print`                                                                                                                                                                               |
| **Types**       | `type`, `int`, `str`, `bool`, `is_number`, `is_string`, `is_bool`, `is_array`, `is_object`, `is_function`, `is_null`                                                                  |
| **Input**       | `input`                                                                                                                                                                               |
| **Strings**     | `upper`, `lower`, `trim`, `split`, `join`, `replace`, `contains`, `len`, `starts_with`, `ends_with`, `repeat`, `substr`, `pad_start`, `pad_end`                                      |
| **Regex**       | `regex_match`, `regex_find`, `regex_find_all`, `regex_replace`                                                                                                                        |
| **Math**        | `sqrt`, `pow`, `abs`, `min`, `max`, `floor`, `ceil`, `round`, `random`, `sin`, `cos`, `tan`, `log`, `exp`, `clamp`                                                                    |
| **Constants**   | `PI`, `E`                                                                                                                                                                             |
| **Paths**       | `dirname`, `basename`, `extname`, `join_path`                                                                                                                                        |
| **JSON**        | `json`, `json_parse`, `read_json`, `write_json`, `export`, `import_json`                                                                                                             |
| **HTTP(S)**     | `http_get`, `http_post` (both transparently support `http://` and `https://` URLs)                                                                                                   |
| **Files**       | `read`, `write`, `exists`                                                                                                                                                             |
| **Time**        | `now`, `sleep`                                                                                                                                                                        |
| **System**      | `exec`, `env`, `load_plugin`                                                                                                                                                          |
| **Modules**     | `import`                                                                                                                                                                              |
| **Collections** | `len`, `range`, `filter`, `map`, `reduce`, `find`, `keys`, `values`, `entries`, `has`, `merge`, `array_push`, `array_pop`, `first`, `last`, `reverse`, `sort`, `slice`, `concat`, `unique`, `sum`, `index_of`, `includes` |
| **Colors**      | `red`, `green`, `blue`, `yellow`, `magenta`, `cyan`, `bold`                                                                                                                           |
| **Debug**       | `debug`, `dump`, `error`, `help`, `task`, `watch`, `gc_get_count`, `gc_get_allocations`                                                                                              |
| **SQLite**      | `db_open`, `db_close`, `db_query`, `db_exec`, `db_prepare`, `db_bind`, `db_step`, `db_finalize`, `db_last_insert_id`, `db_changes`, `db_begin`, `db_commit`, `db_rollback`, `db_error` |

---

## Security & Sandboxing

Just can restrict what a script is allowed to do. This is useful when running scripts you don't fully trust (plugins, user-submitted automation, config files with logic, etc.) via the C API:

```c
#include "just.h"

// Only allow pure computation — no files, network, exec, plugins, or DB.
// This is also a good fit for "config file with logic" use cases: the
// script can compute values but can't touch the outside world.
JustState *j = just_init_ex(0);

// Or pick specific capabilities:
JustState *j2 = just_init_ex(JUST_CAP_FILES | JUST_CAP_NET);

// just_init() is unchanged and grants everything:
JustState *full = just_init();   // == just_init_ex(JUST_CAP_ALL)
```

| Flag              | Gates                          |
| ----------------- | ------------------------------- |
| `JUST_CAP_EXEC`    | `exec()`                        |
| `JUST_CAP_FILES`   | `read`, `write`, `exists`, `read_json`, `write_json` |
| `JUST_CAP_NET`     | `http_get`, `http_post`         |
| `JUST_CAP_PLUGIN`  | `load_plugin()`                 |
| `JUST_CAP_DB`      | all `db_*` functions            |
| `JUST_CAP_ALL`     | everything (default via `just_init()`) |

A script that tries a disabled capability gets a catchable runtime error rather than crashing or silently succeeding. Regex has no capability flag — it's always available since it can't reach outside the interpreter.

### Resource Limits

Also aimed at running untrusted or long scripts safely:

```c
just_set_max_iterations(j, 1000000);   // cap on loop iterations
just_set_max_call_depth(j, 100);       // cap on recursion depth
just_set_max_tokens(j, 500000);        // cap on parsed token count

just_gc_set_threshold(j, 20000);       // allocations before an auto GC pass
int live = just_gc_get_count(j);       // currently live GC-tracked values
int total = just_gc_get_allocations(j);// allocations since last collection
```

Defaults (overridable at compile time via `#define` before including `just.h`, or at runtime via the setters above): `MAX_ITERATIONS`, `MAX_CALL_DEPTH` (150), `MAX_TOKENS`, `GC_THRESHOLD` (50000), among others — see `just.h`.

---

## Thread Safety

A single `JustState` instance can now be shared across threads safely — calls to `just_eval`, `just_eval_file`, `just_call`, and the variable/GC/registration API are internally serialized (similar in spirit to Python's GIL: queued, not truly parallel, but never racing or corrupting state). Separate `JustState` instances remain fully independent and run in true parallel on separate threads with no shared locking at all.

```c
JustState *shared = just_init();
// safe to call just_eval(shared, ...) / just_call(shared, ...) from multiple
// worker threads concurrently — they'll be serialized, not corrupted
```

---

## Calling Just Functions from C (`just_call`)

For host applications that call the same Just function repeatedly — once per frame in a game loop, once per request in a server — `just_call` invokes an already-defined function directly without re-tokenizing a call-expression string through `just_eval()` every time:

```c
just_eval(j, "func update(dt) { ... }");   // define once

// then, every frame:
Value *args[1] = { just_number(delta_time) };
Value *result = just_call(j, "update", args, 1);
```

---

## HTTPS (Optional, mbedTLS)

`http_get`/`http_post` transparently support `https://` URLs when built with mbedTLS (see [Building](#building)). Without it (`-DJUST_NO_TLS`), `https://` calls return an empty string with a clear error message — `http://` always works either way.

**Known limitation (v1):** certificate verification is disabled — there's no cross-platform way to reliably locate a trusted CA bundle (especially on older Windows versions), so this is deferred rather than shipped half-working. Connections are encrypted (safe from passive eavesdropping) but not verified against a certificate authority (not safe from an active man-in-the-middle). Treat it accordingly — fine for calling APIs over your own network/VPN or trusted infrastructure, not yet for handling untrusted/adversarial network paths.

---

## SQLite Built-in (Optional, Zero Dependencies)

Just can bundle **SQLite** directly in the binary — no external libraries needed. It's opt-in at build time: drop `sqlite3.c`/`sqlite3.h` next to `just.c` and build normally (or answer "yes" in `build.bat`). Build with `-DJUST_NO_SQLITE` to leave it out entirely.

```just
let db = db_open("app.db")

// Create table
db_query(db, "CREATE TABLE IF NOT EXISTS users (id INTEGER, name TEXT, age INTEGER)")

// Insert data with transaction
db_begin(db)
db_query(db, "INSERT INTO users VALUES (1, 'Alex', 25)")
db_query(db, "INSERT INTO users VALUES (2, 'Bob', 30)")
db_commit(db)

// Query data
let users = db_query(db, "SELECT * FROM users")
print(users)  // [{id: 1, name: Alex, age: 25}, ...]

// Safe prepared statements (protects against SQL injection)
let stmt = db_prepare(db, "SELECT * FROM users WHERE age > ?")
db_bind(stmt, 1, 25)

while true {
    let row = db_step(stmt)
    if row == null { break }
    print(row.name, "is", row.age, "years old")
}
db_finalize(stmt)

// Get last inserted ID
let id = db_last_insert_id(db)
print("Last ID:", id)

db_close(db)
```

**SQLite Functions:**

| Function                  | Description                             |
| ------------------------- | ---------------------------------------- |
| `db_open(filename)`       | Open or create database                 |
| `db_close(db)`            | Close database                          |
| `db_query(db, sql)`       | Execute SQL (returns array for SELECT)  |
| `db_exec(db, sql)`        | Alias for db_query                      |
| `db_prepare(db, sql)`     | Prepare statement                       |
| `db_bind(stmt, idx, val)` | Bind value (NULL, number, string, bool) |
| `db_step(stmt)`           | Fetch next row (returns object or null) |
| `db_finalize(stmt)`       | Finalize statement                      |
| `db_last_insert_id(db)`   | Last auto-increment ID                  |
| `db_changes(db)`          | Number of changed rows                  |
| `db_begin(db)`            | Start transaction                       |
| `db_commit(db)`           | Commit transaction                      |
| `db_rollback(db)`         | Rollback transaction                    |
| `db_error(db)`            | Last error message                      |

Gated by `JUST_CAP_DB` — see [Security & Sandboxing](#security--sandboxing).

---

## C API (Embedding)

Just is designed to be embedded in C/C++ projects:

```c
#include "just.h"

JustState *j = just_init();               // or just_init_ex(capabilities) for a sandboxed instance

// Register C functions
just_register_function(j, "c_hello", c_hello);

// Set variables
Value *player = just_object_new(j);
just_object_set(player, "health", just_number(100));
just_set_var(j, "player", player);

// Execute script
just_eval_file(j, "game_logic.just");

// Get result
Value *result = just_get_var(j, "result");

// Call a specific function repeatedly without re-parsing (e.g. per frame)
Value *args[1] = { just_number(0.016) };
just_call(j, "update", args, 1);

just_destroy(j);
```

**C API Functions:**

| Function                                | Description                                  |
| ---------------------------------------- | --------------------------------------------- |
| `just_init()`                           | Create interpreter state with full capabilities |
| `just_init_ex(capabilities)`             | Create interpreter state with a specific capability set |
| `just_destroy(j)`                       | Destroy interpreter                          |
| `just_eval(j, code)`                    | Execute string                               |
| `just_eval_file(j, filename)`           | Execute file                                 |
| `just_call(j, func_name, args, argc)`   | Call an already-defined function directly, no re-parsing |
| `just_register_function(j, name, func)` | Register C function                          |
| `just_get_var(j, name)`                 | Get variable                                 |
| `just_set_var(j, name, val)`            | Set variable                                 |
| `just_set_const(j, name, val)`          | Set constant                                 |
| `just_number(n)` / `just_string(s)` / `just_bool(b)` / `just_null()` | Create primitive values |
| `just_object_new(j)` / `just_object_set/get/has` | Create and manipulate objects           |
| `just_array_new(j)` / `just_array_push/get/set/length` | Create and manipulate arrays       |
| `just_to_json(j, v)` / `just_from_json(j, json)` | Convert between `Value*` and JSON strings |
| `just_to_string(j, v)`                  | Stringify a value                            |
| `just_gc_collect(j)`                    | Run garbage collector                        |
| `just_gc_set_threshold(j, n)`           | Set allocations-before-auto-GC threshold     |
| `just_gc_get_count(j)` / `just_gc_get_allocations(j)` | Inspect GC state                |
| `just_set_max_iterations/call_depth/tokens(j, n)` | Set resource limits                |
| `just_print_state(j)`                   | Dump interpreter state to stdout (debugging) |
| `just_version()` / `just_version_major/minor/patch()` | Query the linked library version    |

All of the above are safe to call concurrently on the same `JustState` from multiple threads (see [Thread Safety](#thread-safety)).

---

## C Plugins

Write native functions in C and call them from Just. Plugin loading is thread-safe. Plugins are entirely separate from the core build — the core interpreter never needs to know a plugin exists until `load_plugin()` is called at runtime.

**myplugin.c:**

```c
#include "just.h"

Value* double_number(JustState *j, Value **args, int count) {
    double x = args[0]->data.number;
    return just_number(x * 2);
}

void init_plugin(void (*register_func)(const char*, NativeFunc)) {
    register_func("double", double_number);
}
```

Build it as a shared library (`gcc -shared -fPIC myplugin.c -o myplugin.so`) and, importantly, **build the core `just` executable itself with `-rdynamic`** (already the default in `build.bat` and the commands below) — without it, any plugin that calls back into `just_*` functions (i.e. almost any useful plugin) fails to load with a silent "undefined symbol" error.

**In Just:**

```just
load_plugin("myplugin")
print(double(21))  // 42
```

Gated by `JUST_CAP_PLUGIN` — see [Security & Sandboxing](#security--sandboxing).

---

## Building

Just's core (`just.c`/`just.h`/`main.c`) always compiles with a single command — no CMake, no complicated build system. This assumes you keep the extra libraries (SQLite, mbedTLS, regex) in a `src/` folder next to `just.c`, the same way this project is already set up. SQLite and HTTPS are optional — turn them on or off by adding/removing one flag. Regex is always included, no setup needed.

**Important:** All build commands assume you're in the `src/` folder. First do:
```bash
cd src
```

---

### 1. Core only — no SQLite, no HTTPS

**Smallest binary (~167 KB).** Good starting point if you're not sure yet what you need.

```
# Linux
gcc -std=c11 -O2 -pthread -rdynamic -DJUST_NO_SQLITE -DJUST_NO_TLS just.c main.c -o just -ldl -lm

# Windows (MinGW)
gcc -std=c11 -O2 -pthread -Wl,--export-all-symbols -DJUST_NO_SQLITE -DJUST_NO_TLS just.c main.c -o just.exe -lws2_32 -lm
```

---

### 2. With SQLite (no HTTPS)

Make sure `src/sqlite3.c` and `src/sqlite3.h` are present.

```
# Linux
gcc -std=c11 -O2 -pthread -rdynamic -DJUST_NO_TLS just.c main.c -o just -ldl -lm

# Windows (MinGW)
gcc -std=c11 -O2 -pthread -Wl,--export-all-symbols -DJUST_NO_TLS just.c main.c -o just.exe -lws2_32 -lm
```

> `-DJUST_NO_SQLITE` is **not** passed — SQLite is included automatically when `sqlite3.c` is found.

---

### 3. With HTTPS (no SQLite)

```
# Linux
gcc -std=c11 -O2 -pthread -rdynamic -DJUST_NO_SQLITE -Imbedtls/include just.c main.c \
    -o just -Lmbedtls/library -lmbedtls -lmbedx509 -lmbedcrypto -ldl -lm

# Windows (MinGW)
gcc -std=c11 -O2 -pthread -Wl,--export-all-symbols -DJUST_NO_SQLITE -Imbedtls\include just.c main.c \
    -o just.exe -Lmbedtls\library -lmbedtls -lmbedx509 -lmbedcrypto -lws2_32 -lm
```

> `-DJUST_NO_TLS` is **not** passed — HTTPS is included when mbedTLS libraries are present.

---

### 4. Everything together — SQLite + HTTPS

**Full build (~1.93 MB).** Requires both `src/sqlite3.c` and built mbedTLS libraries.

```
# Linux
gcc -std=c11 -O2 -pthread -rdynamic -Imbedtls/include just.c main.c \
    -o just -Lmbedtls/library -lmbedtls -lmbedx509 -lmbedcrypto -ldl -lm

# Windows (MinGW)
gcc -std=c11 -O2 -pthread -Wl,--export-all-symbols -Imbedtls\include just.c main.c \
    -o just.exe -Lmbedtls\library -lmbedtls -lmbedx509 -lmbedcrypto -lws2_32 -lm
```

> Neither `-DJUST_NO_SQLITE` nor `-DJUST_NO_TLS` is passed — both are included.

---

### Quick reference: build variants

| Variant | Binary size | Linux command | Windows command |
|---------|-------------|---------------|-----------------|
| **Core only** | ~167 KB | `gcc -std=c11 -O2 -pthread -rdynamic -DJUST_NO_SQLITE -DJUST_NO_TLS just.c main.c -o just -ldl -lm` | `gcc -std=c11 -O2 -pthread -Wl,--export-all-symbols -DJUST_NO_SQLITE -DJUST_NO_TLS just.c main.c -o just.exe -lws2_32 -lm` |
| **+ SQLite** | ~1.22 MB | `gcc -std=c11 -O2 -pthread -rdynamic -DJUST_NO_TLS just.c main.c -o just -ldl -lm` | `gcc -std=c11 -O2 -pthread -Wl,--export-all-symbols -DJUST_NO_TLS just.c main.c -o just.exe -lws2_32 -lm` |
| **+ HTTPS** | ~902 KB | `gcc -std=c11 -O2 -pthread -rdynamic -DJUST_NO_SQLITE -Imbedtls/include just.c main.c -o just -Lmbedtls/library -lmbedtls -lmbedx509 -lmbedcrypto -ldl -lm` | `gcc -std=c11 -O2 -pthread -Wl,--export-all-symbols -DJUST_NO_SQLITE -Imbedtls\include just.c main.c -o just.exe -Lmbedtls\library -lmbedtls -lmbedx509 -lmbedcrypto -lws2_32 -lm` |
| **Full** (SQL + HTTPS) | ~1.93 MB | `gcc -std=c11 -O2 -pthread -rdynamic -Imbedtls/include just.c main.c -o just -Lmbedtls/library -lmbedtls -lmbedx509 -lmbedcrypto -ldl -lm` | `gcc -std=c11 -O2 -pthread -Wl,--export-all-symbols -Imbedtls\include just.c main.c -o just.exe -Lmbedtls\library -lmbedtls -lmbedx509 -lmbedcrypto -lws2_32 -lm` |

---

### Or just double-click the script (Windows)

```
build.bat
```

It asks two simple yes/no questions (SQLite? HTTPS?) and builds `just.exe` for you — no need to remember any of the commands above. If something's missing, it tells you clearly what and where, instead of a confusing wall of linker errors.

### Turning SQLite/HTTPS off on purpose

Add `-DJUST_NO_SQLITE` and/or `-DJUST_NO_TLS` to any command above if you want to build without them even though the files are present.

---

## Project Status

Just is under active development, now at **version 1.0.0** — the first public release. It handles real tasks — HTTP(S) requests, JSON processing, file I/O, SQLite databases, regex, C plugins — with structured error handling, closures, thread safety, and a capability-based sandbox for running untrusted scripts safely. The entire language can still be learned in a couple of evenings. Fully open to contributions: libraries, plugins, documentation, ideas.

### Highlights in v1.0.0

- **First public release** — versioning reset to 1.0.0
- **Regex**, built into the core with zero setup
- **HTTPS**, optional at build time via mbedTLS (certificate verification not yet implemented — see [HTTPS](#https-optional-mbedtls))
- **Thread-safe interpreter instances** — one `JustState` can now be called safely from multiple threads
- **`just_call()`** — call a Just function from C repeatedly without re-parsing a call string each time
- **Structured error handling** — `try`/`catch`/`finally`/`throw`, catchable division-by-zero and stack-overflow errors instead of crashes
- **Closures & lambdas** — `func(x) { ... }` expressions with capture-by-value
- **Capability-based sandboxing** — `just_init_ex()` with `JUST_CAP_EXEC/FILES/NET/PLUGIN/DB`
- **Resource limits** — configurable iteration, call-depth, and token caps for untrusted scripts
- **Expanded standard library** — `reduce`, `find`, `slice`, `concat`, `unique`, `sum`, `clamp`, `pad_start`/`pad_end`, `entries`, `merge`, and more
- **Negative array indexing** — `arr[-1]`
- **Zero-dependency SQLite**, still optional at build time
- **Single binary**, still no runtime, no package manager
- **C embeddable**, still easy to integrate into any C/C++ project

Bugs, suggestions, and contributions are welcome.

---

## License

MIT © 2026 T-bit/BDD — Beyond Digital Dominance
