---
url: https://chatgpt.com/c/6a9fba3e-fb3c-83eb-9509-d15d0bed2caf
---

# ICUex

`icuex` adds two ICU-backed Unicode collations and two Unicode transformation functions to SQLite. The same source supports two build modes:

- With `SQLITE_CORE`, it is a built-in component registered for each new connection through SQLite's `SQLITE_EXTRA_AUTOEXT` hook.
- Without `SQLITE_CORE`, it is a conventional loadable extension exporting `sqlite3_icuex_init()`.

[`src/icuex.c`](src/icuex.c) complements SQLite's `ext/icu/icu.c`: it provides predefined collations, full case folding, normalization, and search keys through SQL, using only public SQLite and ICU APIs. Enable SQLite's own ICU extension separately if you also want its SQL functions.

> [!IMPORTANT]
> 
> **AI-Assisted Development Disclosure**
> 
> This project has been developed with extensive generative-AI assistance. Assistance covered project exploration, design discussion, specification development, implementation, testing, technical review, and documentation. See [AI_DISCLOSURE.md](AI_DISCLOSURE.md) for further details. Responsibility for the published software remains with the maintainer.

[Build with the Windows harness](#windows-msvc-build-from-this-checkout) · [Build against a standard amalgamation](#built-in-mode) · [Build a loadable extension](#loadable-extension-mode) · [Run the tests](#6-testing)

## 1. SQL API

### Collations

| Name        | ICU configuration                          | Semantics                                                                 |
| ----------- | ------------------------------------------ | ------------------------------------------------------------------------- |
| `UTF_CI`    | root locale, `UCOL_SECONDARY`              | Case-insensitive; accents remain significant                              |
| `UTF_CI_AI` | lexical comparison of `NFKD_CF_STRIP` keys | Locale-independent case- and accent-insensitive normalized-key comparison |

Examples:

```sql
SELECT 'АБВГДЙЬЁ' = 'абвгдйьё' COLLATE UTF_CI;    -- 1
SELECT 'е' = 'ё' COLLATE UTF_CI;                  -- 0
SELECT 'и' = 'й' COLLATE UTF_CI;                  -- 0

SELECT 'ЙЁ' = 'ие' COLLATE UTF_CI_AI;             -- 1
SELECT 'É' = 'e' COLLATE UTF_CI_AI;               -- 1
SELECT 'Straße' = 'STRASSE' COLLATE UTF_CI_AI;    -- 1
SELECT 'Ａ①' = 'a1' COLLATE UTF_CI_AI;             -- 1
```

`UTF_CI` is an ICU root linguistic collation and is not defined as a comparison of `str_casefold()` output. `UTF_CI_AI` has deliberately different semantics: each operand is transformed by NFKC_Casefold, then NFKD, then removal of every code point whose canonical combining class is nonzero. The resulting keys are compared lexicographically by Unicode code point. Thus `UTF_CI_AI` equality is the same as equality of the corresponding `NFKD_CF_STRIP` keys; no original text tiebreaker is applied.

`UTF_CI_AI` is not equivalent to ICU root collation at `UCOL_PRIMARY`. For example, with ICU 78.3, root primary treats `Ё/е` as equal but distinguishes `Й/и`; `UTF_CI_AI` equates both pairs. The normalized keys also apply compatibility mappings and remove default ignorables.

The collations may be used in schema declarations and indexes:

```sql
CREATE TABLE terms(
    term TEXT NOT NULL COLLATE UTF_CI
);

CREATE INDEX terms_by_name ON terms(term COLLATE UTF_CI);
```

### `str_casefold(text)`

`str_casefold()` performs full, locale-independent [Unicode Default Case Folding](https://www.unicode.org/Public/UCD/latest/ucd/CaseFolding.txt) using ICU `u_strFoldCase(..., U_FOLD_CASE_DEFAULT, ...)`. It does not perform Unicode normalization before or after folding.

```sql
SELECT str_casefold('Straße');  -- strasse
SELECT str_casefold('ЁЙ');      -- ёй
SELECT str_casefold('Σσς');     -- σσσ
```

This is the same standard operation exposed by Python's `str.casefold()`. It is intended for caseless binary comparison, not for producing linguistically appropriate lowercase text. Full folding handles mappings such as `ß → ss` and unifies Greek sigma forms, while preserving distinctions unrelated to case, including Cyrillic `е/ё` and `и/й`. Its exact behavior follows the Unicode version bundled with ICU. Because case folding does not provide normalization equivalence, callers should normalize separately or use `NFKC_CF` when compatibility-normalized caseless keys are required.

Default case folding is deliberately locale-independent and is not a substitute for locale-sensitive lowercasing or language-specific caseless matching. Locale- and context-sensitive lowercase mappings are documented separately in Unicode's [SpecialCasing.txt](https://www.unicode.org/Public/UCD/latest/ucd/SpecialCasing.txt). Locale-sensitive casing can map an ASCII capital letter to output containing non-ASCII code points. A notable example occurs in Turkish and Azerbaijani: bare ASCII `I` (U+0049) lowercases to dotless `ı` (U+0131), while `İ` lowercases to `i`. Default case folding instead maps `I` to ASCII `i` and `İ` to `i` followed by U+0307 COMBINING DOT ABOVE. Context-sensitive lowercase mappings also exist. For example, Lithuanian lowercasing can add U+0307 after ASCII `I` or `J` when certain accents follow.

Applications requiring language-specific semantics should use locale-aware ICU casing and locale-tailored collations, such as those exposed separately by `ext/icu/icu.c`. `NFKD_CF_STRIP` and `UTF_CI_AI` deliberately provide broader language-neutral matching and should not be used where the linguistic or orthographic distinctions removed by case folding, compatibility decomposition, or combining-mark stripping must be preserved.

#### Relationship to ICU `lower()`

When SQLite is also built with `ext/icu/icu.c`, that extension overloads `lower()` with ICU `u_strToLower()`. Its one-argument form uses ICU's default locale, and its two-argument form accepts an explicit locale. This substantially overlaps `str_casefold()` for ASCII and ordinary uppercase letters, but the two functions have different Unicode contracts:

- `lower()` performs a locale-sensitive, context-sensitive lowercase mapping, suitable when the desired result is lowercase text.
- `str_casefold()` performs locale-independent full default case folding with `u_strFoldCase(..., U_FOLD_CASE_DEFAULT, ...)`, suitable for caseless matching and key construction.
- Neither operation performs Unicode normalization. `NFKC_CF` is the API to use when compatibility normalization, default-ignorable removal, and case folding are required together.

The distinction is observable in stable mappings:

| Input                     | `lower(input, 'root')` from `icu.c` | `str_casefold(input)`    |
| ------------------------- | ----------------------------------- | ------------------------ |
| `Straße`                  | `straße`                            | `strasse`                |
| `ß`                       | `ß`                                 | `ss`                     |
| `ẞ`                       | `ß`                                 | `ss`                     |
| Greek `ΟΣ`                | `ος` with final sigma U+03C2        | `οσ` with sigma U+03C3   |
| Greek final sigma `ς`     | `ς`                                 | `σ`                      |
| long s `ſ`                | `ſ`                                 | `s`                      |
| ligatures `ﬀ ﬁ ﬂ ﬃ ﬄ ﬅ ﬆ` | unchanged                           | `ff fi fl ffi ffl st st` |
| Cyrillic `ИЙЕЁЬЪ`         | `ийеёьъ`                            | `ийеёьъ`                 |

Thus ICU `lower()` is not a substitute for `str_casefold()` when the stored value is intended to represent Unicode caseless equivalence. Conversely, `str_casefold()` should not be presented as locale-aware lowercasing. The test suite treats `str_casefold()` as an `icuex` requirement. Tests of the overloaded two-argument `lower()` are compatibility probes for `ext/icu/icu.c`: a missing overload or an unexpected ICU-lowercase result produces an explicit pytest warning, not an `icuex` test failure.

### `str_normalize(text, kind)`

The mode is an exact ASCII token matched case-insensitively. Whitespace is not
trimmed and aliases are not accepted.

| Mode            | ICU processing                                                       |
| --------------- | -------------------------------------------------------------------- |
| `NFC`           | `unorm2_getNFCInstance()`                                            |
| `NFD`           | `unorm2_getNFDInstance()`                                            |
| `NFKC`          | `unorm2_getNFKCInstance()`                                           |
| `NFKD`          | `unorm2_getNFKDInstance()`                                           |
| `NFKC_CF`       | `unorm2_getNFKCCasefoldInstance()`                                   |
| `NFKD_CF_STRIP` | NFKC_Casefold, then NFKD, then retain only code points with CCC zero |

Examples:

```sql
SELECT str_normalize('e' || char(0x301), 'NFC');
-- é

SELECT str_normalize('É', 'NFD');
-- e followed by U+0301 COMBINING ACUTE ACCENT

SELECT str_normalize('①Ａ', 'NFKC');
-- 1A

SELECT str_normalize('Straße', 'NFKC_CF');
-- strasse

SELECT str_normalize('ЁЙÉ', 'NFKD_CF_STRIP');
-- еиe
```

The final character in that result is Latin `e`; normalization and CCC filtering do not transliterate Latin characters into Cyrillic.

The final `NFKD_CF_STRIP` step filters by canonical combining class (CCC), not Unicode general category. A surviving CCC-zero mark such as U+20DD COMBINING ENCLOSING CIRCLE is retained; a default-ignorable mark such as U+FE0F VARIATION SELECTOR-16 has already been removed by NFKC_Casefold.

## 2. SQL contract

Both functions are registered with exactly:

```c
SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS
```

They do not use `SQLITE_SUBTYPE`, `SQLITE_RESULT_SUBTYPE`, or `SQLITE_DIRECTONLY`.

Behavior common to both functions:

- `NULL` propagates. Either argument being `NULL` makes `str_normalize()` return `NULL`.
- Every non-`NULL` argument must have SQLite storage class `TEXT`.
- Integers, real values, and blobs are rejected instead of being coerced.
- Successful output has storage class `text`.
- Empty text is supported.
- Embedded U+0000 characters are preserved in transformed text.
- Explicit lengths are used throughout; embedded NUL does not terminate text.

Invalid normalization modes produce an error containing a safely escaped, bounded preview and the complete list of supported modes. Invalid, empty, whitespace-padded, non-ASCII, and embedded-NUL mode names are rejected.

Inputs are expected to contain well-formed Unicode. Deliberately malformed SQLite text encodings are outside the portable contract.

## 3. Build and integration

### Windows: MSVC build from this checkout

Use this recipe for a SQLite shell, DLL, and static library with ICUex **built in**. For a standalone `icuex.dll` to load into an existing SQLite, use [loadable-extension mode](#loadable-extension-mode) instead.

**1. Activate a matching Conda environment.** Install MSVC C++ Build Tools and the Windows SDK first. Automatic discovery targets x64; x86 requires an initialized x86 tools prompt and a separately prepared x86 Conda prefix. If you already have a matching environment with ICU (headers, import libraries, and versioned runtime DLLs) and pytest, activate it and go to step 2.

To create the bundled x64 environment, first read the [bootstrap cache notes](https://github.com/pchemguy/Conda-Bootstrap/blob/main/windows/Anaconda.md#cache-and-filesystem-changes): the scripts can replace the user's pip/uv caches with junctions and remove `%APPDATA%\mamba`. From the repository root in a clean Windows CMD (`cmd /E:ON /V:ON`, no Python or Conda on `PATH`):

```bat
set "PKGS=icu"
call pyenv\Anaconda.bat
call pyenv\conda_far.bat /batch
```

The full first run creates `pyenv\Anaconda` using the two local YAMLs; `PKGS` explicitly requests ICU at creation. `/init` skips the project test packages, and rerunning the bootstrap does not update an existing prefix. Install missing packages in an existing prefix after activation (e.g., `conda install -c conda-forge icu pytest`).

**2. Select the source and build.** The [ICUex hook](sqlite_ext_icuex.bat) uses `out\cache\SQLiteICUex\src\icuex.c`. On a cache miss it downloads `main`; it does **not** pick up edits to this checkout. To build your checkout, start from the repository root with no previous `out` and seed the cache:

```bat
mkdir "out\cache\SQLiteICUex\src"
copy /B "src\icuex.c" "out\cache\SQLiteICUex\src\icuex.c"
call sqlite_msbuild_bare_icu_ext_extra.bat
```

The [preset](sqlite_msbuild_bare_icu_ext_extra.bat) enables Conda ICU, ZLIB, selected SQLite `ext/misc` modules, and this repository's ICUex hook. It registers ICUex through `sqlite3ExtraAutoExtInit`; do not add the single-extension compiler flag from the manual recipe below.

SQLite is downloaded as a source snapshot, not a pinned release. Downloads and staged dependencies are reused: use separate build bases for different architectures, source revisions, or ICU installations. See the [upstream MSVC guide](https://github.com/pchemguy/SQLite-ICU-MinGW/blob/master/Current/MSVC/README.md) for cache recovery and build options. Its other project hooks and harness tests are not shipped here.

**3. Check the packaged shell.** From the repository root, start `out\bin\sqlite3.exe -init NUL :memory:` and run this query without `.load` or setup SQL:

```sql
WITH letters(e, yo) AS (VALUES (char(0x435), char(0x451)))
SELECT e = yo COLLATE UTF_CI, e = yo COLLATE UTF_CI_AI,
       str_casefold('Stra' || char(0xDF) || 'e'),
       str_normalize(char(0xC9), 'NFKD_CF_STRIP')
FROM letters;
-- Expected columns: 0, 1, strasse, e
```

Use `SELECT sqlite_source_id();` to identify the SQLite snapshot, and `.quit` to return to CMD.

The runtime files (`sqlite3.exe`, `sqlite3.dll`, versioned ICU DLLs, and `zlib1.dll`) go in `out\bin`. Headers go in `out\include`; DLL consumers link `out\lib\import\sqlite3.lib`, while static consumers link `out\lib\static\libsqlite3.lib`. Collection is best effort and clears files from `out\bin`; check the artifacts you need and keep unrelated files elsewhere. This smoke test checks the shell, **not** the SQLite library loaded by Python; see [Testing](#6-testing).

### Built-in mode

For a manual build without the Windows harness, use a SQLite amalgamation that supports `SQLITE_EXTRA_AUTOEXT`. In the following two built-in recipes, run the commands in the amalgamation directory with `shell.c`, `sqlite3.c`, `sqlite3.h`, and a copy of this repository's `src/icuex.c` alongside them. For the single-translation-unit recipe, define:

```text
SQLITE_EXTRA_AUTOEXT=sqlite3IcuexInit
```

The source can be placed after the amalgamation text directly or included at the end of `sqlite3.c`:

```c
#include "icuex.c"
```

The standard amalgamation already builds with `SQLITE_CORE`. SQLite declares the named function as `int (sqlite3*)`, adds it to its built-in extension list, and invokes it for every new connection. No additional initializer source, generated registration module, or `sqlite3_auto_extension()` call is required.

Built-in mode provides this component initializer:

```c
int sqlite3IcuexInit(sqlite3 *db);
```

For example, on a Unix-like system with pkg-config:

```sh
cc -O2 \
  -DSQLITE_EXTRA_AUTOEXT=sqlite3IcuexInit \
  shell.c sqlite3.c -o sqlite3 \
  $(pkg-config --cflags --libs icu-i18n icu-uc) \
  -ldl -lpthread -lm
```

#### Alternative: separate objects and a static library

`sqlite3.c` and `icuex.c` can instead remain separate. Compile the pristine amalgamation with `SQLITE_EXTRA_AUTOEXT` so its built-in-extension list refers to `sqlite3IcuexInit()`. Compile `icuex.c` with `SQLITE_CORE` so it defines that built-in initializer rather than the loadable-extension entry point. Archive the two objects together and link the shell against the resulting static library:

```sh
cc -O2 \
  -DSQLITE_EXTRA_AUTOEXT=sqlite3IcuexInit \
  -c sqlite3.c -o sqlite3.o

cc -O2 -std=c99 \
  -DSQLITE_CORE \
  $(pkg-config --cflags icu-i18n icu-uc) \
  -c icuex.c -o icuex.o

ar rcs libsqlite3-icuex.a sqlite3.o icuex.o

cc -O2 shell.c libsqlite3-icuex.a \
  -o sqlite3-icuex \
  $(pkg-config --libs icu-i18n icu-uc) \
  -ldl -lpthread -lm
```

The static library contains both `sqlite3.o` and `icuex.o`. No SQLite source concatenation or final `#include "icuex.c"` is used. The SQLite object has an unresolved reference to `sqlite3IcuexInit`, which is resolved by the `icuex` object when the archive is linked.

The compiler must see the ICU headers, and the final target must link the ICU internationalization, common, and data libraries. Enabling SQLite's separate ICU extension is optional and imposes no source-order requirement relative to `icuex.c`.

### Loadable-extension mode

From the repository root, compile `src/icuex.c` as a shared library **without** defining `SQLITE_CORE`. You need SQLite's `sqlite3.h` and `sqlite3ext.h` and the ICU development files. On Linux with pkg-config:

```sh
cc -O2 -fPIC -shared $(pkg-config --cflags sqlite3) src/icuex.c -o icuex.so \
  $(pkg-config --cflags --libs icu-i18n icu-uc)
```

For MSVC with ICU supplied by Conda:

```bat
cl /nologo /O2 /LD ^
  /I"C:\path\to\sqlite-amalgamation" ^
  /I"%CONDA_PREFIX%\Library\include" ^
  src\icuex.c ^
  /link /LIBPATH:"%CONDA_PREFIX%\Library\lib" ^
  icuin.lib icuuc.lib icudt.lib /OUT:icuex.dll
```

Point the first include directory at your SQLite headers (e.g., `out\include` after the MSVC build). The loadable module and its ICU runtime libraries must match the host architecture and the libraries must be discoverable at runtime. In the SQLite CLI:

```text
.load ./icuex
```

The basename `icuex` maps to the exported `sqlite3_icuex_init()` entry point. The loadable build uses `sqlite3ext.h` and SQLite's supplied API table; it does not need to link directly against the SQLite library.

## 4. Memory and ownership

- `UTF_CI` owns one `UCollator`; `UTF_CI_AI` owns no persistent ICU object.
- After successful registration of `UTF_CI`, SQLite owns the collator and closes it through the private `icuexIcuCollationDelete()` callback.
- If registration fails, `icuex` closes the untransferred collator directly.
- `UTF_CI_AI` constructs two temporary UTF-16 keys for each comparison and releases both on every path. It uses explicit lengths, so embedded U+0000 is compared as ordinary text rather than as a terminator.
- SQLite collation callbacks cannot return SQL errors. If key allocation or ICU processing fails, `UTF_CI_AI` logs the failure, interrupts the active database operation, cleans up, and returns a provisional explicit-length comparison only to satisfy the callback ABI.
- ICU transformation output is sized with preflight calls.
- Intermediate UTF-16 and final UTF-8 result buffers use `sqlite3_malloc64()`. The final UTF-8 buffer is transferred to SQLite with `sqlite3_free()` as its destructor.
- Final results use UTF-8 so SQLite does not reinterpret and remove a leading U+FEFF as a UTF-16 byte-order mark.
- Buffer lengths distinguish bytes, UTF-16 code units, and Unicode code points.
- Supplementary characters are iterated as complete code points.

The standard ICU Normalizer2 objects are immutable singletons owned by ICU. `icuex` resolves them as needed and never closes them. No mutable global cache is used.

## 5. Unicode and index stability

Case-fold mappings, normalization data, and collation weights come from the ICU used by ICUex, whether built into SQLite or loaded separately. Results may change after an ICU upgrade.

Persistent indexes and constraints using `UTF_CI` depend on ICU collation data; those using `UTF_CI_AI` depend on ICU case-folding, compatibility, normalization, and combining-class data. After changing ICU versions or collation semantics, rebuild affected indexes:

```sql
REINDEX;
```

`REINDEX` does not recompute values in ordinary or STORED generated columns. Recompute persisted keys separately, and check for previously distinct keys that may now collide under a `UNIQUE` constraint.

## 6. Testing

Run `python -m pytest` from the repository root with pytest installed in that interpreter. Both modes run the same SQL tests in [`icuex/tests/`](icuex/tests); neither requires installing a Python package from this repository.

**Built-in.** Leave `ICUEX_EXTENSION` unset. Python's `sqlite3` module must already use the SQLite build containing ICUex. Building the Windows preset does not install `out\bin\sqlite3.dll` into Conda; running the packaged CLI or comparing SQLite version numbers alone does not establish which DLL Python loaded. Check in the interpreter that will run pytest:

```bat
set "ICUEX_EXTENSION="
python -c "import sqlite3; db=sqlite3.connect(':memory:'); print(db.execute('SELECT sqlite_source_id(), str_casefold(?)', ('ABC',)).fetchone()); db.close()"
python -m pytest
```

Expect the shell's SQLite source ID and `abc` without loading an extension. The source ID identifies the SQLite snapshot, not the ICUex revision or DLL path. On Windows the [upstream DLL-location helper](https://github.com/pchemguy/SQLite-ICU-MinGW/blob/master/Current/MSVC/tool/sqlite3_dll_path.py) reports the actual `sqlite3.dll` path when one is loaded.

If the probe reports `no such function`, that Python runtime does not expose built-in ICUex. Use a Python runtime configured for the intended SQLite, or test a standalone module as described below. On POSIX, use `unset ICUEX_EXTENSION` to select built-in mode.

**Loadable.** Use a Python SQLite build that supports extension loading, preferably without ICUex built in. Set `ICUEX_EXTENSION` to the full path of the **separately built module**, not the harness's `sqlite3.dll`. The fixture loads that module into each new connection and then disables further loading. From the directory where the loadable recipe produced its output:

```bat
set "ICUEX_EXTENSION=%CD%\icuex.dll"
python -m pytest
```

```sh
ICUEX_EXTENSION="$PWD/icuex.so" python -m pytest
```

The tests deliberately do not:

- Execute `icu_load_collation()` as setup.
- Register functions or collations from Python.
- Use `ctypes`, CFFI, or compiled C test fixtures.
- Call private C helpers.

`PRAGMA collation_list` and `PRAGMA function_list` verify registration, arities, preferred encoding, and flags. Behavioral modules cover collations, normalization modes, strict SQL types, embedded NUL, long input, indexes, generated columns, and reopening file-backed databases.

## 7. Files

- [`src/icuex.c`](src/icuex.c) — both extension entry points and the SQL implementation.
- [`pyenv/`](pyenv) — Windows environment bootstrap, launcher, and project-specific package lists.
- [`sqlite_msbuild.bat`](sqlite_msbuild.bat), [`sqlite_ext_icuex.bat`](sqlite_ext_icuex.bat), [`tool/`](tool) — Windows build, ICUex hook, and Tcl helpers.
- [`pyproject.toml`](pyproject.toml) and [`icuex/tests/`](icuex/tests) — pytest configuration, connection fixtures, SQL acceptance tests, and optional `icu.c` compatibility probes.
- [`docs/develop/SPEC.md`](docs/develop/SPEC.md) — extension contract.
