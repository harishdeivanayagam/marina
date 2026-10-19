# marina-lsp (VSIX)

Language support for **marina** (`.tsm`) programs in Cursor / VS Code.

## Features

- **Syntax highlighting** — full TextMate grammar for marina: keywords,
  types (`Int32`, `Promise<Int32>`, …), strings/template literals, comments,
  numbers, function/class/variable declarations.
- **🌊 Wave file icons** — `.tsm` files get a custom wave icon
  (select the **Marina Wave** file icon theme once: `Ctrl/Cmd+Shift+P` →
  *Preferences: File Icon Theme* → *Marina Wave*).
- **Live diagnostics** — the full marina type checker runs on every save/keystroke:
  strict mode errors, naming conventions (`camelCase` functions/`let`s,
  `UPPER_SNAKE_CASE` consts, `PascalCase` classes), and object/class checks.
- **Hover** — hover a class name (`new Point()`), an object variable (`abc`), or
  a ghost class to see its field shape:
  - declared class: `**class Point** - x: Int32 - y: Int32`
  - object literal `let abc = { a: 5, ... }` shows the auto-created ghost class
    `**Class_abc_0001** (ghost class)`.

## Install

1. Build the tooling binaries (installs `wave`, `marina` and `marina-lsp`):

   ```
   cargo install --path crates/wave
   cargo install --path crates/marina
   ```

2. Install the extension: Extensions panel → `…` menu →
   **Install from VSIX…** → select `marina-lsp-0.2.2.vsix`
   (build it with `editors/marina-lsp/build-vsix.sh`).

3. Open any `.tsm` file. If the server is not on your `PATH`, point
   `marina.serverPath` at the binary. To get the wave icons, pick the
   *Marina Wave* file icon theme.

## Building the VSIX

```
cd editors/marina-lsp
./build-vsix.sh   # produces marina-lsp-0.2.2.vsix (uses zip only)
```