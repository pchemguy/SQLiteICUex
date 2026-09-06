"""Exercise icuex in persistent schemas, indexes, and hardened connections."""

from __future__ import annotations

import sqlite3
from pathlib import Path


def test_functions_work_with_trusted_schema_disabled(
    db: sqlite3.Connection,
) -> None:
    """Use deterministic innocuous functions in generated and indexed schema."""

    db.execute("PRAGMA trusted_schema = OFF")
    db.execute(
        "CREATE TABLE terms("
        "source TEXT, "
        "folded TEXT GENERATED ALWAYS AS (str_casefold(source)) STORED, "
        "search_key TEXT GENERATED ALWAYS AS ("
        "str_normalize(source, 'NFKD_CF_STRIP')"
        ") STORED"
        ")"
    )
    db.execute(
        "CREATE INDEX terms_nfkc_cf "
        "ON terms(str_normalize(source, 'NFKC_CF'))"
    )
    db.execute("INSERT INTO terms(source) VALUES ('Straße É')")

    row = db.execute(
        "SELECT folded, search_key FROM terms"
    ).fetchone()
    assert row == ("strasse é", "strasse e")

    plan = db.execute(
        "EXPLAIN QUERY PLAN "
        "SELECT source FROM terms "
        "WHERE str_normalize(source, 'NFKC_CF') = ?",
        ("strasse é",),
    ).fetchall()
    assert any("terms_nfkc_cf" in row[-1] for row in plan)


def test_declared_collation_index_is_used(db: sqlite3.Connection) -> None:
    """Verify an indexed UTF_CI lookup without relying on full plan wording."""

    db.execute("CREATE TABLE terms(term TEXT COLLATE UTF_CI)")
    db.execute("CREATE INDEX terms_utf_ci ON terms(term)")
    db.executemany(
        "INSERT INTO terms VALUES (?)", [("Alpha",), ("Beta",), ("Gamma",)]
    )

    plan = db.execute(
        "EXPLAIN QUERY PLAN SELECT term FROM terms WHERE term = ?", ("ALPHA",)
    ).fetchall()
    assert any("terms_utf_ci" in row[-1] for row in plan)
    assert db.execute(
        "SELECT term FROM terms WHERE term = ?", ("ALPHA",)
    ).fetchone()[0] == "Alpha"


def test_utf_ci_ai_index_is_used_for_normalized_lookup(
    db: sqlite3.Connection,
) -> None:
    """Use a declared UTF_CI_AI index for a normalized-key equality lookup."""

    db.execute("CREATE TABLE search_terms(term TEXT COLLATE UTF_CI_AI)")
    db.execute("CREATE INDEX search_terms_ai ON search_terms(term)")
    db.executemany(
        "INSERT INTO search_terms VALUES (?)", [("ЙЁ",), ("École",), ("Beta",)]
    )

    plan = db.execute(
        "EXPLAIN QUERY PLAN "
        "SELECT term FROM search_terms WHERE term = ?",
        ("ие",),
    ).fetchall()
    assert any("search_terms_ai" in row[-1] for row in plan)
    assert db.execute(
        "SELECT term FROM search_terms WHERE term = ?", ("ие",)
    ).fetchone()[0] == "ЙЁ"


def test_file_schema_reopens_without_setup_sql(connect, tmp_path: Path) -> None:
    """Use collations and generated expressions immediately after reopening."""

    path = tmp_path / "persistent.db"
    first = connect(path)
    try:
        first.execute("PRAGMA trusted_schema = OFF")
        first.execute(
            "CREATE TABLE terms("
            "term TEXT COLLATE UTF_CI UNIQUE, "
            "key TEXT GENERATED ALWAYS AS ("
            "str_normalize(term, 'NFKD_CF_STRIP')"
            ") STORED"
            ")"
        )
        first.execute("INSERT INTO terms(term) VALUES ('Ёж')")
        first.commit()
    finally:
        first.close()

    second = connect(path)
    try:
        second.execute("PRAGMA trusted_schema = OFF")
        assert second.execute(
            "SELECT term, key FROM terms WHERE term = ?", ("ёЖ",)
        ).fetchone() == ("Ёж", "еж")
        second.execute("INSERT INTO terms(term) VALUES ('École')")
        second.commit()
    finally:
        second.close()


def test_parameterized_statement_on_fresh_connection(connect) -> None:
    """Prepare and execute SQL using icuex without prior initialization SQL."""

    connection = connect(":memory:")
    try:
        row = connection.execute(
            "SELECT str_casefold(?), str_normalize(?, ?), "
            "? = ? COLLATE UTF_CI",
            ("Straße", "É", "NFKD_CF_STRIP", "Ё", "ё"),
        ).fetchone()
        assert row == ("strasse", "e", 1)
    finally:
        connection.close()
