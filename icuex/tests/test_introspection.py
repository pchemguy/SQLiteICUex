"""Verify per-connection icuex registration through SQLite PRAGMAs."""

from __future__ import annotations

import sqlite3
from pathlib import Path


SQLITE_DETERMINISTIC = 0x000000800
SQLITE_DIRECTONLY = 0x000080000
SQLITE_SUBTYPE = 0x000100000
SQLITE_INNOCUOUS = 0x000200000
SQLITE_RESULT_SUBTYPE = 0x001000000


def _collation_names(connection: sqlite3.Connection) -> set[str]:
    """Return the SQL collation names reported for one connection."""

    return {row[1] for row in connection.execute("PRAGMA collation_list")}


def _function_rows(
    connection: sqlite3.Connection, name: str
) -> list[sqlite3.Row]:
    """Return function-list rows for one case-insensitive SQL function name."""

    connection.row_factory = sqlite3.Row
    return [
        row
        for row in connection.execute("PRAGMA function_list")
        if row["name"].casefold() == name.casefold()
    ]


def _assert_icuex_surface(connection: sqlite3.Connection) -> None:
    """Assert the exact icuex names, arities, encoding, and function flags."""

    collations = _collation_names(connection)
    assert "UTF_CI" in collations
    assert "UTF_CI_AI" in collations

    expected = {"str_casefold": 1, "str_normalize": 2}
    for name, arity in expected.items():
        rows = _function_rows(connection, name)
        matching = [row for row in rows if row["narg"] == arity]
        assert len(matching) == 1
        row = matching[0]
        assert row["type"] == "s"
        assert row["enc"].casefold() == "utf8"
        assert row["flags"] & SQLITE_DETERMINISTIC
        assert row["flags"] & SQLITE_INNOCUOUS
        assert not row["flags"] & SQLITE_DIRECTONLY
        assert not row["flags"] & SQLITE_SUBTYPE
        assert not row["flags"] & SQLITE_RESULT_SUBTYPE


def test_surface_is_present_on_fresh_connection(
    db: sqlite3.Connection,
) -> None:
    """Require all SQL features on a newly prepared test connection."""

    _assert_icuex_surface(db)


def test_surface_is_present_on_simultaneous_connections(connect) -> None:
    """Require independent registration on concurrent connections."""

    first = connect(":memory:")
    second = connect(":memory:")
    try:
        _assert_icuex_surface(first)
        _assert_icuex_surface(second)
    finally:
        first.close()
        second.close()


def test_surface_survives_file_database_reopen(
    connect, tmp_path: Path
) -> None:
    """Require registration before and after reopening a database."""

    path = tmp_path / "introspection.db"
    first = connect(path)
    try:
        _assert_icuex_surface(first)
        first.execute("CREATE TABLE marker(value TEXT)")
        first.commit()
    finally:
        first.close()

    second = connect(path)
    try:
        _assert_icuex_surface(second)
    finally:
        second.close()
