"""Test Unicode NFKC_Casefold behavior exposed as NFKC_CF."""

from __future__ import annotations

import sqlite3

import pytest


@pytest.mark.parametrize(
    ("source", "expected"),
    [
        ("", ""),
        ("Straße", "strasse"),
        ("ЁЙÉ", "ёйé"),
        ("ＡＢＣ", "abc"),
        ("①", "1"),
        ("A\u00adB", "ab"),
        ("\ufeffA", "a"),
        ("😀", "😀"),
    ],
)
def test_nfkc_cf_examples(
    db: sqlite3.Connection, source: str, expected: str
) -> None:
    """Verify case, compatibility, composition, and ignorable handling."""

    assert db.execute(
        "SELECT str_normalize(?, 'NFKC_CF')", (source,)
    ).fetchone()[0] == expected


def test_nfkc_cf_is_composed(db: sqlite3.Connection) -> None:
    """Verify the NFKC_Casefold result includes final NFC composition."""

    assert db.execute(
        "SELECT str_normalize('E' || char(0x301), 'NFKC_CF')"
    ).fetchone()[0] == "é"


def test_nfkc_cf_is_idempotent(db: sqlite3.Connection) -> None:
    """Require repeated NFKC case-fold normalization to be stable."""

    source = "Straße Ａ ① ЁЙÉ A\u00adB"
    once, twice = db.execute(
        "SELECT str_normalize(?, 'NFKC_CF'), "
        "str_normalize(str_normalize(?, 'NFKC_CF'), 'NFKC_CF')",
        (source, source),
    ).fetchone()
    assert once == twice


def test_nfkc_cf_mode_name_is_case_insensitive(
    db: sqlite3.Connection,
) -> None:
    """Accept a mixed-case spelling of the exact NFKC_CF token."""

    assert db.execute(
        "SELECT str_normalize('Straße', 'nfkc_cf')"
    ).fetchone()[0] == "strasse"
