"""Probe ext/icu/icu.c lower() without making it an icuex requirement.

The two-argument ``lower(text, locale)`` overload is supplied by SQLite's
separate ICU extension, not by icuex. Missing or divergent behavior therefore
emits a visible warning instead of failing this package's acceptance suite.
"""

from __future__ import annotations

import sqlite3
import warnings

import pytest

from test_casefold import CASEFOLD_DIVERGENCES


class IcuLowerBehaviorWarning(UserWarning):
    """Report that ext/icu/icu.c lower() behavior was not confirmed."""


LOWER_EQUIVALENCES = [
    pytest.param("ASCII", "ascii", id="ascii"),
    pytest.param("Σ", "σ", id="nonfinal-sigma"),
    pytest.param("ИЙЕЁЬЪ", "ийеёьъ", id="cyrillic"),
]

_ICU_LOWER_UNAVAILABLE_REPORTED = False


def _icu_root_lower(
    db: sqlite3.Connection, source: str
) -> str | None:
    """Return ICU root lowercase or warn when icu.c is absent/unusable."""

    global _ICU_LOWER_UNAVAILABLE_REPORTED

    try:
        return db.execute(
            "SELECT lower(?, 'root')", (source,)
        ).fetchone()[0]
    except sqlite3.Error as error:
        if not _ICU_LOWER_UNAVAILABLE_REPORTED:
            warnings.warn(
                "ext/icu/icu.c lower() behavior not confirmed: the "
                "two-argument lower(text, 'root') overload is unavailable "
                f"({error})",
                IcuLowerBehaviorWarning,
                stacklevel=2,
            )
            _ICU_LOWER_UNAVAILABLE_REPORTED = True
        return None


def _warn_if_icu_assertions_fail(description: str, check) -> None:
    """Run explicit assertions, converting only their failures to warnings."""

    try:
        check()
    except AssertionError as error:
        detail = str(error) or "assertion failed"
        warnings.warn(
            "ext/icu/icu.c lower() behavior not confirmed for "
            f"{description}: {detail}",
            IcuLowerBehaviorWarning,
            stacklevel=2,
        )


@pytest.mark.parametrize(
    ("source", "expected_fold", "expected_lower"), CASEFOLD_DIVERGENCES
)
def test_icu_lower_divergences_are_documented_without_gating_icuex(
    db: sqlite3.Connection,
    source: str,
    expected_fold: str,
    expected_lower: str,
) -> None:
    """Assert expected lower results and inequalities as warning-only probes."""

    folded = db.execute("SELECT str_casefold(?)", (source,)).fetchone()[0]
    assert folded == expected_fold
    assert folded != expected_lower

    lowered = _icu_root_lower(db, source)
    if lowered is None:
        return

    def check() -> None:
        assert lowered == expected_lower, (
            f"lower({source!r}, 'root') returned {lowered!r}; "
            f"expected {expected_lower!r}"
        )
        assert lowered != expected_fold, (
            f"lower({source!r}, 'root') unexpectedly equals the full-fold "
            f"result {expected_fold!r}"
        )
        assert lowered != folded, (
            f"lower({source!r}, 'root') unexpectedly equals str_casefold()"
        )

    _warn_if_icu_assertions_fail(repr(source), check)


@pytest.mark.parametrize(("source", "expected"), LOWER_EQUIVALENCES)
def test_icu_lower_and_casefold_expected_equivalences(
    db: sqlite3.Connection, source: str, expected: str
) -> None:
    """Probe cases where ICU root lower and default full fold should agree."""

    folded = db.execute("SELECT str_casefold(?)", (source,)).fetchone()[0]
    assert folded == expected

    lowered = _icu_root_lower(db, source)
    if lowered is None:
        return

    def check() -> None:
        assert lowered == expected, (
            f"lower({source!r}, 'root') returned {lowered!r}; "
            f"expected {expected!r}"
        )
        assert lowered == folded, (
            f"lower({source!r}, 'root') differs from str_casefold(): "
            f"{lowered!r} != {folded!r}"
        )

    _warn_if_icu_assertions_fail(repr(source), check)
