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
- **100+ builtins** — JSON, HTTP, files, strings, math, SQLite, and more
- **SQLite built-in** — full database support out of the box, zero dependencies

It compiles in seconds and runs anywhere. Good for automation scripts, learning how languages work, or embedding into larger C/C++ projects.

---

## Quick Start

### Build

**Windows (MinGW):**
```bash
gcc -o just.exe main.c just.c -lm -lws2_32 -Wall -O2
```

**Linux:**
```bash
gcc -o just main.c just.c -lm -ldl -Wall -O2
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
| **Types** | `type`, `int`, `str`, `bool`, `is_number`, `is_string`, `is_bool`, `is_array`, `is_object`, `is_function`, `is_null` |
| **Input** | `input` |
| **Strings** | `upper`, `lower`, `trim`, `split`, `join`, `replace`, `contains`, `len`, `starts_with`, `ends_with`, `repeat` |
| **Math** | `sqrt`, `pow`, `abs`, `min`, `max`, `floor`, `ceil`, `round`, `random`, `sin`, `cos`, `tan`, `log`, `exp` |
| **Constants** | `PI`, `E` |
| **Paths** | `dirname`, `basename`, `extname`, `join_path` |
| **JSON** | `json`, `json_parse`, `read_json`, `write_json`, `export`, `import_json` |
| **HTTP** | `http_get`, `http_post` |
| **Files** | `read`, `write`, `exists` |
| **Time** | `now`, `sleep` |
| **System** | `exec`, `env`, `load_plugin` |
| **Modules** | `import` |
| **Collections** | `len`, `range`, `filter`, `map`, `keys`, `values`, `has`, `array_push`, `array_pop`, `first`, `last`, `reverse`, `sort` |
| **Colors** | `red`, `green`, `blue`, `yellow`, `magenta`, `cyan`, `bold` |
| **Debug** | `debug`, `dump`, `error`, `help`, `task`, `watch` |
| **SQLite** | `db_open`, `db_close`, `db_query`, `db_exec`, `db_prepare`, `db_bind`, `db_step`, `db_finalize`, `db_last_insert_id`, `db_changes`, `db_begin`, `db_commit`, `db_rollback`, `db_error` |

---

## SQLite Built-in (Zero Dependencies)

Just includes **SQLite** directly in the binary — no external libraries needed.

```javascript
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
| Function | Description |
|----------|-------------|
| `db_open(filename)` | Open or create database |
| `db_close(db)` | Close database |
| `db_query(db, sql)` | Execute SQL (returns array for SELECT) |
| `db_exec(db, sql)` | Alias for db_query |
| `db_prepare(db, sql)` | Prepare statement |
| `db_bind(stmt, idx, val)` | Bind value (NULL, number, string, bool) |
| `db_step(stmt)` | Fetch next row (returns object or null) |
| `db_finalize(stmt)` | Finalize statement |
| `db_last_insert_id(db)` | Last auto-increment ID |
| `db_changes(db)` | Number of changed rows |
| `db_begin(db)` | Start transaction |
| `db_commit(db)` | Commit transaction |
| `db_rollback(db)` | Rollback transaction |
| `db_error(db)` | Last error message |

---

## C API (Embedding)

Just is designed to be embedded in C/C++ projects:

```c
#include "just.h"

JustState *j = just_init();

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
| Function | Description |
|----------|-------------|
| `just_init()` | Create interpreter state |
| `just_destroy(j)` | Destroy interpreter |
| `just_eval(j, code)` | Execute string |
| `just_eval_file(j, filename)` | Execute file |
| `just_register_function(j, name, func)` | Register C function |
| `just_get_var(j, name)` | Get variable |
| `just_set_var(j, name, val)` | Set variable |
| `just_set_const(j, name, val)` | Set constant |
| `just_number(n)` | Create number |
| `just_string(s)` | Create string |
| `just_bool(b)` | Create boolean |
| `just_null()` | Create null |
| `just_object_new(j)` | Create object |
| `just_array_new(j)` | Create array |
| `just_gc_collect(j)` | Run garbage collector |

---

## C Plugins

Write native functions in C and call them from Just:

**myplugin.c:**
```c
#include "just_api.h"

Value* double_number(JustState *j, Value **args, int count) {
    double x = args[0]->data.number;
    return just_number(x * 2);
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
gcc -o just.exe main.c just.c -lm -lws2_32 -Wall -O2

# Linux
gcc -o just main.c just.c -lm -ldl -Wall -O2
```

That's it. No CMake, no Makefile, no build system. One command.

---

## Project Status

Just is under active development. It already handles real tasks — HTTP requests, JSON processing, file I/O, SQLite databases, C plugins — and keeps improving. The entire language can be learned in a couple of evenings. Fully open to contributions: libraries, plugins, documentation, ideas.

### Unique Features
- **Zero-dependency SQLite** — Full database support in a 2 MB binary
- **Single binary** — No runtime, no package manager
- **C embeddable** — Easy to integrate into any C/C++ project

Bugs, suggestions, and contributions are welcome.

---

## License

MIT © 2026 T-bit/BDD — Beyond Digital Dominance
