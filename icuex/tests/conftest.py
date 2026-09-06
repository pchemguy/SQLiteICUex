"""Shared SQL-only fixtures for the statically integrated icuex extension.

The fixtures intentionally perform no extension loading, SQL initialization,
function registration, or collation registration. A missing SQL feature is a
test failure because automatic per-connection availability is part of the
extension contract.
"""

from __future__ import annotations

import sqlite3
from collections.abc import Iterator

import pytest


@pytest.fixture
def db() -> Iterator[sqlite3.Connection]:
    """Yield a fresh in-memory connection without extension setup SQL."""

    connection = sqlite3.connect(":memory:")
    try:
        yield connection
    finally:
        connection.close()


@pytest.fixture
def connect():
    """Return the unmodified sqlite3 connection constructor.

    Tests use this factory when they must prove that independently opened
    connections receive icuex automatically.
    """

    return sqlite3.connect

