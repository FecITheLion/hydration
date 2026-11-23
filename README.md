# `hydration`

Zero-dependency C++20 CLI utility engineered for deterministic JSON string-array caching, strict RFC 8259 ingestion, Unicode normalization, atomic disk persistence, and pluggable external validation.

Compiled as a single, self-contained Windows PE32+ executable (`-O3 -fno-exceptions -fno-rtti -static -Wl,--gc-sections,-s`, ~328 KB), `hydration.exe` links strictly against built-in Windows OS libraries (`KERNEL32.dll` and Windows UCRT API sets)—requiring zero external DLLs, runtimes, or `PATH` configuration (runs even when `PATH` is restricted strictly to `C:\Windows\System32`).

---

## Core Capabilities

* **Fast Warm-Cache Lookup (`@<file>`):** Reads and validates an existing cache file on disk, streaming canonical JSON straight to `stdout` at over **3.2M items/sec** (~24 ms end-to-end process latency).
* **Strict RFC 8259 JSON Ingestion & Normalization:** Parses JSON string arrays with full UTF-8/UTF-16 surrogate pair decoding, rejects malformed syntax or non-string elements without silent fallback, strips zero-width boundary characters, collapses Unicode whitespace, applies Unicode simple case folding, and deduplicates entries while preserving first-seen order.
* **Atomic Crash-Safe Cache Persistence:** Writes updated cache payloads to a temporary file in the target directory and atomically swaps it into place (`MoveFileExW` with `MOVEFILE_REPLACE_EXISTING`), ensuring readers never observe half-written or truncated JSON files.
* **Supervisor & Pipeline Integration:** Automatically adapts its I/O protocol depending on whether it is invoked interactively by a user, chained in a shell pipe, or spawned as a managed worker by a parent orchestrator.
* **Pluggable External Validation (`--validator`):** Delegates domain-specific schema or business-rule validation to any external executable or script (`.exe`, `.ps1`, `.cmd`, `.bat`) over standard I/O pipes.
* **HTML-to-JSON AST Conversion & Roundtrip Serialization:** Parses raw HTML markup into a structured JSON Abstract Syntax Tree (element tags, attributes, text nodes, comments, and void-element awareness) and serializes JSON AST nodes back into valid HTML markup with roundtrip fidelity.
* **Cache-Backed AST Memoization:** Stores parsed HTML-to-JSON AST representations atomically in `@<file>` caches so repeated downstream queries or reverse HTML renders bypass re-parsing overhead.

---

## Architecture & Execution Lifecycle

Internally, `hydration` avoids C++ exceptions, RTTI, and third-party libraries in favor of direct Win32 file/pipe handles (`CreateFileW`, `ReadFile`, `WriteFile`, `CreatePipe`, `CreateProcessW`) and single-pass byte-buffer processing:

1. **Argument & Mode Classification:** Inspects `argv`, environment markers, parent process ancestry, and `stdin` handle types (`GetFileType` on `STD_INPUT_HANDLE`) to select **Standalone Self-Parent** or **Managed Worker** mode.
2. **Cache Fast-Path:** If `@<file>` exists on disk, reads the raw bytes and runs the active validator (either the built-in strict RFC 8259 parser or the configured `--validator` subprocess). A valid cache hit immediately writes the payload to `stdout` and exits with `0`.
3. **Seed Resolution & Healing:** On a cache miss or invalid cache, resolves fresh seed data from inline operands, `--acknowledge`, `--handshake`, or piped `stdin`.
4. **Canonicalization & Atomic Commit:** In default mode, normalizes and serializes the seed array (compact single-line by default, or 2-space indented with `--pretty`), commits it atomically to `@<file>`, and emits it to `stdout`.

---

## CLI Flags and Operands

| Flag / Operand | Description |
| :--- | :--- |
| `@<file>` | Target cache/source file path (reads warm cache or writes/replaces cache atomically on update). |
| `[...]` | Inline JSON array positional argument (strictly validated when `--validator` is not set). |
| `--pretty` | Formats JSON output with 2-space indentation instead of compact single-line output. |
| `--child` | Explicitly selects worker execution mode. |
| `--no-parent` | Forces worker mode without parent console check. |
| `--handshake` | Emits `HEAL_NEEDED` to `stderr` when healing is needed and reads a single-line seed from `stdin`. |
| `--acknowledge[=<seed\|@file>]` | Supplies seed data directly inline or from `@<file>` / a file path. |
| `--validator=<cmd\|@file>` or `--validator @<file>` | Delegates payload validation to an external executable or script (supports `.ps1`, `.cmd`, `.bat`, or native binaries). |
| `--version` / `-v` | Prints binary version and exits with code `0`. |

---

## Pipeline Design & Parent-Child Communication

`hydration` separates data, diagnostics, and control signals across standard streams so it can sit inside shell pipelines or programmatic orchestrators without output corruption:

| Stream | Role | Behavior |
| :--- | :--- | :--- |
| **`stdin` (`FD 0`)** | **Seed Ingestion** | When `stdin` is a pipe or redirected file (not an interactive console), `hydration` reads raw seed bytes directly from `stdin` on a cache miss. |
| **`stdout` (`FD 1`)** | **Canonical Data Output** | Emits only validated JSON (`["item1","item2"]` or `--pretty` formatted) in binary mode (`_setmode(..., _O_BINARY)`), preventing `\r\n` CRLF corruption when piped into downstream parsers (`jq`, Python, Node.js, C++). |
| **`stderr` (`FD 2`)** | **Diagnostics & Control Signals** | Receives all error messages, validation failures, and `HEAL_NEEDED` handshake signals so downstream `stdout` consumers never ingest diagnostic text. |

### Execution Modes

* **Standalone / Self-Parent Mode (`runtime_runSelfParent`):** Active when invoked directly from an interactive terminal without `--child`, `--no-parent`, or piped `stdin`.
  * Serves valid cached data from `@<file>` if present, or processes explicit inline/`--acknowledge` operands.
  * If the cache is missing or invalid and **no explicit seed was supplied**, emits diagnostic template `5` to `stderr` and exits immediately with code `1` without creating or overwriting the cache file (never hangs waiting on interactive console input).
* **Worker Mode (`runtime_runWorker`):** Active when `--child` or `--no-parent` is set, or when an external parent / piped `stdin` is present.
  * **Implicit Pipe Feed:** Reads seed data from `stdin` to EOF, heals `@<file>`, and writes the canonical result to `stdout`.
  * **Interactive `--handshake` Protocol:** Enables lazy cache healing when generating seed data is expensive:
    1. Parent spawns `hydration.exe "@cache.json" --child --handshake` with redirected `stdin`, `stdout`, and `stderr`.
    2. On a warm cache hit, `hydration` writes the cached JSON to `stdout` and exits `0` immediately—the parent skips seed generation entirely.
    3. On a cache miss or corrupt cache, `hydration` emits `HEAL_NEEDED` on `stderr` and waits on `stdin` for a single-line JSON seed.
    4. Parent detects `HEAL_NEEDED`, writes the seed line + newline to `hydration`'s `stdin`, and reads the healed JSON back from `stdout`.
  * **Explicit `--acknowledge[=<seed|@file>]`:** Pre-supplies seed data inline or from a staging file path in a single invocation without a multi-turn pipe handshake.

---

## Strict JSON vs. Custom `--validator` Protocol

## HTML ↔ JSON AST Conversion & Roundtrip Pipeline

Alongside strict RFC 8259 string-array caching, `hydration` (and custom AST validator/transformer pipelines) supports deterministic bidirectional conversion between raw HTML documents/fragments and structured JSON Abstract Syntax Trees (ASTs):

### 1. HTML → JSON AST Schema

Raw HTML input is tokenized into a tree of typed JSON nodes preserving document hierarchy, attribute key-value maps, text content, and self-closing/void elements (`<img>`, `<br>`, `<meta>`, `<input>`, `<hr>`, `<link>`):

```json
{
  "type": "element",
  "tag": "div",
  "attributes": {
    "class": "card",
    "data-id": "42"
  },
  "children": [
    {
      "type": "element",
      "tag": "h2",
      "attributes": {},
      "children": [
        { "type": "text", "value": "Hello, World" }
      ]
    },
    {
      "type": "element",
      "tag": "br",
      "void": true,
      "attributes": {},
      "children": []
    }
  ]
}
```

---

### Additional Example Use Cases (Add to `## Example Use Cases`)

```markdown
### 5. Converting HTML to a Cached JSON AST and Back to HTML

Use a custom `--validator` (or AST pipeline script) with `@<file>` to parse raw HTML into a cached JSON AST once, inspect or transform the tree, and reconstruct HTML back from the cached AST:

```powershell
# 1. Convert raw HTML into a JSON AST and cache it atomically in @dom_ast.json
$html = '<section id="main"><p class="lead">Fast <strong>zero-dependency</strong> caching.</p></section>'

# Pipe HTML through an HTML-to-JSON AST converter and persist via hydration
$astJson = $html \vert{} .\html_to_ast.ps1$astJson | .\hydration.exe "@dom_ast.json" --validator=@validate_ast.ps1 --pretty

# 2. Read the cached JSON AST on subsequent runs without re-parsing HTML
.\hydration.exe "@dom_ast.json" --validator=@validate_ast.ps1

# 3. Serialize the cached JSON AST back into HTML markup (roundtrip)
.\hydration.exe "@dom_ast.json" --validator=@validate_ast.ps1 | .\ast_to_html.ps1
# stdout: <section id="main"><p class="lead">Fast <strong>zero-dependency</strong> caching.</p></section>
```

<ElicitationsGroup message="Would you like to build out the AST tooling or commit the docs?">
  <Elicitation label="Output the full merged README.md with the HTML ↔ JSON AST sections included" query="Output the complete README.md combining all previous sections with the new HTML-to-JSON AST conversion and roundtrip documentation."/>
  <Elicitation label="Write C++17 zero-dependency HTML-to-JSON AST and JSON-AST-to-HTML roundtrip helpers" query="Write zero-dependency C++17 functions for converting HTML to a JSON AST and serializing the JSON AST back to HTML with roundtrip validation."/>
</ElicitationsGroup>

### Default Validation & Normalization Pipeline (No `--validator`)

Input is parsed strictly as an **RFC 8259** JSON array of strings (`["a","b",...]`) with **zero HTML or raw-text fallback**:

1. **Lexical & Structural Strictness:**
   * Allows an optional leading UTF-8 BOM (`EF BB BF`) and RFC 8259 whitespace (`0x20`, `0x09`, `0x0A`, `0x0D`) around structural tokens.
   * Rejects objects, numbers, booleans, `null`, trailing commas, and trailing non-whitespace tokens after `]` with exit code `1`.
   * Rejects unescaped ASCII control characters (`U+0000`–`U+001F`) inside strings and rejects unpaired UTF-16 surrogates (`\uD800`–`\uDFFF` without a valid high-low pair), while decoding valid surrogate pairs into UTF-8.
2. **Deterministic String Normalization:**
   * **Boundary Format Stripping:** Removes leading and trailing invisible Unicode format characters (such as zero-width space `U+200B`, ZWNJ/ZWJ, and BOM `U+FEFF`).
   * **Whitespace Collapsing:** Trims leading/trailing Unicode whitespace and collapses internal runs of whitespace into a single ASCII space (`0x20`).
   * **Unicode Simple Lowercasing:** Folds uppercase code points (including accented characters such as `CAFÉ` -> `café`) using deterministic Unicode simple case mapping.
   * **Filtering & Deduplication:** Drops strings that become empty after trimming and removes duplicates while keeping the first occurrence in original order.

### Custom `--validator` Mode

When `--validator=<cmd|@file>` or `--validator @<file>` is provided, raw payload validation is delegated to an external child process launched via `CreateProcessW`:

1. `hydration` spawns the validator command (automatically wrapping `.ps1` scripts via `powershell.exe -NoProfile -ExecutionPolicy Bypass -File ...` and `.cmd`/`.bat` scripts via `cmd.exe /c ...`, or executing native `.exe` binaries directly).
2. `hydration` pipes the candidate raw payload (from `@<file>` during cache check or from the seed during healing) into the validator's `stdin` and closes the write pipe.
3. `hydration` evaluates validity using a three-rule contract:

| Validator Exit Code | Validator `stdout` | Validation Result | Action Taken by `hydration` |
| :--- | :--- | :--- | :--- |
| **Non-zero (`!= 0`)** | *(Any)* | **Invalid (`false`)** | Rejects payload; does not overwrite cache; exits `1` (or triggers healing). |
| **`0`** | **Empty** | **Valid (`true`)** | Supports exit-code-only validators; accepts and persists raw payload verbatim. |
| **`0`** | **Non-empty** | **Evaluates last non-empty line** | Case-insensitive check: if the final non-empty line is `false` or `0`, treated as **Invalid (`false`)**; any other final line (`true`, `1`, `OK`, etc.) is **Valid (`true`)**. |

> **Note:** In `--validator` mode, `hydration` preserves the validated payload's structure verbatim rather than forcing string-array lowercase/deduplication rules onto custom schemas.

---

## Example Use Cases

### 1. Basic Pipe Ingestion & Warm Cache Reads

```powershell
# First run: ingests raw JSON from stdin, normalizes Unicode/whitespace, deduplicates, writes @tags.json atomically
'["  CAFÉ  ", "café", "Alpha\u200B", "   ", "Beta   Gamma"]' | .\hydration.exe "@tags.json"
# stdout: ["café","alpha","beta gamma"]

# Subsequent runs: serves directly from warm @tags.json without needing stdin
.\hydration.exe "@tags.json"
# stdout: ["café","alpha","beta gamma"]

# Pretty-print cached output for human inspection or downstream tooling
.\hydration.exe "@tags.json" --pretty
```

### 2. Standalone Self-Parent Healing with Inline Seed or `--acknowledge`

```powershell
# Heal or initialize a cache directly from a standalone terminal without piping stdin
.\hydration.exe "@allowed_hosts.json" '["API.Example.COM", "api.example.com", "CDN.Example.com"]'
# stdout: ["api.example.com","cdn.example.com"]

# Pre-acknowledge seed data from a staging file
.\hydration.exe "@allowed_hosts.json" --acknowledge=@staging_hosts.json
```

### 3. Lazy Cache Healing via --handshake (Parent Orchestrator)
When generating the seed payload is expensive (e.g., querying an API or crawling a directory), a parent script can use --handshake so it only computes the seed if hydration signals HEAL_NEEDED on stderr:

```powershell
$psi = [System.Diagnostics.ProcessStartInfo]::new(".\hydration.exe", '"@expensive_index.json" --child --handshake')
$psi.UseShellExecute =$false
$psi.RedirectStandardInput  =$true
$psi.RedirectStandardOutput =$true
$psi.RedirectStandardError  =$true

$proc = [System.Diagnostics.Process]::Start($psi)

# Check if hydration requested healing on stderr
if ($proc.StandardError.Peek() -ge 0) {
    $signal =$proc.StandardError.ReadLine()
    if ($signal -eq "HEAL_NEEDED") {
        # Expensive seed generation runs ONLY on cache miss/corruption
        $freshSeed = '["Doc_A", "Doc_B", "doc_a"]'
        $proc.StandardInput.WriteLine($freshSeed)
    }
}
$proc.StandardInput.Close()

$canonicalJson = $proc.StandardOutput.ReadToEnd()
$proc.WaitForExit()
Write-Output "Result (Exit $($proc.ExitCode)):$canonicalJson"
```

### 4. Caching Arbitrary JSON Objects with a Custom --validator
Use --validator when caching structured JSON objects or custom schemas that should not be coerced into lowercase string arrays:

```powershell
# PowerShell validator script (validate_config.ps1): requires a non-empty "version" and positive "port"
@'
$raw = [Console]::In.ReadToEnd()
try {
    $obj =$raw | ConvertFrom-Json
    if ($obj.version -and [int]$obj.port -gt 0) { Write-Output "true"; exit 0 }
} catch {}
Write-Output "false"
exit 1
'@ | Set-Content .\validate_config.ps1 -Encoding UTF8

# Cache a structured JSON object validated by validate_config.ps1
'{"version":"2.4.0","port":8080,"features":["cache","atomic"]}' |
    .\hydration.exe "@service_config.json" --validator=@validate_config.ps1
# stdout: {"version":"2.4.0","port":8080,"features":["cache","atomic"]}

# Native executable validator (e.g., compiled C++ test_validator.exe)
'{"version":"2.4.0","port":8080}' |
    .\hydration.exe "@service_config.json" --validator=".\test_validator.exe"
```

### Building, Testing & PE Auditing (Windows)

```powershell
# 1. Configure & build standalone release binaries (hydration.exe ~328 KB, test_validator.exe ~228 KB)
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# 2. Run end-to-end functional, CLI flag, ancestry, and cache roundtrip test suite
.\test.ps1

# 3. Record or compare latency, throughput, and RSS memory benchmarks (bench_baseline.json)
.\benchmark.ps1 -SaveBaseline
.\benchmark.ps1 -CompareBaseline

# 4. Check binary file sizes
Get-Item .\hydration.exe, .\test_validator.exe | Select-Object Name, Length, @{N='SizeKB';E={[Math]::Round($_.Length/1KB, 2)}}

# 5. Inspect linked DLLs via objdump (verifies zero external MinGW DLLs)
objdump -p .\hydration.exe | Select-String "DLL Name:"
objdump -p .\test_validator.exe | Select-String "DLL Name:"

# 6. PowerShell PE Header & Import Table Auditors
.\dllload.ps1                                       # DLLs loaded by EXE
.\pehead.ps1                                        # PE headers and section layout
.\kernel_rw.ps1                                     # Grouped KERNEL32.dll & UCRT imports for hydration.exe
.\kernel_rw.ps1 -BinaryPath ".\test_validator.exe"  # Grouped KERNEL32.dll & UCRT imports for test_validator.exe
```
