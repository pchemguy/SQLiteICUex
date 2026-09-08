"""Verify full Unicode default case folding through str_casefold()."""

from __future__ import annotations

import sqlite3

import pytest


CASEFOLD_DIVERGENCES = [
    pytest.param("Straße", "strasse", "straße", id="sharp-s-in-word"),
    pytest.param("ß", "ss", "ß", id="sharp-s"),
    pytest.param("ẞ", "ss", "ß", id="capital-sharp-s"),
    pytest.param("ΟΣ", "οσ", "ος", id="contextual-final-sigma"),
    pytest.param("ς", "σ", "ς", id="final-sigma"),
    pytest.param("ſ", "s", "ſ", id="long-s"),
    pytest.param("ﬀ", "ff", "ﬀ", id="ligature-ff"),
    pytest.param("ﬁ", "fi", "ﬁ", id="ligature-fi"),
    pytest.param("ﬂ", "fl", "ﬂ", id="ligature-fl"),
    pytest.param("ﬃ", "ffi", "ﬃ", id="ligature-ffi"),
    pytest.param("ﬄ", "ffl", "ﬄ", id="ligature-ffl"),
    pytest.param("ﬅ", "st", "ﬅ", id="ligature-long-s-t"),
    pytest.param("ﬆ", "st", "ﬆ", id="ligature-st"),
    pytest.param("ŉ", "ʼn", "ŉ", id="apostrophe-n-expansion"),
]


@pytest.mark.parametrize(
    ("source", "expected"),
    [
        ("", ""),
        ("ASCII", "ascii"),
        ("Straße", "strasse"),
        ("ЁЙ", "ёй"),
        ("Σσς", "σσσ"),
        ("ﬀ", "ff"),
        ("ИЙЕЁЬЪ", "ийеёьъ"),
        ("\ufeffA", "\ufeffa"),
        ("😀", "😀"),
    ],
)
def test_casefold_examples(
    db: sqlite3.Connection, source: str, expected: str
) -> None:
    """Check stable folding mappings, expansions, and unchanged characters."""

    assert db.execute(
        "SELECT str_casefold(?)", (source,)
    ).fetchone()[0] == expected


@pytest.mark.parametrize(
    ("source", "expected_fold", "expected_lower"), CASEFOLD_DIVERGENCES
)
def test_casefold_differs_from_lowercase_mappings(
    db: sqlite3.Connection,
    source: str,
    expected_fold: str,
    expected_lower: str,
) -> None:
    """Assert both the full-fold result and its expected lowercase contrast."""

    folded = db.execute("SELECT str_casefold(?)", (source,)).fetchone()[0]
    assert folded == expected_fold
    assert folded != expected_lower


def test_casefold_does_not_normalize(db: sqlite3.Connection) -> None:
    """Confirm canonically equivalent inputs retain distinct binary forms."""

    composed, decomposed = db.execute(
        "SELECT str_casefold('É'), str_casefold('E' || char(0x301))"
    ).fetchone()
    assert composed == "é"
    assert decomposed == "e\u0301"
    assert composed != decomposed


def test_casefold_is_idempotent(db: sqlite3.Connection) -> None:
    """Require a second case-fold pass to leave the first result unchanged."""

    source = "Straße ЁЙ Σσς ﬀ"
    once, twice = db.execute(
        "SELECT str_casefold(?), str_casefold(str_casefold(?))",
        (source, source),
    ).fetchone()
    assert once == twice


def test_casefold_preserves_embedded_nul(db: sqlite3.Connection) -> None:
    """Require explicit-length processing on both sides of U+0000."""

    result = db.execute(
        "SELECT str_casefold(?)", ("A\x00Straße",)
    ).fetchone()[0]
    assert result == "a\x00strasse"
    assert len(result.encode("utf-8")) == len(b"a\x00strasse")


def test_casefold_handles_long_expanding_input(db: sqlite3.Connection) -> None:
    """Exercise ICU preflight allocation with substantial expanding input."""

    source = "Straße" * 20_000
    expected = "strasse" * 20_000
    assert db.execute(
        "SELECT str_casefold(?)", (source,)
    ).fetchone()[0] == expected


def test_casefold_returns_sql_text(db: sqlite3.Connection) -> None:
    """Verify the SQLite storage class of a successful result."""

    assert db.execute(
        "SELECT typeof(str_casefold('A'))"
    ).fetchone()[0] == "text"
