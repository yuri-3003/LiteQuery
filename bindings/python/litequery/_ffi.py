"""Low-level ctypes bindings to the LiteQuery shared library.

This module locates and loads liblitequery (.dll/.so/.dylib) and declares the
signatures of the C API (see include/litequery/litequery.h). Everything the rest
of the package needs goes through the `lib` object defined here. Nothing in this
module is part of the public Python API — use `litequery.connect()`.
"""

from __future__ import annotations

import ctypes
import os
import sys
from ctypes import (
    c_char_p,
    c_double,
    c_int,
    c_int64,
    c_size_t,
    c_void_p,
    POINTER,
)


def _candidate_names() -> list[str]:
    if sys.platform == "win32":
        return ["liblitequery.dll", "litequery.dll"]
    if sys.platform == "darwin":
        return ["liblitequery.dylib", "litequery.dylib"]
    return ["liblitequery.so", "litequery.so"]


def _load_library() -> ctypes.CDLL:
    """Find and load the shared library.

    Search order:
      1. $LITEQUERY_LIBRARY (an explicit path to the shared library),
      2. next to this file (where the wheel ships it),
      3. the OS default search path (ctypes.util.find_library / loader).
    """
    names = _candidate_names()

    env = os.environ.get("LITEQUERY_LIBRARY")
    if env:
        return ctypes.CDLL(env)

    here = os.path.dirname(os.path.abspath(__file__))
    for name in names:
        candidate = os.path.join(here, name)
        if os.path.exists(candidate):
            return ctypes.CDLL(candidate)

    # Fall back to the system loader by bare name.
    last_err: OSError | None = None
    for name in names:
        try:
            return ctypes.CDLL(name)
        except OSError as e:  # pragma: no cover - platform dependent
            last_err = e
    raise OSError(
        "Could not locate the LiteQuery shared library. Build it "
        "(cmake --build build --target litequery_shared) and either copy "
        f"{names[0]} next to this file or set LITEQUERY_LIBRARY to its path."
    ) from last_err


lib = _load_library()

# Opaque handle pointers.
lq_db_p = c_void_p
lq_result_p = c_void_p

# ---- Signature declarations (mirror litequery.h) ---------------------------

lib.lq_version.restype = c_char_p
lib.lq_version.argtypes = []

lib.lq_open.restype = lq_db_p
lib.lq_open.argtypes = []

lib.lq_close.restype = None
lib.lq_close.argtypes = [lq_db_p]

lib.lq_query.restype = lq_result_p
lib.lq_query.argtypes = [lq_db_p, c_char_p]

lib.lq_import_csv.restype = c_int
lib.lq_import_csv.argtypes = [
    lq_db_p, c_char_p, c_char_p, ctypes.c_char, c_int,
    POINTER(c_int64), POINTER(c_char_p),
]

lib.lq_save.restype = c_int
lib.lq_save.argtypes = [lq_db_p, c_char_p, POINTER(c_char_p)]

lib.lq_load.restype = c_int
lib.lq_load.argtypes = [lq_db_p, c_char_p, POINTER(c_char_p)]

lib.lq_result_ok.restype = c_int
lib.lq_result_ok.argtypes = [lq_result_p]

lib.lq_result_error.restype = c_char_p
lib.lq_result_error.argtypes = [lq_result_p]

lib.lq_result_column_count.restype = c_size_t
lib.lq_result_column_count.argtypes = [lq_result_p]

lib.lq_result_row_count.restype = c_size_t
lib.lq_result_row_count.argtypes = [lq_result_p]

lib.lq_result_rows_affected.restype = c_int64
lib.lq_result_rows_affected.argtypes = [lq_result_p]

lib.lq_result_elapsed_micros.restype = c_int64
lib.lq_result_elapsed_micros.argtypes = [lq_result_p]

lib.lq_result_column_name.restype = c_char_p
lib.lq_result_column_name.argtypes = [lq_result_p, c_size_t]

lib.lq_result_next.restype = c_int
lib.lq_result_next.argtypes = [lq_result_p]

lib.lq_result_reset.restype = None
lib.lq_result_reset.argtypes = [lq_result_p]

lib.lq_result_column_type.restype = c_int  # lq_type enum
lib.lq_result_column_type.argtypes = [lq_result_p, c_size_t]

lib.lq_result_is_null.restype = c_int
lib.lq_result_is_null.argtypes = [lq_result_p, c_size_t]

lib.lq_result_get_int.restype = c_int
lib.lq_result_get_int.argtypes = [lq_result_p, c_size_t, POINTER(c_int64)]

lib.lq_result_get_double.restype = c_int
lib.lq_result_get_double.argtypes = [lq_result_p, c_size_t, POINTER(c_double)]

lib.lq_result_get_bool.restype = c_int
lib.lq_result_get_bool.argtypes = [lq_result_p, c_size_t, POINTER(c_int)]

lib.lq_result_get_text.restype = c_char_p
lib.lq_result_get_text.argtypes = [lq_result_p, c_size_t]

lib.lq_result_free.restype = None
lib.lq_result_free.argtypes = [lq_result_p]

# lq_type enum values (mirror litequery.h)
LQ_TYPE_NULL = 0
LQ_TYPE_BOOL = 1
LQ_TYPE_INT = 2
LQ_TYPE_DOUBLE = 3
LQ_TYPE_TEXT = 4
