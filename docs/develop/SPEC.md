---
url: https://chatgpt.com/c/6a9fba3e-fb3c-83eb-9509-d15d0bed2caf
---

## `icuex` SQLite extension specification

### 1. Purpose and scope

Implement `icuex`, a dual built-in/loadable SQLite extension providing:

* Two Unicode collations backed by ICU.
* Unicode case folding.
* Unicode normalization and normalized search-key generation.

Its complete supported user-facing interface is SQL. The conditional C initializers are integration entry points rather than an additional data API.

`icuex` shall complement rather than reproduce the SQL surface of SQLite's `ext/icu/icu.c`. It implements focused features not readily provided there: predefined per-connection collations, full Unicode case folding, normalization, and normalized search-key generation. The source must use public SQLite and ICU APIs and own every callback and helper that it registers. Building or enabling `ext/icu/icu.c` is optional; no source ordering, same-translation-unit arrangement, private declaration, or private symbol from that extension is permitted.

All helpers and data authored by `icuex` shall be `static`; standard loader state emitted by `SQLITE_EXTENSION_INIT1` is exempt. Exactly one non-static initializer is compiled in each mode:

* With `SQLITE_CORE`, `int sqlite3IcuexInit(sqlite3 *db)` supports built-in integration.
* Without `SQLITE_CORE`, `int sqlite3_icuex_init(...)` is the conventional loadable-extension entry point and is exported on Windows.

In built-in mode, SQLite invokes `sqlite3IcuexInit()` through its `SQLITE_EXTRA_AUTOEXT` built-in-extension hook. The loadable initializer is invoked by SQLite's extension loader. Neither mode performs process-global self-registration.

### 2. Registration and build-system integration

#### Extension responsibility

`icuex.c` shall implement the shared registration routine:

```c
static int icuexRegister(sqlite3 *db);
```

The source shall begin with the standard dual-mode SQLite declarations:

```c
#ifndef SQLITE_CORE
## include "sqlite3ext.h"
  SQLITE_EXTENSION_INIT1
#else
## include "sqlite3.h"
#endif
```

It shall end with these conditional initializers:

```c
#ifdef SQLITE_CORE

int sqlite3IcuexInit(sqlite3 *db) {
    return icuexRegister(db);
}

#else

## if defined(_WIN32)
__declspec(dllexport)
## endif
int sqlite3_icuex_init(
    sqlite3 *db,
    char **pzErrMsg,
    const sqlite3_api_routines *pApi
) {
    SQLITE_EXTENSION_INIT2(pApi);
    (void)pzErrMsg;  /* Unused parameter */
    return icuexRegister(db);
}

#endif /* SQLITE_CORE */
```

`icuexRegister()` performs all registration owned by the extension. Its registration sequence shall be equivalent to:

```c
static int icuexRegister(sqlite3 *db) {
    int rc;

    rc = icuexRegisterIcuCollation(
        db, "UTF_CI", "root", UCOL_SECONDARY
    );
    if (rc == SQLITE_OK) {
        rc = sqlite3_create_collation_v2(
            db,
            "UTF_CI_AI",
            SQLITE_UTF16,
            db,
            icuexCiAiCollation,
            0
        );
    }
    if (rc == SQLITE_OK) {
        rc = sqlite3_create_function(
            db,
            "str_casefold",
            1,
            SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS,
            0,
            icuexCasefoldFunc,
            0,
            0
        );
    }
    if (rc == SQLITE_OK) {
        rc = sqlite3_create_function(
            db,
            "str_normalize",
            2,
            SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS,
            0,
            icuexNormalizeFunc,
            0,
            0
        );
    }

    return rc;
}
```

The exact private helper names may differ, but the registration behavior and order are normative. Each conditional wrapper is required even if the compiler ultimately inlines or eliminates it.

`icuex` must not:

* Define `SQLITE_EXTRA_AUTOEXT` within `icuex.c`.
* Call `sqlite3_auto_extension()`.
* Register itself through a constructor or process-global side effect.
* Know which other third-party extensions are included.
* Implement aggregation or registration ordering among extensions.

It must also not reference any private definition from `ext/icu/icu.c`.

#### Built-in build-system responsibility

The surrounding build system is responsible for:

1. Starting with SQLite's standard amalgamation.
2. Placing the contents of `icuex.c`, or an `#include "icuex.c"` directive, after `sqlite3.c` in the same translation unit.
3. Building that translation unit with `SQLITE_CORE`, which the standard amalgamation already defines for itself.
4. Supplying this compiler definition:

   ```text
   SQLITE_EXTRA_AUTOEXT=sqlite3IcuexInit
   ```

5. Providing ICU headers and linking the ICU internationalization, common, and data libraries required by the selected ICU build.

With `SQLITE_EXTRA_AUTOEXT` defined, standard SQLite declares the selected function with signature `int (sqlite3*)`, places it in its built-in-extension initializer array, calls it for every new connection, and propagates a non-`SQLITE_OK` result. No additional initializer module, direct `sqlite3_auto_extension()` call, or SQLite source modification other than appending `icuex.c` is required.

`ext/icu/icu.c` may independently be included when its own SQL API is wanted, but it is neither a prerequisite nor a provider of implementation details for `icuex`.

#### Loadable-extension build responsibility

Without `SQLITE_CORE`, compile `icuex.c` as a shared library using position-independent code where required and link it to matching ICU libraries. SQLite itself supplies the extension API table at load time through `SQLITE_EXTENSION_INIT2(pApi)`. The resulting library shall export `sqlite3_icuex_init`; Windows builds shall use the explicit export declaration shown above.

#### Initialization result

On successful invocation of either initializer, that connection must support:

```sql
COLLATE UTF_CI
COLLATE UTF_CI_AI
str_casefold(...)
str_normalize(...)
```

Each initializer must return `SQLITE_OK` only if every `icuex` collation and function was registered successfully. Otherwise it must return the first non-`SQLITE_OK` registration result. SQLite's built-in-extension machinery propagates the built-in result; SQLite's extension loader propagates the loadable result.

Neither mode requires `icu_load_collation()` or SQL initialization. Resources directly owned by a failing registration operation must be released.

### 3. SQL collations

Register two independent collations for every connection:

| SQL name    | Definition                   | Meaning                                                                                   |
| ----------- | ---------------------------- | ----------------------------------------------------------------------------------------- |
| `UTF_CI`    | ICU `root`, `UCOL_SECONDARY` | Linguistic, case-insensitive comparison; secondary accent distinctions remain significant |
| `UTF_CI_AI` | Compare `NFKD_CF_STRIP` keys | Locale-independent case- and accent-insensitive normalized-key comparison                 |

#### `UTF_CI`

`UTF_CI` shall be equivalent to:

```sql
SELECT icu_load_collation('root', 'UTF_CI', 'SECONDARY');
```

Registration instead occurs directly during connection initialization:

```c
ucol_open("root", ...)
ucol_setStrength(..., UCOL_SECONDARY)
sqlite3_create_collation_v2(
    db,
    "UTF_CI",
    SQLITE_UTF16,
    collator,
    icuexIcuCollation,
    icuexIcuCollationDelete
)
```

The collation owns one `UCollator`:

* After successful registration, SQLite owns it through the private `icuexIcuCollationDelete` callback.
* If `sqlite3_create_collation_v2()` fails, `icuex` must call `ucol_close()` itself.
* If later extension initialization fails, the transferred collator remains connection-owned and is released when the failed connection is closed.

Required behavior:

```sql
SELECT 'АБВГДЙЬЁ' = 'абвгдйьё' COLLATE UTF_CI;
-- 1

SELECT 'е' = 'ё' COLLATE UTF_CI;
-- 0

SELECT 'и' = 'й' COLLATE UTF_CI;
-- 0
```

`UTF_CI` is an ICU root linguistic collation, not a comparison of `str_casefold()` results. Do not claim exact Python or Qt case-folded lexical ordering.

#### `UTF_CI_AI`

`UTF_CI_AI` shall not use `UCOL_PRIMARY` and shall not be described as equivalent to `icu_load_collation()`. ICU root primary strength preserves some letter distinctions, including `Й/и` and `Ё/е`; it is not a universal decomposition-and-diacritic-removal operation.

For each input string, `UTF_CI_AI` shall construct exactly the same normalized key as:

```sql
str_normalize(text, 'NFKD_CF_STRIP')
```

The key operation is:

```text
1. NFKC_Casefold
2. NFKD
3. Retain only code points whose canonical combining class is zero
```

Two values compare equal if and only if their resulting UTF-16 keys contain the same sequence of Unicode code points. Unequal keys shall be ordered lexicographically by Unicode code point. No original-text tiebreaker may be used, because differently spelled values with equal keys must compare equal for SQLite equality, `UNIQUE`, and index lookup semantics.

The comparison callback shall:

* Receive UTF-16 strings and explicit byte lengths from SQLite.
* Validate that both byte lengths are nonnegative and divisible by `sizeof(UChar)`.
* Normalize each complete input independently.
* Iterate by Unicode code point when filtering and comparing.
* Preserve embedded U+0000 as an ordinary code point.
* Never use C-string termination to determine a length.
* Release both temporary keys on every path.
* Use no mutable global state or cache.

Registration shall use:

```c
sqlite3_create_collation_v2(
    db,
    "UTF_CI_AI",
    SQLITE_UTF16,
    db,
    icuexCiAiCollation,
    0
)
```

No persistent application-data allocation is required. Passing `db` as the callback argument permits comparison-time failures to be logged and the active operation to be interrupted as described in Section 7.

Required behavior:

```sql
SELECT 'ЙЁ' = 'ие' COLLATE UTF_CI_AI;
-- 1

SELECT 'É' = 'e' COLLATE UTF_CI_AI;
-- 1

SELECT 'Straße' = 'STRASSE' COLLATE UTF_CI_AI;
-- 1

SELECT 'Ａ①' = 'a1' COLLATE UTF_CI_AI;
-- 1
```

`UTF_CI_AI` deliberately includes compatibility normalization and removal of default ignorables because those operations are part of `NFKC_CF`. Its name is concise SQL terminology, not a claim that case and accents are its only equivalences.

Both collation definitions depend on the linked ICU version. Persistent indexes and constraints using either collation may require `REINDEX` after an ICU upgrade or any semantic change to `icuex`.

### 4. `str_casefold(text)`

Signature:

```sql
str_casefold(text) -> TEXT
```

Implement full, locale-independent Unicode default case folding using:

```c
u_strFoldCase(..., U_FOLD_CASE_DEFAULT, ...)
```

Do not normalize before or after folding.

Required behavior includes:

```text
Straße -> strasse
Ё      -> ё
Й      -> й
Σ      -> σ
σ      -> σ
ς      -> σ
ﬀ      -> ff
ſ      -> s
```

Results depend on the Unicode data version provided by the linked ICU library.

`str_casefold()` is not an alias for the overloaded `lower()` supplied when SQLite is separately built with `ext/icu/icu.c`. That function uses `u_strToLower()` and provides locale-sensitive, context-sensitive lowercase mapping; `str_casefold()` uses locale-independent full default case folding for caseless matching. The operations commonly agree, including for Cyrillic `ИЙЕЁЬЪ -> ийеёьъ`, but must remain observably distinct for stable cases such as `Straße`, `ß`/`ẞ`, Greek final sigma, long s, and Latin compatibility ligatures. `str_casefold()` must not acquire locale-dependent behavior merely to agree with `lower()`.

### 5. `str_normalize(text, kind)`

Signature:

```sql
str_normalize(text, kind) -> TEXT
```

`kind` is an ASCII, case-insensitive mode name. Matching must use its explicit byte length. Do not trim whitespace, accept aliases, accept prefixes or suffixes, or treat an embedded U+0000 as the end of the mode name.

Supported modes:

| Mode            | Processing                                                   |
| --------------- | ------------------------------------------------------------ |
| `NFC`           | `unorm2_getNFCInstance()`                                    |
| `NFD`           | `unorm2_getNFDInstance()`                                    |
| `NFKC`          | `unorm2_getNFKCInstance()`                                   |
| `NFKD`          | `unorm2_getNFKDInstance()`                                   |
| `NFKC_CF`       | `unorm2_getNFKCCasefoldInstance()`                           |
| `NFKD_CF_STRIP` | `NFKC_CF` -> `NFKD` -> retain only code points with CCC zero |

ICU defines NFKC case folding as the Unicode NFKC_Casefold mappings followed by NFC. The standard normalizer objects are immutable singletons and must not be deleted. [ICU Normalizer2 API](https://unicode-org.github.io/icu-docs/apidoc/dev/icu4c/unorm2_8h.html)

#### `NFKD_CF_STRIP`

The processing order is mandatory:

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

The filtering pass must iterate over Unicode code points using ICU UTF-16 iteration macros such as `U16_NEXT`, not individual UTF-16 code units.

This operation filters by canonical combining class, not Unicode general category. A character categorized as `Mn`, `Mc`, or `Me` must remain if its CCC is zero. ICU defines `u_getCombiningClass()` as returning the canonical combining class of a code point. [ICU character API](https://unicode-org.github.io/icu-docs/apidoc/dev/icu4c/uchar_8h.html)

Required behavior:

```text
NFKC_CF:
    Straße -> strasse
    Ё      -> ё
    Й      -> й
    É      -> é
    U+00AD SOFT HYPHEN -> removed

NFKD_CF_STRIP:
    Straße -> strasse
    Ё      -> е
    Й      -> и
    É      -> e
```

For mixed scripts, normalization does not transliterate. Consequently:

```text
ЁЙÉ -> еиe
```

where the final `e` is Latin. A test must also demonstrate preservation of a stable mark whose CCC is zero, such as U+20DD COMBINING ENCLOSING CIRCLE, provided it survives the preceding NFKC_CF stage.

### 6. SQL contract

Register the functions with exactly:

```c
SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS
```

For both functions:

* A `NULL` argument produces SQL `NULL`.
* For `str_normalize()`, either argument being `NULL` produces SQL `NULL`.
* Every non-`NULL` argument must have SQLite storage class `TEXT`.
* Integers, reals, and blobs must produce an SQL error instead of coercion.
* Empty input text returns empty text.
* Every successful result has storage class `text`.
* Embedded U+0000 characters in input text must be preserved.
* Explicit lengths must be used throughout.

For `str_normalize()`:

* Mode matching is ASCII case-insensitive.
* Invalid, empty, whitespace-padded, non-ASCII, or embedded-NUL modes produce an SQL error.
* The error must identify the invalid mode safely and list every supported mode.
* Error formatting must not truncate at an embedded U+0000 or read beyond the supplied mode length.

Wrong argument counts may use SQLite's standard arity error.

Inputs are expected to represent well-formed Unicode text. Behavior for deliberately malformed SQLite text encodings is outside the supported contract and must not be used as a portability guarantee.

### 7. UTF-16 processing, memory, and errors

The implementation should obtain text for ICU processing as native-endian UTF-16 using:

```c
sqlite3_value_text16(...)
sqlite3_value_bytes16(...)
```

Registration as `SQLITE_UTF8` specifies SQLite's preferred function encoding; it does not prevent the implementation from requesting UTF-16.

All lengths must clearly distinguish among bytes, UTF-16 code units, and Unicode code points.

For SQL functions, the implementation shall:

* Use ICU preflight calls to determine destination capacity.
* Treat `U_BUFFER_OVERFLOW_ERROR` as the normal nonempty preflight result.
* Handle zero-length output whose preflight succeeds with `U_ZERO_ERROR`.
* Reset `UErrorCode` before the real conversion call.
* Check multiplication by `sizeof(UChar)` for overflow.
* Check conversions among `size_t`, `sqlite3_uint64`, `int`, and `int32_t`.
* Respect `SQLITE_LIMIT_LENGTH`.
* Use `sqlite3_malloc64()` and `sqlite3_free()` for dynamic buffers.
* Return `sqlite3_result_error_nomem()` on allocation failure.
* Return `sqlite3_result_error_toobig()` for unrepresentable or over-limit output.
* Report ICU failures consistently through an `icuex`-owned helper using `u_errorName()` and `sqlite3_result_error()`.
* Pass explicit byte lengths to SQLite result APIs.
* Never rely on NUL termination to determine input or output length.
* Never leak temporary buffers or `UCollator` objects.

The `NFKD_CF_STRIP` filtering pass may compact a UTF-16 buffer in place because it only removes code points. It must still decode and re-encode complete code points correctly.

#### Collation-callback failures

SQLite's collation callback has no error-result channel. The `UTF_CI_AI` callback shall therefore use a private normalization helper that reports success or failure without requiring an `sqlite3_context`.

If allocation, length validation, or ICU processing fails during comparison, the callback shall:

1. Release every temporary allocation.
2. Log the failure with `sqlite3_log()` when a useful reason is available.
3. Call `sqlite3_interrupt(db)` using its callback argument so the active SQL operation cannot safely continue building or modifying an index under degraded comparison semantics.
4. Return an explicit-length UTF-16 code-unit comparison as the callback's mandatory provisional integer result.

The provisional fallback exists only because `xCompare` must return an integer; it is not part of the supported `UTF_CI_AI` semantics. Correct operation assumes sufficient memory and valid Unicode input. Tests need not attempt deterministic simulation of process-wide allocation exhaustion.

Mutable global caches are unnecessary. Calling ICU's normalizer-instance getters for each SQL invocation or comparison is acceptable and avoids custom synchronization.

### 8. Documentation requirements

`icuex.c` must contain professional documentation comments covering:

* Module purpose and integration constraints.
* Its complementary relationship to `ext/icu/icu.c`, its non-duplication of
  that extension's SQL surface, and its use of public APIs only.
* Conditional built-in and loadable-extension initialization.
* The different implementations and semantics of the two collations.
* Why ICU root primary strength does not implement `UTF_CI_AI`.
* The exact relationship between `UTF_CI_AI` and `NFKD_CF_STRIP`.
* SQL contracts.
* Initialization order and failure propagation.
* Collator and result-buffer ownership.
* Every nontrivial function.
* Byte, UTF-16-code-unit, and code-point length conventions.
* ICU preflight behavior, including empty output.
* SQL-function and collation-callback error paths.
* Strict `TEXT` input handling and embedded U+0000 handling.
* CCC filtering versus general-category mark filtering.
* The required `NFKD_CF_STRIP` operation order.

Python test modules, fixtures, and helpers must have corresponding professional docstrings.

Generate `README.md` containing:

* Purpose and features.
* Standard-amalgamation built-in and loadable-extension build requirements.
* Automatic built-in initialization and explicit loadable initialization.
* Complete SQL API reference.
* The distinct definitions of `UTF_CI` and `UTF_CI_AI`.
* Collation examples.
* Normalization-mode table.
* NULL, type, mode, and error behavior.
* Unicode and ICU version dependence.
* An index compatibility and `REINDEX` warning.
* Test instructions.

### 9. SQL-only pytest suite

Testing shall use Python's configured `sqlite3` module and SQL. The same behavioral suite shall support two modes:

* With no test environment override, Python's SQLite library is assumed to contain built-in `icuex` registered through `SQLITE_EXTRA_AUTOEXT`.
* When `ICUEX_EXTENSION` names the compiled shared library, each fresh test connection shall enable extension loading, load that library, and disable further extension loading before yielding the connection.

Tests must not:

* Register SQL functions or collations from Python.
* Invoke C symbols through `ctypes`, CFFI, or compiled test fixtures.
* Test private C helpers directly.
* Execute `icu_load_collation()` as setup.

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

`conftest.py` may provide fresh connection and connection-factory fixtures. It must perform no setup in built-in mode; in loadable mode it may only load the library selected by `ICUEX_EXTENSION`.

#### Introspection

Use only:

```sql
PRAGMA collation_list;
PRAGMA function_list;
```

Verify:

* Exact presence of `UTF_CI` and `UTF_CI_AI`.
* `str_casefold` with arity 1.
* `str_normalize` with arity 2.
* Scalar-function type.
* UTF-8 preferred encoding.
* Presence of `SQLITE_DETERMINISTIC` and `SQLITE_INNOCUOUS`.
* Absence of `SQLITE_DIRECTONLY`, `SQLITE_SUBTYPE`, and `SQLITE_RESULT_SUBTYPE`.

Repeat introspection using multiple simultaneous in-memory connections, a newly opened file-backed connection, and a closed and reopened file-backed database. In loadable mode the library must be loaded into each connection; in built-in mode no setup is permitted.

#### Behavioral coverage

Include focused tests for:

* ASCII and empty strings.
* Latin, Greek, and Cyrillic case folding.
* Multi-code-point folding expansions, including sharp s and the stable Latin presentation-form ligatures.
* Hard equality and inequality assertions distinguishing full case folding from lowercase mapping for `Straße`, `ß`/`ẞ`, Greek contextual/final sigma, long s, and compatibility ligatures.
* The same focused special-case corpus, plus Cyrillic `ИЙЕЁЬЪ`, through every supported normalization mode: `NFC`, `NFD`, `NFKC`, `NFKD`, `NFKC_CF`, and `NFKD_CF_STRIP`.
* NFC/NFD composition and decomposition.
* NFKC/NFKD compatibility characters.
* Precomposed and decomposed accents.
* U+00AD or another stable default-ignorable character under `NFKC_CF`.
* Nonzero-CCC removal and preservation of a stable CCC-zero mark.
* Supplementary-plane characters and emoji.
* Embedded U+0000.
* Long inputs requiring dynamic output allocation.
* Expansion outputs longer than their inputs.
* Idempotence of every normalization mode.
* Mixed-case mode names.
* Empty, invalid, whitespace-padded, non-ASCII, and embedded-NUL modes.
* NULL propagation from each argument position.
* Rejection of integers, reals, and blobs.
* `typeof(result) = 'text'`.

Use established Unicode characters whose behavior is stable across supported ICU versions. Do not compare the complete implementation against Python's `unicodedata` or `str.casefold()`, because Python and ICU may use different Unicode versions.

When `ext/icu/icu.c` is present, separately probe its two-argument `lower(text, 'root')` overload over the same corpus. Those tests document and compare another SQLite extension; they are not acceptance tests for `icuex`. They shall contain explicit expected equality and inequality assertions, but shall catch an unavailable overload or assertion mismatch and issue a clear pytest warning identifying `ext/icu/icu.c` instead of failing the `icuex` suite. Assertions concerning `str_casefold()` and `str_normalize()` remain ordinary hard failures.

#### Collation integration

Test the collations through:

* Equality and inequality.
* The specified case and accent distinctions.
* `UTF_CI_AI` equivalence for `ЙЁ/ие`, `É/e`, `Straße/STRASSE`, and compatibility forms.
* Agreement between `UTF_CI_AI` equality and equality of `NFKD_CF_STRIP` keys for a focused set of stable examples.
* `ORDER BY` without asserting a broad exact ICU sort order.
* Column collation declarations.
* Named indexes and indexed equality lookups.
* `UNIQUE` constraints over equivalent normalized keys.
* `EXPLAIN QUERY PLAN` use of the named index.
* Parameterized Python SQL statements.
* File-backed schemas reopened without setup SQL.
* Strings containing embedded U+0000.

For query-plan tests, check use of the named index rather than matching the entire unstable plan-description string.

#### Function integration

With:

```sql
PRAGMA trusted_schema = OFF;
```

verify both functions in schema contexts requiring deterministic and innocuous functions, including expression indexes, generated columns, and partial indexes where appropriate. Verify use of a named expression index without depending on the complete textual formatting of `EXPLAIN QUERY PLAN`.

### 10. Acceptance criteria

Implementation is complete only when:

1. Direct invocation of `sqlite3IcuexInit(db)` registers both collations and both functions on that connection or returns the first registration failure.
2. Loading the separately compiled shared library invokes `sqlite3_icuex_init()` and registers the same SQL surface.
3. In the completed built-in build, plain `sqlite3.connect(...)` immediately exposes all four SQL features because SQLite's `SQLITE_EXTRA_AUTOEXT` hook invokes `sqlite3IcuexInit()`.
4. `icuex.c` compiles as its own source in both modes using public SQLite and ICU headers and has no dependency on `ext/icu/icu.c`.
5. `icuex.c` does not define `SQLITE_EXTRA_AUTOEXT`, call `sqlite3_auto_extension()`, or perform process-global self-registration.
6. `UTF_CI` implements ICU root secondary-strength collation.
7. `UTF_CI_AI` compares exact `NFKD_CF_STRIP` keys and makes `ЙЁ` equivalent to `ие`.
8. All functionality is verified exclusively through SQL executed by pytest.
9. All specified Unicode transformations and collation examples pass.
10. Embedded U+0000 and supplementary characters are preserved.
11. Function introspection reports exactly the required arities, encoding, and flags.
12. Multiple independent and reopened connections work in each test mode.
13. Resource ownership and every failure path are documented and leak-free by construction and review.
14. The complete pytest suite passes in loadable mode and in the target built-in environment.
15. `README.md` accurately describes the implemented behavior.
