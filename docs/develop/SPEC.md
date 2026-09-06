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

* Two automatically available ICU collations.
* Unicode case folding.
* Unicode normalization and normalized search-key generation.

`icuex` is not a loadable extension. Its complete supported user-facing interface is SQL.

The extension shall be compiled only as part of a custom SQLite amalgamation in which:

1. SQLite’s `ext/icu/icu.c` is enabled.
2. `icu.c` occurs in the same C translation unit before `icuex.c`.
3. `icuex.c` may therefore reuse `static` implementation details from `icu.c`, particularly:

   ```c
   icuCollationColl
   icuCollationDel
   icuFunctionError
   ```

`icu.c` itself shall remain unchanged. Do not duplicate its ICU collation comparison or destruction callbacks in `icuex.c`.

All `icuex` helpers and data shall be `static`. The only non-static symbol shall be the internal component initializer:

```c
int sqlite3IcuexInit(sqlite3 *db);
```

This function is the integration entry point through which the surrounding amalgamation registers `icuex` with a connection. It is not a supported public API and shall have:

* No public header declaration.
* No `SQLITE_API` annotation.
* No three-argument loadable-extension wrapper.
* No `sqlite3_icuex_init()` loadable-extension symbol.

The mechanism that invokes `sqlite3IcuexInit()` automatically is external to `icuex` and outside the extension’s implementation scope.

Because `icuex` deliberately depends on private `static` definitions in the preceding `icu.c`, changes to those definitions or to amalgamation ordering are build-time compatibility concerns and shall fail at compilation rather than silently select another implementation.

### 2. Registration and build-system integration

#### Extension responsibility

`icuex.c` shall implement:

```c
static int icuexRegister(sqlite3 *db);
int sqlite3IcuexInit(sqlite3 *db);
```

The component initializer shall remain a one-statement wrapper:

```c
int sqlite3IcuexInit(sqlite3 *db) {
    return icuexRegister(db);
}
```

`icuexRegister()` performs all registration owned by the extension:

```c
static int icuexRegister(sqlite3 *db) {
    int rc;

    rc = icuexRegisterCollation(db, "UTF_CI", "root", UCOL_SECONDARY);
    if (rc == SQLITE_OK) {
        rc = icuexRegisterCollation(db, "UTF_CI_AI", "root", UCOL_PRIMARY);
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

The wrapper is required even if the compiler ultimately inlines or eliminates it.

`icuex` must not:

* Define or declare `sqlite3ExtraAutoExtInit`.
* Define or configure `SQLITE_EXTRA_AUTOEXT`.
* Call `sqlite3_auto_extension()`.
* Register itself through a constructor or process-global side effect.
* Know which other third-party extensions are included.
* Implement aggregation or registration ordering among extensions.

#### External build-system responsibility

The surrounding build system is responsible for:

1. Enabling inclusion of `icuex.c`, conventionally through:

   ```text
   SQLITE_ENABLE_ICUEX
   ```

2. Placing `icuex.c` after `ext/icu/icu.c` in the same amalgamation translation unit.

3. Generating a separate aggregate-initializer module that invokes the component initializers for all included third-party extensions.

4. Calling:

   ```c
   sqlite3IcuexInit(db)
   ```

   from that generated aggregate initializer when `icuex` is enabled.

5. Configuring SQLite so that the aggregate initializer is invoked for every new connection, conventionally through:

   ```text
   SQLITE_EXTRA_AUTOEXT=sqlite3ExtraAutoExtInit
   ```

6. Propagating any non-`SQLITE_OK` result returned by `sqlite3IcuexInit()`.

The name, implementation, generation, and extension ordering of `sqlite3ExtraAutoExtInit()` are outside the `icuex` source contract.

#### Initialization result

On successful invocation of `sqlite3IcuexInit(db)`, that connection must immediately support:

```sql
COLLATE UTF_CI
COLLATE UTF_CI_AI
str_casefold(...)
str_normalize(...)
```

`sqlite3IcuexInit()` must return:

* `SQLITE_OK` only if every `icuex` collation and function was registered successfully.
* The first non-`SQLITE_OK` registration result otherwise.

The external aggregate initializer is responsible for propagating this result to SQLite connection initialization.

No application-side `load_extension()`, `icu_load_collation()`, SQL initialization, or Python registration is permitted.

Failure to register any collation or function must produce a non-`SQLITE_OK` initialization result. All resources directly owned by the failing operation must be released.

### 3. SQL collations

Register two independent collations for every connection:

| SQL name    | ICU locale |         Strength | Meaning                                      |
| ----------- | ---------: | ---------------: | -------------------------------------------- |
| `UTF_CI`    |     `root` | `UCOL_SECONDARY` | Case-insensitive; accents remain significant |
| `UTF_CI_AI` |     `root` |   `UCOL_PRIMARY` | Case-insensitive and accent-insensitive      |

Their semantics must be equivalent to:

```sql
SELECT icu_load_collation('root', 'UTF_CI',    'SECONDARY');
SELECT icu_load_collation('root', 'UTF_CI_AI', 'PRIMARY');
```

Registration must instead occur directly during connection initialization:

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

Each collation owns a distinct `UCollator`.

* After successful registration, SQLite owns the collator through `icuCollationDel`.
* If `sqlite3_create_collation_v2()` fails, `icuex` must call `ucol_close()` itself.
* If later extension initialization fails, resources already transferred to SQLite remain connection-owned and are released when the failed connection is closed.

Required behavior:

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

These are ICU root collations, not comparisons of `str_casefold()` results. Do not claim exact Python or Qt case-folded lexical ordering.

Collation behavior and sort keys depend on the linked ICU version. Persistent indexes using these collations may require `REINDEX` after an ICU upgrade.

### 4. `str_casefold(text)`

Signature:

```sql
str_casefold(text) → TEXT
```

Implement full, locale-independent Unicode default case folding using:

```c
u_strFoldCase(..., U_FOLD_CASE_DEFAULT, ...)
```

Do not normalize before or after folding.

Required behavior includes:

```text
Straße → strasse
Ё      → ё
Й      → й
Σ      → σ
σ      → σ
ς      → σ
```

Results depend on the Unicode data version provided by the linked ICU library.

### 5. `str_normalize(text, kind)`

Signature:

```sql
str_normalize(text, kind) → TEXT
```

`kind` is an ASCII, case-insensitive mode name. Matching must use its explicit byte length.

Do not:

* Trim whitespace.
* Accept aliases.
* Accept prefixes or suffixes.
* Treat an embedded U+0000 as the end of the mode name.

Supported modes:

| Mode            | Processing                                                 |
| --------------- | ---------------------------------------------------------- |
| `NFC`           | `unorm2_getNFCInstance()`                                  |
| `NFD`           | `unorm2_getNFDInstance()`                                  |
| `NFKC`          | `unorm2_getNFKCInstance()`                                 |
| `NFKD`          | `unorm2_getNFKDInstance()`                                 |
| `NFKC_CF`       | `unorm2_getNFKCCasefoldInstance()`                         |
| `NFKD_CF_STRIP` | `NFKC_CF` → `NFKD` → retain only code points with CCC zero |

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

The final filtering pass must iterate over Unicode code points using ICU UTF-16 iteration macros such as `U16_NEXT`, not over individual UTF-16 code units.

This operation filters by canonical combining class, not Unicode general category. A character categorized as `Mn`, `Mc`, or `Me` must remain if its CCC is zero. ICU defines `u_getCombiningClass()` as returning the canonical combining class of a code point. [ICU character API](https://unicode-org.github.io/icu-docs/apidoc/dev/icu4c/uchar_8h.html)

Required behavior:

```text
NFKC_CF:
    Straße → strasse
    Ё      → ё
    Й      → й
    É      → é
    U+00AD SOFT HYPHEN → removed

NFKD_CF_STRIP:
    Straße → strasse
    Ё      → е
    Й      → и
    É      → e
```

A test must also demonstrate preservation of a stable mark whose CCC is zero, such as U+20DD COMBINING ENCLOSING CIRCLE, provided it survives the preceding NFKC_CF stage.

### 6. SQL contract

Register the functions with exactly:

```c
SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS
```

For both functions:

* A `NULL` argument produces SQL `NULL`.
* For `str_normalize()`, either argument being `NULL` produces SQL `NULL`.
* Every non-`NULL` argument must have SQLite storage class `TEXT`.
* Integers, reals, and blobs must produce an SQL error rather than being coerced.
* Empty input text returns empty text.
* Every successful result has storage class `text`.
* Embedded U+0000 characters in input text must be preserved.
* Explicit lengths must be used throughout.

For `str_normalize()`:

* Mode matching is ASCII case-insensitive.
* Invalid, empty, whitespace-padded, non-ASCII, or embedded-NUL modes produce an SQL error.
* The error must identify the invalid mode safely and list all supported modes.
* Error formatting must not truncate at an embedded U+0000 or read beyond the supplied mode length.

Wrong argument counts may use SQLite’s standard arity error.

Inputs are expected to represent well-formed Unicode text. Behavior for deliberately malformed SQLite text encodings is outside the supported contract and must not be used as a portability guarantee.

### 7. UTF-16 processing, memory, and errors

The implementation should obtain text for ICU processing as native-endian UTF-16 using:

```c
sqlite3_value_text16(...)
sqlite3_value_bytes16(...)
```

Registration as `SQLITE_UTF8` specifies SQLite’s preferred function encoding; it does not prevent the implementation from requesting a UTF-16 representation.

All lengths must clearly distinguish among:

* Bytes.
* UTF-16 code units.
* Unicode code points.

The implementation shall:

* Use ICU preflight calls to determine destination capacity.
* Treat `U_BUFFER_OVERFLOW_ERROR` as the normal nonempty preflight result.
* Also handle a zero-length result whose preflight succeeds with `U_ZERO_ERROR`.
* Reset `UErrorCode` before the real conversion call.
* Check multiplication by `sizeof(UChar)` for overflow.
* Check conversion between `size_t`, `sqlite3_uint64`, `int`, and `int32_t`.
* Respect `SQLITE_LIMIT_LENGTH`.
* Use `sqlite3_malloc64()` and `sqlite3_free()` for dynamically allocated result buffers.
* Return `sqlite3_result_error_nomem()` on allocation failure.
* Return `sqlite3_result_error_toobig()` for an unrepresentable or over-limit result.
* Report ICU failures consistently, preferably through `icuFunctionError()`.
* Pass explicit byte lengths to `sqlite3_result_text16()` or explicit byte lengths to `sqlite3_result_text()`.
* Never rely on NUL termination to determine input or output length.
* Never leak temporary buffers or `UCollator` objects.

The `NFKD_CF_STRIP` filtering pass may compact a UTF-16 buffer in place because it only removes code points. It must nevertheless decode and re-encode complete code points correctly.

Mutable global caches are unnecessary. Calling ICU’s normalizer-instance getters for each SQL invocation is acceptable and avoids custom synchronization.

### 8. Documentation requirements

`icuex.c` must contain professional documentation comments covering:

* Module purpose and integration constraints.
* Its intentional dependency on preceding `icu.c` definitions.
* SQL contracts.
* Initialization order and failure propagation.
* Collator and result-buffer ownership.
* Every nontrivial function.
* Byte, UTF-16-code-unit, and code-point length conventions.
* ICU preflight behavior, including empty output.
* Error and cleanup paths.
* Strict `TEXT` input handling.
* Embedded U+0000 handling.
* CCC filtering versus general-category mark filtering.
* The required `NFKD_CF_STRIP` operation order.

Python test modules, fixtures, and helpers must have corresponding professional docstrings.

Generate `README.md` containing:

* Purpose and features.
* Build and amalgamation requirements.
* Automatic initialization design.
* Complete SQL API reference.
* Collation examples.
* Normalization-mode table.
* NULL, type, mode, and error behavior.
* Unicode and ICU version dependence.
* An index compatibility and `REINDEX` warning.
* Test instructions.

### 9. SQL-only pytest suite

Testing shall use Python’s configured `sqlite3` module, assumed to link against the custom SQLite library.

Tests must not:

* Load an extension.
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

`conftest.py` may provide fresh connection and connection-factory fixtures but must perform no extension setup.

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
* Presence of `SQLITE_DETERMINISTIC`.
* Presence of `SQLITE_INNOCUOUS`.
* Absence of `SQLITE_DIRECTONLY`, `SQLITE_SUBTYPE`, and `SQLITE_RESULT_SUBTYPE`.

Repeat introspection using:

* Multiple simultaneous in-memory connections.
* A newly opened file-backed connection.
* A closed and reopened file-backed database.

#### Behavioral coverage

Include focused tests for:

* ASCII and empty strings.
* Latin, Greek, and Cyrillic case folding.
* Multi-code-point folding expansions.
* NFC/NFD composition and decomposition.
* NFKC/NFKD compatibility characters.
* Precomposed and decomposed accents.
* U+00AD or another stable default-ignorable character under `NFKC_CF`.
* Nonzero-CCC removal.
* Preservation of a stable CCC-zero mark.
* Supplementary-plane characters and emoji.
* Embedded U+0000.
* Long inputs requiring dynamically allocated output.
* Expansion outputs longer than their inputs.
* Idempotence of every normalization mode.
* Mixed-case mode names.
* Empty, invalid, whitespace-padded, non-ASCII, and embedded-NUL modes.
* NULL propagation from each argument position.
* Rejection of integers, reals, and blobs.
* `typeof(result) = 'text'`.

Use established Unicode characters whose behavior is stable across the supported ICU versions. Do not compare the entire implementation against Python’s `unicodedata` or `str.casefold()`, because Python and ICU may use different Unicode versions.

#### Collation integration

Test the collations through:

* Equality and inequality.
* Case-sensitive versus accent-sensitive distinctions.
* `ORDER BY`.
* Column collation declarations.
* Named indexes.
* `UNIQUE` constraints.
* Indexed equality lookups.
* `EXPLAIN QUERY PLAN`.
* Parameterized Python SQL statements.
* File-backed schemas reopened without setup SQL.
* Strings containing embedded U+0000.

Avoid asserting a broad exact ICU sort order. Assert only required equivalence, distinction, and index-use properties. For query-plan tests, check use of the named index rather than matching the entire unstable plan-description string.

#### Function integration

With:

```sql
PRAGMA trusted_schema = OFF;
```

verify both functions in schema contexts requiring deterministic and innocuous functions, including:

* Expression indexes.
* Generated columns.
* Partial indexes, where appropriate.

Verify that queries use a named expression index without depending on the complete textual formatting of `EXPLAIN QUERY PLAN`.

### 10. Acceptance criteria

Implementation is complete only when:

1. Direct invocation of `sqlite3IcuexInit(db)` registers both collations and both functions on that connection or returns the first registration failure.
2. In the completed amalgamation, a plain `sqlite3.connect(...)` immediately exposes all four SQL features because the external build-generated aggregate initializer invokes `sqlite3IcuexInit()`.
3. `icuex.c` neither defines nor depends on the identity or implementation of the aggregate initializer and performs no process-global self-registration.
4. All functionality is verified exclusively through SQL executed by pytest.
5. All specified Unicode transformations and collation examples pass.
6. Embedded U+0000 and supplementary characters are preserved.
7. Function introspection reports exactly the required arities, encoding, and flags.
8. Multiple independent and reopened connections require no setup.
9. Resource ownership and every failure path are documented and leak-free by construction and review.
10. The complete pytest suite passes.
11. `README.md` accurately describes the implemented behavior.
