---
url: https://chatgpt.com/c/6a9cf257-d748-83eb-93ad-1f1a3999eb9a
---

```
I’ll turn this into an implementation-ready specification and tighten the underspecified contracts—especially naming, NULL/type behavior, error behavior, initialization, memory handling, and SQL-only verification. I’ll also verify the current SQLite/ICU integration points before fixing them in the spec.

I would settle the second collation name as `UTF_CI_AI`: it clearly means case-insensitive and accent-insensitive. `UTF_CIEX` and `UTF_CIACC` are ambiguous.

I have also made one input-policy choice explicit: both functions accept only `TEXT` or `NULL`; they do not silently coerce integers, reals, or blobs.
```

## `icuex` SQLite extension specification

### 1. Purpose and scope

Implement `icuex`, a statically integrated SQLite extension providing:

* Two automatically registered ICU collations.
* Unicode case folding.
* Unicode normalization and normalized search-key generation.

`icuex` is not a loadable extension and shall expose no supported public C API. Its complete user-facing interface is SQL.

The extension shall be compiled only as part of a custom SQLite amalgamation in which:

1. SQLite’s `ext/icu/icu.c` is enabled.
2. The contents of `icu.c` occur in the same C translation unit before `icuex.c`.
3. `icuex.c` may therefore reuse `static` implementation details from `icu.c`, particularly:

   ```c
   icuCollationColl
   icuCollationDel
   icuFunctionError
   ```

`icu.c` itself should remain unchanged.

All `icuex` helper functions and data shall be `static`. The only non-static symbol may be the internal initializer required by SQLite’s built-in-extension mechanism; it is not a documented or supported API and shall have no public header or loadable-extension entry point.

### 2. Build and automatic initialization

Compile SQLite with at least:

```text
SQLITE_ENABLE_ICU
SQLITE_ENABLE_ICUEX
SQLITE_EXTRA_AUTOEXT=sqlite3ExtraAutoExtInit
```

The initializer must use the built-in-extension signature:

```c
int sqlite3IcuexInit(sqlite3 *db);
```

It must not use the three-argument loadable-extension signature.

This initializer will be called from the automatically generated orchestrator `sqlite3ExtraAutoExtInit`. 

Use the following initialization design in `icuex.c`:

```
static int icuexRegister(sqlite3 *db) {
    int rc;
    
    int flags = SQLITE_UTF8 | SQLITE_INNOCUOUS | SQLITE_DETERMINISTIC
              | SQLITE_RESULT_SUBTYPE;
    rc = sqlite3_create_function(db, "regexp_matches", 2, flags,
                                 0, remSqlFuncCase, 0, 0);
    if ( rc==SQLITE_OK ) {
        rc = sqlite3_create_function(db, "regexpi_matches", 2, flags,
                                     0, remSqlFuncNocase, 0, 0);
    }
    
    return rc;
}


int sqlite3IcuexInit(sqlite3 *db) {
    return icuexRegister(db);
}

```

Above, `sqlite3IcuexInit` is a stub function with a single command as shown. While it is unnecessary in this context, this source template should be followed (compiler may optimize it away). `icuexRegister` is actual function responsible for SQLite registration process (the shown demo contents must be replaced with appropriate calls for both function and collations registration).

Current SQLite declares `SQLITE_EXTRA_AUTOEXT` as `int function(sqlite3*)` and places it in `sqlite3BuiltinExtensions[]`, whose members are initialized for every new connection. [SQLite source](https://github.com/sqlite/sqlite/blob/master/src/main.c)

Consequently, every connection opened through the custom SQLite library must immediately support:

```sql
COLLATE UTF_CI
COLLATE UTF_CI_AI
str_casefold(...)
str_normalize(...)
```

No application-side initialization, `load_extension()`, `icu_load_collation()`, or Python registration code is permitted.

Failure to initialize either collation or either SQL function must cause connection initialization to fail with a non-`SQLITE_OK` result. All partially allocated ICU objects must be released correctly.

### 3. SQL collations

Register these collations independently for every SQLite connection:

| SQL name    | ICU locale |         Strength | Meaning                                      |
| ----------- | ---------: | ---------------: | -------------------------------------------- |
| `UTF_CI`    |     `root` | `UCOL_SECONDARY` | Case-insensitive; accents remain significant |
| `UTF_CI_AI` |     `root` |   `UCOL_PRIMARY` | Case-insensitive and accent-insensitive      |

Their semantics must be equivalent to:

```sql
SELECT icu_load_collation('root', 'UTF_CI',    'SECONDARY');
SELECT icu_load_collation('root', 'UTF_CI_AI', 'PRIMARY');
```

but registration must be performed directly during initialization using:

```c
ucol_open("root", ...)
ucol_setStrength(...)
sqlite3_create_collation_v2(
    db,
    name,
    SQLITE_UTF16,
    collator,
    icuCollationColl,
    icuCollationDel
)
```

Each collation owns a separate `UCollator`. On successful registration, SQLite owns it through `icuCollationDel`. If registration fails, `icuex` must close it directly.

Required examples:

```sql
SELECT 'АБВГДЙЬЁ' = 'абвгдйьё' COLLATE UTF_CI;
-- 1

SELECT 'е' = 'ё' COLLATE UTF_CI;
-- 0

SELECT 'и' = 'й' COLLATE UTF_CI;
-- 0

SELECT 'ЙЁ' = 'ие' COLLATE UTF_CI_AI;
-- 1

SELECT 'É' = 'e' COLLATE UTF_CI_AI;
-- 1
```

These are ICU root collations, not direct comparisons of `str_casefold()` results. No claim of exact Python/Qt case-folded lexical ordering shall be made.

### 4. `str_casefold(text)`

Signature:

```sql
str_casefold(text) → TEXT
```

Implement full, locale-independent Unicode default case folding using:

```c
u_strFoldCase(..., U_FOLD_CASE_DEFAULT, ...)
```

Do not apply normalization before or after folding.

Required semantics include:

```text
Straße → strasse
Ё      → ё
Й      → й
Σ/σ/ς  → σ
```

The result depends on the Unicode data version supplied by the linked ICU build.

### 5. `str_normalize(text, kind)`

Signature:

```sql
str_normalize(text, kind) → TEXT
```

`kind` is an ASCII, case-insensitive mode name. Do not trim whitespace or accept aliases.

Supported modes:

| Mode            | Processing                                                   |
| --------------- | ------------------------------------------------------------ |
| `NFC`           | `unorm2_getNFCInstance()`                                    |
| `NFD`           | `unorm2_getNFDInstance()`                                    |
| `NFKC`          | `unorm2_getNFKCInstance()`                                   |
| `NFKD`          | `unorm2_getNFKDInstance()`                                   |
| `NFKC_CF`       | `unorm2_getNFKCCasefoldInstance()`                           |
| `NFKD_CF_STRIP` | `NFKC_CF` → `NFKD` → remove code points whose CCC is nonzero |

ICU defines its NFKC case-fold normalizer as the Unicode NFKC_Casefold mappings followed by NFC. The returned normalizer objects are immutable singletons and must not be deleted. [ICU Normalizer2 API](https://unicode-org.github.io/icu-docs/apidoc/dev/icu4c/unorm2_8h.html)

#### `NFKD_CF_STRIP`

The operation order is mandatory:

```text
1. NFKC_Casefold
2. NFKD
3. Retain only code points for which u_getCombiningClass(c) == 0
```

Conceptually:

```python
def nfkd_cf_strip(text):
    text = NFKC_Casefold(text)
    text = normalize("NFKD", text)
    return "".join(c for c in text if combining(c) == 0)
```

Iteration must be by Unicode code point using ICU UTF-16 macros such as `U16_NEXT`, not by individual UTF-16 code unit.

This deliberately removes characters according to canonical combining class, not Unicode general category. Marks having CCC zero must remain. ICU defines `u_getCombiningClass()` as returning the code point’s canonical combining class. [ICU character API](https://unicode-org.github.io/icu-docs/apidoc/dev/icu4c/uchar_8h.html)

Required examples:

```text
NFKC_CF:
    Straße → strasse
    Ё      → ё
    Й      → й
    É      → é
    default-ignorable characters → removed where specified by NFKC_Casefold

NFKD_CF_STRIP:
    Straße → strasse
    Ё      → е
    Й      → и
    É      → e
```

### 6. SQL behavior

Register both functions with exactly:

```c
SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS
```

Required argument behavior:

* A `NULL` argument produces SQL `NULL`.
* Non-`NULL` arguments must have SQLite type `TEXT`.
* Numeric and `BLOB` arguments must produce an SQL error rather than being coerced.
* An unsupported, empty, whitespace-padded, or embedded-NUL mode name must produce an SQL error listing the supported modes.
* Mode matching is ASCII case-insensitive.
* Empty input text returns empty text.
* Every successful result has SQLite type `text`.
* Embedded U+0000 characters in input text must be preserved.
* Explicit lengths must be used throughout; C-string termination must never determine text length.

Wrong argument counts may use SQLite’s normal arity error.

### 7. Memory and error handling

The implementation shall:

* Use ICU preflight calls to determine required destination capacity.
* Correctly handle `U_BUFFER_OVERFLOW_ERROR` as the expected preflight result.
* Check all length conversions and integer overflows.
* Respect SQLite’s configured string-length limits.
* Use `sqlite3_malloc64()` and `sqlite3_free()` for result buffers.
* Return `sqlite3_result_error_nomem()` on allocation failure.
* Return `sqlite3_result_error_toobig()` for unrepresentable or over-limit results.
* Report ICU failures consistently, preferably through `icuFunctionError()`.
* Pass explicit byte lengths to `sqlite3_result_text16()` or `sqlite3_result_text()`.
* Never leak buffers or `UCollator` instances on any failure path.

Mutable global caches are unnecessary. Calling ICU’s normalizer-instance getters per invocation is acceptable and avoids custom synchronization.

### 8. Documentation requirements

`icuex.c` must contain professional documentation comments covering:

* Module purpose and integration constraints.
* SQL contracts.
* Initialization and ownership.
* Every nontrivial function.
* Buffer units: bytes, UTF-16 code units, and code points.
* ICU preflight behavior.
* Error and cleanup paths.
* The distinction between CCC filtering and general-category mark filtering.
* Why `NFKD_CF_STRIP` uses the specified operation order.

Python test modules, fixtures, and helper functions must have corresponding professional docstrings.

Generate `README.md` containing:

* Purpose and feature summary.
* Build and amalgamation requirements.
* Complete SQL API reference.
* Collation examples.
* Normalization-mode table.
* NULL, type, and error behavior.
* Unicode/ICU version dependence.
* Index compatibility warning: after changing ICU versions or collation semantics, affected indexes may require `REINDEX`.
* Test instructions.

### 9. SQL-only pytest suite

Testing shall use Python’s configured `sqlite3` module, which is assumed to link against the custom SQLite library.

Tests must not:

* Load an extension.
* Register functions or collations from Python.
* Invoke C symbols through `ctypes`, CFFI, or compiled test fixtures.
* Test private C helpers directly.

Suggested structure:

```text
tests/
    conftest.py
    test_introspection.py
    test_collations.py
    test_casefold.py
    test_normalization_standard.py
    test_nfkc_cf.py
    test_nfkd_cf_strip.py
    test_sql_contract.py
    test_schema_integration.py
```

`conftest.py` may provide fresh connection and connection-factory fixtures, but must perform no extension setup.

#### Introspection

Use:

```sql
PRAGMA collation_list;
PRAGMA function_list;
```

Verify:

* Exact presence of `UTF_CI` and `UTF_CI_AI`.
* `str_casefold` with arity 1.
* `str_normalize` with arity 2.
* Scalar-function type.
* UTF-8 registration.
* `SQLITE_DETERMINISTIC` and `SQLITE_INNOCUOUS` flag bits.

These pragmas report collations and functions known to the current connection. [SQLite PRAGMA documentation](https://www.sqlite.org/pragma.html)

Repeat introspection on multiple independently opened connections and after closing and reopening a file-backed database.

#### Behavioral coverage

Include focused tests for:

* ASCII and empty strings.
* Latin, Greek, and Cyrillic case folding.
* Multi-code-point case-fold expansions.
* NFC/NFD composition and decomposition.
* NFKC/NFKD compatibility characters.
* Precomposed and decomposed accents.
* Default-ignorable removal by `NFKC_CF`.
* Nonzero-CCC removal.
* Preservation of a stable CCC-zero mark example.
* Supplementary-plane characters and emoji.
* Embedded U+0000.
* Long inputs forcing dynamic buffer allocation.
* Idempotence of every normalization mode.
* Mixed-case mode names.
* Invalid mode names and whitespace.
* NULL propagation.
* Rejection of integers, reals, and blobs.
* Output `typeof(...) = 'text'`.

Use stable, long-established Unicode characters so tests do not depend unnecessarily on differences between recent Unicode versions.

#### Collation integration

Test collations through:

* Equality.
* Inequality.
* `ORDER BY`.
* Column declarations.
* `CREATE INDEX`.
* `UNIQUE` constraints.
* Indexed lookup and `EXPLAIN QUERY PLAN`.
* Prepared statements on a fresh connection.
* File-backed schemas reopened without initialization SQL.
* Strings containing embedded U+0000.

#### Function integration

With:

```sql
PRAGMA trusted_schema = OFF;
```

verify that both functions remain usable in valid schema contexts requiring deterministic and innocuous functions, including expression indexes and generated columns.

### 10. Acceptance criteria

The implementation is complete only when:

1. A plain `sqlite3.connect(...)` immediately exposes both collations and both functions.
2. No loading or registration SQL is executed by the application or tests.
3. All functionality is verified exclusively through SQL.
4. All specified Unicode transformations and collation examples pass.
5. Functions preserve embedded NULs and supplementary code points.
6. Error, ownership, and allocation paths are documented and leak-free.
7. The complete pytest suite passes against both in-memory and reopened file-backed databases.
8. `README.md` accurately documents the implemented behavior.
