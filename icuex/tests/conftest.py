"""Shared SQL-only fixtures for built-in and loadable icuex testing.

By default, connections are untouched and therefore verify automatic built-in
registration. If ``ICUEX_EXTENSION`` names a compiled shared library, each
fresh connection loads it through SQLite's public extension-loading interface.
Neither mode registers SQL functions or collations from Python.
"""

from __future__ import annotations

import os
import sqlite3
from collections.abc import Iterator
from pathlib import Path

import pytest


def _connect(database: str | Path) -> sqlite3.Connection:
    """Open one connection and optionally load the configured icuex library."""

    connection = sqlite3.connect(database)
    extension = os.environ.get("ICUEX_EXTENSION")
    if extension:
        try:
            connection.enable_load_extension(True)
            connection.load_extension(extension)
            connection.enable_load_extension(False)
        except Exception:
            connection.close()
            raise
    return connection


@pytest.fixture
def db() -> Iterator[sqlite3.Connection]:
    """Yield a fresh built-in or explicitly loaded in-memory connection."""

    connection = _connect(":memory:")
    try:
        yield connection
    finally:
        connection.close()


@pytest.fixture
def connect():
    """Return the test suite's connection constructor.

    In built-in mode this proves automatic registration on independently
    opened connections. In loadable mode it consistently loads the configured
    shared library into each connection.
    """

    return _connect
