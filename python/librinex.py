"""Python wrapper for the RINEX parsing library via ctypes."""

import ctypes
import numpy as np
from pathlib import Path

_lib = None

def _load_lib():
    global _lib
    if _lib is not None:
        return _lib

    base = Path(__file__).resolve().parent.parent
    candidates = [
        base / "+debug" / "librinex.dylib",
        base / "+release" / "librinex.dylib",
        base / "+debug" / "librinex.so",
        base / "+release" / "librinex.so",
    ]
    for path in candidates:
        if path.exists():
            _lib = ctypes.CDLL(str(path))
            break
    else:
        raise FileNotFoundError(
            f"Cannot find librinex shared library. "
            f"Searched: {[str(p) for p in candidates]}"
        )

    c_int = ctypes.c_int
    c_char = ctypes.c_char
    c_char_p = ctypes.c_char_p
    c_void_p = ctypes.c_void_p
    c_double_p = ctypes.POINTER(ctypes.c_double)
    c_int64_p = ctypes.POINTER(ctypes.c_int64)

    _lib.rb_load.argtypes = [c_char_p, ctypes.POINTER(c_char_p)]
    _lib.rb_load.restype = c_void_p

    _lib.rb_free.argtypes = [c_void_p]
    _lib.rb_free.restype = None

    _lib.rb_epoch_count.argtypes = [c_void_p]
    _lib.rb_epoch_count.restype = c_int

    _lib.rb_rinex_version.argtypes = [c_void_p]
    _lib.rb_rinex_version.restype = c_int

    _lib.rb_interval.argtypes = [c_void_p]
    _lib.rb_interval.restype = c_int

    _lib.rb_n_obs.argtypes = [c_void_p, c_char]
    _lib.rb_n_obs.restype = c_int

    _lib.rb_obs_name.argtypes = [c_void_p, c_char, c_int, c_char_p]
    _lib.rb_obs_name.restype = None

    _lib.rb_list_satellites.argtypes = [c_void_p, c_char, c_char_p, c_int]
    _lib.rb_list_satellites.restype = c_int

    _lib.rb_extract_obs.argtypes = [c_void_p, c_char_p, c_int, c_int64_p, c_int]
    _lib.rb_extract_obs.restype = c_int

    _lib.rb_extract_lli.argtypes = [c_void_p, c_char_p, c_int, c_char_p, c_int]
    _lib.rb_extract_lli.restype = c_int

    _lib.rb_extract_epochs_sod.argtypes = [c_void_p, c_double_p, c_int]
    _lib.rb_extract_epochs_sod.restype = c_int

    return _lib


class RinexFile:
    """Load and query a RINEX observation file."""

    def __init__(self, filename):
        lib = _load_lib()
        self._lib = lib
        err = ctypes.c_char_p()
        self._handle = lib.rb_load(str(filename).encode(), ctypes.byref(err))
        if not self._handle:
            detail = err.value.decode() if err.value else "unknown error"
            raise RuntimeError(f"Failed to load {filename}: {detail}")
        self.n_epochs = lib.rb_epoch_count(self._handle)
        self.rinex_version = lib.rb_rinex_version(self._handle)
        self.interval = lib.rb_interval(self._handle)

    def __del__(self):
        if hasattr(self, "_handle") and self._handle:
            self._lib.rb_free(self._handle)
            self._handle = None

    def close(self):
        if self._handle:
            self._lib.rb_free(self._handle)
            self._handle = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def obs_codes(self, sys="G"):
        """Return list of observation code strings for a constellation."""
        n = self._lib.rb_n_obs(self._handle, sys.encode())
        codes = []
        buf = ctypes.create_string_buffer(4)
        for i in range(n):
            self._lib.rb_obs_name(self._handle, sys.encode(), i, buf)
            codes.append(buf.value.decode())
        return codes

    def satellites(self, sys="G"):
        """Return list of satellite ID strings (e.g. ['G01', 'G02', ...])."""
        max_sats = 100
        buf = ctypes.create_string_buffer(4 * max_sats)
        n = self._lib.rb_list_satellites(
            self._handle, sys.encode(), buf, max_sats
        )
        ids = []
        for i in range(n):
            sat = buf[i * 4 : i * 4 + 3].decode()
            ids.append(sat)
        return ids

    def get_obs(self, sat_id, obs_idx):
        """Extract observation column as a numpy int64 array.

        Missing observations are INT64_MIN. Use `get_obs_masked()` for
        a masked array instead.
        """
        buf = np.empty(self.n_epochs, dtype=np.int64)
        ret = self._lib.rb_extract_obs(
            self._handle,
            sat_id.encode(),
            obs_idx,
            buf.ctypes.data_as(ctypes.POINTER(ctypes.c_int64)),
            self.n_epochs,
        )
        if ret < 0:
            raise ValueError(f"Satellite {sat_id} not found")
        if ret == 0:
            return np.full(self.n_epochs, np.iinfo(np.int64).min, dtype=np.int64)
        return buf

    def get_obs_masked(self, sat_id, obs_idx):
        """Extract observation column as a numpy masked array.

        Missing observations are masked out.
        """
        raw = self.get_obs(sat_id, obs_idx)
        return np.ma.masked_equal(raw, np.iinfo(np.int64).min)

    def get_epochs_sod(self):
        """Extract epoch timestamps as seconds-of-day."""
        buf = np.empty(self.n_epochs, dtype=np.float64)
        self._lib.rb_extract_epochs_sod(
            self._handle,
            buf.ctypes.data_as(ctypes.POINTER(ctypes.c_double)),
            self.n_epochs,
        )
        return buf
