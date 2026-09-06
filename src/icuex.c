/*
** 2026-09-06
**
** The author disclaims copyright to this source code.  In place of a legal
** notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** ICU-backed collations and Unicode normalization for a custom SQLite
** amalgamation.
**
** This file is deliberately not a loadable SQLite extension.  The amalgamation
** generator must place ext/icu/icu.c before this file in the same translation
** unit and must define SQLITE_CORE, SQLITE_ENABLE_ICU, and
** SQLITE_ENABLE_ICUEX.  That arrangement allows this module to reuse the
** private icuCollationColl(), icuCollationDel(), and icuFunctionError()
** routines from the upstream SQLite ICU extension without modifying icu.c or
** duplicating its collation implementation.
**
** The surrounding build system is responsible for invoking
** sqlite3IcuexInit() from its aggregate built-in-extension initializer.  This
** file neither defines that aggregate initializer nor calls
** sqlite3_auto_extension().
**
** SQL surface:
**
**   UTF_CI       ICU root collation at secondary strength.
**   UTF_CI_AI    ICU root collation at primary strength.
**   str_casefold(text)
**   str_normalize(text, kind)
**
** Both scalar functions accept only TEXT or NULL.  They are registered as
** deterministic and innocuous and preserve embedded U+0000 characters by
** using explicit lengths throughout.
*/

#if !defined(SQLITE_CORE)
# error "icuex.c must be compiled inside the SQLite amalgamation"
#endif

#if !defined(SQLITE_ENABLE_ICU)
# error "icuex.c requires ext/icu/icu.c with SQLITE_ENABLE_ICU"
#endif

#if !defined(SQLITE_ENABLE_ICUEX)
# error "icuex.c requires SQLITE_ENABLE_ICUEX"
#endif

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <unicode/uchar.h>
#include <unicode/unorm2.h>
#include <unicode/ustring.h>
#include <unicode/utf16.h>

/*
** Function flags shared by the two SQL scalar functions.  Neither function
** consumes nor produces SQLite subtypes, so SQLITE_SUBTYPE and
** SQLITE_RESULT_SUBTYPE must not be added here.
*/
#define ICUEX_FUNCTION_FLAGS \
  (SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS)

/* Largest number of invalid mode bytes included in an error message. */
#define ICUEX_MODE_PREVIEW_LIMIT 64

/* ICU UChar and SQLite UTF-16 code units must both be exactly two bytes. */
typedef char IcuexUCharMustBeTwoBytes[(sizeof(UChar)==2) ? 1 : -1];

typedef enum IcuexNormalizeMode {
  ICUEX_NORMALIZE_NFC = 0,
  ICUEX_NORMALIZE_NFD,
  ICUEX_NORMALIZE_NFKC,
  ICUEX_NORMALIZE_NFKD,
  ICUEX_NORMALIZE_NFKC_CF,
  ICUEX_NORMALIZE_NFKD_CF_STRIP
} IcuexNormalizeMode;

/*
** Report that a named SQL argument must be TEXT or NULL.
**
** zFunction and zArgument are compile-time strings owned by the caller.  The
** resulting message is deliberately independent of the value that had the
** wrong SQLite storage class.
*/
static void icuexTypeError(
  sqlite3_context *pCtx,
  const char *zFunction,
  const char *zArgument
){
  char *zMessage = sqlite3_mprintf(
      "%s(): %s must be TEXT or NULL", zFunction, zArgument
  );
  if( zMessage==0 ){
    sqlite3_result_error_nomem(pCtx);
    return;
  }
  sqlite3_result_error(pCtx, zMessage, -1);
  sqlite3_free(zMessage);
}

/*
** Return nonzero if zValue is exactly the ASCII token zLiteral, ignoring only
** ASCII letter case.
**
** nValue is a byte count, not a character count.  Requiring equal explicit
** lengths prevents embedded NUL bytes and token prefixes from being accepted.
*/
static int icuexAsciiEqualNoCase(
  const unsigned char *zValue,
  int nValue,
  const char *zLiteral
){
  int i;
  size_t nLiteral = strlen(zLiteral);

  if( nValue<0 || (size_t)nValue!=nLiteral ) return 0;
  for(i=0; i<nValue; i++){
    unsigned char a = zValue[i];
    unsigned char b = (unsigned char)zLiteral[i];
    if( a>=(unsigned char)'A' && a<=(unsigned char)'Z' ){
      a = (unsigned char)(a + ((unsigned char)'a' - (unsigned char)'A'));
    }
    if( b>=(unsigned char)'A' && b<=(unsigned char)'Z' ){
      b = (unsigned char)(b + ((unsigned char)'a' - (unsigned char)'A'));
    }
    if( a!=b ) return 0;
  }
  return 1;
}

/*
** Render an invalid normalization mode safely.
**
** Printable ASCII bytes other than backslash and double quote are copied as
** written.  All other bytes use a hexadecimal escape.  The preview is bounded
** so that an attacker cannot force construction of an error message
** proportional to SQLITE_LIMIT_LENGTH.  The complete supported-mode list is
** always included.
*/
static void icuexModeError(
  sqlite3_context *pCtx,
  const unsigned char *zMode,
  int nMode
){
  sqlite3_str *pMessage = sqlite3_str_new(sqlite3_context_db_handle(pCtx));
  int i;
  int nPreview = nMode<ICUEX_MODE_PREVIEW_LIMIT
               ? nMode : ICUEX_MODE_PREVIEW_LIMIT;
  char *zFinished;

  if( pMessage==0 ){
    sqlite3_result_error_nomem(pCtx);
    return;
  }

  sqlite3_str_appendall(pMessage, "str_normalize(): unsupported mode \"");
  for(i=0; i<nPreview; i++){
    unsigned char c = zMode[i];
    if( c>=0x20 && c<=0x7e && c!=(unsigned char)'\\'
                             && c!=(unsigned char)'\"' ){
      sqlite3_str_appendchar(pMessage, 1, (char)c);
    }else{
      sqlite3_str_appendf(pMessage, "\\x%02X", (unsigned int)c);
    }
  }
  if( nMode>nPreview ) sqlite3_str_appendall(pMessage, "...");
  sqlite3_str_appendall(
      pMessage,
      "\"; expected one of: NFC NFD NFKC NFKD NFKC_CF NFKD_CF_STRIP"
  );

  zFinished = sqlite3_str_finish(pMessage);
  if( zFinished==0 ){
    sqlite3_result_error_nomem(pCtx);
    return;
  }
  sqlite3_result_error(pCtx, zFinished, -1);
  sqlite3_free(zFinished);
}

/*
** Parse the explicitly sized ASCII mode token.
**
** Return nonzero and populate *pMode on success.  On failure, install a safe,
** bounded SQL error in pCtx and return zero.
*/
static int icuexParseMode(
  sqlite3_context *pCtx,
  const unsigned char *zMode,
  int nMode,
  IcuexNormalizeMode *pMode
){
  if( icuexAsciiEqualNoCase(zMode, nMode, "NFC") ){
    *pMode = ICUEX_NORMALIZE_NFC;
  }else if( icuexAsciiEqualNoCase(zMode, nMode, "NFD") ){
    *pMode = ICUEX_NORMALIZE_NFD;
  }else if( icuexAsciiEqualNoCase(zMode, nMode, "NFKC") ){
    *pMode = ICUEX_NORMALIZE_NFKC;
  }else if( icuexAsciiEqualNoCase(zMode, nMode, "NFKD") ){
    *pMode = ICUEX_NORMALIZE_NFKD;
  }else if( icuexAsciiEqualNoCase(zMode, nMode, "NFKC_CF") ){
    *pMode = ICUEX_NORMALIZE_NFKC_CF;
  }else if( icuexAsciiEqualNoCase(zMode, nMode, "NFKD_CF_STRIP") ){
    *pMode = ICUEX_NORMALIZE_NFKD_CF_STRIP;
  }else{
    icuexModeError(pCtx, zMode, nMode);
    return 0;
  }
  return 1;
}

/*
** Obtain a native-endian UTF-16 view of a non-NULL SQLite TEXT value.
**
** sqlite3_value_bytes16() returns bytes whereas ICU accepts a count of UChar
** code units.  SQLite's byte count is an int and is required to be even for a
** UTF-16 representation.  The returned pointer remains owned by SQLite and is
** valid until another conversion is requested from the same sqlite3_value or
** the SQL callback returns.
**
** Return nonzero on success.  On allocation failure or an impossible byte
** count, install an SQL error and return zero.
*/
static int icuexInputUtf16(
  sqlite3_context *pCtx,
  sqlite3_value *pValue,
  const UChar **pzText,
  int32_t *pnText
){
  const UChar *zText;
  int nByte;

  /* Obtain the size first.  A later sqlite3_value_bytes16() call is allowed
  ** to invalidate a pointer previously returned by sqlite3_value_text16(). */
  nByte = sqlite3_value_bytes16(pValue);
  zText = (const UChar *)sqlite3_value_text16(pValue);
  if( zText==0 ){
    sqlite3_result_error_nomem(pCtx);
    return 0;
  }
  if( nByte<0 || (nByte & 1)!=0 ){
    sqlite3_result_error(pCtx, "invalid SQLite UTF-16 text length", -1);
    return 0;
  }

  *pzText = zText;
  *pnText = (int32_t)(nByte / (int)sizeof(UChar));
  return 1;
}

/*
** Allocate a UTF-16 buffer for nRequired output code units plus a trailing
** UChar reserved for defensive NUL termination.
**
** The terminator is not part of the SQL value and is never used to calculate
** its length.
*/
static UChar *icuexAllocateUtf16(
  sqlite3_context *pCtx,
  int32_t nRequired
){
  sqlite3_uint64 nAllocate;
  UChar *zResult;

  if( nRequired<0 || nRequired==INT32_MAX ){
    sqlite3_result_error_toobig(pCtx);
    return 0;
  }
  nAllocate = ((sqlite3_uint64)(uint32_t)nRequired + 1u)
            * (sqlite3_uint64)sizeof(UChar);
  zResult = (UChar *)sqlite3_malloc64(nAllocate);
  if( zResult==0 ) sqlite3_result_error_nomem(pCtx);
  return zResult;
}

/*
** Normalize a UTF-16 string with one immutable ICU Normalizer2 instance.
**
** ICU output lengths are UTF-16 code units.  A preflight call determines the
** exact capacity.  U_BUFFER_OVERFLOW_ERROR is expected for a nonempty result;
** U_ZERO_ERROR is also valid when the result is empty.  On success, *pzResult
** is owned by the caller and must be released with sqlite3_free().
*/
static int icuexNormalizeUtf16(
  sqlite3_context *pCtx,
  const UNormalizer2 *pNormalizer,
  const UChar *zInput,
  int32_t nInput,
  UChar **pzResult,
  int32_t *pnResult
){
  UErrorCode status = U_ZERO_ERROR;
  int32_t nRequired;
  int32_t nWritten;
  UChar *zResult;

  nRequired = unorm2_normalize(
      pNormalizer, zInput, nInput, 0, 0, &status
  );
  if( status!=U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status) ){
    icuFunctionError(pCtx, "unorm2_normalize", status);
    return 0;
  }
  zResult = icuexAllocateUtf16(pCtx, nRequired);
  if( zResult==0 ) return 0;

  status = U_ZERO_ERROR;
  nWritten = unorm2_normalize(
      pNormalizer, zInput, nInput, zResult, nRequired + 1, &status
  );
  if( U_FAILURE(status) ){
    sqlite3_free(zResult);
    icuFunctionError(pCtx, "unorm2_normalize", status);
    return 0;
  }
  if( nWritten<0 || nWritten>nRequired ){
    sqlite3_free(zResult);
    sqlite3_result_error(pCtx, "ICU normalization length mismatch", -1);
    return 0;
  }
  zResult[nWritten] = 0;
  *pzResult = zResult;
  *pnResult = nWritten;
  return 1;
}

/*
** Apply full, locale-independent Unicode default case folding to UTF-16 text.
**
** No normalization is performed.  The allocation and ownership contract is
** the same as icuexNormalizeUtf16().
*/
static int icuexCasefoldUtf16(
  sqlite3_context *pCtx,
  const UChar *zInput,
  int32_t nInput,
  UChar **pzResult,
  int32_t *pnResult
){
  UErrorCode status = U_ZERO_ERROR;
  int32_t nRequired;
  int32_t nWritten;
  UChar *zResult;

  nRequired = u_strFoldCase(
      0, 0, zInput, nInput, U_FOLD_CASE_DEFAULT, &status
  );
  if( status!=U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status) ){
    icuFunctionError(pCtx, "u_strFoldCase", status);
    return 0;
  }
  zResult = icuexAllocateUtf16(pCtx, nRequired);
  if( zResult==0 ) return 0;

  status = U_ZERO_ERROR;
  nWritten = u_strFoldCase(
      zResult, nRequired + 1, zInput, nInput,
      U_FOLD_CASE_DEFAULT, &status
  );
  if( U_FAILURE(status) ){
    sqlite3_free(zResult);
    icuFunctionError(pCtx, "u_strFoldCase", status);
    return 0;
  }
  if( nWritten<0 || nWritten>nRequired ){
    sqlite3_free(zResult);
    sqlite3_result_error(pCtx, "ICU case-fold length mismatch", -1);
    return 0;
  }
  zResult[nWritten] = 0;
  *pzResult = zResult;
  *pnResult = nWritten;
  return 1;
}

/*
** Remove code points whose canonical combining class is nonzero.
**
** nText and the return value are UTF-16 code-unit counts.  The source is
** compacted in place.  Iterating with U16_NEXT and writing with
** U16_APPEND_UNSAFE ensures supplementary code points are treated atomically.
** Marks whose Unicode general category is Mn, Mc, or Me remain when their CCC
** is zero; this is intentionally not a general-category filter.
*/
static int32_t icuexStripNonzeroCcc(UChar *zText, int32_t nText){
  int32_t iRead = 0;
  int32_t iWrite = 0;

  while( iRead<nText ){
    UChar32 c;
    U16_NEXT(zText, iRead, nText, c);
    if( u_getCombiningClass(c)==0 ){
      U16_APPEND_UNSAFE(zText, iWrite, c);
    }
  }
  zText[iWrite] = 0;
  return iWrite;
}

/*
** Convert a UTF-16 transformation result to UTF-8 and return it as SQL TEXT.
**
** nResult is measured in UTF-16 code units.  UTF-8 is used for the final
** sqlite3_result_text() call because SQLite's UTF-16 result interfaces treat a
** leading U+FEFF as a byte-order mark and remove it.  A UTF-8 result preserves
** that code point for transformations, such as NFC and plain case folding,
** whose Unicode semantics retain it.  Explicit lengths preserve embedded
** U+0000 characters.
*/
static void icuexReturnText(
  sqlite3_context *pCtx,
  UChar *zResult,
  int32_t nResult
){
  UErrorCode status = U_ZERO_ERROR;
  int32_t nRequired = 0;
  int32_t nWritten = 0;
  int nLimit;
  char *zUtf8;

  u_strToUTF8(0, 0, &nRequired, zResult, nResult, &status);
  if( status!=U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status) ){
    sqlite3_free(zResult);
    icuFunctionError(pCtx, "u_strToUTF8", status);
    return;
  }
  nLimit = sqlite3_limit(
      sqlite3_context_db_handle(pCtx), SQLITE_LIMIT_LENGTH, -1
  );
  if( nRequired<0 || nRequired==INT32_MAX || nRequired>nLimit ){
    sqlite3_free(zResult);
    sqlite3_result_error_toobig(pCtx);
    return;
  }
  zUtf8 = (char *)sqlite3_malloc64((sqlite3_uint64)nRequired + 1u);
  if( zUtf8==0 ){
    sqlite3_free(zResult);
    sqlite3_result_error_nomem(pCtx);
    return;
  }

  status = U_ZERO_ERROR;
  u_strToUTF8(
      zUtf8, nRequired + 1, &nWritten, zResult, nResult, &status
  );
  sqlite3_free(zResult);
  if( U_FAILURE(status) ){
    sqlite3_free(zUtf8);
    icuFunctionError(pCtx, "u_strToUTF8", status);
    return;
  }
  if( nWritten<0 || nWritten>nRequired ){
    sqlite3_free(zUtf8);
    sqlite3_result_error(pCtx, "ICU UTF-8 length mismatch", -1);
    return;
  }
  zUtf8[nWritten] = 0;
  sqlite3_result_text(pCtx, zUtf8, nWritten, sqlite3_free);
}

/*
** SQL implementation of str_casefold(text).
**
** NULL propagates.  Non-NULL values must have SQLite storage class TEXT;
** numeric and BLOB values are rejected rather than coerced.
*/
static void icuexCasefoldFunc(
  sqlite3_context *pCtx,
  int argc,
  sqlite3_value **argv
){
  const UChar *zInput;
  int32_t nInput;
  UChar *zResult;
  int32_t nResult;

  assert(argc==1);
  (void)argc;
  if( sqlite3_value_type(argv[0])==SQLITE_NULL ) return;
  if( sqlite3_value_type(argv[0])!=SQLITE_TEXT ){
    icuexTypeError(pCtx, "str_casefold", "text");
    return;
  }
  if( !icuexInputUtf16(pCtx, argv[0], &zInput, &nInput) ) return;
  if( !icuexCasefoldUtf16(
          pCtx, zInput, nInput, &zResult, &nResult
      ) ) return;
  icuexReturnText(pCtx, zResult, nResult);
}

/*
** Resolve an immutable ICU normalizer for every directly supported mode.
**
** NFKD_CF_STRIP is composite and is therefore handled by its caller.  ICU
** owns all returned singleton instances; callers must never close them.
*/
static const UNormalizer2 *icuexGetNormalizer(
  IcuexNormalizeMode mode,
  UErrorCode *pStatus
){
  switch( mode ){
    case ICUEX_NORMALIZE_NFC:
      return unorm2_getNFCInstance(pStatus);
    case ICUEX_NORMALIZE_NFD:
      return unorm2_getNFDInstance(pStatus);
    case ICUEX_NORMALIZE_NFKC:
      return unorm2_getNFKCInstance(pStatus);
    case ICUEX_NORMALIZE_NFKD:
      return unorm2_getNFKDInstance(pStatus);
    case ICUEX_NORMALIZE_NFKC_CF:
      return unorm2_getNFKCCasefoldInstance(pStatus);
    case ICUEX_NORMALIZE_NFKD_CF_STRIP:
      break;
  }
  return 0;
}

/*
** SQL implementation of str_normalize(text, kind).
**
** The NFKD_CF_STRIP composite mode intentionally performs NFKC_Casefold
** first, then NFKD, then removes all code points with nonzero CCC.  This order
** exposes and removes marks introduced by compatibility mapping, folding, or
** canonical decomposition while preserving CCC-zero marks.
*/
static void icuexNormalizeFunc(
  sqlite3_context *pCtx,
  int argc,
  sqlite3_value **argv
){
  const unsigned char *zModeText;
  int nModeText;
  IcuexNormalizeMode mode;
  const UChar *zInput;
  int32_t nInput;
  UErrorCode status = U_ZERO_ERROR;
  const UNormalizer2 *pNormalizer;
  UChar *zResult;
  int32_t nResult;

  assert(argc==2);
  (void)argc;
  if( sqlite3_value_type(argv[0])==SQLITE_NULL
   || sqlite3_value_type(argv[1])==SQLITE_NULL ){
    return;
  }
  if( sqlite3_value_type(argv[0])!=SQLITE_TEXT ){
    icuexTypeError(pCtx, "str_normalize", "text");
    return;
  }
  if( sqlite3_value_type(argv[1])!=SQLITE_TEXT ){
    icuexTypeError(pCtx, "str_normalize", "kind");
    return;
  }

  /* As with the UTF-16 input helper, obtain the length before the pointer so
  ** no subsequent conversion call can invalidate the pointer being parsed. */
  nModeText = sqlite3_value_bytes(argv[1]);
  zModeText = sqlite3_value_text(argv[1]);
  if( zModeText==0 ){
    sqlite3_result_error_nomem(pCtx);
    return;
  }
  if( !icuexParseMode(pCtx, zModeText, nModeText, &mode) ) return;
  if( !icuexInputUtf16(pCtx, argv[0], &zInput, &nInput) ) return;

  if( mode!=ICUEX_NORMALIZE_NFKD_CF_STRIP ){
    pNormalizer = icuexGetNormalizer(mode, &status);
    if( U_FAILURE(status) || pNormalizer==0 ){
      if( U_SUCCESS(status) ) status = U_INTERNAL_PROGRAM_ERROR;
      icuFunctionError(pCtx, "unorm2_getInstance", status);
      return;
    }
    if( !icuexNormalizeUtf16(
            pCtx, pNormalizer, zInput, nInput, &zResult, &nResult
        ) ) return;
    icuexReturnText(pCtx, zResult, nResult);
    return;
  }

  {
    const UNormalizer2 *pNfkcCf;
    const UNormalizer2 *pNfkd;
    UChar *zFolded;
    int32_t nFolded;

    pNfkcCf = unorm2_getNFKCCasefoldInstance(&status);
    if( U_FAILURE(status) || pNfkcCf==0 ){
      if( U_SUCCESS(status) ) status = U_INTERNAL_PROGRAM_ERROR;
      icuFunctionError(pCtx, "unorm2_getNFKCCasefoldInstance", status);
      return;
    }
    pNfkd = unorm2_getNFKDInstance(&status);
    if( U_FAILURE(status) || pNfkd==0 ){
      if( U_SUCCESS(status) ) status = U_INTERNAL_PROGRAM_ERROR;
      icuFunctionError(pCtx, "unorm2_getNFKDInstance", status);
      return;
    }
    if( !icuexNormalizeUtf16(
            pCtx, pNfkcCf, zInput, nInput, &zFolded, &nFolded
        ) ) return;
    if( !icuexNormalizeUtf16(
            pCtx, pNfkd, zFolded, nFolded, &zResult, &nResult
        ) ){
      sqlite3_free(zFolded);
      return;
    }
    sqlite3_free(zFolded);
    nResult = icuexStripNonzeroCcc(zResult, nResult);
    icuexReturnText(pCtx, zResult, nResult);
  }
}

/*
** Register one ICU root collation and transfer its UCollator to SQLite.
**
** sqlite3_create_collation_v2() does not invoke xDestroy when registration
** fails, so this function closes the collator itself on that path.
*/
static int icuexRegisterCollation(
  sqlite3 *db,
  const char *zName,
  const char *zLocale,
  UColAttributeValue strength
){
  UErrorCode status = U_ZERO_ERROR;
  UCollator *pCollator = ucol_open(zLocale, &status);
  int rc;

  if( U_FAILURE(status) || pCollator==0 ){
    sqlite3_log(
        SQLITE_ERROR,
        "icuex: ucol_open(%s) failed: %s",
        zLocale,
        u_errorName(status)
    );
    if( pCollator!=0 ) ucol_close(pCollator);
    return SQLITE_ERROR;
  }
  ucol_setStrength(pCollator, strength);
  rc = sqlite3_create_collation_v2(
      db,
      zName,
      SQLITE_UTF16,
      (void *)pCollator,
      icuCollationColl,
      icuCollationDel
  );
  if( rc!=SQLITE_OK ) ucol_close(pCollator);
  return rc;
}

/*
** Register the complete SQL surface owned by icuex on one SQLite connection.
**
** Registration stops at the first failure.  Objects already transferred to
** SQLite remain connection-owned and are destroyed when the failed connection
** is closed.
*/
static int icuexRegister(sqlite3 *db){
  int rc;

  rc = icuexRegisterCollation(db, "UTF_CI", "root", UCOL_SECONDARY);
  if( rc==SQLITE_OK ){
    rc = icuexRegisterCollation(db, "UTF_CI_AI", "root", UCOL_PRIMARY);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(
        db, "str_casefold", 1, ICUEX_FUNCTION_FLAGS,
        0, icuexCasefoldFunc, 0, 0
    );
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(
        db, "str_normalize", 2, ICUEX_FUNCTION_FLAGS,
        0, icuexNormalizeFunc, 0, 0
    );
  }
  return rc;
}

/*
** Internal amalgamation component initializer.
**
** The separately generated aggregate initializer calls this function for each
** new database connection when icuex is enabled.  This is intentionally not a
** three-argument loadable-extension entry point.
*/
int sqlite3IcuexInit(sqlite3 *db){
  return icuexRegister(db);
}
