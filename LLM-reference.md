### LLM Reference

1. Types: `number`, `string`, `bool`, `array`, `object`, `null`.
2. Variables: `let` (mutable), `const` (immutable). Block-scoped.
3. Operators: `+ - * / %`, `== != < > <= >=`, `=== !==` (type+value), `&&/and ||/or !/not`.
4. `+` with string = concatenation. All other math coerces to number.
5. Functions: `func name(params) { return expr }`. Lambdas: `func(params) { return expr }` capture variables **by value** (copy).
6. Control: `if/else`, `while`, `for (let i=0; i<n; i=i+1)`. `break/continue`.
7. Errors: `try/catch/finally`, `throw (value)`. Division by zero, modulo by zero, stack overflow are catchable.
8. Arrays: `[1,2,3]` support **negative indices** (`arr[-1]` last element).
9. Objects: `{key: value}`. Access: `obj.key`. Assign: `obj.key = value`. `obj.key += 1` works.
10. Imports: `import "file.just"` (once per file, global scope).
11. Sandbox: `just_init_ex(flags)` gates `exec`, `read/write`, `http_*`, `load_plugin`, `db_*`.
12. Key built-ins: `print`, `len`, `type`, `json`/`json_parse`, `http_get`/`http_post`, `read`/`write`, `map`/`filter`, `db_open`/`db_query`.
13. Strings: `upper`, `lower`, `trim`, `split`, `join`, `replace`, `contains`, `substr`, `starts_with`, `ends_with`.
14. Math: `sqrt`, `pow`, `abs`, `min`, `max`, `floor`, `ceil`, `round`, `random`, `clamp`. Constants: `PI`, `E`.
15. Arrays helpers: `array_push`, `array_pop`, `first`, `last`, `reverse`, `sort`, `slice`, `concat`, `unique`, `sum`, `index_of`, `includes`.
16. Object helpers: `keys`, `values`, `has`, `entries`, `merge`.
17. System: `exec(cmd)`, `sleep(ms)`, `env(name)`, `now(format?)`.
18. Functions return `null` if no `return`. Semicolons optional. Comments: `//` and `/* */`.

Full function list in README.
