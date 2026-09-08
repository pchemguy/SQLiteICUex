---
url: https://chatgpt.com/c/6a9fba3e-fb3c-83eb-9509-d15d0bed2caf
---

## 📗 SQLite Forum Post

> [!NOTE] Prompt
>  
> I want a concise post for SQLite Forum. It should probably highlight key features, briefly state scope/functionality, complementarity to icu.c, motivation (such as unicode case folding without additional init or language concerns, optional cyrillic е/ё, etc.), dual extension building/loading options. Use Markdown table for collations, two functions, and normalization modes. Perhaps proposition that this functionality could be integrated into icu.c.
> 
> Important statement that that source repo is released to public domain (The Unlicense license), repo ref pchemguy/SQLiteICUex, AI disclosure. 
> 
> Balance size/competence, tailor towards SQLite Forum.
> 
> What else? - Fill the blanks.

### SQLiteICUex: predefined ICU collations, Unicode case folding and normalization

I have published [SQLiteICUex](https://github.com/pchemguy/SQLiteICUex), a small C extension providing a focused Unicode SQL interface backed by ICU.

It complements `ext/icu/icu.c` rather than reproducing its functionality. The standard ICU extension already provides locale-aware `upper()`/`lower()`, Unicode-aware `LIKE`, regular expressions, and dynamically registered locale collations. SQLiteICUex adds predefined automatically available collations, locale-independent full case folding, Unicode normalization, and normalized search-key generation.

### Collations

| Collation   | Semantics                                                                       | Intended use                                                                                     |
| ----------- | ------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------ |
| `UTF_CI`    | ICU root collation at secondary strength: case-insensitive but accent-sensitive | General language-neutral caseless comparison; Cyrillic `е/ё` and `и/й` remain distinct           |
| `UTF_CI_AI` | Compares `NFKD_CF_STRIP` keys by Unicode code point                             | Case-, compatibility- and diacritic-insensitive matching; Cyrillic `е/ё` and `и/й` compare equal |

`UTF_CI_AI` is deliberately a normalized-key collation, not an ICU primary-strength linguistic collation. Its ordering is deterministic key ordering rather than locale-tailored dictionary ordering.

Both collations are registered when a connection is initialized; no `icu_load_collation()` call, locale name or application setup SQL is required.

### Functions

| Function                    | Description                                                                                                    |
| --------------------------- | -------------------------------------------------------------------------------------------------------------- |
| `str_casefold(text)`        | Locale-independent full Unicode case folding using ICU’s default fold rules; it does not perform normalization |
| `str_normalize(text, mode)` | Applies one of the normalization or normalized-key modes below                                                 |

Full case folding is not equivalent to the overloaded ICU `lower()`. For example, lowercasing preserves German `ß` in `Straße`, whereas case folding produces `strasse`; folding also handles Greek sigma variants and compatibility ligatures for caseless matching. The two operations therefore serve different purposes: `lower()` is a potentially locale-sensitive text transformation, while `str_casefold()` produces locale-independent caseless text.

### Normalization modes

| Mode            | Operation                                                                                          |
| --------------- | -------------------------------------------------------------------------------------------------- |
| `NFC`           | Canonical decomposition followed by canonical composition                                          |
| `NFD`           | Canonical decomposition                                                                            |
| `NFKC`          | Compatibility decomposition followed by canonical composition                                      |
| `NFKD`          | Compatibility decomposition                                                                        |
| `NFKC_CF`       | ICU NFKC case folding, including removal of default-ignorable code points                          |
| `NFKD_CF_STRIP` | NFKD, removal of code points with nonzero canonical combining class, followed by NFKC case folding |

For example, `NFKD_CF_STRIP` provides a convenient language-neutral search key:

```sql
SELECT str_normalize('ЁЙÉ', 'NFKD_CF_STRIP');
-- еиe
```

All functions are registered as deterministic and innocuous, so they can be used in expression indexes, generated columns and other schema contexts with `trusted_schema=OFF`. Inputs use explicit lengths, including embedded U+0000, and supplementary-plane characters are supported.

### Build modes

The same source supports two modes:

1. **Loadable extension:** compile as a shared library and load through `.load` or `sqlite3_load_extension()`.
2. **Built-in auto-extension:** compile with `SQLITE_CORE` and arrange for SQLite to call `sqlite3IcuexInit()` using the amalgamation’s `SQLITE_EXTRA_AUTOEXT` hook.

For the built-in form, `icuex.c` may either be included after the amalgamation or compiled as a separate object and linked statically with `sqlite3.o`. It has no dependency on private symbols from `ext/icu/icu.c`, so the latter remains optional and can be enabled independently.

The repository includes the specification, build examples and an SQL-only pytest suite covering Unicode edge cases, embedded NULs, indexes, constraints, generated columns, reopened connections and both extension modes.

Collation-derived indexes remain dependent on the ICU/Unicode version used by the build. As with other externally defined collations, such indexes should be rebuilt with `REINDEX` after an ICU upgrade that changes comparison behavior.

### Possible integration with `ext/icu/icu.c`

The SQL surface is intentionally small, and some or all of it might also be suitable for integration into SQLite’s existing `ext/icu/icu.c`. In particular, full case folding and normalization appear to be natural complements to its current case-conversion functions. The predefined collations additionally address applications that want stable, immediately available Unicode comparison behavior without connection-specific initialization.

I would be interested in feedback on the API, names, semantics, implementation approach, and whether any subset would be appropriate for `ext/icu/icu.c`.

The source repository is released into the public domain under [The Unlicense](https://github.com/pchemguy/SQLiteICUex/blob/main/LICENSE).

The project was developed with extensive generative-AI assistance, primarily OpenAI ChatGPT, covering design discussion, implementation, testing, technical review and documentation. Further details are provided in the repository’s `AI_DISCLOSURE.md`; responsibility for the published software remains with the maintainer.

---
---

## 📗 Compact SQLite Post

> [!NOTE] Prompt
>  
>  Way too big. Needs to be at least 2-3 times smaller

**ICUex: predefined ICU collations, case folding, and normalization**

I have published [ICUex](https://github.com/pchemguy/SQLiteICUex), a small ICU-backed C extension designed to complement SQLite’s `ext/icu/icu.c`. ICUex adds focused functionality not readily exposed by `icu.c`: predefined collations, locale-independent full case folding, Unicode normalization, and normalized search keys.

It can be built either as a loadable extension or statically with SQLite. Some of this functionality might also be suitable for direct inclusion in `ext/icu/icu.c`.

ICUex registers two predefined collations, requiring no `icu_load_collation()`, locale selection, or connection-level setup SQL:

| Collation   | Behavior                                                                                                          |
| ----------- | ----------------------------------------------------------------------------------------------------------------- |
| `UTF_CI`    | ICU root collation, case-insensitive and accent-sensitive; `е/ё` and `и/й` remain distinct                        |
| `UTF_CI_AI` | Compares `NFKD_CF_STRIP` keys; case-, compatibility-, and diacritic-insensitive, so `е/ё` and `и/й` compare equal |

Two functions provide Unicode case folding and normalization:

| Function                    | Behavior                                       |
| --------------------------- | ---------------------------------------------- |
| `str_casefold(text)`        | Locale-independent full Unicode case folding   |
| `str_normalize(text, mode)` | Unicode normalization or search-key generation |

Unlike `icu.c`’s `lower()` overload, full case folding is intended for caseless matching. For example, `Straße` becomes `strasse`; Greek sigma variants and compatibility ligatures are also folded consistently.

| Mode            | Operation                                                                                   |
| --------------- | ------------------------------------------------------------------------------------------- |
| `NFC`, `NFD`    | Canonical normalization                                                                     |
| `NFKC`, `NFKD`  | Compatibility normalization                                                                 |
| `NFKC_CF`       | NFKC case folding and removal of default-ignorable code points                              |
| `NFKD_CF_STRIP` | NFKD, removal of code points with nonzero canonical combining class, then NFKC case folding |

The source is released into the public domain under [The Unlicense](https://unlicense.org). Development involved extensive generative-AI assistance across implementation, testing, review, and documentation; details are provided in [AI_DISCLOSURE.md](https://github.com/pchemguy/SQLiteICUex/blob/main/AI_DISCLOSURE.md).

---
---

## 📗 Reddit

**ICUex: Unicode collations, case folding, and normalization**

I have published [ICUex](https://github.com/pchemguy/SQLiteICUex), a small ICU-backed public-domain SQLite C extension providing two automatically registered ICU collations, locale-independent full Unicode case folding, and normalization/search-key functions. It can be loaded dynamically or linked statically and is designed to complement the official SQLite ICU extension.

**⚡ Automatically Registered Collations**

- `UTF_CI` — ICU root collation, case-insensitive and accent-sensitive; `е/ё` and `и/й` remain distinct.
- `UTF_CI_AI` — compares `NFKD_CF_STRIP` keys; case- and compatibility-insensitive, with nonzero-CCC combining marks removed; thus, `е/ё` and `и/й` compare equal, `Straße` becomes `strasse`; Greek sigma variants and compatibility ligatures are also folded consistently.

**🧩 Case Folding and Normalization**

- `str_casefold(text)` — locale-independent full Unicode case folding.
- `str_normalize(text, mode)` — Unicode normalization or search-key generation.
    - `NFC`, `NFD` — canonical normalization.
    - `NFKC`, `NFKD` — compatibility normalization.
    - `NFKC_CF` — NFKC case folding and removal of default-ignorable code points.
    - `NFKD_CF_STRIP` — NFKD, removal of nonzero-CCC code points, then NFKC case folding.

**🚀 Feature Highlights**

- automatically registered collations
- locale-independent full case folding
- Unicode normalization and normalized search keys
- loadable extension or a static SQLite build
- released into the public domain under [The Unlicense](https://unlicense.org)
- developed with extensive generative-AI assistance ([disclosure](https://github.com/pchemguy/SQLiteICUex/blob/main/AI_DISCLOSURE.md))
