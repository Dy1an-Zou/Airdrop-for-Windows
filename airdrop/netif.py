# SPDX-License-Identifier: MIT
"""Finds the AWDL adapter that awdld creates on Windows."""
import ctypes
import socket
import time

POLL_SECONDS = 0.5


def interface_index(alias):
    """The index of the adapter with this name, or None when there is none.
    socket.if_nametoindex wants Windows' internal name, not the one shown."""
    iphlpapi = ctypes.WinDLL('iphlpapi')
    luid = ctypes.c_uint64()
    index = ctypes.c_ulong()
    if iphlpapi.ConvertInterfaceAliasToLuid(ctypes.c_wchar_p(alias), ctypes.byref(luid)):
        return None
    if iphlpapi.ConvertInterfaceLuidToIndex(ctypes.byref(luid), ctypes.byref(index)):
        return None
    return index.value


def wait_for_interface(alias, seconds):
    """Index and address of the adapter once it has both, or None if that
    takes longer than `seconds`. It gets its address a moment after it appears."""
    deadline = time.monotonic() + seconds
    while True:
        index = interface_index(alias)
        address = link_local_address(index) if index else None
        if address:
            return index, address
        if time.monotonic() >= deadline:
            return None
        time.sleep(POLL_SECONDS)


def link_local_address(index):
    """The address Windows sends from on this interface, or None if it has none.
    Connecting a datagram socket picks the source address and sends nothing."""
    with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as probe:
        try:
            probe.connect(('ff02::fb', 5353, 0, index))
        except OSError:
            return None
        return probe.getsockname()[0].split('%')[0]
