"""Exercise UTF_CI and UTF_CI_AI through ordinary SQL collation contexts."""

from __future__ import annotations

import sqlite3

import pytest


@pytest.mark.parametrize(
    ("left", "right"),
    [
        ("АБВГДЙЬЁ", "абвгдйьё"),
        ("Hello", "hELLO"),
        ("ΜΆΙΟΣ", "μάιος"),
    ],
)
def test_utf_ci_ignores_case(
    db: sqlite3.Connection, left: str, right: str
) -> None:
    """Verify case-insensitive comparison across several writing systems."""

    result = db.execute(
        "SELECT ? = ? COLLATE UTF_CI", (left, right)
    ).fetchone()[0]
    assert result == 1


@pytest.mark.parametrize(
    ("left", "right"), [("е", "ё"), ("и", "й"), ("e", "é")]
)
def test_utf_ci_preserves_accent_distinctions(
    db: sqlite3.Connection, left: str, right: str
) -> None:
    """Verify that secondary strength does not discard accents."""

    result = db.execute(
        "SELECT ? = ? COLLATE UTF_CI", (left, right)
    ).fetchone()[0]
    assert result == 0


@pytest.mark.parametrize(
    ("left", "right"),
    [
        ("ЙЁ", "ие"),
        ("É", "e"),
        ("Å", "a"),
        ("Straße", "STRASSE"),
        ("Ａ①", "a1"),
        ("A\u00adB", "ab"),
    ],
)
def test_utf_ci_ai_normalized_key_equivalence(
    db: sqlite3.Connection, left: str, right: str
) -> None:
    """Verify equality through NFKD_CF_STRIP normalized keys."""

    result = db.execute(
        "SELECT ? = ? COLLATE UTF_CI_AI", (left, right)
    ).fetchone()[0]
    assert result == 1


@pytest.mark.parametrize(
    ("left", "right"),
    [("ЙЁ", "ие"), ("É", "e"), ("Ａ①", "a1"), ("A\u00adB", "ab")],
)
def test_utf_ci_ai_agrees_with_nfkd_cf_strip_keys(
    db: sqlite3.Connection, left: str, right: str
) -> None:
    """Verify selected collation equivalences match exposed search keys."""

    collation_equal, keys_equal = db.execute(
        "SELECT ? = ? COLLATE UTF_CI_AI, "
        "str_normalize(?, 'NFKD_CF_STRIP') = "
        "str_normalize(?, 'NFKD_CF_STRIP')",
        (left, right, left, right),
    ).fetchone()
    assert collation_equal == keys_equal == 1


def test_utf_ci_ai_does_not_merge_scripts(db: sqlite3.Connection) -> None:
    """Verify that Latin and Cyrillic lookalikes remain distinct."""

    assert db.execute(
        "SELECT 'e' = 'е' COLLATE UTF_CI_AI"
    ).fetchone()[0] == 0


def test_collation_applies_to_declared_column(db: sqlite3.Connection) -> None:
    """Verify equality inherits UTF_CI from a column declaration."""

    db.execute("CREATE TABLE terms(term TEXT COLLATE UTF_CI)")
    db.executemany("INSERT INTO terms VALUES (?)", [("Alpha",), ("beta",)])
    assert db.execute(
        "SELECT count(*) FROM terms WHERE term = 'ALPHA'"
    ).fetchone()[0] == 1


def test_utf_ci_unique_constraint(db: sqlite3.Connection) -> None:
    """Verify case variants conflict under a UTF_CI uniqueness constraint."""

    db.execute("CREATE TABLE terms(term TEXT UNIQUE COLLATE UTF_CI)")
    db.execute("INSERT INTO terms VALUES ('Alpha')")
    with pytest.raises(sqlite3.IntegrityError):
        db.execute("INSERT INTO terms VALUES ('ALPHA')")


def test_utf_ci_ai_unique_constraint(db: sqlite3.Connection) -> None:
    """Verify normalized-key variants conflict under UTF_CI_AI uniqueness."""

    db.execute("CREATE TABLE terms(term TEXT UNIQUE COLLATE UTF_CI_AI)")
    db.execute("INSERT INTO terms VALUES ('ЙЁ')")
    with pytest.raises(sqlite3.IntegrityError):
        db.execute("INSERT INTO terms VALUES ('ие')")


def test_order_by_uses_utf_ci(db: sqlite3.Connection) -> None:
    """Verify root-collation ordering while making equivalent ties explicit."""

    db.execute("CREATE TABLE terms(term TEXT)")
    db.executemany(
        "INSERT INTO terms VALUES (?)", [("b",), ("A",), ("a",), ("B",)]
    )
    rows = db.execute(
        "SELECT term FROM terms "
        "ORDER BY term COLLATE UTF_CI, term COLLATE BINARY"
    ).fetchall()
    assert [row[0] for row in rows] == ["A", "a", "B", "b"]


def test_embedded_nul_is_not_a_collation_terminator(
    db: sqlite3.Connection,
) -> None:
    """Verify comparison examines characters following embedded U+0000."""

    assert db.execute(
        "SELECT ? = ? COLLATE UTF_CI", ("a\x00x", "A\x00X")
    ).fetchone()[0] == 1
    assert db.execute(
        "SELECT ? = ? COLLATE UTF_CI", ("a\x00x", "A\x00Y")
    ).fetchone()[0] == 0
    assert db.execute(
        "SELECT ? = ? COLLATE UTF_CI_AI", ("É\x00Й", "e\x00и")
    ).fetchone()[0] == 1
    assert db.execute(
        "SELECT ? = ? COLLATE UTF_CI_AI", ("É\x00Й", "e\x00к")
    ).fetchone()[0] == 0
