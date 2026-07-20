# Just Language

*A lightweight embeddable scripting language written in pure C.*

**Current version: 3.0.0**

---

## What is Just?

**Just** is a small scripting language built from scratch in C.

- **Single binary** — no dependencies, no package manager, no runtime
- **Cross-platform** — Windows and Linux from the same code
- **Familiar syntax** — if you know JavaScript or Python, you'll feel at home
- **Tracing GC** — mark-and-sweep garbage collector with a configurable threshold
- **Modules** — split code across files with `import`
- **C plugins** — extend the language with native code (thread-safe loading)
- **Structured error handling** — `try` / `catch` / `finally` / `throw`, with catchable runtime errors instead of crashes (division by zero, stack overflow, etc.)
- **Closures & lambdas** — `func(x) { ... }` expressions with capture-by-value
- **Capability-based sandboxing** — turn off file, network, exec, plugin, or database access per-interpreter
- **Resource limits** — cap iterations, call depth, and token count to safely run untrusted scripts
- **100+ builtins** — JSON, HTTP, files, strings, math, collections, SQLite, and more
- **SQLite built-in** — full database support out of the box, zero dependencies (optional at build time)

It compiles in seconds and runs anywhere. Good for automation scripts, learning how languages work, or embedding into larger C/C++ projects — including ones that need to run scripts from untrusted sources.

---

## Quick Start

### Build

**Windows (MinGW), with the interactive build script:**

```
build.bat
```

It will ask whether to include SQLite (`sqlite3.c` must be next to `just.c`) and produce `just.exe` either way.

**Windows (MinGW), manually:**

```
gcc -std=c11 -Wall -Wextra -O2 -o just.exe just.c main.c -lws2_32 -lm
```

**Linux:**

```
gcc -std=c11 -Wall -Wextra -O2 -o just just.c main.c -lm -ldl
```

**Without SQLite** (skip bundling `sqlite3.c`), add `-DJUST_NO_SQLITE` on either platform.

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
const version = 3

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
| **Math**        | `sqrt`, `pow`, `abs`, `min`, `max`, `floor`, `ceil`, `round`, `random`, `sin`, `cos`, `tan`, `log`, `exp`, `clamp`                                                                    |
| **Constants**   | `PI`, `E`                                                                                                                                                                             |
| **Paths**       | `dirname`, `basename`, `extname`, `join_path`                                                                                                                                        |
| **JSON**        | `json`, `json_parse`, `read_json`, `write_json`, `export`, `import_json`                                                                                                             |
| **HTTP**        | `http_get`, `http_post`                                                                                                                                                              |
| **Files**       | `read`, `write`, `exists`                                                                                                                                                            |
| **Time**        | `now`, `sleep`                                                                                                                                                                       |
| **System**      | `exec`, `env`, `load_plugin`                                                                                                                                                         |
| **Modules**     | `import`                                                                                                                                                                              |
| **Collections** | `len`, `range`, `filter`, `map`, `reduce`, `find`, `keys`, `values`, `entries`, `has`, `merge`, `array_push`, `array_pop`, `first`, `last`, `reverse`, `sort`, `slice`, `concat`, `unique`, `sum`, `index_of`, `includes` |
| **Colors**      | `red`, `green`, `blue`, `yellow`, `magenta`, `cyan`, `bold`                                                                                                                           |
| **Debug**       | `debug`, `dump`, `error`, `help`, `task`, `watch`, `gc_get_count`, `gc_get_allocations`                                                                                              |
| **SQLite**      | `db_open`, `db_close`, `db_query`, `db_exec`, `db_prepare`, `db_bind`, `db_step`, `db_finalize`, `db_last_insert_id`, `db_changes`, `db_begin`, `db_commit`, `db_rollback`, `db_error` |

### New in this release

`reduce`, `find`, `index_of`, `includes`, `slice`, `concat`, `unique`, `sum`, `clamp`, `pad_start`, `pad_end`, `entries`, `merge`, `substr`, `gc_get_count`, `gc_get_allocations`.

```js
let nums = [1, 2, 3, 4, 5, 6]

print(reduce(nums, func(a, b) { return a + b }))   // 21
print(find(nums, func(n) { return n > 3 }))        // 4
print(slice(nums, 1, 3))                           // [2, 3]
print(concat([1, 2], [2, 3]))                      // [1, 2, 2, 3]
print(unique(concat([1, 2], [2, 3])))              // [1, 2, 3]
print(sum(unique(concat([1, 2], [2, 3]))))         // 6
print(clamp(-5, 0, 10))                            // 0
print(pad_start("7", 3, "0"))                      // "007"
print(index_of([10, 20, 30], 20))                  // 1
print(includes([1, 2, 3], 3))                      // true

let user = { name: "Alex", age: 25 }
print(entries(user))                               // [["name","Alex"],["age",25]]
print(merge(user, { age: 26, city: "NY" }))         // { name:"Alex", age:26, city:"NY" }
```

---

## Security & Sandboxing

Just can restrict what a script is allowed to do. This is useful when running scripts you don't fully trust (plugins, user-submitted automation, etc.) via the C API:

```c
#include "just.h"

// Only allow pure computation — no files, network, exec, plugins, or DB
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

A script that tries a disabled capability gets a catchable runtime error rather than crashing or silently succeeding.

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

## SQLite Built-in (Optional, Zero Dependencies)

Just can bundle **SQLite** directly in the binary — no external libraries needed. It's opt-in at build time: drop `sqlite3.c` next to `just.c` and build normally (or answer "yes" in `build.bat`). Build with `-DJUST_NO_SQLITE` to leave it out entirely.

```js
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
| ------------------------- | --------------------------------------- |
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

---

## C Plugins

Write native functions in C and call them from Just. Plugin loading is thread-safe.

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

**In Just:**

```js
load_plugin("myplugin")
print(double(21))  // 42
```

Gated by `JUST_CAP_PLUGIN` — see [Security & Sandboxing](#security--sandboxing).

---

## Building

```
# Windows (MinGW) — interactive, asks about SQLite
build.bat

# Windows (MinGW) — manual
gcc -std=c11 -Wall -Wextra -O2 -o just.exe just.c main.c -lws2_32 -lm

# Linux
gcc -std=c11 -Wall -Wextra -O2 -o just just.c main.c -lm -ldl

# Either platform, without SQLite
gcc -std=c11 -Wall -Wextra -O2 -DJUST_NO_SQLITE -o just just.c main.c -lm -ldl
```

No CMake, no external Makefile required — one command (or `build.bat` on Windows).

---

## Project Status

Just is under active development, now at **version 3.0.0**. It handles real tasks — HTTP requests, JSON processing, file I/O, SQLite databases, C plugins — and now adds proper error handling, closures, and a capability-based sandbox for running untrusted scripts safely. The entire language can still be learned in a couple of evenings. Fully open to contributions: libraries, plugins, documentation, ideas.

### Highlights in v3.0.0

- **Structured error handling** — `try`/`catch`/`finally`/`throw`, catchable division-by-zero and stack-overflow errors instead of crashes
- **Closures & lambdas** — `func(x) { ... }` expressions with capture-by-value
- **Capability-based sandboxing** — `just_init_ex()` with `JUST_CAP_EXEC/FILES/NET/PLUGIN/DB`
- **Resource limits** — configurable iteration, call-depth, and token caps for untrusted scripts
- **Thread-safe plugin loading**
- **Expanded standard library** — `reduce`, `find`, `slice`, `concat`, `unique`, `sum`, `clamp`, `pad_start`/`pad_end`, `entries`, `merge`, and more
- **Negative array indexing** — `arr[-1]`
- **Zero-dependency SQLite**, still optional at build time
- **Single binary**, still no runtime, no package manager
- **C embeddable**, still easy to integrate into any C/C++ project

Bugs, suggestions, and contributions are welcome.

---

## License

MIT © 2026 T-bit/BDD — Beyond Digital Dominance
