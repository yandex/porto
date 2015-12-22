import termios
import os
import sys
from ctypes import CDLL, Structure, byref
from ctypes import c_ushort, c_int, c_char_p
from ctypes.util import find_library

__all__ = ["make_tty"]


LIBC = CDLL(find_library("c"))

IOCTL = LIBC.ioctl
IOCTL.restype = c_int

GRANTPT = LIBC.grantpt
GRANTPT.restype = c_int
GRANTPT.argtypes = [c_int]

UNLOCKPT = LIBC.unlockpt
UNLOCKPT.restype = c_int
UNLOCKPT.argtypes = [c_int]

PTSNAME = LIBC.ptsname
PTSNAME.restype = c_char_p
PTSNAME.argtypes = [c_int]


class Winsize(Structure):
    """wrapper class for "struct winsize" from <asm-generic/termios.h>"""
    _fields_ = [
        ("ws_row", c_ushort),
        ("ws_col", c_ushort),
        ("ws_xpixel", c_ushort),
        ("ws_ypixel", c_ushort),
    ]


def copy_winsize(fromfd, tofd):
    ws = Winsize()
    res = LIBC.ioctl(fromfd, termios.TIOCGWINSZ, byref(ws))
    if res == 0:
        LIBC.ioctl(tofd, termios.TIOCSWINSZ, byref(ws))


def make_tty():
    ptm = os.open("/dev/ptmx", os.O_RDWR)
    copy_winsize(sys.stdin.fileno(), ptm)

    if GRANTPT(ptm) != 0:
        raise Exception("Can't open pseudoterminal")

    if UNLOCKPT(ptm) != 0:
        raise Exception("Can't open pseudoterminal")

    slavept = PTSNAME(ptm)

    return ptm, slavept
