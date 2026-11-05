"""Opt-in shared macOS opens using HIDAPI's public Darwin API, no library patch."""
import sys


def shared_open(hid):
    if sys.platform != "darwin":
        return
    import ctypes
    library = ctypes.CDLL(hid.__file__)
    try:
        configure = library.hid_darwin_set_open_exclusive
    except AttributeError as e:
        raise RuntimeError("Installed HIDAPI does not export its shared-open Darwin API") from e
    configure.argtypes = [ctypes.c_int]
    configure.restype = None
    configure(0)
