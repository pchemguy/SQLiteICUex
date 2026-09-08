"""Exercise case-fold/lowercase edge cases through every normalization mode."""

from __future__ import annotations

import sqlite3

import pytest


MODES = ("NFC", "NFD", "NFKC", "NFKD", "NFKC_CF", "NFKD_CF_STRIP")

CASES = [
    ("Straße", ("Straße", "Straße", "Straße", "Straße", "strasse", "strasse")),
    ("ß", ("ß", "ß", "ß", "ß", "ss", "ss")),
    ("ẞ", ("ẞ", "ẞ", "ẞ", "ẞ", "ss", "ss")),
    ("ΟΣ", ("ΟΣ", "ΟΣ", "ΟΣ", "ΟΣ", "οσ", "οσ")),
    ("ς", ("ς", "ς", "ς", "ς", "σ", "σ")),
    ("Σ", ("Σ", "Σ", "Σ", "Σ", "σ", "σ")),
    ("ſ", ("ſ", "ſ", "s", "s", "s", "s")),
    ("ﬀ", ("ﬀ", "ﬀ", "ff", "ff", "ff", "ff")),
    ("ﬁ", ("ﬁ", "ﬁ", "fi", "fi", "fi", "fi")),
    ("ﬂ", ("ﬂ", "ﬂ", "fl", "fl", "fl", "fl")),
    ("ﬃ", ("ﬃ", "ﬃ", "ffi", "ffi", "ffi", "ffi")),
    ("ﬄ", ("ﬄ", "ﬄ", "ffl", "ffl", "ffl", "ffl")),
    ("ﬅ", ("ﬅ", "ﬅ", "st", "st", "st", "st")),
    ("ﬆ", ("ﬆ", "ﬆ", "st", "st", "st", "st")),
    ("ŉ", ("ŉ", "ŉ", "ʼn", "ʼn", "ʼn", "ʼn")),
    ("İ", ("İ", "I\u0307", "İ", "I\u0307", "i\u0307", "i")),
    (
        "ИЙЕЁЬЪ",
        (
            "ИЙЕЁЬЪ",
            "ИИ\u0306ЕЕ\u0308ЬЪ",
            "ИЙЕЁЬЪ",
            "ИИ\u0306ЕЕ\u0308ЬЪ",
            "ийеёьъ",
            "ииееьъ",
        ),
    ),
]

NORMALIZATION_CASES = [
    pytest.param(source, mode, expected, id=f"U+{ord(source[0]):04X}-{mode}")
    for source, expected_by_mode in CASES
    for mode, expected in zip(MODES, expected_by_mode, strict=True)
]


@pytest.mark.parametrize(
    ("source", "mode", "expected"), NORMALIZATION_CASES
)
def test_casefold_edge_cases_across_every_normalization_mode(
    db: sqlite3.Connection, source: str, mode: str, expected: str
) -> None:
    """Require exact stable output for every edge-case/mode combination."""

    actual = db.execute(
        "SELECT str_normalize(?, ?)", (source, mode)
    ).fetchone()[0]
    assert actual == expected


@pytest.mark.parametrize(
    ("source", "expected_by_mode"),
    [pytest.param(source, expected, id=f"U+{ord(source[0]):04X}") for source, expected in CASES],
)
def test_normalization_modes_preserve_their_expected_distinctions(
    db: sqlite3.Connection,
    source: str,
    expected_by_mode: tuple[str, ...],
) -> None:
    """Assert both equal and unequal mode relationships for each edge case."""

    actual = tuple(
        db.execute("SELECT str_normalize(?, ?)", (source, mode)).fetchone()[0]
        for mode in MODES
    )
    assert actual == expected_by_mode

    for left_index, left in enumerate(MODES):
        for right_index, right in enumerate(MODES[left_index + 1 :], left_index + 1):
            if expected_by_mode[left_index] == expected_by_mode[right_index]:
                assert actual[left_index] == actual[right_index], f"{left} != {right}"
            else:
                assert actual[left_index] != actual[right_index], f"{left} == {right}"
