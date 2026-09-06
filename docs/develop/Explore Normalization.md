---
url: https://chatgpt.com/c/6a9cfb24-1e98-83eb-b1a0-b3d1a7fa34da
---

## 📗 Unicode Casefold in Python

> [!NOTE] Prompt
> 
> Python provides unicode .casefold. Is this function actually provided by icu4c?

Yes. ICU4C provides the same Unicode operation: **full Unicode case folding** based on `CaseFolding.txt`.

Relevant APIs:

* UTF-16 C API: `u_strFoldCase()`
* UTF-8 C API: `ucasemap_utf8FoldCase()`
* C++ API: `icu::UnicodeString::foldCase()`
* Direct comparison without materializing folded strings: `u_strCaseCompare()`

Use `U_FOLD_CASE_DEFAULT` for behavior corresponding to Python’s normal `str.casefold()`:

```cpp
icu::UnicodeString s = icu::UnicodeString::fromUTF8("Straße");
s.foldCase(U_FOLD_CASE_DEFAULT);

// Result: "strasse"
```

Important distinction: Python does **not** call ICU4C internally. CPython implements case folding using its own compiled Unicode Character Database. ICU4C independently implements the same Unicode default case-folding specification. Results should match when both use the same Unicode version, but version differences can produce discrepancies for newly added or revised characters.

Also:

* Case folding is locale-independent.
* It may change string length: `ß → ss`.
* It does not perform Unicode normalization.
* `U_FOLD_CASE_EXCLUDE_SPECIAL_I` enables the Turkic dotted/dotless-I variant; do not use it if you want Python-compatible default folding.
* For combined compatibility normalization plus folding, ICU also provides `unorm2_getNFKCCasefoldInstance()`, but that is **not** equivalent to plain Python `str.casefold()`.

[ICU case-folding documentation](https://unicode-org.github.io/icu/userguide/transforms/casemappings.html#case-folding) · [ICU4C `u_strFoldCase()` API](https://unicode-org.github.io/icu-docs/apidoc/released/icu4c/ustring_8h.html) · [UTF-8 `ucasemap_utf8FoldCase()` API](https://unicode-org.github.io/icu-docs/apidoc/released/icu4c/ucasemap_8h.html)

---
---

## 📗 Unicode Casefold in SQLite

> [!NOTE] Prompt
> 
> If I want to expose casefold in SQLite, do I use UTF-8 C API: `ucasemap_utf8FoldCase()`?

Yes. If your SQLite scalar function accepts and returns normal SQLite `TEXT`, register it as `SQLITE_UTF8` and use `ucasemap_utf8FoldCase()` directly.

The mapping is:

```text
sqlite3_value_text() + sqlite3_value_bytes()
        ↓ UTF-8 bytes
ucasemap_utf8FoldCase()
        ↓ UTF-8 bytes
sqlite3_result_text()
```

Important implementation details:

* Pass `sqlite3_value_bytes()` as `srcLength`; do not use `strlen()`, because SQLite strings may contain embedded NULs.
* Open one `UCaseMap` with `U_FOLD_CASE_DEFAULT`.
* Reuse that object through the SQLite function’s `pApp` pointer.
* Register with `sqlite3_create_function_v2()` so its destructor can call `ucasemap_close()`.
* Use `SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS`, especially if the function may appear in generated columns or expression indexes.
* Handle ICU’s variable-length output: `ß` becomes `ss`.
* Case folding does not normalize Unicode. Apply NFC/NFKC separately if required.

Conceptually:

```c
sqlite3_create_function_v2(
    db,
    "casefold",
    1,
    SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS,
    case_map,
    sqlite_casefold,
    NULL,
    NULL,
    destroy_case_map
);
```

Inside `sqlite_casefold()`:

```c
const char *src = (const char *)sqlite3_value_text(argv[0]);
int32_t src_len = sqlite3_value_bytes(argv[0]);

int32_t required = ucasemap_utf8FoldCase(
    case_map, NULL, 0, src, src_len, &status
);

/* U_BUFFER_OVERFLOW_ERROR is expected during sizing. */
status = U_ZERO_ERROR;

char *dest = sqlite3_malloc64((sqlite3_uint64)required + 1);

int32_t dest_len = ucasemap_utf8FoldCase(
    case_map, dest, required + 1, src, src_len, &status
);

sqlite3_result_text64(
    context,
    dest,
    (sqlite3_uint64)dest_len,
    sqlite3_free,
    SQLITE_UTF8
);
```

Also preserve SQL `NULL`:

```c
if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
    sqlite3_result_null(context);
    return;
}
```

For direct case-insensitive comparison, a collation using ICU comparison APIs may avoid allocating folded copies. But for an exposed SQL transformation such as:

```sql
SELECT casefold('Straße');  -- strasse
```

`ucasemap_utf8FoldCase()` is the appropriate ICU4C API.

---
---

## 📗 SQLite Auto Extension

> [!NOTE] Prompt
> 
> I want to make an extension, which will only be loaded as part of amalgamation with enabled ICU after it, so that any useful code provided by ext/icu/icu.c, including static methods, could be reused to minimize new code.

In that setup, I would **not** use `ucasemap_utf8FoldCase()`. Reuse the design already used by `ext/icu/icu.c`:

* Obtain input using `sqlite3_value_text16()`.
* Fold using `u_strFoldCase()`.
* Return using `sqlite3_result_text16()`.
* Reuse the existing static `xFree()` and `icuFunctionError()` helpers.
* Register UTF-8 and UTF-16 overloads pointing to the same UTF-16 implementation, exactly as ICU does for `lower()` and `upper()`.

`ucasemap_utf8FoldCase()` requires a `UCaseMap` object and therefore adds lifecycle code that you do not need. `u_strFoldCase()` is a direct stateless function.

```c
#if defined(SQLITE_ENABLE_ICU)

static void icuCasefoldFunc16(
  sqlite3_context *pCtx,
  int nArg,
  sqlite3_value **apArg
){
  const UChar *zInput;
  UChar *zOutput = 0;
  int nInput;
  sqlite3_int64 nOut;
  int cnt;
  UErrorCode status;

  assert(nArg==1);

  zInput = sqlite3_value_text16(apArg[0]);
  if( zInput==0 ){
    /* This also propagates SQL NULL. */
    return;
  }

  nOut = nInput = sqlite3_value_bytes16(apArg[0]);

  if( nOut==0 ){
    sqlite3_result_text16(pCtx, "", 0, SQLITE_STATIC);
    return;
  }

  for(cnt=0; cnt<2; cnt++){
    UChar *zNew = sqlite3_realloc64(zOutput, nOut);

    if( zNew==0 ){
      sqlite3_free(zOutput);
      sqlite3_result_error_nomem(pCtx);
      return;
    }

    zOutput = zNew;
    status = U_ZERO_ERROR;

    nOut = 2LL * u_strFoldCase(
        zOutput,
        (int32_t)(nOut / 2),
        zInput,
        nInput / 2,
        U_FOLD_CASE_DEFAULT,
        &status
    );

    if( U_SUCCESS(status) ){
      sqlite3_result_text16(pCtx, zOutput, nOut, xFree);
      return;
    }

    if( status!=U_BUFFER_OVERFLOW_ERROR ){
      sqlite3_free(zOutput);
      icuFunctionError(pCtx, "u_strFoldCase", status);
      return;
    }

    assert(cnt==0);
  }

  assert(0); /* Unreachable */
}

int sqlite3CasefoldInit(sqlite3 *db){
  int rc;

  rc = sqlite3_create_function(
      db,
      "casefold",
      1,
      SQLITE_UTF16 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS,
      0,
      icuCasefoldFunc16,
      0,
      0
  );

  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(
        db,
        "casefold",
        1,
        SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS,
        0,
        icuCasefoldFunc16,
        0,
        0
    );
  }

  return rc;
}

#endif
```

Then invoke `sqlite3CasefoldInit(db)` for each new connection immediately after `sqlite3IcuInit(db)` succeeds.

One essential qualification: your source must be **textually included in `sqlite3.c` after `icu.c`**, meaning the two files become one C translation unit:

```c
#include "../ext/icu/icu.c"
#include "../ext/icu/casefold.c"
```

Merely compiling `casefold.c` as another object file and linking it after `icu.o` is insufficient. The `static` functions `xFree()` and `icuFunctionError()` have translation-unit scope and cannot be referenced from another object file.

This approach is deliberately coupled to SQLite’s internal ICU implementation, but for a controlled custom amalgamation it is the smallest implementation and follows SQLite ICU’s own allocation, error-handling, and encoding conventions. The current ICU extension uses this exact UTF-16 strategy for both UTF-8- and UTF-16-registered `lower()`/`upper()` variants. [SQLite `ext/icu/icu.c`](https://github.com/sqlite/sqlite/blob/master/ext/icu/icu.c)

---
---

## 📗 Unicode Normalization

> [!NOTE] Prompt
> 
> Python also provides for Unicode normalization variants:  
> NFKC, NFKD, NFC, NFD.  
> Are those available from icu4c?  

Yes. ICU4C provides all four standard Unicode normalization forms through its `Normalizer2` C API:

| Python                 | ICU4C normalizer           |
| ---------------------- | -------------------------- |
| `normalize("NFC", s)`  | `unorm2_getNFCInstance()`  |
| `normalize("NFD", s)`  | `unorm2_getNFDInstance()`  |
| `normalize("NFKC", s)` | `unorm2_getNFKCInstance()` |
| `normalize("NFKD", s)` | `unorm2_getNFKDInstance()` |

Then apply the selected normalizer with:

```c
int32_t unorm2_normalize(
    const UNormalizer2 *normalizer,
    const UChar *src,
    int32_t srcLength,
    UChar *dest,
    int32_t destCapacity,
    UErrorCode *status
);
```

Example:

```c
#include <unicode/unorm2.h>

UErrorCode status = U_ZERO_ERROR;

const UNormalizer2 *nfc =
    unorm2_getNFCInstance(&status);

int32_t resultLength = unorm2_normalize(
    nfc,
    input,
    inputLength,
    output,
    outputCapacity,
    &status
);
```

The returned standard instances are immutable ICU-owned singletons:

* Reuse them freely.
* They are thread-safe.
* Do not close or free them.

For your amalgamation extension, use UTF-16 again. `unorm2_normalize()` accepts `UChar` strings, so the allocation/retry logic can closely follow ICU’s existing `icuCaseFunc16()`:

```text
sqlite3_value_text16()
        ↓
unorm2_normalize()
        ↓
sqlite3_result_text16()
```

You only need to add:

```c
#include <unicode/unorm2.h>
```

because current `ext/icu/icu.c` does not include that header.

ICU additionally provides:

```c
unorm2_getNFKCCasefoldInstance()
```

That performs Unicode `NFKC_Casefold`, approximately the standardized combined operation:

```python
normalize("NFKC", text.casefold())
```

with additional Unicode-defined processing. It is not identical to plain `casefold()` or plain NFKC.

As with case folding, Python and ICU implement the same Unicode standards independently. Exact results can differ if Python and ICU use different Unicode database versions.

[ICU4C `Normalizer2` C API](https://unicode-org.github.io/icu-docs/apidoc/released/icu4c/unorm2_8h.html)

---
---

## 📗 Unicode Normalization Stripped

> [!NOTE] Prompt
> 
> I need one more normalization variant: NFKD + drop every char combining char:
>
> ```
> def normalize_form(s: str) -> str:
>    return "".join(c for c in normalize("NFKD", s.casefold()) if not combining(c))
> ```
> 
> separate from casefold

That Python function can be reproduced exactly with ICU4C as this pipeline:

```text
full case fold
→ NFKD normalization
→ remove code points whose canonical combining class is nonzero
```

Use:

* `u_strFoldCase(..., U_FOLD_CASE_DEFAULT, ...)`
* `unorm2_getNFKDInstance()`
* `unorm2_normalize()`
* `u_getCombiningClass(codePoint)`

Add these headers:

```c
#include <unicode/uchar.h>
#include <unicode/unorm2.h>
#include <unicode/utf16.h>
```

The filtering step must iterate over Unicode code points, not individual UTF-16 `UChar` units:

```c
static int32_t removeCombiningChars(UChar *z, int32_t n){
  int32_t i = 0;
  int32_t j = 0;

  while( i<n ){
    int32_t start = i;
    UChar32 c;

    U16_NEXT(z, i, n, c);

    if( u_getCombiningClass(c)==0 ){
      while( start<i ){
        z[j++] = z[start++];
      }
    }
  }

  return j;
}
```

After case-folding and NFKD normalization:

```c
nNormalized = removeCombiningChars(zNormalized, nNormalized);

sqlite3_result_text16(
    pCtx,
    zNormalized,
    2LL * nNormalized,
    xFree
);
```

One important precision: your Python code does **not literally remove every Unicode combining-mark character**. It removes every character for which:

```python
unicodedata.combining(c) != 0
```

The ICU equivalent is exactly:

```c
u_getCombiningClass(c) != 0
```

Some Unicode marks have canonical combining class zero. Those remain in both your Python implementation and the ICU equivalent. Filtering by Unicode general category `Mn`, `Mc`, or `Me` would be a different, broader operation.

For exact equivalence, preserve this order:

```c
u_strFoldCase(...)
unorm2_normalize(nfkd, ...)
removeCombiningChars(...)
```

Do not replace it with `unorm2_getNFKCCasefoldInstance()`: that performs a different standardized transformation and also removes certain default-ignorable characters.

A sensible SQL name would therefore be something explicit such as:

```sql
normalize_form(text)
```

while exposing the primitive operations separately:

```sql
casefold(text)
normalize(text, 'NFC')
normalize(text, 'NFD')
normalize(text, 'NFKC')
normalize(text, 'NFKD')
normalize_form(text)
```


Understood. Case folding remains an entirely separate SQL function.

The additional normalization variant performs only:

```python
"".join(
    c
    for c in normalize("NFKD", s)
    if not combining(c)
)
```

ICU pipeline:

```text
input
→ unorm2_getNFKDInstance()
→ unorm2_normalize()
→ remove code points where u_getCombiningClass(c) != 0
```

No call to `u_strFoldCase()` inside this normalization variant.

Then the original Python behavior is composed explicitly in SQL:

```sql
SELECT normalize_form(casefold(?));
```

Or, if exposed through one normalization function:

```sql
SELECT normalize(casefold(?), 'NFKD_STRIP');
```

This cleanly provides:

* `casefold(text)` — full Unicode case folding only
* `normalize(text, 'NFC')`
* `normalize(text, 'NFD')`
* `normalize(text, 'NFKC')`
* `normalize(text, 'NFKD')`
* `normalize(text, 'NFKD_STRIP')` — NFKD plus removal of characters with nonzero canonical combining class

Again, `NFKD_STRIP` is your custom name; it is not a standard Unicode normalization form.

---
---

## 📗 Unicode Normalization Lesson

> [!NOTE] Prompt
> 
> Tell me about
> 
> ```
> Some Unicode marks have canonical combining class zero. Those remain in both your Python implementation and the ICU equivalent. Filtering by Unicode general category Mn, Mc, or Me would be a different, broader operation.
> ```
> 
> and
> 
> ```
> unorm2_getNFKCCasefoldInstance()
> ```

These are two fundamentally different ideas:

1. `NFKD` plus removal of characters with nonzero canonical combining class is a custom “strip many combining distinctions” transformation.
2. `NFKC_Casefold` is a standardized Unicode transformation for compatibility-aware, case-insensitive identifier matching.

### 1. Combining marks versus combining class

Each Unicode code point has multiple independent properties.

#### General category

The general categories beginning with `M` classify characters as marks:

| Category | Meaning                |
| -------- | ---------------------- |
| `Mn`     | Nonspacing mark        |
| `Mc`     | Spacing combining mark |
| `Me`     | Enclosing mark         |

ICU obtains this through:

```c
u_charType(c)
```

#### Canonical combining class

Canonical Combining Class, or CCC, is a numeric property used primarily by Unicode normalization to determine the canonical ordering of characters.

ICU:

```c
uint8_t ccc = u_getCombiningClass(c);
```

Python:

```python
ccc = unicodedata.combining(c)
```

The important point is:

```text
General_Category = Mark
```

does not imply:

```text
Canonical_Combining_Class != 0
```

CCC zero means `Not_Reordered`. It does not mean “this character is not a combining mark.”

#### Examples

| Character                         | Category | CCC | Removed by your Python code? |
| --------------------------------- | -------: | --: | ---------------------------: |
| U+0301 COMBINING ACUTE ACCENT     |     `Mn` | 230 |                          Yes |
| U+0308 COMBINING DIAERESIS        |     `Mn` | 230 |                          Yes |
| U+093F DEVANAGARI VOWEL SIGN I    |     `Mc` |   0 |                           No |
| U+0902 DEVANAGARI SIGN ANUSVARA   |     `Mn` |   0 |                           No |
| U+20DD COMBINING ENCLOSING CIRCLE |     `Me` |   0 |                           No |
| U+FE0F VARIATION SELECTOR-16      |     `Mn` |   0 |                           No |

Therefore:

```python
if not combining(c)
```

means:

```text
retain characters whose CCC is zero
```

It does not mean:

```text
retain characters that are not Unicode marks
```

The exact ICU equivalent remains:

```c
if( u_getCombiningClass(c)==0 ){
    /* retain c */
}
```

#### What category-based filtering would do

A broader mark filter would be:

```c
int8_t category = u_charType(c);

if(
    category!=U_NON_SPACING_MARK &&
    category!=U_COMBINING_SPACING_MARK &&
    category!=U_ENCLOSING_MARK
){
    /* retain c */
}
```

That removes all `Mn`, `Mc`, and `Me` characters, including marks with CCC zero.

This is often too destructive for general multilingual text. For example, U+093F DEVANAGARI VOWEL SIGN I is an essential vowel sign, not an optional accent. Removing all marks can materially change or destroy words in Brahmic and other writing systems.

Your CCC-based filter is narrower and is usually more appropriate for the familiar “decompose and remove accents” recipe.

#### Consequences for Russian

After NFKD:

```text
Й → И + COMBINING BREVE
Ё → Е + COMBINING DIAERESIS
```

After removing nonzero-CCC characters:

```text
Й → И
й → и
Ё → Е
ё → е
```

Therefore, your custom normalization intentionally makes:

```text
Й equivalent to И
Ё equivalent to Е
```

It will also remove ordinary Latin accents:

```text
é → e
Å → A
ñ → n
```

That is substantially more aggressive than Unicode normalization alone.

There is another edge case: some standalone compatibility characters decompose to a space plus a combining mark. For example:

```text
U+00AF MACRON → SPACE + U+0304 COMBINING MACRON
```

After stripping nonzero CCC, this becomes a space. So the transformation is useful as a search key, but it should not be treated as clean display text or a reversible normalization.

Unicode describes CCC zero as `Not_Reordered` and explicitly notes that it includes spacing and enclosing marks plus many vowel and consonant signs. [Unicode canonical combining classes](https://www.unicode.org/reports/tr44/#Canonical_Combining_Class_Values)

### 2. `unorm2_getNFKCCasefoldInstance()`

This ICU call returns the standard normalizer for Unicode’s `toNFKC_Casefold()` operation:

```c
UErrorCode status = U_ZERO_ERROR;

const UNormalizer2 *nfkcCf =
    unorm2_getNFKCCasefoldInstance(&status);
```

You then apply it using the normal `Normalizer2` function:

```c
int32_t nResult = unorm2_normalize(
    nfkcCf,
    src,
    srcLength,
    dest,
    destCapacity,
    &status
);
```

The returned object is an immutable ICU-owned singleton. It should be reused and must not be freed.

#### What it combines

`NFKC_Casefold` combines:

* Full Unicode case-insensitive mapping
* Compatibility normalization
* Canonical recomposition to NFC
* Removal of certain default-ignorable characters according to the Unicode-defined `NFKC_CF` mapping

Examples include transformations conceptually like:

```text
Straße → strasse
ＡＢＣ → abc
① → 1
ﬃ → ffi
ª → a
SOFT HYPHEN → empty string
```

The last behavior is an important difference. Unicode explicitly defines U+00AD SOFT HYPHEN as mapping to an empty string under `NFKC_Casefold`. [Unicode `NFKC_Casefold` property](https://www.unicode.org/reports/tr44/#NFKC_Casefold)

#### Intended purpose

It is designed for caseless matching of strings interpreted as identifiers, keys, usernames, symbols, or similar machine-oriented values.

It is not designed to preserve every visual or semantic distinction in arbitrary prose.

#### What it does not do

It does not generally strip accents or diacritics:

```text
É → é
Ё → ё
Й → й
```

Thus:

```text
NFKC_Casefold("Ё") != NFKC_Casefold("Е")
NFKC_Casefold("Й") != NFKC_Casefold("И")
```

It removes case distinctions, but retains these letter distinctions.

### Practical comparison

| Transformation                   | Case-insensitive | Compatibility folding | Strips ordinary accents |   Removes default ignorables | Output composition |
| -------------------------------- | ---------------: | --------------------: | ----------------------: | ---------------------------: | ------------------ |
| `casefold()`                     |              Yes |                    No |                      No |                 Generally no | Unspecified        |
| `NFKD_STRIP`                     |               No |                   Yes |       Yes, when CCC ≠ 0 |                           No | Decomposed         |
| `NFKD_STRIP(casefold(s))`        |              Yes |                   Yes |       Yes, when CCC ≠ 0 |                           No | Decomposed         |
| `NFKC_Casefold`                  |              Yes |                   Yes |                      No | Yes, as defined by `NFKC_CF` | NFC                |
| Remove all `Mn/Mc/Me` after NFKD |               No |                   Yes |            Very broadly |           Some, incidentally | Decomposed         |

For your SQLite extension, these should remain distinct operations:

```sql
casefold(text)
normalize(text, 'NFKD_STRIP')
normalize(text, 'NFKC_CASEFOLD')
```

Then callers can choose explicitly:

```sql
-- Case-insensitive, but preserve Ё/Е and Й/И distinctions:
normalize(value, 'NFKC_CASEFOLD')

-- Case-insensitive and intentionally collapse Ё→Е and Й→И:
normalize(casefold(value), 'NFKD_STRIP')
```

That separation avoids silently giving a general-purpose normalization function unexpectedly destructive semantics. [ICU4C Normalizer2 API](https://unicode-org.github.io/icu-docs/apidoc/released/icu4c/unorm2_8h.html)

---
---

## 📗 Unicode Normalization API

> [!NOTE] Prompt
> 
> I guess I want
> 
> - casefold(text)
> - normalize(text, 'NFC') 
> - normalize(text, 'NFD') 
> - normalize(text, 'NFKC') 
> - normalize(text, 'NFKD')
> - normalize(text, 'NFKD\_CF\_STRIP'):
> 
>      \<NFKD> + if( u\_getCombiningClass(c)==0 ){} + NFKC\_CF case folding and removal of ignorables (I changed my mind, bake in case folding here)
> 
> - normalize(text, 'NFKC\_CF') - unorm2\_getNFKCCasefoldInstance  

That API set is coherent, with one change to the order of `NFKD_CF_STRIP`.

Use:

```text
NFKC_CF → NFKD → remove characters with CCC != 0
```

not:

```text
NFKD → strip → NFKC_CF
```

The recommended definition is therefore:

```python
def nfkd_cf_strip(s):
    s = NFKC_Casefold(s)          # case-fold + compatibility + ignorables
    s = normalize("NFKD", s)      # expose accents as separate characters
    return "".join(
        c for c in s
        if combining(c) == 0
    )
```

This ensures any combining characters introduced by case folding or normalization are also stripped, and the final result remains in the decomposed/stripped form implied by the name.

Your final SQL API would be:

```sql
casefold(text)

normalize(text, 'NFC')
normalize(text, 'NFD')
normalize(text, 'NFKC')
normalize(text, 'NFKD')

normalize(text, 'NFKC_CF')
normalize(text, 'NFKD_CF_STRIP')
```

The ICU mappings are:

| SQL mode        | ICU processing                                                       |
| --------------- | -------------------------------------------------------------------- |
| `NFC`           | `unorm2_getNFCInstance()`                                            |
| `NFD`           | `unorm2_getNFDInstance()`                                            |
| `NFKC`          | `unorm2_getNFKCInstance()`                                           |
| `NFKD`          | `unorm2_getNFKDInstance()`                                           |
| `NFKC_CF`       | `unorm2_getNFKCCasefoldInstance()`                                   |
| `NFKD_CF_STRIP` | `NFKC_CF`, then `NFKD`, then retain only `u_getCombiningClass(c)==0` |

The resulting semantics are:

```text
casefold:
    case-insensitive only
    Straße → strasse
    Ё → ё
    Й → й

NFKC_CF:
    case-insensitive
    compatibility-normalized
    default ignorables removed
    canonically composed
    Ё → ё
    Й → й

NFKD_CF_STRIP:
    case-insensitive
    compatibility-normalized
    default ignorables removed
    nonzero-CCC marks removed
    Ё → е
    Й → и
    É → e
```

Implementation sketch:

```c
static const UNormalizer2 *icuGetNormalizer(
  const char *zForm,
  UErrorCode *pStatus
){
  if( sqlite3_stricmp(zForm, "NFC")==0 ){
    return unorm2_getNFCInstance(pStatus);
  }
  if( sqlite3_stricmp(zForm, "NFD")==0 ){
    return unorm2_getNFDInstance(pStatus);
  }
  if( sqlite3_stricmp(zForm, "NFKC")==0 ){
    return unorm2_getNFKCInstance(pStatus);
  }
  if( sqlite3_stricmp(zForm, "NFKD")==0 ){
    return unorm2_getNFKDInstance(pStatus);
  }
  if( sqlite3_stricmp(zForm, "NFKC_CF")==0 ){
    return unorm2_getNFKCCasefoldInstance(pStatus);
  }
  return 0;
}
```

For `NFKD_CF_STRIP`, implement it as a special composite mode:

```c
nfkcCf = unorm2_getNFKCCasefoldInstance(&status);
nfkd   = unorm2_getNFKDInstance(&status);

/* 1. NFKC_Casefold */
unorm2_normalize(nfkcCf, ...);

/* 2. NFKD */
unorm2_normalize(nfkd, ...);

/* 3. Remove characters with nonzero CCC */
nResult = removeNonzeroCombiningClass(zResult, nResult);
```

The standard normalizer instances are immutable singletons, so acquiring them on each call is permitted and cheap. Alternatively, you can resolve and cache them during extension initialization.

Register both functions as:

```c
SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS
```

One operational warning: if these functions are used in expression indexes, stored generated columns, or partial-index expressions, rebuild the affected data/indexes after upgrading ICU to a version with different Unicode case-folding data. The function is deterministic for a particular ICU build, but case-folding mappings for newly assigned Unicode characters can evolve between versions.
