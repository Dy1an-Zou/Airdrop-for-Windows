# SPDX-License-Identifier: MIT
"""Keeps identities out of what the receiver prints, for a recording or a
shared screen: addresses become numbered devices, and this computer's name and
the account's folder are replaced."""
import getpass
import ipaddress
import os
import re
import socket
import threading

# Anything made of hex digits and colons; _key decides whether it is an address.
TOKEN = re.compile(r'(?<![\w:])[0-9A-Fa-f:]*:[0-9A-Fa-f:]*(?:%\w+)?')
MAC = re.compile(r'(?:[0-9A-Fa-f]{1,2}:){5}[0-9A-Fa-f]{1,2}')


def _key(address):
    """One spelling per address, or None for text that is not one."""
    if MAC.fullmatch(address):
        return ':'.join(f'{int(part, 16):02x}' for part in address.split(':'))
    try:
        return ipaddress.IPv6Address(address).compressed
    except ValueError:
        return None


def local_words():
    """This computer's name, and the account's folder and name."""
    home = os.path.expanduser('~')
    return [(socket.gethostname().split('.')[0], 'this-pc'), (home, '~'),
            (home.replace('\\', '/'), '~'), (getpass.getuser(), 'user')]


class Redactor:
    """words are (text, replacement) pairs, matched as whole words in any case."""

    def __init__(self, words=()):
        # Longest first, so a folder wins over the account name inside it.
        pairs = sorted(((text, shown) for text, shown in words if text),
                       key=lambda pair: -len(pair[0]))
        self._shown = {text.lower(): shown for text, shown in pairs}
        self._words = re.compile(
            '|'.join(rf'(?<!\w){re.escape(text)}(?!\w)' for text, _ in pairs),
            re.IGNORECASE) if pairs else None
        self._labels = {}
        self._devices = 0
        self._lock = threading.Lock()

    def name(self, address, label):
        """Show one address under a name of its own."""
        self._labels[_key(address)] = label

    def _label(self, match):
        token = match.group(0)
        address = token.split('%')[0]
        # A colon that ends a clause is not part of the address before it.
        tail = ':' if address.endswith(':') and not address.endswith('::') else ''
        key = _key(address[:len(address) - len(tail)])
        if key is None:
            return token
        with self._lock:
            if key not in self._labels:
                self._devices += 1
                self._labels[key] = f'device-{self._devices}'
            return self._labels[key] + tail

    def hide(self, text):
        if self._words:
            text = self._words.sub(lambda match: self._shown[match.group(0).lower()], text)
        return TOKEN.sub(self._label, text)


class Stream:
    """A text stream that redacts what is written to it. It holds each line
    back until it is complete, so that no address goes out in two halves."""

    def __init__(self, stream, hide):
        self._stream = stream
        self._hide = hide
        self._pending = ''
        self._lock = threading.Lock()

    def write(self, text):
        with self._lock:
            *lines, self._pending = (self._pending + text).split('\n')
            for line in lines:
                self._stream.write(self._hide(line) + '\n')
        return len(text)

    def flush(self):
        self._stream.flush()

    def __getattr__(self, name):
        return getattr(self._stream, name)
