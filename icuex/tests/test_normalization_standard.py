"""Test NFC, NFD, NFKC, and NFKD normalization modes."""

from __future__ import annotations

import sqlite3

import pytest


@pytest.mark.parametrize(
    ("source", "mode", "expected"),
    [
        ("e\u0301", "NFC", "é"),
        ("É", "NFD", "E\u0301"),
        ("①Ａ", "NFKC", "1A"),
        ("①É", "NFKD", "1E\u0301"),
        ("각", "NFD", "각"),
        ("각", "NFC", "각"),
        ("\ufeffA", "NFC", "\ufeffA"),
        ("😀", "NFKD", "😀"),
    ],
)
def test_standard_normalization_examples(
    db: sqlite3.Connection, source: str, mode: str, expected: str
) -> None:
    """Verify stable canonical, compatibility, Hangul, and astral examples."""

    assert db.execute(
        "SELECT str_normalize(?, ?)", (source, mode)
    ).fetchone()[0] == expected


@pytest.mark.parametrize("mode", ["NFC", "NFD", "NFKC", "NFKD"])
def test_standard_modes_are_idempotent(
    db: sqlite3.Connection, mode: str
) -> None:
    """Require every standard normalization form to be idempotent."""

    source = "①É e\u0301 각 각 😀"
    once, twice = db.execute(
        "SELECT str_normalize(?, ?), "
        "str_normalize(str_normalize(?, ?), ?)",
        (source, mode, source, mode, mode),
    ).fetchone()
    assert once == twice


@pytest.mark.parametrize(
    "mode", ["nfc", "NfD", "nfkc", "NfKd"]
)
def test_mode_names_are_ascii_case_insensitive(
    db: sqlite3.Connection, mode: str
) -> None:
    """Accept mixed ASCII case without accepting broader aliases."""

    assert db.execute(
        "SELECT str_normalize('ASCII', ?)", (mode,)
    ).fetchone()[0]


@pytest.mark.parametrize("mode", ["NFC", "NFD", "NFKC", "NFKD"])
def test_standard_modes_preserve_embedded_nul(
    db: sqlite3.Connection, mode: str
) -> None:
    """Require normalization to process text following embedded U+0000."""

    source = "A\x00e\u0301"
    result = db.execute(
        "SELECT str_normalize(?, ?)", (source, mode)
    ).fetchone()[0]
    assert "\x00" in result
    assert result.startswith("A\x00")


def test_standard_normalization_handles_long_input(
    db: sqlite3.Connection,
) -> None:
    """Exercise normalization allocation beyond small stack-like sizes."""

    source = "e\u0301" * 30_000
    expected = "é" * 30_000
    assert db.execute(
        "SELECT str_normalize(?, 'NFC')", (source,)
    ).fetchone()[0] == expected
