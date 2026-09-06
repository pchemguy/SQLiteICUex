# icuex

`icuex` adds two ICU collations and two Unicode transformation functions to a
custom SQLite amalgamation. Every feature is registered automatically for each
new database connection by the surrounding amalgamation's aggregate built-in
extension initializer.

The extension is intentionally not loadable and has no supported public C API.
Its complete public interface is SQL.

## 1. SQL API

### Collations

| Name | ICU configuration | Semantics |
|---|---|---|
| `UTF_CI` | root locale, `UCOL_SECONDARY` | Case-insensitive; accents remain significant |
| `UTF_CI_AI` | root locale, `UCOL_PRIMARY` | Case-insensitive and accent-insensitive |

Examples:

```sql
SELECT 'АБВГДЙЬЁ' = 'абвгдйьё' COLLATE UTF_CI;    -- 1
SELECT 'е' = 'ё' COLLATE UTF_CI;                  -- 0
SELECT 'и' = 'й' COLLATE UTF_CI;                  -- 0

SELECT 'ЙЁ' = 'ие' COLLATE UTF_CI_AI;             -- 1
SELECT 'É' = 'e' COLLATE UTF_CI_AI;               -- 1
```

These are ICU root collations. `UTF_CI` is not defined as a comparison of
`str_casefold()` results, and neither collation promises Python or Qt lexical
ordering.

The collations may be used in schema declarations and indexes:

```sql
CREATE TABLE terms(
    term TEXT NOT NULL COLLATE UTF_CI
);

CREATE INDEX terms_by_name ON terms(term COLLATE UTF_CI);
```

### `str_casefold(text)`

`str_casefold()` performs full, locale-independent Unicode default case
folding using ICU `u_strFoldCase(..., U_FOLD_CASE_DEFAULT, ...)`. It does not
normalize its input or output.

```sql
SELECT str_casefold('Straße');  -- strasse
SELECT str_casefold('ЁЙ');      -- ёй
SELECT str_casefold('Σσς');     -- σσσ
```

### `str_normalize(text, kind)`

The mode is an exact ASCII token matched case-insensitively. Whitespace is not
trimmed and aliases are not accepted.

| Mode | ICU processing |
|---|---|
| `NFC` | `unorm2_getNFCInstance()` |
| `NFD` | `unorm2_getNFDInstance()` |
| `NFKC` | `unorm2_getNFKCInstance()` |
| `NFKD` | `unorm2_getNFKDInstance()` |
| `NFKC_CF` | `unorm2_getNFKCCasefoldInstance()` |
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
-- еие
```

`NFKD_CF_STRIP` filters by canonical combining class, not Unicode general
category. A mark in category `Mn`, `Mc`, or `Me` remains when its canonical
combining class is zero.

## 2. SQL contract

Both functions are registered with exactly:

```c
SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS
```

They do not use `SQLITE_SUBTYPE`, `SQLITE_RESULT_SUBTYPE`, or
`SQLITE_DIRECTONLY`.

Behavior common to both functions:

- `NULL` propagates. Either argument being `NULL` makes `str_normalize()` return
  `NULL`.
- Every non-`NULL` argument must have SQLite storage class `TEXT`.
- Integers, real values, and blobs are rejected instead of being coerced.
- Successful output has storage class `text`.
- Empty text is supported.
- Embedded U+0000 characters are preserved in transformed text.
- Explicit lengths are used throughout; embedded NUL does not terminate text.

Invalid normalization modes produce an error containing a safely escaped,
bounded preview and the complete list of supported modes. Invalid, empty,
whitespace-padded, non-ASCII, and embedded-NUL mode names are rejected.

Inputs are expected to contain well-formed Unicode. Deliberately malformed
SQLite text encodings are outside the portable contract.

## 3. Amalgamation integration

The build must define:

```text
SQLITE_ENABLE_ICU
SQLITE_ENABLE_ICUEX
```

The generated amalgamation must place sources in this order within one C
translation unit:

```text
ext/icu/icu.c
icuex.c
```

This order is required because `icuex.c` reuses these private `static`
definitions from the upstream SQLite ICU extension:

```c
icuCollationColl
icuCollationDel
icuFunctionError
```

`icuex.c` provides only this internal component initializer:

```c
int sqlite3IcuexInit(sqlite3 *db);
```

The surrounding build system must generate a separate aggregate-initializer
module that calls `sqlite3IcuexInit(db)` when `icuex` is enabled. The build
system, not this extension, is responsible for configuring SQLite with an
aggregate initializer, conventionally:

```text
SQLITE_EXTRA_AUTOEXT=sqlite3ExtraAutoExtInit
```

The name, implementation, generation, and ordering logic of
`sqlite3ExtraAutoExtInit()` are outside this package. `icuex.c` neither defines
nor references that function and does not call `sqlite3_auto_extension()`.

The aggregate initializer must propagate a non-`SQLITE_OK` result from
`sqlite3IcuexInit()`. The component initializer itself stops at the first
registration failure.

There is no `sqlite3_icuex_init()` loadable-extension entry point. Do not
compile `icuex.c` as a separate object or shared library.

## 4. Memory and ownership

- Each collation owns a distinct `UCollator`.
- After successful `sqlite3_create_collation_v2()`, SQLite owns the collator and
  closes it through the upstream `icuCollationDel()` callback.
- If registration fails, `icuex` closes the untransferred collator directly.
- ICU transformation output is sized with preflight calls.
- Intermediate UTF-16 and final UTF-8 result buffers use `sqlite3_malloc64()`.
  The final UTF-8 buffer is transferred to SQLite with `sqlite3_free()` as its
  destructor.
- Final results use UTF-8 so SQLite does not reinterpret and remove a leading
  U+FEFF as a UTF-16 byte-order mark.
- Buffer lengths distinguish bytes, UTF-16 code units, and Unicode code points.
- Supplementary characters are iterated as complete code points.

The standard ICU Normalizer2 objects are immutable singletons owned by ICU.
`icuex` resolves them as needed and never closes them. No mutable global cache
is used.

## 5. Unicode and index stability

Case-fold mappings, normalization data, and collation weights come from the ICU
version linked into SQLite. Results for characters added or changed in newer
Unicode versions may therefore change after an ICU upgrade.

Persistent indexes and constraints using `UTF_CI` or `UTF_CI_AI` depend on the
linked ICU collation data. After changing ICU versions or collation semantics,
rebuild affected indexes:

```sql
REINDEX;
```

## 6. Testing

The test suite uses only Python's standard `sqlite3` module and SQL. That Python
module must already be linked against the custom SQLite library containing ICU,
`icuex`, and the external aggregate initializer.

Install pytest in the active environment and run:

```text
python -m pytest
```

The tests deliberately do not:

- Load an extension.
- Execute `icu_load_collation()` as setup.
- Register functions or collations from Python.
- Use `ctypes`, CFFI, or compiled C test fixtures.
- Call private C helpers.

`PRAGMA collation_list` and `PRAGMA function_list` verify automatic
registration, arities, preferred encoding, and flags. Behavioral modules cover
collations, normalization modes, strict SQL types, embedded NUL, long input,
indexes, generated columns, and reopening file-backed databases.

## 7. Files

```text
icuex.c                         implementation
README.md                       usage and integration guide
pyproject.toml                  pytest configuration
tests/conftest.py               connection fixtures without setup SQL
tests/test_introspection.py     automatic-registration checks
tests/test_collations.py        collation behavior
tests/test_casefold.py          Unicode default case folding
tests/test_normalization_*.py   standard and composite normalization
tests/test_sql_contract.py      NULL, type, mode, and length contracts
tests/test_schema_integration.py schema, index, and reopen behavior
```
