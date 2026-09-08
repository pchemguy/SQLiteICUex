---
url: https://chatgpt.com/c/6a9fba3e-fb3c-83eb-9509-d15d0bed2caf
---

# ICUex

`icuex` adds two ICU-backed Unicode collations and two Unicode transformation functions to SQLite. The same source supports two build modes:

- With `SQLITE_CORE`, it is a built-in component registered for each new connection through SQLite's `SQLITE_EXTRA_AUTOEXT` hook.
- Without `SQLITE_CORE`, it is a conventional loadable extension exporting `sqlite3_icuex_init()`.

`icuex.c` uses only public SQLite and ICU APIs. It does not copy or call private implementation from SQLite's `ext/icu/icu.c`. That extension may be enabled separately when its broader SQL surface is wanted, but it is not a build or
source-order prerequisite. `icuex` instead implements focused facilities not readily exposed by `icu.c`: predefined collations, full case folding, Unicode normalization, and normalized search-key generation. The supported data interface is SQL.

> [!IMPORTANT]
> 
> **AI-Assisted Development Disclosure**
> 
> This project has been developed with extensive generative-AI assistance. Assistance covered design discussion, implementation, testing, technical review, and documentation. See [AI_DISCLOSURE.md](AI_DISCLOSURE.md) for further details. Responsibility for the published software remains with the maintainer.

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

`UTF_CI_AI` is not equivalent to ICU root collation at `UCOL_PRIMARY`. Root primary strength retains some distinctions, including `Й/и` and `Ё/е`, and therefore does not implement universal accent removal. The normalized-key collation also applies compatibility mappings and removes default ignorables, so its concise name does not imply that case and accents are its only equivalences.

The collations may be used in schema declarations and indexes:

```sql
CREATE TABLE terms(
    term TEXT NOT NULL COLLATE UTF_CI
);

CREATE INDEX terms_by_name ON terms(term COLLATE UTF_CI);
```

### `str_casefold(text)`

`str_casefold()` performs full, locale-independent Unicode default case folding using ICU `u_strFoldCase(..., U_FOLD_CASE_DEFAULT, ...)`. It does not normalize its input or output.

```sql
SELECT str_casefold('Straße');  -- strasse
SELECT str_casefold('ЁЙ');      -- ёй
SELECT str_casefold('Σσς');     -- σσσ
```

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

`NFKD_CF_STRIP` filters by canonical combining class, not Unicode general category. A mark in category `Mn`, `Mc`, or `Me` remains when its canonical combining class is zero.

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

### Built-in mode

Download SQLite's standard amalgamation, place `icuex.c` after `sqlite3.c` in the same translation unit, and define:

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

Compile the source as a shared library without defining `SQLITE_CORE`. For example, on a Unix-like system with pkg-config:

```sh
cc -O2 -fPIC -shared icuex.c -o icuex.so \
  $(pkg-config --cflags --libs icu-i18n icu-uc)
```

For MSVC with ICU supplied by Conda:

```bat
cl /nologo /O2 /LD ^
  /I"C:\path\to\sqlite-amalgamation" ^
  /I"%CONDA_PREFIX%\Library\include" ^
  icuex.c ^
  /link /LIBPATH:"%CONDA_PREFIX%\Library\lib" ^
  icuin.lib icuuc.lib icudt.lib /OUT:icuex.dll
```

Matching ICU DLLs must be discoverable at runtime. Load the resulting library through SQLite's normal extension interface:

```sql
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

Case-fold mappings, normalization data, and collation weights come from the ICU version linked into SQLite. Results for characters added or changed in newer Unicode versions may therefore change after an ICU upgrade.

Persistent indexes and constraints using `UTF_CI` depend on ICU collation data; those using `UTF_CI_AI` depend on ICU case-folding, compatibility, normalization, and combining-class data. After changing ICU versions or collation semantics, rebuild affected indexes:

```sql
REINDEX;
```

## 6. Testing

The test suite uses Python's standard `sqlite3` module and exercises behavior through SQL. It supports both integration modes.

For a built-in build, use the Python module linked to that custom SQLite and run without setup:

Install pytest in the active environment and run:

```text
python -m pytest
```

For a loadable build, specify the shared library. On Windows CMD:

```bat
set "ICUEX_EXTENSION=B:\path\to\icuex.dll"
python -m pytest
```

On a Unix-like system:

```sh
ICUEX_EXTENSION=/path/to/icuex.so python -m pytest
```

The fixture loads the configured library into every fresh connection and then disables further extension loading on that connection.

The tests deliberately do not:

- Execute `icu_load_collation()` as setup.
- Register functions or collations from Python.
- Use `ctypes`, CFFI, or compiled C test fixtures.
- Call private C helpers.

`PRAGMA collation_list` and `PRAGMA function_list` verify registration, arities, preferred encoding, and flags. Behavioral modules cover collations, normalization modes, strict SQL types, embedded NUL, long input, indexes, generated columns, and reopening file-backed databases.

## 7. Files

```text
icuex.c                         implementation
README.md                       usage and integration guide
pyproject.toml                  pytest configuration
tests/conftest.py               built-in/loadable connection fixtures
tests/test_introspection.py     per-connection registration checks
tests/test_collations.py        collation behavior
tests/test_casefold.py          Unicode default case folding
tests/test_icu_lower.py         warning-based ext/icu lower() compatibility probes
tests/test_normalization_*.py   standard and composite normalization
tests/test_sql_contract.py      NULL, type, mode, and length contracts
tests/test_schema_integration.py schema, index, and reopen behavior
```
