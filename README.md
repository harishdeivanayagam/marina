<p align="center">
  <img src="assets/logo.svg" alt="Marina" width="160">
</p>
<p align="center"><em>a strictly-typed, AOT-compiled TypeScript subset</em></p>

> **Experimental.** Marina is a research project. Do not use it in production.

# Marina

**Marina** is a strictly-typed, ahead-of-time compiled subset of TypeScript that emits **LLVM IR** and links against a custom **C++ runtime**. The language has single-threaded semantics like JavaScript, but the runtime itself is a **multithreaded event loop** — async code is lowered to promise state machines that are resumed on a worker pool.

Memory management is **Automatic Reference Counting (ARC)**, in the style of Swift: every heap object (strings, promises) carries a reference count, and the compiler inserts retain/release calls. Interned string literals are immortal (`rc < 0`).

```
marina/
├── examples/                  # example programs (see below)
├── stdlib/                    # std modules written in Marina itself (map)
├── compiler/                  # the compiler library + LSP (C++20)
│   ├── include/marina/        # public headers
│   ├── src/                   # lexer, parser, checker, codegen, pipeline
│   ├── lsp/main.cpp           # LSP server (see "Editor support" below)
│   └── tools/                 # tokdump / astdump debug tools
├── runtime/                   # runtime staticlib linked into every binary
│   └── src/                   # ARC, promises, event loop, objects, printing, net/http/fs/crypto/sql std modules
├── wave/                      # the package manager & build tool (`wave run`, …)
├── editors/                   # "marina-lsp" VSIX extension for Cursor/VS Code
└── CMakeLists.txt             # CMake build (C++20)
```

## Install

The installer clones [marina-lang-hq/marina](https://github.com/marina-lang-hq/marina), builds `wave` and `marina-lsp`, and links them into `~/.local/bin`. The source tree stays at `~/.marina/src` because `wave` links its runtime from that checkout.

You need git, CMake ≥ 3.20, a C++20 compiler, `clang`, and `libsqlite3`.

macOS:

```sh
xcode-select --install
brew install cmake
```

Linux (Debian / Ubuntu):

```sh
sudo apt install build-essential cmake clang libsqlite3-dev git curl
```

### macOS and Linux

```sh
curl -fsSL https://marina-lang.org/install.sh | sh
```

### Windows

Native Windows is not supported (the compiler and runtime use POSIX). The PowerShell installer builds Marina inside WSL. Install WSL once (`wsl --install`), then:

```powershell
irm https://marina-lang.org/install.ps1 | iex
```

After that, `wave` lives in the Linux distro. Run it with `wsl wave`.

## Requirements

- CMake (≥ 3.20) and a C++20 compiler — build the compiler, runtime, and wave
- `clang` — compiles the generated `.ll` and links the runtime; the compiler searches `clang`, `clang-21`, `clang-20`, `clang-18`, `cc`
- `libsqlite3` — linked into every binary for the `sql` std module (macOS ships it; on Linux install the `sqlite3`/`libsqlite3-dev` package)

## Building

```sh
cmake -S . -B build && cmake --build build -j
```

## Running programs

Marina programs use the **`.tsm`** extension (ts marina), and file names must be **kebab-case** (`hello-world.tsm`).

### The CLI: `wave`

**wave** is the cargo of Marina — the single CLI for the language. Build it (plus `marina-lsp` for editor support) from the repo root:

```sh
cmake -S . -B build && cmake --build build -j
# binaries: build/wave, build/marina-lsp
```

| Command | What it does |
|---|---|
| `wave init` | create `package.json` + `src/main.tsm` in the current directory |
| `wave add <path\|git-url>` | install a package into the global registry + record it in `package.json` |
| `wave remove <pkg>` | remove a dependency from `package.json` |
| `wave check [file]` | validate `package.json` + type-check the entry point (no codegen) |
| `wave build [file]` | compile to a native binary in `./.marina/` |
| `wave run [file]` | compile + execute (binary and IR go to a temp dir) |

Without `[file]`, wave uses the project entry point: the `"main"` field of `package.json`, defaulting to `src/main.tsm`. With a `[file]` argument, wave works anywhere — even outside a project (`wave run examples/objects.tsm`).

```sh
wave init                        # start a new project
wave add net                     # std module: just informational, nothing to install
wave add ./my-pkg                # install a local package into the registry
wave add https://github.com/u/p.git
wave check                       # manifest + type check
wave run                         # compile + run src/main.tsm
```

Projects are described by a **`package.json`** in the project root:

```json
{
    "name": "<project-name>",
    "version": "0.0.1",
    "dependencies": {
        "my-pkg": "0.3.0"
    }
}
```

**The registry model** — like Rust's crates.io, not node_modules. Packages are installed *once* into a global registry (default `~/.wave/registry/<name>/<version>/main.tsm`, override with the `WAVE_REGISTRY` env var) and every project on the machine uses that copy. There is no per-project dependency folder to commit or vendor. A package is a directory (or git repo) with a `main.tsm` entry file and optional `package.json` metadata (`name`, `version`); `wave add` copies it into the registry and records the dependency. In code, `import my-pkg` merges the package behind a namespace: `my-pkg.fn(...)` calls an `export`ed function. A function or class without `export` is not visible to other modules (`no exported member 'fn' from module 'my-pkg'`). Packages expose functions and classes only — no top-level statements, no `main`, and no dependencies on other packages yet (std modules `net`/`http`/`fs`/`crypto`/`sql`/`map` are allowed).

### One-off files

`wave run` / `wave build` work on any `.tsm` file, even outside a project:

```sh
# compile + run in one step (binary and IR go to a temp dir)
wave run examples/hello-world.tsm

# compile to a native binary in ./.marina/
wave build examples/hello-world.tsm
./.marina/hello-world
```

`build` also writes the textual LLVM IR next to the binary (`./.marina/<name>.ll`) so you can inspect what your program compiles to. From this repo you can also use `build/wave run examples/hello-world.tsm`.

## The language

Marina is a **subset** of TypeScript, compiled ahead-of-time. What it does have:

### Types & inference

| Type | Example |
|---|---|
| `Int8` `Int16` `Int32` `Int64` | `let a = 10` is `Int32`; `let b: Int8 = -12` |
| `UInt8` `UInt16` `UInt32` `UInt64` | `let n: UInt8 = 255` |
| `Float32` `Float64` | `let x = 20.5` is `Float64`; `let y: Float32 = 1.5` |
| `string` | `let s = "hi"` |
| `bool` | `let ok = true` |
| `Promise<T>` | return value of an `async function` |
| `Array<T>` | `let xs = [1, 2, 3]` (also `Array<Int32>` annotations) |
| `interface` | user-defined structural data contracts (see below) |
| generic `T` | type parameters on functions and classes (see below) |

Type annotations are available (`let a: Int32 = 10`, `function f(x: Int8): void {}`) and **required for function parameters** (strict mode). There is no `any`.

**Integers.** `Int8` through `Int64` are signed; `UInt8` through `UInt64` are unsigned. An unsuffixed integer literal is `Int32` when it fits, `Int64` when it fits only in 64 bits, and `UInt64` otherwise. A literal may be used as any integer type it fits in (`let b: Int8 = -12`, `let n: UInt64 = 10`). Different integer widths do not convert: `Int8` and `Int32` are different types, and signed does not convert to unsigned. Arithmetic, `%`, and comparisons require both operands to be the same integer type (a literal adopts the other operand's type when it fits). Division and remainder are truncating; unsigned operations wrap modulo 2^bits. Unary `-` is only allowed on signed integers.

**Floats.** Unsuffixed float literals are `Float64`. `Float32` is available as an annotation (`let y: Float32 = 1.5`); a float literal adopts `Float32` when that is the expected type. `Float32` widens to `Float64`. Any integer widens to `Float32` or `Float64` in mixed arithmetic and assignment — if either operand is `Float64` the result is `Float64`, otherwise a `Float32` operand keeps the result `Float32`:

```ts
let a = 10              // Int32
let b = 20.5            // Float64
let result = a + b      // Float64 (a is widened)
let s: Int8 = -12
let w: Float32 = 1.5
let z = s + w           // Float32
```

Short aliases are accepted too: `i8`/`int8`, `i16`/`int16`, `i32`/`int`/`int32`, `i64`/`int64`, `u8`/`uint8`, `u16`/`uint16`, `u32`/`uint32`, `u64`/`uint64`, `f32`/`float32`, `f64`/`float`/`float64`/`double`.

String arithmetic is rejected — use template literals:

```ts
console.log(`The result of ${a} + ${b} is ${result}`)
```

### Variables: `let` vs `const`

`let` declares a **mutable** variable; `const` declares an **immutable** one. Assigning to a `const` in any form — plain assignment, `++`/`--`, `+=`/`-=` — is a compile-time error, and `const` declarations must be initialized:

```ts
let y = 1
y = 2          // ok — let is mutable
y++

const x = 10
x = 20         // error: cannot assign to 'const x'
x++            // error
```

`const` freezes the **binding**, not the object it refers to (same as TypeScript): you can still mutate an array's contents through a `const` binding with `arr[0] = v` or `arr.push(...)`, but you cannot rebind it with `arr = ...`. A `for` loop can declare its init variable as `const` too — updating it inside the loop is then rejected.

### Top-level variables are globals

Variables declared at the top level of a file live for the whole program and are visible inside **every** function and lambda — even ones defined before the declaration executes is fine, as long as the function runs after it. Lambdas capture them by reference to the global: reads always see the current value, and assigning back (`todos = ...`) writes the shared global rather than a private copy. Locals inside functions keep normal by-value capture semantics (assigning to a captured local from a lambda is a compile-time error).

```tsm
let counter = 0            // global

function bump() {
    counter += 1           // reads and writes the global
}

bump()
console.log(counter)       // 1
```

### Arrays

Typed lists with literal syntax, indexing, and the usual methods. Element types are inferred (or taken from an annotation, e.g. an empty `let a: Array<string> = []`). Indexing (`a[i]`) and index assignment (`a[i] = v`) are bounds-checked and abort the program with a runtime panic on out-of-bounds access.

```ts
let xs = [1, 2, 3, 4, 5]          // Array<Int32>

xs.push(6)
xs.pop()
xs.unshift(0)
xs.splice(2, 1)
console.log(xs.length)            // 5

xs[0] = 10                        // index assignment
console.log(xs[0])                // 10

let strs: Array<string> = []
strs.push("hello")
strs.push("world")
console.log(strs.join("-"))       // hello-world

xs.forEach((item) => {
    console.log(item)
})

let doubled = xs.map((item) => {
    return item * 2               // Array<Int32>
})

let big = xs.filter((item) => {
    return item > 3
})

let sum = xs.reduce((acc, item) => {
    return acc + item
}, 0)
```

Supported methods: `push`, `pop`, `shift`, `unshift`, `splice`, `slice`, `concat`, `join`, `reverse`, `sort`, `length`, and the higher-order `forEach`, `map`, `filter`, `reduce` (callbacks are lambdas with strict types). `sort` compares numbers numerically and strings bytewise; strings also support `.length`. Arrays are reference-counted like strings, and nested arrays (`[[1, 2], [3, 4]]`) work too.

### Objects & classes

Objects are struct-of-fields values, ARC-managed like strings and arrays. There are two ways to get one:

**Ghost classes** — an object literal infers its shape at compile time. The checker mints an anonymous class named `Class_{variable}_{NNNN}` (e.g. `Class_abc_0001`) from the literal; field types come from the values:

```ts
let abc = {
    a: 5,          // Int32
    b: 6,          // Int32
    c: "Test"      // string
}

console.log(abc.a)   // 5
abc.b = 10           // field write (also +=, -=, ++, --)
console.log(abc)     // Class_abc_0001 { a: 5, b: 10, c: Test }
```

This is "JSON objects are automatic objects": no class definition is needed, and the shape is still fully static — accessing `abc.d` is a compile-time error, and every field has one concrete type.

**Declared classes** — `class` defines a named shape. Fields and methods are public by default; `public` is optional and `private` restricts a field or method to the class's own methods and constructor. Instantiate with `new Name()`:

```ts
class Point {
    public x: Float64
    public y: Float64
    label: string
}

let origin = new Point()
origin.x = 0.0
origin.y = 0.0
origin.label = "origin"
```

With a class annotation on a `let`, an object literal must match the class field-for-field (missing or extra fields are errors):

```ts
let p: Point = { x: 1.0, y: 2.0, label: "p1" }   // ok
let q: Point = { x: 1.0 }                        // error: missing field 'y'
```

Field rules (all strict, compile-time):
- Reading or writing an unknown field is an error that lists the available fields
- Fields are addressed by index in the generated code; the runtime bounds-checks anyway
- Objects can be nested (`{ pos: { x: 1, y: 2 } }`), stored in arrays (`Array<Point>`), captured by lambdas, and printed by `console.log` (which shows the class name and every field)
- Declared classes may have methods (`name(params) { ... }`), lowered to `Class__method(this, ...)`. `private` methods can only be called from inside the class

### Inheritance & TypeScript-style OOP

Classes support the full TypeScript OOP surface: single inheritance, abstract classes, access modifiers, statics, accessors and `super`.

```ts
interface Describable {
    describe(): string,
}

abstract class Shape {
    protected originX: Float64      // visible here and in subclasses
    static created: Int32           // static field (shared, not per-object)

    constructor(originX: Float64) {
        this.originX = originX
        Shape.created = Shape.created + 1
    }

    abstract area(): Float64;       // subclasses must implement

    describe(): string {            // virtual — overridden methods dispatch
        return `area ${this.area()}`
    }

    get origin(): string {          // getter: `s.origin`
        return `(${this.originX}, 0)`
    }
}

class Circle extends Shape implements Describable {
    radius: Float64

    constructor(x: Float64, radius: Float64) {
        super(x)                    // base constructor call
        this.radius = radius
    }

    area(): Float64 { return 3.14 * this.radius * this.radius }

    override describe(): string {
        return `circle: ${super.describe()}`
    }
}
```

Semantics:
- **Inheritance** — `class Sub extends Base`: flattened field layout (base fields first), inherited fields/methods/statics are callable on the subclass, and a subclass value is accepted wherever the base is expected (assignment, parameters, `Array<Base>`)
- **Virtual dispatch** — calls on an abstract receiver or an overridden method dispatch on the runtime class id, so `Array<Shape>` polymorphism works with zero runtime overhead for non-virtual calls
- **Abstract classes** — `abstract class` cannot be instantiated (`new Shape()` is an error); `abstract` methods have a signature only and must be implemented by concrete subclasses
- **Visibility** — `private` members are usable only inside the defining class; `protected` also inside subclasses; anything else is a compile-time error
- **Statics** — `static` fields/methods/getters belong to the class (`Shape.created`, `Shape.total`), are inherited, and are stored as module globals
- **Getters/setters** — `get x()` / `set x(v)` read and write like fields: `obj.x = v` calls the setter, `obj.x` the getter
- **`super`** — `super(args)` calls the base constructor (required in a subclass constructor when the base declares one); `super.method(args)` calls the base implementation directly
- **`implements`** — `class Sub implements I1, I2` verifies the interfaces' fields and methods (inherited implementations count) at compile time


### Generics

Functions and classes can take type parameters, monomorphized at compile time: every distinct instantiation generates its own specialized copy of the code (the generated LLVM function is named `name__Int32`, `Box$string`, ...), so generics cost nothing at runtime.

```ts
function pick<T>(a: T, b: T, takeFirst: bool): T {
    if takeFirst {
        return a
    }
    return b
}

class Box<T> {
    value: T
    label: string
}

function main() {
    let n = pick(10, 20, true)          // Int32 — inferred from the arguments
    let s = pick<string>("a", "b", false)
    let box = new Box<Int32>()          // explicit type arguments
    box.value = 42
}
```

Rules:
- Type bindings are inferred from the call arguments when possible; explicit type arguments (`pick<Int32>(...)`) override inference
- A generic function whose return type is annotated may call itself recursively; without an annotation, recursion needs an explicit type argument at the recursive call
- Generic classes are instantiated with `new Name<T>()`; each `T` used produces a distinct static class with its own field layout
- Generic classes support constructors and methods (the std `map` module is built on this): every method is specialized per instantiation with a `this` parameter, and `private` methods are usable inside the class

### Interfaces

`interface` declares a structural data contract: any object — a ghost class (object literal) or a declared class — that has the interface's fields with matching types conforms to it, and extra fields are allowed. Conformance is checked at compile time.

```ts
interface Point {
    x: Int32
    y: Int32
    name: string
}

function describe(pt: Point): string {
    return `${pt.name}: ${pt.x}, ${pt.y}`
}

let p = { x: 1, y: 2, name: "origin" }   // object literal conforms
class Vehicle {
    name: string
    wheels: Int32
}
let car = new Vehicle()                  // declared classes conform too (extra fields ok)
car.name = "bolt"

describe(p)
describe(car)                            // ok: has `name` (plus more)
```

An interface may also declare methods, TypeScript-style (`title(): string` — a signature, no body). A class conforms only when it has a public method with the same name and the same parameter and return types. Calling a method on an interface value dispatches to that class's method. Field access is still resolved **by name** through the runtime (`marina_object_get_field`). Interfaces can also be used as array element types (`Array<Named>` collects values of different concrete shapes).

### Naming conventions (strict)

The checker enforces the naming rules and rejects the program otherwise:

| Thing | Rule | Example |
|---|---|---|
| File name | kebab-case | `user-profile.tsm` |
| Functions | camelCase | `fetchData` |
| `let` variables | camelCase | `maxRetries` |
| `const` variables | UPPER_SNAKE_CASE | `MAX_RETRIES` |
| Classes | PascalCase | `UserProfile` |

Ghost classes are exempt (their names are generated by the compiler), and `main` is exempt from the camelCase function rule.

### Async

`async/await` with JS-style promises. `setTimeout(callback, ms)` schedules work on the runtime's timer thread. The program exits when the event loop reaches quiescence: no pending tasks, no pending timers, and no unresolved promises.

```ts
async function fetchData() {
    return new Promise((resolve) => {
        setTimeout(() => {
            resolve("Data fetched")
        }, 1000)
    })
}

async function main() {
    let data = await fetchData()  // suspends until the promise resolves
    fetchData()                     // fire-and-forget
    console.log(data)
}
```

Under the hood, each `async function` compiles to an entry function (allocates a frame, creates the promise) plus a `resume` **state machine** — one basic block per continuation segment. `await` registers the frame with the runtime and returns; when the promise resolves, the runtime calls `resume(frame, state, value)` on a worker thread and execution continues from the right segment.

### Control flow

`if`/`else`, `while`, and C-style `for` loops, with `++`, `--`, `+=`, and `-=`:

```ts
for (let i = 0; i < 10; i++) {
    console.log(i)
}

let n = 5
while (n > 0) {
    n--
}

let total = 0
for (let i = 0; i < 4; i += 2) {
    total += i
}
```

Loop conditions must be `bool` (no truthiness), and the init variable of a `for` is scoped to the loop.

### Exceptions: `try` / `catch` / `throw`

`throw` raises an exception carrying a `string` message; `try`/`catch` runs code and handles anything raised inside it — including **runtime panics** (array index out of bounds, popping an empty array, fs/sql errors), which are catchable like in JavaScript:

```tsm
let numbers = [1, 2, 3]

try {
    throw "custom failure"
} catch (err) {
    console.log(`caught: ${err}`)  // caught: custom failure
}

try {
    console.log(numbers[10])       // runtime panic: index out of bounds
} catch (err) {
    console.log(`caught: ${err}`)  // caught: array index 10 out of bounds (length 3)
}
```

The `catch` binding is a `string` (the error message) and is optional — a bare `catch { ... }` ignores it. Exceptions propagate across function calls to the nearest enclosing `try`; an exception that reaches the top of the program prints `uncaught exception: ...` (or `marina runtime panic: ...` for runtime errors) and exits with status 101. `throw` requires a string value, and `await` is not allowed inside a `try` or `catch` block (v0.2). There is no `finally`.

### Entry point

Define `main` (sync or `async`). If it is `async`, the runtime drives its promise to completion before exiting.

## Standard libraries: `net`, `http`, `fs`, `crypto`, `sql` and `map`

Std modules ship with the compiler and runtime — bring them in with a top-level `import` statement (`import http`). **`http` is built on top of `net`**: its client literally composes `net.connect`/`write`/`read_all`/`close` in the runtime. **`map` is different: it is written in Marina itself** (`stdlib/map/main.tsm`) — a generic `Map<V>` class monomorphized per value type, exactly like a user-defined module.

### `net` — raw TCP

```tsm
import net

async function main() {
    let s = net.connect("localhost", 8080)  // returns a Stream (blocks until connected)
    s.write("ping")                          // write bytes
    let reply = s.read_all()                 // read to EOF (30s timeout)
    console.log(reply)
    s.close()
}
```

`net.connect(host, port) -> Stream`; a `Stream` has exactly three methods: `write(str)`, `read_all() -> string`, `close()`. Reads time out after 30 seconds.

### `http` — client and server

```tsm
import http

// --- low-level server: http.listen(port, handler) ---
let todos = ["buy groceries", "ship v0.2"]
http.listen(8080, (req, res) => {
    if (req.path == "/todos") {
        res.json(todos)        // any value: arrays, objects, strings, numbers
    } else {
        res.send("try /todos") // plain text
    }
})
console.log("todo server: http://localhost:8080/todos")
// the event loop keeps the server alive — Ctrl-C to stop
```

The handler receives an `http_Request` (`method`, `path`, `body` — all strings) and an `http_Res` with `send(str)` and `json(value)`.

```tsm
// --- client ---
let resp = http.get("http://localhost:8080/todos")
console.log(resp.status)   // Int32
console.log(resp.body)     // response body as a string
let resp2 = http.post("http://localhost:8080/todos", "new todo")
```

`http.get(url) -> http_Response` and `http.post(url, body) -> http_Response` where `http_Response` has `status` (Int32) and `body` (string). Only `http://` URLs are supported (https is not available yet).

### `http` — the express-style server API

For real services there is a higher-level, express-flavored API: a `Server` with path-prefixed routers and a JSON body validator, all built from module-qualified `new` expressions and dotted type annotations.

```tsm
import http

let todos = ["Buy groceries", "Buy a new phone", "Buy a new car"]

// a schema: field `text` must be a string, required, 4..20 chars long.
// JsonValidator.string() / .number() start a chainable field validator;
// .required() / .min(n) / .max(n) refine it.
let todoSchemaValidator = new http.JsonValidator({
    text: http.JsonValidator.string().required().min(4).max(20)
})

async function main() {
    let server = new http.Server()
    let todosRouter = server.router({ path: "/todos" })

    todosRouter.get("/", (req: http.Request, res: http.Response) => {
        return res.json(todos)
    })
    todosRouter.post("/", (req: http.Request, res: http.Response) => {
        let todo = todoSchemaValidator.validate(req.body)  // JSON body -> object
        todos.push(todo.text)                              // validated field
        return res.json(todos)
    })

    await server.listen("0.0.0.0", 8080)
    console.log("Server is running on port localhost:8080")
}
```

What this exercises:
- `new http.Server()` / `new http.JsonValidator({...})` — `new` with module-qualified class paths (`new http.Server()`, `new a.b.Class(...)`)
- Dotted type annotations: `req: http.Request`, `res: http.Response`
- `server.router({ path: "/todos" })` — returns a router whose routes are prefixed with the path (a `"/"` route on the `/todos` router matches `/todos`)
- `router.get(path, handler)` / `router.post(path, handler)` — register a handler `(req: http.Request, res: http.Response) => void`; unmatched routes get a 404
- `await server.listen(host, port)` — binds asynchronously (host is a string, e.g. `"0.0.0.0"`); the await resolves once the port is bound, so the startup `console.log` prints *after* the server is live
- `validator.validate(req.body)` — parses the JSON body and returns a statically-typed object (a ghost class is synthesized from the schema, so `todo.text` is checked at compile time); a body that fails validation aborts the program with a runtime panic naming the field and the problem
- **Top-level variables are globals**: `todos` is declared at the top level and captured by the route handlers below — reads see the current value, and since `Array.push` mutates the array in place, every handler shares the same list

Try it:

```sh
wave run examples/todo-http.tsm
curl localhost:8080/todos                                   # GET  -> JSON array
curl -X POST localhost:8080/todos -d '{"text":"write rust"}' # POST -> appended
curl -X POST localhost:8080/todos -d '{"text":"hi"}'         # -> runtime panic (min 4 chars)
```

### `fs` — the filesystem

```ts
import fs

fs.writeFile("/tmp/demo.txt", "hello from marina")
console.log(fs.exists("/tmp/demo.txt"))     // true
let data = fs.readFile("/tmp/demo.txt")     // whole file as a string
fs.mkdir("/tmp/demo-dir")                   // creates parents as needed
let entries = fs.readDir("/tmp/demo-dir")   // Array<string>, sorted file names
fs.remove("/tmp/demo.txt")                  // file or directory (recursive for dirs)
```

- `fs.readFile(path) -> string`, `fs.writeFile(path, data) -> void`, `fs.exists(path) -> bool`, `fs.readDir(path) -> Array<string>`, `fs.mkdir(path) -> void`, `fs.remove(path) -> void`
- I/O errors (missing file, permission denied, ...) abort the program with a runtime panic naming the path and the OS error — there is no error value to handle (marina promises never fail either)

### `crypto` — hashing, encoding, randomness

Pure C++ implementations in the runtime (no external libraries beyond libc++/sqlite): a from-scratch SHA-256, hand-rolled base64, and `/dev/urandom` for randomness.

```ts
import crypto

console.log(crypto.sha256("hello"))          // 64-char lowercase hex digest
console.log(crypto.base64Encode("marina"))   // bWFyaW5h
console.log(crypto.base64Decode("bWFyaW5h")) // marina
let key = crypto.randomHex(8)                // 16 hex chars from 8 random bytes
let id = crypto.uuid()                       // random v4 UUID (8-4-4-4-12)
```

- `crypto.sha256(data) -> string`, `crypto.base64Encode(data) -> string`, `crypto.base64Decode(data) -> string`, `crypto.randomHex(numBytes) -> string`, `crypto.uuid() -> string`

### `sql` — databases (SQLite)

SQL with prepared statements via tagged templates. In v0.2 the only driver is **SQLite** (FFI against the system `libsqlite3` — macOS ships it; Linux needs the `sqlite3` package). `mysql`, `pg` and `mariadb` are accepted driver names but abort at compile time (or runtime panic) until implemented.

```ts
import sql

// driver: "sqlite" | "mysql" | "pg" | "mariadb" (only sqlite works in v0.2)
let db = new sql.Connection("sqlite", "/tmp/data.db")

// sql.stmt`...` prepares the statement; ${x} interpolations become ? binds
const R = db.execute(sql.stmt`CREATE TABLE IF NOT EXISTS todos (id INTEGER PRIMARY KEY, text TEXT)`)
db.execute(sql.stmt`INSERT INTO todos (text) VALUES (${"Buy groceries"})`)

// db.query collects rows as objects whose fields are the column names
let rows: Array<Todo> = db.query(sql.stmt`SELECT id, text FROM todos WHERE id = ${R.lastInsertId}`)

db.close()
```

- `new sql.Connection(driver, url)` — opens the database (SQLite: a file path; the file is created if missing)
- `db.execute(sql.stmt\`...\`)` — runs a statement that returns no rows (CREATE/INSERT/UPDATE/DELETE) and returns a `sql_Result` object with `lastInsertId: Int32` and `rowsAffected: Int32`; executing a statement that returns rows is a runtime panic (use `query`)
- `db.query(sql.stmt\`...\`)` — runs a statement and returns `Array<sql_Row>`; each row is an object whose fields are the SELECT's column names (integers → `Int32`, floats → `Float64`, text → `string`, NULL → void). Annotate the result with your own interface to get a typed result: `let rows: Array<Todo> = db.query(...)` — the checker accepts it because `sql_Row` is a fieldless interface that coerces to anything
- `db.close()` — closes the connection; using it afterwards is a runtime panic
- Binds accept any integer (`Int8`–`Int64`, `UInt8`–`UInt64`), any float (`Float32`, `Float64`), `string`, `bool` and null; anything else is rejected by the checker. Values are passed to SQLite as a 64-bit integer or a 64-bit float (`UInt64` above 2^63−1 does not round-trip through SQLite's signed integer)
- Values interpolated into `sql.stmt\`...\`` are always bound as parameters (prepared statements) — never string-concatenated, so no SQL injection

### `map` — a dictionary, written in Marina itself

`map` is the proof that the module system and generics are real: the whole module is **Marina code** (`stdlib/map/main.tsm`), not a C++ builtin. It provides a string-keyed dictionary `Map<V>` whose values can be any one type — each distinct `V` produces its own specialized copy of the code, exactly like generic functions — so it costs nothing at runtime.

```ts
import map

let ages = map.newMap<Int32>()   // one factory call per value type
ages.set("lena", 30)
ages.set("kim", 25)
ages.set("lena", 31)             // set overwrites an existing key

console.log(ages.get("lena"))    // 31
console.log(ages.size)           // 2
console.log(ages.has("kim"))     // true
console.log(ages.keys)           // [lena, kim] — insertion order
console.log(ages.values)         // [31, 25]

ages.delete("kim")
ages.clear()                     // remove everything
```

API:
- `map.newMap<V>() -> Map<V>` — create an empty map (the factory is the public way in; generic `new map.Map<V>()` is not supported yet)
- `set(key: string, value: V): void` — insert or overwrite
- `get(key: string) -> V` — the value at `key`; a missing key aborts with the array bounds panic (no error values — same philosophy as fs/http)
- `getOr(key: string, fallback: V) -> V` — safe read: returns `fallback` when the key is absent
- `has(key: string) -> bool` — key present?
- `delete(key: string): void`, `clear(): void`
- getters: `size -> Int32`, `keys -> Array<string>`, `values -> Array<V>` (in key insertion order)

Implementation notes: `Map<V>` keeps two parallel private arrays (`keyList: Array<string>`, `valList: Array<V>`) in sync and does a linear scan for lookup — fine for the small collections Marina targets. Everything (including the `private indexOf` helper) is ordinary Marina: a generic class with methods, exported through a module.

## How a program is compiled

1. **Lex & parse** → AST
2. **Type check** — strict, with inference; a type error aborts compilation (the program "does not pass")
3. **Codegen** — textual LLVM IR (`.ll`), including ARC retain/release insertion and async state machines
4. **Link** — `clang` compiles the `.ll` and links `libmarina_runtime.a` into a native binary

The runtime exports a stable C ABI (`marina_retain`, `marina_release`, `marina_promise_create`, `marina_await`, `marina_set_timeout`, `marina_print`, …) that the generated IR calls directly.

## Editor support

Marina ships its own language server (`marina-lsp`, in `compiler/lsp/main.cpp`) — a dependency-free JSON-RPC implementation over stdio that reuses the compiler's lexer/parser/checker.

**Features**

- **Syntax highlighting** — TextMate grammar for Marina (keywords, types, strings/template literals, declarations)
- **🌊 Wave file icons** — `.tsm` files get a wave icon (pick the *Marina Wave* file icon theme once)
- Live diagnostics on open/keystroke/save: the full strict type checker plus file-naming rules (`.tsm`, kebab-case stems)
- Hover on a **declared class** name → field shape (`x: Int32`, `y: Int32`, …)
- Hover on an **object variable** → the shape of its class, including auto-created ghost classes (`Class_abc_0001 (ghost class)`)

**Install for Cursor / VS Code**

1. Install the server on your `PATH`:
   ```
   cmake -S . -B build && cmake --build build -j && cp build/marina-lsp /usr/local/bin/
   ```
   (builds the `marina-lsp` server)
2. Extensions panel → `…` → **Install from VSIX…** → `editors/marina-lsp/marina-lsp-0.2.2.vsix`
   (rebuild with `editors/marina-lsp/build-vsix.sh`)
3. Open any `.tsm` file — highlighting applies immediately and diagnostics appear on open and every edit. If the binary
   is elsewhere, set `marina.serverPath` in settings. For the 🌊 wave icons:
   `Ctrl/Cmd+Shift+P` → *Preferences: File Icon Theme* → **Marina Wave**.

## Example programs

| File | Demonstrates |
|---|---|
| `examples/hello-world.tsm` | variables, strings, `console.log` |
| `examples/calculate.tsm` | Int32/Float64 inference, Int→Float widening, template literals |
| `examples/numeric-types.tsm` | Int8–Int64, UInt8–UInt64, Float32; literal fitting, same-width arithmetic, integer→float widening |
| `examples/for-loop.tsm` | C-style `for` loops, `++`, loop scoping |
| `examples/const-test.tsm` | `let` vs `const`: mutable bindings, immutable bindings, naming rules |
| `examples/array-list.tsm` | array literals, mutation methods, `forEach`/`map`/`filter`, `console.log` of arrays |
| `examples/array-extra.tsm` | string/float/nested arrays, indexing, `join`, `sort`/`reverse`, async functions over arrays |
| `examples/async-example.tsm` | `async/await`, promises, promise executors, `setTimeout`, fire-and-forget calls |
| `examples/objects.tsm` | ghost classes from object literals, declared classes, field access/mutation, objects in arrays |
| `examples/todo-http.tsm` | express-style server: `new http.Server`, `server.router`, `router.get/post`, `new http.JsonValidator`, `await server.listen`, top-level globals shared with handlers (try `curl localhost:8080/todos`) |
| `examples/http-get.tsm` | `import http` client: `http.get`, reading `resp.status`/`resp.body` (start the todo server first) |
| `examples/tcp-client.tsm` | `import net`: `net.connect`, `stream.write`/`read_all`/`close` (run against `nc -l 8080`) |
| `examples/generics.tsm` | generic functions with inferred and explicit type arguments, generic `class Box<T>` |
| `examples/interfaces.tsm` | structural interfaces: fields, methods dispatched by class, `Array<Named>`, private methods |
| `examples/inheritance.tsm` | TypeScript-style OOP: `extends`, `implements`, abstract classes/methods, virtual dispatch through `Array<Shape>`, `protected`, statics, getters, `super` |
| `examples/oop-all.tsm` | full OOP sweep: two-level inheritance with `super.method()`, abstract members implemented (and inherited), `get`/`set` pairs, statics + static getters, interfaces satisfied by inheritance, polymorphic factory functions |
| `examples/map-demo.tsm` | `import map`: the std `Map<V>` dictionary — `newMap`, `set`/`get`/`getOr`/`has`/`delete`/`clear`, `size`/`keys`/`values` getters, overwrite-on-set, key iteration, one instantiation per value type |
| `examples/try-catch.tsm` | `try`/`catch`/`throw`: catching thrown strings, catching runtime panics, the catch binding, propagation across calls, `return` inside a `try` |
| `examples/fs-crypto.tsm` | `import fs` (write/read/exists/mkdir/readDir/remove) and `import crypto` (sha256, base64, randomHex, uuid) |
| `examples/todos-http-sqlite.tsm` | `import sql`: `new sql.Connection`, `sql.stmt\`...\`` prepared statements, `db.execute`/`db.query`, typed rows via interfaces — a persistent todo REST API on SQLite (try `curl localhost:8080/todos`) |

## Current limitations (v0.2)

- Packages expose functions and classes only: no top-level statements, no `main`, and no package-on-package dependencies (std modules `net`/`http`/`fs`/`crypto`/`sql`/`map` are allowed)
- sql: SQLite only (via the system `libsqlite3`); `mysql`/`pg`/`mariadb` drivers are planned but abort at compile time; rows of `db.query` are dynamically bound to the SELECT's columns (no nested transactions, no prepared-statement reuse across calls)
- http: `http://` URLs only (no TLS/https), sequential request handling on the server (one connection at a time), no request headers, no streaming responses; one HTTP server per program; the express-style API supports `get`/`post` routes only
- Schema validation (`JsonValidator.validate`) aborts the program with a runtime panic on an invalid body (no per-request error responses yet)
- net reads time out after 30 seconds
- Classes have no multiple inheritance and no method overloading (interface methods must match exactly; no default implementations); an interface field cannot be an `Array` or `Promise`
- Interfaces cannot appear as class fields through `Array<Interface>`/`Promise<Interface>` annotations yet
- Generic recursion requires an annotated return type (or an explicit type argument at the recursive call); no generic constraints/bounds on type parameters
- No `break`/`continue` inside loops
- `await` is only allowed at the top level of an `async function` body (not inside `if`/`while`/`for`/`try`)
- Exceptions carry a string message only (`throw` takes a string, `catch` binds a string); no `finally`, no typed/structured errors, no re-throw sugar
- Promises never reject (`reject` is rejected by the checker); no promise-of-promise
- Lambdas are supported only where the compiler needs them (promise executors, `setTimeout` callbacks, array callbacks)
- Return types of unannotated functions are inferred in declaration order — recursive calls to the function being checked need an explicit return-type annotation