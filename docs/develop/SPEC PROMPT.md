---
url: https://chatgpt.com/c/6a9cf257-d748-83eb-93ad-1f1a3999eb9a
---

## 📗 SPEC Prompt

So, let's summarize, with focus on a compact yet comprehensive spec for a coding agent. I want an SQLite auto extension `icuex`, which will only be built as part of SQLite amalgamation with ext/icu/icu.c enabled and included in the same amalgamation file before icuex, so that I could take advantage of any static methods implemented by ext/icu/icu.c. icuex will not expose any public C API. It will only expose SQL facing functionality summarized bellow.

Testing will be performed solely via the SQL surface via `pytest` within environment where python uses custom sqlite3 library - no test fixtures will be implemented for testing the C code via C interfaces. Test suite must use standard SQLite function and collation SQL introspection functionality to verify presence of both collations and normalization routines. Then implement a comprehensive testing including edge cases. Tests should be well structured using focused pytest test modules and `conftest` as necessary.

Code must include comprehensive pro docstrings and README.md generation will also be necessary.

I want expose the following functionality:

### Collations

Two collations, which must be automatically available on any db connection by default, like the built-in collations:

- UTF_CI: icu_load_collation('root', 'UNICODE_NOCASE', 'SECONDARY') 
- UTF_CIEX (or maybe UTF_CIACC or something?): icu_load_collation('root', 'UNICODE_PRIMARY', 'PRIMARY')

### Unicode Normalization Routines

`str_casefold(text)` and `str_normalize(text, kind)` SQL functions described below. They shall accept and return SQLITE_TEXT:

`````
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
str_casefold(text)

str_normalize(text, 'NFC')
str_normalize(text, 'NFD')
str_normalize(text, 'NFKC')
str_normalize(text, 'NFKD')

str_normalize(text, 'NFKC_CF')
str_normalize(text, 'NFKD_CF_STRIP')
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
SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS
```
`````
