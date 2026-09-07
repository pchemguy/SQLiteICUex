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
** ICU-backed collations and Unicode normalization for SQLite.
**
** This file is deliberately independent of SQLite's ext/icu/icu.c.  It uses
** only the public SQLite and ICU APIs and owns all comparison, destruction,
** transformation, and error-reporting callbacks that it registers.
**
** When compiled with SQLITE_CORE, a surrounding built-in-extension
** initializer calls sqlite3IcuexInit() for each new connection.  Otherwise
** this source builds as a conventional loadable extension exporting
** sqlite3_icuex_init().
**
** In built-in mode, the surrounding build system is responsible for invoking
** sqlite3IcuexInit() from its aggregate initializer.  This file neither
** defines that aggregate initializer nor calls sqlite3_auto_extension().
**
** SQL surface:
**
**   UTF_CI       ICU root collation at secondary strength.
**   UTF_CI_AI    Lexical comparison of NFKD_CF_STRIP normalized keys.
**   str_casefold(text)
**   str_normalize(text, kind)
**
** Both scalar functions accept only TEXT or NULL.  They are registered as
** deterministic and innocuous and preserve embedded U+0000 characters by
** using explicit lengths throughout.
*/

#ifndef SQLITE_CORE
# include "sqlite3ext.h"
  SQLITE_EXTENSION_INIT1
#else
# include "sqlite3.h"
#endif

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <unicode/uchar.h>
#include <unicode/ucol.h>
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
** Result codes for transformations performed outside an SQL-function
** context.  A collation callback has no sqlite3_context through which it can
** report allocation, capacity, or ICU failures, so it must preserve this
** information until it can log and interrupt the active database operation.
*/
typedef enum IcuexKeyResult {
  ICUEX_KEY_OK = 0,
  ICUEX_KEY_NOMEM,
  ICUEX_KEY_TOOBIG,
  ICUEX_KEY_ICU,
  ICUEX_KEY_LENGTH
} IcuexKeyResult;

/*
** Report an ICU failure from an SQL scalar-function callback.
**
** zFunction is a static operation name supplied by this module.  ICU owns the
** string returned by u_errorName().  A fixed stack buffer avoids introducing
** an allocation-failure path while constructing the diagnostic; SQLite's
** snprintf implementation always terminates the result.
*/
static void icuexFunctionError(
  sqlite3_context *pCtx,
  const char *zFunction,
  UErrorCode status
){
  char zMessage[160];
  sqlite3_snprintf(
      (int)sizeof(zMessage),
      zMessage,
      "ICU error: %s(): %s",
      zFunction,
      u_errorName(status)
  );
  sqlite3_result_error(pCtx, zMessage, -1);
}

/*
** Compare explicitly sized native-endian UTF-16 strings with one UCollator.
**
** SQLite supplies byte lengths, while ucol_strcoll() accepts UTF-16 code-unit
** lengths.  SQLite's UTF-16 collation contract guarantees complete two-byte
** code units.  Embedded U+0000 is ordinary data because both lengths remain
** explicit.
*/
static int icuexIcuCollation(
  void *pContext,
  int nLeftByte,
  const void *pLeft,
  int nRightByte,
  const void *pRight
){
  UCollationResult result = ucol_strcoll(
      (const UCollator *)pContext,
      (const UChar *)pLeft,
      (int32_t)(nLeftByte / (int)sizeof(UChar)),
      (const UChar *)pRight,
      (int32_t)(nRightByte / (int)sizeof(UChar))
  );
  if( result==UCOL_LESS ) return -1;
  if( result==UCOL_GREATER ) return 1;
  return 0;
}

/* Close the UCollator whose ownership SQLite accepted at registration. */
static void icuexIcuCollationDelete(void *pContext){
  ucol_close((UCollator *)pContext);
}

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
    icuexFunctionError(pCtx, "unorm2_normalize", status);
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
    icuexFunctionError(pCtx, "unorm2_normalize", status);
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
    icuexFunctionError(pCtx, "u_strFoldCase", status);
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
    icuexFunctionError(pCtx, "u_strFoldCase", status);
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
** Normalize one stage of a collation key without an sqlite3_context.
**
** nInput, nRequired, and *pnResult are UTF-16 code-unit counts.  ICU's first
** call is a capacity preflight: U_BUFFER_OVERFLOW_ERROR is expected for a
** nonempty result, while U_ZERO_ERROR is valid for an empty result.  The
** caller owns *pzResult on success and must release it with sqlite3_free().
**
** The detailed result code is necessary because SQLite's xCompare interface
** cannot return an SQL error.  *pIcuStatus is meaningful for ICUEX_KEY_ICU.
*/
static IcuexKeyResult icuexNormalizeKeyStage(
  const UNormalizer2 *pNormalizer,
  const UChar *zInput,
  int32_t nInput,
  UChar **pzResult,
  int32_t *pnResult,
  UErrorCode *pIcuStatus
){
  UErrorCode status = U_ZERO_ERROR;
  sqlite3_uint64 nAllocate;
  int32_t nRequired;
  int32_t nWritten;
  UChar *zResult;

  *pzResult = 0;
  *pnResult = 0;
  *pIcuStatus = U_ZERO_ERROR;
  if( pNormalizer==0 || zInput==0 || nInput<0 ) return ICUEX_KEY_LENGTH;

  nRequired = unorm2_normalize(
      pNormalizer, zInput, nInput, 0, 0, &status
  );
  if( status!=U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status) ){
    *pIcuStatus = status;
    return ICUEX_KEY_ICU;
  }
  if( nRequired<0 || nRequired==INT32_MAX ) return ICUEX_KEY_TOOBIG;

  nAllocate = ((sqlite3_uint64)(uint32_t)nRequired + 1u)
            * (sqlite3_uint64)sizeof(UChar);
  zResult = (UChar *)sqlite3_malloc64(nAllocate);
  if( zResult==0 ) return ICUEX_KEY_NOMEM;

  status = U_ZERO_ERROR;
  nWritten = unorm2_normalize(
      pNormalizer, zInput, nInput, zResult, nRequired + 1, &status
  );
  if( U_FAILURE(status) ){
    sqlite3_free(zResult);
    *pIcuStatus = status;
    return ICUEX_KEY_ICU;
  }
  if( nWritten<0 || nWritten>nRequired ){
    sqlite3_free(zResult);
    return ICUEX_KEY_LENGTH;
  }

  zResult[nWritten] = 0;
  *pzResult = zResult;
  *pnResult = nWritten;
  return ICUEX_KEY_OK;
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
** Construct the canonical UTF_CI_AI comparison key.
**
** This is exactly the NFKD_CF_STRIP operation exposed by str_normalize():
** NFKC_Casefold, then NFKD, then removal of every code point whose canonical
** combining class is nonzero.  The order ensures marks introduced by case
** folding or compatibility mapping are exposed before filtering.  It also
** intentionally preserves general-category marks whose CCC is zero.
**
** The returned length is in UTF-16 code units.  The caller owns *pzKey on
** success.  Immutable Normalizer2 instances remain owned by ICU.
*/
static IcuexKeyResult icuexBuildCiAiKey(
  const UChar *zInput,
  int32_t nInput,
  UChar **pzKey,
  int32_t *pnKey,
  UErrorCode *pIcuStatus
){
  UErrorCode status = U_ZERO_ERROR;
  const UNormalizer2 *pNfkcCf;
  const UNormalizer2 *pNfkd;
  UChar *zFolded = 0;
  int32_t nFolded = 0;
  IcuexKeyResult result;

  *pzKey = 0;
  *pnKey = 0;
  *pIcuStatus = U_ZERO_ERROR;

  pNfkcCf = unorm2_getNFKCCasefoldInstance(&status);
  if( U_FAILURE(status) || pNfkcCf==0 ){
    *pIcuStatus = U_FAILURE(status) ? status : U_INTERNAL_PROGRAM_ERROR;
    return ICUEX_KEY_ICU;
  }
  status = U_ZERO_ERROR;
  pNfkd = unorm2_getNFKDInstance(&status);
  if( U_FAILURE(status) || pNfkd==0 ){
    *pIcuStatus = U_FAILURE(status) ? status : U_INTERNAL_PROGRAM_ERROR;
    return ICUEX_KEY_ICU;
  }

  result = icuexNormalizeKeyStage(
      pNfkcCf, zInput, nInput, &zFolded, &nFolded, pIcuStatus
  );
  if( result!=ICUEX_KEY_OK ) return result;

  result = icuexNormalizeKeyStage(
      pNfkd, zFolded, nFolded, pzKey, pnKey, pIcuStatus
  );
  sqlite3_free(zFolded);
  if( result!=ICUEX_KEY_OK ) return result;

  *pnKey = icuexStripNonzeroCcc(*pzKey, *pnKey);
  return ICUEX_KEY_OK;
}

/*
** Compare two explicitly sized native-endian UTF-16 strings by code unit.
**
** This is the emergency fallback required when UTF_CI_AI key construction
** fails.  It is deterministic for a single invocation and does not inspect a
** NUL terminator.  Odd or negative byte counts are outside SQLite's UTF-16
** collation contract; they are nevertheless ordered by their nonnegative raw
** byte prefixes so this mandatory callback result remains bounded.
*/
static int icuexCompareUtf16Fallback(
  int nLeftByte,
  const void *pLeft,
  int nRightByte,
  const void *pRight
){
  int i;
  int nLeft;
  int nRight;
  const UChar *zLeft;
  const UChar *zRight;

  if( nLeftByte<0 || pLeft==0 ) nLeftByte = 0;
  if( nRightByte<0 || pRight==0 ) nRightByte = 0;
  if( (nLeftByte & 1)!=0 || (nRightByte & 1)!=0 ){
    int nCommon = nLeftByte<nRightByte ? nLeftByte : nRightByte;
    int cmp = nCommon>0 ? memcmp(pLeft, pRight, (size_t)nCommon) : 0;
    if( cmp<0 ) return -1;
    if( cmp>0 ) return 1;
    return (nLeftByte>nRightByte) - (nLeftByte<nRightByte);
  }

  nLeft = nLeftByte / (int)sizeof(UChar);
  nRight = nRightByte / (int)sizeof(UChar);
  zLeft = (const UChar *)pLeft;
  zRight = (const UChar *)pRight;
  for(i=0; i<nLeft && i<nRight; i++){
    if( zLeft[i]<zRight[i] ) return -1;
    if( zLeft[i]>zRight[i] ) return 1;
  }
  return (nLeft>nRight) - (nLeft<nRight);
}

/*
** Compare two valid UTF-16 normalized keys lexicographically by code point.
**
** U16_NEXT advances across surrogate pairs atomically.  Returning zero for
** different source spellings that produce identical keys is essential for
** SQLite equality, UNIQUE constraints, and indexed lookup semantics; no
** source-text tiebreaker is permitted.
*/
static int icuexCompareCiAiKeys(
  const UChar *zLeft,
  int32_t nLeft,
  const UChar *zRight,
  int32_t nRight
){
  int32_t iLeft = 0;
  int32_t iRight = 0;

  while( iLeft<nLeft && iRight<nRight ){
    UChar32 cLeft;
    UChar32 cRight;
    U16_NEXT(zLeft, iLeft, nLeft, cLeft);
    U16_NEXT(zRight, iRight, nRight, cRight);
    if( cLeft<cRight ) return -1;
    if( cLeft>cRight ) return 1;
  }
  return (iLeft<nLeft) - (iRight<nRight);
}

/*
** SQLite UTF-16 collation callback for UTF_CI_AI.
**
** Each operand is converted to its NFKD_CF_STRIP key and the keys are
** compared by Unicode code point.  Embedded U+0000 is ordinary data because
** both input and key lengths are explicit.
**
** SQLite's xCompare signature has no error-result channel.  If input lengths,
** allocation, or ICU processing fail, this callback frees all temporaries,
** records a diagnostic, and interrupts the active database operation.  It
** then returns a provisional explicit-length ordering solely because SQLite
** requires an integer return value.  The fallback is not part of the
** supported collation semantics and must not be used to continue an index
** mutation after the interrupt is observed.
*/
static int icuexCiAiCollation(
  void *pContext,
  int nLeftByte,
  const void *pLeft,
  int nRightByte,
  const void *pRight
){
  sqlite3 *db = (sqlite3 *)pContext;
  UChar *zLeftKey = 0;
  UChar *zRightKey = 0;
  int32_t nLeftKey = 0;
  int32_t nRightKey = 0;
  UErrorCode status = U_ZERO_ERROR;
  IcuexKeyResult result;
  int cmp;
  UChar emptyInput = 0;
  const UChar *zLeftInput = pLeft!=0
                          ? (const UChar *)pLeft : &emptyInput;
  const UChar *zRightInput = pRight!=0
                           ? (const UChar *)pRight : &emptyInput;

  if( nLeftByte<0 || nRightByte<0
   || (nLeftByte & 1)!=0 || (nRightByte & 1)!=0
   || (pLeft==0 && nLeftByte!=0)
   || (pRight==0 && nRightByte!=0) ){
    result = ICUEX_KEY_LENGTH;
  }else{
    result = icuexBuildCiAiKey(
        zLeftInput,
        (int32_t)(nLeftByte / (int)sizeof(UChar)),
        &zLeftKey,
        &nLeftKey,
        &status
    );
    if( result==ICUEX_KEY_OK ){
      result = icuexBuildCiAiKey(
          zRightInput,
          (int32_t)(nRightByte / (int)sizeof(UChar)),
          &zRightKey,
          &nRightKey,
          &status
      );
    }
  }

  if( result==ICUEX_KEY_OK ){
    cmp = icuexCompareCiAiKeys(
        zLeftKey, nLeftKey, zRightKey, nRightKey
    );
    sqlite3_free(zLeftKey);
    sqlite3_free(zRightKey);
    return cmp;
  }

  sqlite3_free(zLeftKey);
  sqlite3_free(zRightKey);
  if( result==ICUEX_KEY_ICU ){
    sqlite3_log(
        SQLITE_ERROR,
        "icuex: UTF_CI_AI key generation failed: %s",
        u_errorName(status)
    );
  }else if( result==ICUEX_KEY_NOMEM ){
    sqlite3_log(SQLITE_NOMEM, "icuex: UTF_CI_AI key allocation failed");
  }else if( result==ICUEX_KEY_TOOBIG ){
    sqlite3_log(SQLITE_TOOBIG, "icuex: UTF_CI_AI key is too large");
  }else{
    sqlite3_log(SQLITE_ERROR, "icuex: invalid UTF_CI_AI input length");
  }
  if( db!=0 ) sqlite3_interrupt(db);
  return icuexCompareUtf16Fallback(
      nLeftByte, pLeft, nRightByte, pRight
  );
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
    icuexFunctionError(pCtx, "u_strToUTF8", status);
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
    icuexFunctionError(pCtx, "u_strToUTF8", status);
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
      icuexFunctionError(pCtx, "unorm2_getInstance", status);
      return;
    }
    if( !icuexNormalizeUtf16(
            pCtx, pNormalizer, zInput, nInput, &zResult, &nResult
        ) ) return;
    icuexReturnText(pCtx, zResult, nResult);
    return;
  }

  {
    IcuexKeyResult result = icuexBuildCiAiKey(
        zInput, nInput, &zResult, &nResult, &status
    );
    if( result==ICUEX_KEY_NOMEM ){
      sqlite3_result_error_nomem(pCtx);
      return;
    }
    if( result==ICUEX_KEY_TOOBIG ){
      sqlite3_result_error_toobig(pCtx);
      return;
    }
    if( result==ICUEX_KEY_ICU ){
      icuexFunctionError(pCtx, "NFKD_CF_STRIP", status);
      return;
    }
    if( result!=ICUEX_KEY_OK ){
      sqlite3_result_error(
          pCtx, "NFKD_CF_STRIP transformation length error", -1
      );
      return;
    }
    icuexReturnText(pCtx, zResult, nResult);
  }
}

/*
** Register one ICU root collation and transfer its UCollator to SQLite.
**
** sqlite3_create_collation_v2() does not invoke xDestroy when registration
** fails, so this function closes the collator itself on that path.
*/
static int icuexRegisterIcuCollation(
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
      icuexIcuCollation,
      icuexIcuCollationDelete
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

  rc = icuexRegisterIcuCollation(
      db, "UTF_CI", "root", UCOL_SECONDARY
  );
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_collation_v2(
        db,
        "UTF_CI_AI",
        SQLITE_UTF16,
        (void *)db,
        icuexCiAiCollation,
        0
    );
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
** Expose exactly the initializer appropriate to the selected build mode.
**
** SQLITE_CORE builds are registered by the surrounding built-in aggregate
** initializer.  Loadable builds initialize SQLite's extension API table and
** are called by sqlite3_load_extension().  Both paths return the first
** registration failure from icuexRegister(); neither performs global
** registration or owns the database connection.
*/
#ifdef SQLITE_CORE

int sqlite3IcuexInit(sqlite3 *db) {
    return icuexRegister(db);
}

#else

# if defined(_WIN32)
__declspec(dllexport)
# endif
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
