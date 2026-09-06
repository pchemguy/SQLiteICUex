"""Test the composite NFKD_CF_STRIP normalized search-key mode."""

from __future__ import annotations

import sqlite3

import pytest


@pytest.mark.parametrize(
    ("source", "expected"),
    [
        ("", ""),
        ("Straße", "strasse"),
        ("Ё", "е"),
        ("Й", "и"),
        ("É", "e"),
        ("ЁЙÉ", "еиe"),
        ("Ａ①", "a1"),
        ("A\u00adB", "ab"),
        ("😀", "😀"),
    ],
)
def test_nfkd_cf_strip_examples(
    db: sqlite3.Connection, source: str, expected: str
) -> None:
    """Verify folding, compatibility, ignorables, and accent removal."""

    assert db.execute(
        "SELECT str_normalize(?, 'NFKD_CF_STRIP')", (source,)
    ).fetchone()[0] == expected


def test_nfkd_cf_strip_removes_nonzero_ccc_marks(
    db: sqlite3.Connection,
) -> None:
    """Remove independently supplied accents with nonzero combining class."""

    source = "A\u0327\u0301"
    assert db.execute(
        "SELECT str_normalize(?, 'NFKD_CF_STRIP')", (source,)
    ).fetchone()[0] == "a"


def test_nfkd_cf_strip_preserves_ccc_zero_mark(
    db: sqlite3.Connection,
) -> None:
    """Preserve U+20DD, a stable enclosing mark whose CCC is zero."""

    source = "A\u20dd"
    assert db.execute(
        "SELECT str_normalize(?, 'NFKD_CF_STRIP')", (source,)
    ).fetchone()[0] == "a\u20dd"


def test_nfkd_cf_strip_is_idempotent(db: sqlite3.Connection) -> None:
    """Require the complete composite transformation to be idempotent."""

    source = "Straße ЁЙÉ Ａ① A\u0327\u0301 😀"
    once, twice = db.execute(
        "SELECT str_normalize(?, 'NFKD_CF_STRIP'), "
        "str_normalize("
        "str_normalize(?, 'NFKD_CF_STRIP'), 'NFKD_CF_STRIP'"
        ")",
        (source, source),
    ).fetchone()
    assert once == twice


def test_nfkd_cf_strip_mode_name_is_case_insensitive(
    db: sqlite3.Connection,
) -> None:
    """Accept mixed ASCII case for the exact composite-mode token."""

    assert db.execute(
        "SELECT str_normalize('É', 'nfkd_cf_strip')"
    ).fetchone()[0] == "e"
