"""Verify NULL, strict typing, mode validation, and SQL result contracts."""

from __future__ import annotations

import sqlite3

import pytest


SUPPORTED_MODES = ("NFC", "NFD", "NFKC", "NFKD", "NFKC_CF", "NFKD_CF_STRIP")


def test_null_propagates_from_casefold(db: sqlite3.Connection) -> None:
    """Return SQL NULL for a NULL case-fold input."""

    assert db.execute("SELECT str_casefold(NULL)").fetchone()[0] is None


@pytest.mark.parametrize(
    "sql",
    [
        "SELECT str_normalize(NULL, 'NFC')",
        "SELECT str_normalize('text', NULL)",
        "SELECT str_normalize(NULL, NULL)",
        "SELECT str_normalize(NULL, 'invalid')",
    ],
)
def test_null_propagates_from_either_normalize_argument(
    db: sqlite3.Connection, sql: str
) -> None:
    """Return SQL NULL before interpreting any other normalization argument."""

    assert db.execute(sql).fetchone()[0] is None


@pytest.mark.parametrize("value", [1, 1.5, sqlite3.Binary(b"ABC")])
def test_casefold_rejects_non_text_storage_classes(
    db: sqlite3.Connection, value: object
) -> None:
    """Reject integer, real, and blob case-fold inputs without coercion."""

    with pytest.raises(sqlite3.OperationalError, match="TEXT or NULL"):
        db.execute("SELECT str_casefold(?)", (value,)).fetchone()


@pytest.mark.parametrize("value", [1, 1.5, sqlite3.Binary(b"ABC")])
def test_normalize_rejects_non_text_input(
    db: sqlite3.Connection, value: object
) -> None:
    """Reject non-TEXT transformation input without SQLite coercion."""

    with pytest.raises(sqlite3.OperationalError, match="TEXT or NULL"):
        db.execute("SELECT str_normalize(?, 'NFC')", (value,)).fetchone()


@pytest.mark.parametrize("value", [1, 1.5, sqlite3.Binary(b"NFC")])
def test_normalize_rejects_non_text_kind(
    db: sqlite3.Connection, value: object
) -> None:
    """Reject non-TEXT mode values without SQLite coercion."""

    with pytest.raises(sqlite3.OperationalError, match="TEXT or NULL"):
        db.execute("SELECT str_normalize('text', ?)", (value,)).fetchone()


@pytest.mark.parametrize(
    "mode",
    [
        "",
        "NF",
        "NFCX",
        " NFC",
        "NFC ",
        "NFC\x00",
        "NFC\x00NFD",
        "ＮＦＣ",
        "NFKC-CASEFOLD",
    ],
)
def test_invalid_modes_are_rejected(
    db: sqlite3.Connection, mode: str
) -> None:
    """Reject empty, partial, padded, embedded-NUL, non-ASCII, and alias modes."""

    with pytest.raises(sqlite3.OperationalError) as caught:
        db.execute("SELECT str_normalize('text', ?)", (mode,)).fetchone()
    message = str(caught.value)
    assert "unsupported mode" in message
    for supported in SUPPORTED_MODES:
        assert supported in message


def test_invalid_mode_escapes_nul_safely(db: sqlite3.Connection) -> None:
    """Expose an embedded mode NUL as an escape instead of truncating it."""

    with pytest.raises(sqlite3.OperationalError) as caught:
        db.execute(
            "SELECT str_normalize('text', ?)", ("NFC\x00NFD",)
        ).fetchone()
    assert "NFC\\x00NFD" in str(caught.value)


def test_invalid_mode_preview_is_bounded(db: sqlite3.Connection) -> None:
    """Prevent invalid mode errors from growing with an arbitrarily long value."""

    mode = "X" * 100_000
    with pytest.raises(sqlite3.OperationalError) as caught:
        db.execute("SELECT str_normalize('text', ?)", (mode,)).fetchone()
    message = str(caught.value)
    assert "..." in message
    assert len(message) < 512


@pytest.mark.parametrize("mode", SUPPORTED_MODES)
def test_empty_text_is_supported(
    db: sqlite3.Connection, mode: str
) -> None:
    """Return empty TEXT for every normalization mode."""

    result, storage_class = db.execute(
        "SELECT str_normalize('', ?), typeof(str_normalize('', ?))",
        (mode, mode),
    ).fetchone()
    assert result == ""
    assert storage_class == "text"


def test_wrong_arities_use_sqlite_errors(db: sqlite3.Connection) -> None:
    """Require SQLite to reject signatures other than one and two arguments."""

    statements = (
        "SELECT str_casefold()",
        "SELECT str_casefold('a', 'b')",
        "SELECT str_normalize('a')",
        "SELECT str_normalize('a', 'NFC', 'extra')",
    )
    for sql in statements:
        with pytest.raises(sqlite3.OperationalError):
            db.execute(sql).fetchone()


def test_normalize_preserves_embedded_nul_exactly(
    db: sqlite3.Connection,
) -> None:
    """Verify bytes after embedded U+0000 survive transformation and return."""

    source = "E\x00e\u0301"
    result = db.execute(
        "SELECT str_normalize(?, 'NFC')", (source,)
    ).fetchone()[0]
    assert result == "E\x00é"
    assert len(result.encode("utf-8")) == len(b"E\x00\xc3\xa9")


def test_normalize_returns_sql_text(db: sqlite3.Connection) -> None:
    """Verify successful normalization returns SQLite storage class text."""

    assert db.execute(
        "SELECT typeof(str_normalize('A', 'NFKC_CF'))"
    ).fetchone()[0] == "text"
