---
url: https://chatgpt.com/c/6a9cf257-d748-83eb-93ad-1f1a3999eb9a
---

## 📗 ICU Collations

> [!NOTE] Prompt
> 
> SQLite ext/icu/icu.c provides this
> 
> ```
> /*
> ** Implementation of the scalar function icu_load_collation().
> **
> ** This scalar function is used to add ICU collation based collation 
> ** types to an SQLite database connection. It is intended to be called
> ** as follows:
> **
> **     SELECT icu_load_collation(<locale>, <collation-name>);
> **
> ** Where <locale> is a string containing an ICU locale identifier (i.e.
> ** "en_AU", "tr_TR" etc.) and <collation-name> is the name of the
> ** collation sequence to create.
> */
> static void icuLoadCollation(
>   sqlite3_context *p, 
>   int nArg, 
>   sqlite3_value **apArg
> ){
>   sqlite3 *db = (sqlite3 *)sqlite3_user_data(p);
>   UErrorCode status = U_ZERO_ERROR;
>   const char *zLocale;      /* Locale identifier - (eg. "jp_JP") */
>   const char *zName;        /* SQL Collation sequence name (eg. "japanese") */
>   UCollator *pUCollator;    /* ICU library collation object */
>   int rc;                   /* Return code from sqlite3_create_collation_x() */
> 
>   assert(nArg==2 || nArg==3);
>   (void)nArg; /* Unused parameter */
>   zLocale = (const char *)sqlite3_value_text(apArg[0]);
>   zName = (const char *)sqlite3_value_text(apArg[1]);
> 
>   if( !zLocale || !zName ){
>     return;
>   }
> 
>   pUCollator = ucol_open(zLocale, &status);
>   if( !U_SUCCESS(status) ){
>     icuFunctionError(p, "ucol_open", status);
>     return;
>   }
>   assert(p);
>   if(nArg==3){
>     const char *zOption = (const char*)sqlite3_value_text(apArg[2]);
>     static const struct {
>        const char *zName;
>        UColAttributeValue val;
>     } aStrength[] = {
>       {  "PRIMARY",      UCOL_PRIMARY           },
>       {  "SECONDARY",    UCOL_SECONDARY         },
>       {  "TERTIARY",     UCOL_TERTIARY          },
>       {  "DEFAULT",      UCOL_DEFAULT_STRENGTH  },
>       {  "QUARTERNARY",  UCOL_QUATERNARY        },
>       {  "IDENTICAL",    UCOL_IDENTICAL         },
>     };
>     unsigned int i;
>     for(i=0; i<sizeof(aStrength)/sizeof(aStrength[0]); i++){
>       if( sqlite3_stricmp(zOption,aStrength[i].zName)==0 ){
>         ucol_setStrength(pUCollator, aStrength[i].val);
>         break;
>       }
>     }
>     if( i>=sizeof(aStrength)/sizeof(aStrength[0]) ){
>       sqlite3_str *pStr = sqlite3_str_new(sqlite3_context_db_handle(p));
>       sqlite3_str_appendf(pStr,
>          "unknown collation strength \"%s\" - should be one of:",
>          zOption);
>       for(i=0; i<sizeof(aStrength)/sizeof(aStrength[0]); i++){
>          sqlite3_str_appendf(pStr, " %s", aStrength[i].zName);
>       }
>       sqlite3_result_error(p, sqlite3_str_value(pStr), -1);
>       sqlite3_free(sqlite3_str_finish(pStr));
>       ucol_close(pUCollator);
>       return;
>     }
>   }
>   rc = sqlite3_create_collation_v2(db, zName, SQLITE_UTF16, (void *)pUCollator, 
>       icuCollationColl, icuCollationDel
>   );
>   if( rc!=SQLITE_OK ){
>     ucol_close(pUCollator);
>     sqlite3_result_error(p, "Error registering collation function", -1);
>   }
> }
> ```
> 
> Tell me about it. What does it provide?

`icu_load_collation()` dynamically creates an ICU-backed SQLite collation for the current database connection.

For example:

```sql
SELECT icu_load_collation('en_US', 'english');

SELECT *
FROM entries
ORDER BY word COLLATE english;
```

It can also accept an optional ICU collation strength:

```sql
SELECT icu_load_collation('en_US', 'english_nocase', 'SECONDARY');
```

### What it provides

The function:

1. Opens an ICU `UCollator` for the requested locale:

   ```c
   ucol_open(zLocale, &status)
   ```

2. Optionally configures its comparison strength with:

   ```c
   ucol_setStrength(pUCollator, ...)
   ```

3. Registers that collator with the current SQLite connection under the supplied SQL name:

   ```c
   sqlite3_create_collation_v2(
       db,
       zName,
       SQLITE_UTF16,
       pUCollator,
       icuCollationColl,
       icuCollationDel
   );
   ```

After registration, SQLite can use ICU linguistic comparison wherever a collation is accepted:

```sql
SELECT word
FROM words
ORDER BY word COLLATE english;
```

```sql
SELECT *
FROM words
WHERE word = 'example' COLLATE english;
```

```sql
CREATE INDEX words_english
ON words(word COLLATE english);
```

### Arguments

Although the introductory comment documents only two arguments, this implementation accepts either two or three:

```sql
icu_load_collation(locale, collation_name)
icu_load_collation(locale, collation_name, strength)
```

| Argument         | Meaning                                                              |
| ---------------- | -------------------------------------------------------------------- |
| `locale`         | ICU locale identifier, such as `en_US`, `de_DE`, `tr_TR`, or `ru_RU` |
| `collation_name` | The name by which SQLite will reference the collation                |
| `strength`       | Optional level of distinctions considered during comparison          |

The accepted strength names are case-insensitive:

* `PRIMARY`
* `SECONDARY`
* `TERTIARY`
* `DEFAULT`
* `QUARTERNARY`
* `IDENTICAL`

### Strength semantics

The exact behavior is locale-dependent, but approximately:

| Strength      | Typically distinguishes                                                                 |
| ------------- | --------------------------------------------------------------------------------------- |
| `PRIMARY`     | Base letters; usually ignores case and accents                                          |
| `SECONDARY`   | Base letters and accents; usually ignores case                                          |
| `TERTIARY`    | Base letters, accents, and case                                                         |
| `QUARTERNARY` | Adds distinctions involving punctuation or variable-weighted characters                 |
| `IDENTICAL`   | Uses the strongest available distinction, including a final code-point-level comparison |
| `DEFAULT`     | ICU’s default strength for that locale                                                  |

For example, depending on the locale:

```text
e
E
é
É
```

may compare as equivalent at primary strength, while secondary or tertiary strength progressively distinguishes them.

This is considerably more capable than SQLite’s built-in `NOCASE`, which is essentially ASCII-oriented.

### Scope and persistence

The registration belongs to the current `sqlite3` connection. It is not permanently stored in the database file.

Therefore, every connection that needs the collation must execute something equivalent to:

```sql
SELECT icu_load_collation('ru_RU', 'russian');
```

before preparing statements or accessing schema objects that require it.

This is especially important if the collation is referenced by:

* a column declaration;
* an index;
* a view;
* a trigger;
* a prepared SQL statement.

An index created with the collation remains in the database, but another connection must register a compatible collation with the same name before SQLite can use that index. Registering the same name with different locale or strength semantics can make the index logically inconsistent; it should then be rebuilt with `REINDEX`.

### Resource ownership

The allocated `UCollator` becomes the collation’s application-data object:

```c
(void *)pUCollator
```

`icuCollationColl` uses it to compare UTF-16 strings. `icuCollationDel` closes it when SQLite replaces or removes the collation or closes the connection.

If registration fails, the function closes the collator itself:

```c
ucol_close(pUCollator);
```

This explicit cleanup is important because `sqlite3_create_collation_v2()` does not invoke the destructor when registration fails.

### SQL return value and errors

The function exists for its side effect. On success, it does not call a `sqlite3_result_*()` function, so its SQL result is `NULL`:

```sql
SELECT icu_load_collation('en_US', 'english');
-- NULL, but the collation is now registered
```

If ICU cannot open the locale, it reports an ICU error. If the strength name is unknown, it reports an SQLite error listing the accepted values. If either of the first two arguments is SQL `NULL`, it silently returns SQL `NULL` without registering anything.

One weakness in this exact implementation is that a SQL `NULL` third argument is not explicitly checked before `sqlite3_stricmp()` receives it. The safe public contract should therefore require the optional strength to be non-NULL when supplied.

### What it does not expose

The function only configures locale and strength. ICU supports many additional collation attributes that this SQL interface does not expose, including:

* numeric collation;
* case-first ordering;
* case-level comparison;
* alternate handling of punctuation and spaces;
* normalization mode;
* French secondary ordering;
* custom rule-based collations.

Those would require extending the SQL function or adding another configuration interface.

---
---

## 📗 Locales for `casefold`

> [!NOTE] Prompt
> 
> What locale do I load if I want the effect of unicode .casefold?

No ICU locale exactly reproduces Unicode `.casefold()`. Case folding is a Unicode transformation, whereas ICU collation is locale-sensitive comparison.

The closest locale-independent collation is the ICU root locale with `SECONDARY` strength:

```sql
SELECT icu_load_collation('und', 'unicode_nocase', 'SECONDARY');
```

Then:

```sql
SELECT *
FROM words
WHERE word = ? COLLATE unicode_nocase;
```

`und` means “undetermined language” and effectively selects root/default Unicode collation behavior. `SECONDARY` generally ignores case while preserving accent distinctions.

However, this is only an approximation. ICU collation can introduce equivalences and ordering rules that differ from comparing:

```python
a.casefold() == b.casefold()
```

For exact Unicode default case-folding semantics, implement a function or collation that transforms both strings with ICU’s:

```c
u_strFoldCase(..., U_FOLD_CASE_DEFAULT, ...)
```

and then compares the folded results directly. Locale should not participate. For Turkic-specific folding, use `U_FOLD_CASE_EXCLUDE_SPECIAL_I`, but that would not match ordinary Python `str.casefold()`.

So:

* General case-insensitive linguistic sorting: `und` + `SECONDARY`.
* Exact `.casefold()`-style equality: custom `u_strFoldCase()` function/collation, with no locale.

---
---

## 📗 Unicode CI Locales

> [!NOTE] Prompt
> 
> I need case insensitive Unicode regardless of language.

Use the ICU root locale with `SECONDARY` strength:

```sql
SELECT icu_load_collation('root', 'UNICODE_NOCASE', 'SECONDARY');
```

Then:

```sql
SELECT *
FROM words
ORDER BY word COLLATE UNICODE_NOCASE;
```

or:

```sql
SELECT *
FROM words
WHERE word = ? COLLATE UNICODE_NOCASE;
```

This provides language-independent Unicode case-insensitive comparison:

* Case differences are ignored.
* Accents remain significant.
* No English, Turkish, Russian, or other language-specific tailoring is applied.

Do **not** use `PRIMARY`, because it generally ignores accents as well. `SECONDARY` is the appropriate ICU strength for Unicode case-insensitive but accent-sensitive collation.

---
---

## 📗 Unicode root Locale

> [!NOTE] Prompt
> 
> Under icu_load_collation('root', 'UNICODE_NOCASE', 'SECONDARY'), does "АБВГДЙЬЁ"="абвгдйьё" hold?

Yes. With the collation explicitly applied:

```sql
SELECT 'АБВГДЙЬЁ' = 'абвгдйьё' COLLATE UNICODE_NOCASE;
```

returns:

```text
1
```

At ICU `SECONDARY` strength, Cyrillic case differences are ignored, including `Й/й` and `Ё/ё`. Accents and distinct letters remain significant—for example, `Е` and `Ё` do not compare equal.

---
---

## 📗 Accented

> [!NOTE] Prompt
> 
> And with PRIMARY  "ЙЁ"="ие"?

Yes. With root `PRIMARY` strength:

```sql
SELECT icu_load_collation('root', 'UNICODE_PRIMARY', 'PRIMARY');

SELECT 'ЙЁ' = 'ие' COLLATE UNICODE_PRIMARY;
```

returns `1`.

That is because:

- `Й` is effectively `И` + breve.
- `Ё` is effectively `Е` + diaeresis.
- `PRIMARY` ignores both case and diacritical distinctions.

These are Cyrillic `и` and `е`; Latin `ie` would not compare equal.

---
---

## 📗 Collation Registration with SQLite

> [!NOTE] Prompt
> 
> Can I register these collations via icu_load_collation during db init via an auto extension to make collations available in the engine context?

Yes—but SQLite collations are always registered per `sqlite3` connection. There is no global collation registry shared by all connections.

Register an auto-extension entry point before opening connections:

```c
sqlite3_auto_extension((void(*)(void))myIcuCollationsInit);
```

SQLite then invokes `myIcuCollationsInit()` for every subsequently opened connection. That initializer should register the collations directly with `sqlite3_create_collation_v2()`.

Do not merely auto-register `sqlite3_icu_init()`:

```c
sqlite3_auto_extension((void(*)(void))sqlite3_icu_init);
```

That makes the SQL function `icu_load_collation()` available on every connection, but it does not invoke the function and therefore does not create your named collations.

The auto-extension should effectively perform:

```c
static int myIcuCollationsInit(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
){
  int rc;

  rc = registerIcuCollation(
      db, "root", "UNICODE_NOCASE", UCOL_SECONDARY, pzErrMsg
  );
  if( rc!=SQLITE_OK ){
    return rc;
  }

  rc = registerIcuCollation(
      db, "root", "UNICODE_PRIMARY", UCOL_PRIMARY, pzErrMsg
  );
  return rc;
}
```

The helper would perform the same operations as `icuLoadCollation()`:

```c
pCollator = ucol_open("root", &status);
ucol_setStrength(pCollator, UCOL_SECONDARY);

sqlite3_create_collation_v2(
    db,
    "UNICODE_NOCASE",
    SQLITE_UTF16,
    pCollator,
    icuCollationColl,
    icuCollationDel
);
```

Since `icuLoadCollation()`, `icuCollationColl`, and `icuCollationDel` are `static` inside `icu.c`, code in a separate translation unit cannot call them. The clean options are:

1. Add the auto-initialization logic inside `icu.c`.
2. Refactor the registration operation into a non-static helper.
3. Implement equivalent ICU comparison and destruction callbacks in the new auto-extension module.

Executing:

```sql
SELECT icu_load_collation(...);
```

from the auto-extension initializer is unnecessary and less direct. Register the collators through the C API during connection initialization.

The resulting behavior is:

```text
sqlite3_auto_extension(...)
             ↓
sqlite3_open(...) for each connection
             ↓
myIcuCollationsInit(db)
             ↓
UNICODE_NOCASE and UNICODE_PRIMARY available immediately
```

This covers connections opened after the `sqlite3_auto_extension()` call. Already-open connections must be initialized separately. Also ensure the auto-extension and application use the same SQLite library instance; this matters when multiple SQLite DLLs are loaded into one process.

---
---

## 📗 Collation Auto Registration with SQLite

> [!NOTE] Prompt
> 
> SQLite provides NOCASE, BINARY and something else. These are automatically available on any connection without any loading. I want to use an auto extension, like ICU itself, to make UTF_CI available in the same manner by default

Yes. You want `UTF_CI` registered automatically during every connection’s initialization, so it behaves operationally like SQLite’s built-in `BINARY`, `NOCASE`, and `RTRIM` collations:

```sql
CREATE TABLE words(
    word TEXT COLLATE UTF_CI
);
```

No `icu_load_collation()` call should be required.

Your extension initializer must directly register `UTF_CI`:

```c
int sqlite3_utfci_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
){
  UErrorCode status = U_ZERO_ERROR;
  UCollator *pCollator;
  int rc;

  pCollator = ucol_open("root", &status);
  if( U_FAILURE(status) ){
    return SQLITE_ERROR;
  }

  ucol_setStrength(pCollator, UCOL_SECONDARY);

  rc = sqlite3_create_collation_v2(
      db,
      "UTF_CI",
      SQLITE_UTF16,
      pCollator,
      icuCollationColl,
      icuCollationDel
  );

  if( rc!=SQLITE_OK ){
    ucol_close(pCollator);
  }

  return rc;
}
```

Then make that initializer an automatic extension in your custom SQLite build.

For current SQLite sources, the most direct build-time integration is:

```c
#define SQLITE_EXTRA_AUTOEXT sqlite3_utfci_init
```

or the equivalent compiler option:

```text
-DSQLITE_EXTRA_AUTOEXT=sqlite3_utfci_init
```

`SQLITE_EXTRA_AUTOEXT` places the initializer in SQLite’s built-in extension initializer list. SQLite invokes that list for each new connection; ICU itself is present there when `SQLITE_ENABLE_ICU` or `SQLITE_ENABLE_ICU_COLLATIONS` is enabled. [SQLite `main.c`](https://github.com/sqlite/sqlite/blob/master/src/main.c)

Alternatively, a one-time process initializer can call:

```c
sqlite3_auto_extension((void(*)(void))sqlite3_utfci_init);
```

Every connection opened afterward will invoke it automatically. SQLite guarantees that an automatic-extension entry point is called no more than once for each opened connection. [SQLite C API](https://sqlite.org/capi3ref.html)

Thus, after the custom SQLite library is initialized:

```sql
SELECT 'Привет' = 'ПРИВЕТ' COLLATE UTF_CI;
-- 1
```

The only unavoidable constraint is that the extension must be linked into—or bootstrapped by—the SQLite library used by the application. A standalone extension DLL cannot become universally available to arbitrary processes unless something first loads or registers it. In a custom SQLite build, `SQLITE_EXTRA_AUTOEXT` gives you precisely the default availability you want.
