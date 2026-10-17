# SPDX-License-Identifier: MIT
"""AirDrop sender for Windows: wakes nearby receivers over Bluetooth, finds
them on the AWDL adapter and uploads files to one of them. The receiver has
to be visible to everyone."""
import argparse
import http.client
import logging
import os
import plistlib
import queue
import secrets
import socket
import ssl
import subprocess
import sys
import time
import threading
import uuid
from typing import NamedTuple

import cpio
import identity
import mdns
import netif
import transfer

SERVICE_TYPE = '_airdrop._tcp.local'
DEFAULT_INTERFACE = 'awdl0'
DEFAULT_MODEL = 'MacBookPro18,3'
INTERFACE_WAIT_SECONDS = 20
DEFAULT_SEARCH_SECONDS = 30
# Once one receiver has answered, how long the others get to show up.
SETTLE_SECONDS = 3
CONNECT_SECONDS = 10
CONNECT_ATTEMPTS = 3
# The receiver's owner has this long to accept.
ASK_SECONDS = 90
UPLOAD_IDLE_SECONDS = 30
IO_BLOCK_BYTES = 64 * 1024
PROGRESS_SECONDS = 2
MIB = 1024 * 1024
HEADERS = {'User-Agent': 'AirDrop/1.0', 'Connection': 'keep-alive'}
# A receiver picks the app that takes a file by this type.
UNIFORM_TYPES = {
    '.jpg': 'public.jpeg', '.jpeg': 'public.jpeg', '.png': 'public.png',
    '.heic': 'public.heic', '.gif': 'com.compuserve.gif',
    '.mov': 'com.apple.quicktime-movie', '.mp4': 'public.mpeg-4',
    '.m4v': 'com.apple.m4v-video', '.mp3': 'public.mp3', '.pdf': 'com.adobe.pdf',
    '.txt': 'public.plain-text', '.zip': 'public.zip-archive',
}
DEFAULT_UNIFORM_TYPE = 'public.data'
BEACON_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             'airdrop-beacon.ps1')
BEACON_READY = 'advertising'
BEACON_START_SECONDS = 12
BEACON_LIFETIME_SECONDS = 600
MAX_FILES = 1000


class Cancelled(Exception):
    """The caller cancelled before the next network or archive operation."""


class Unreachable(Exception):
    pass


class Unwilling(Exception):
    """The receiver would not have this computer's certificate."""


def refuses_certificate(error):
    """Whether a receiver ended the TLS handshake over the certificate it was
    shown. An iPhone has been seen to, with "certificate unknown"; it is
    probably what one visible to its contacts only does, since nothing vouches
    for a certificate made here."""
    reason = getattr(error, 'reason', None) or ''
    return (isinstance(error, ssl.SSLError) and 'ALERT' in reason
            and ('CERTIFICATE' in reason or 'UNKNOWN_CA' in reason))


class Outgoing(NamedTuple):
    path: str
    name: str
    size: int
    mtime: float


class Connection(http.client.HTTPConnection):
    """HTTPS to a link-local address, which is only complete with the
    interface it is on."""

    def __init__(self, address, port, scope, context, timeout):
        super().__init__(address, port, timeout=timeout)
        self.scope = scope
        self.context = context
        self.pending_socket = None
        self.checkpoint = lambda: None

    def connect(self):
        sock = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
        self.pending_socket = sock
        try:
            self.checkpoint()
            sock.settimeout(self.timeout)
            sock.connect((self.host, self.port, 0, self.scope))
            # Publish the TLS socket before its blocking handshake, so cancel
            # can interrupt both a TCP connect and a TLS handshake.
            self.sock = self.context.wrap_socket(sock, do_handshake_on_connect=False)
            self.pending_socket = self.sock
            self.checkpoint()
            self.sock.do_handshake()
            self.checkpoint()
        except (OSError, Cancelled):
            sock.close()
            self.close()
            raise
        finally:
            self.pending_socket = None


class Beacon:
    """The Bluetooth advertisement that makes Apple devices nearby turn AWDL
    on and announce AirDrop. Windows PowerShell sends it: it reaches the
    Bluetooth API with nothing installed."""

    def __init__(self, script=BEACON_SCRIPT, stop=None):
        self.script = script
        self.process = None
        self.on = False
        self.said = ''  # why it is off
        self.stop = stop or threading.Event()

    def __enter__(self):
        shell = os.path.join(os.environ.get('SYSTEMROOT', r'C:\Windows'), 'System32',
                             'WindowsPowerShell', 'v1.0', 'powershell.exe')
        try:
            if not os.path.isfile(self.script):
                raise FileNotFoundError('Bluetooth beacon helper is missing')
            # A PC that never ran a script of its own refuses script files; this
            # one is the project's, and the setting holds for this process only.
            self.process = subprocess.Popen(
                [shell, '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
                 '-WindowStyle', 'Hidden', '-File', self.script,
                 '-ParentId', str(os.getpid()), '-LifetimeSeconds', str(BEACON_LIFETIME_SECONDS)],
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True,
                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            answer = queue.Queue(maxsize=1)

            def first_line():
                try:
                    answer.put(self.process.stdout.readline().strip())
                except (OSError, ValueError):
                    answer.put('Bluetooth helper closed before startup')

            threading.Thread(target=first_line, name='beacon-ready', daemon=True).start()
            deadline = time.monotonic() + BEACON_START_SECONDS
            said = 'Bluetooth startup timed out'
            while time.monotonic() < deadline and not self.stop.is_set():
                try:
                    said = answer.get(timeout=0.1)
                    break
                except queue.Empty:
                    pass
            if self.stop.is_set():
                said = 'search cancelled'
        except OSError as error:
            said = str(error)
        self.on = said == BEACON_READY
        self.said = '' if self.on else said or 'no answer'
        if self.on:
            logging.info('Bluetooth beacon on')
        else:
            self._close()
            logging.warning('no Bluetooth beacon (%s): a receiver shows up only while '
                            'its own share sheet is open', said or 'no answer')
        return self

    def __exit__(self, *_):
        self._close()

    def _close(self):
        if self.process:
            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=2)
            if self.process.stdout:
                self.process.stdout.close()


class Watch:
    """What a program that shows a transfer is told. This one says nothing."""

    def connected(self, connection):
        """Closing `connection` from another thread stops the transfer."""

    def checkpoint(self):
        """Raise Cancelled to stop before the next operation."""

    def accepted(self, total):
        pass

    def progress(self, done):
        pass

    def uploaded(self):
        """Everything is on its way; the receiver has yet to confirm it."""


class Progress:
    def __init__(self, total, watch=None):
        self.total = total
        self.done = 0
        self.started = time.monotonic()
        self._last = self.started
        self._watch = watch or Watch()

    def note(self, size):
        self.done += size
        self._watch.progress(self.done)
        now = time.monotonic()
        if now - self._last >= PROGRESS_SECONDS:
            self._last = now
            logging.info('sent %d%% at %.1f MB/s', self.done * 100 // max(self.total, 1),
                         self.rate())

    def rate(self):
        return self.done / MIB / max(time.monotonic() - self.started, 0.001)


def client_context(keys_dir):
    certificate, key = identity.ensure_self_signed(keys_dir)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    # A receiver visible to everyone presents a certificate nothing vouches
    # for, and takes ours the same way.
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    context.load_cert_chain(certificate, key)
    return context


class Unsendable(SystemExit):
    """Why a list of paths cannot go out; `kind` names the reason for a
    program, the message for a person."""

    def __init__(self, kind, message):
        super().__init__(message)
        self.kind = kind


def outgoing(paths):
    """The files to send, refusing what an archive cannot hold apart."""
    if not isinstance(paths, list) or not paths or len(paths) > MAX_FILES:
        raise Unsendable('invalid_files', 'choose between 1 and 1000 files')
    files = []
    for path in paths:
        if not isinstance(path, str) or not path or '\0' in path:
            raise Unsendable('invalid_files', 'each file must have a valid path')
        if not os.path.isfile(path):
            raise Unsendable('not_a_file', f'{path} is not a file')
        status = os.stat(path)
        files.append(Outgoing(path, os.path.basename(path), status.st_size, status.st_mtime))
    names = [file.name.lower() for file in files]
    if len(set(names)) != len(names):
        raise Unsendable('same_name', 'two of the files have the same name')
    return files


def post_plist(connection, path, value):
    connection.request('POST', path, body=plistlib.dumps(value, fmt=plistlib.FMT_BINARY),
                       headers={**HEADERS, 'Content-Type': 'application/octet-stream'})
    response = connection.getresponse()
    return response.status, response.read()


def outline(body):
    """What a plist answer holds, for the trace."""
    try:
        return transfer.outline(plistlib.loads(body))
    except ValueError:
        return f'<{len(body)} bytes that are no plist>'


def receiver_identity(address, port, scope, context):
    """(name, model) the receiver here gives for itself; None from one that
    keeps its name for its contacts. The model may be None. Unwilling is
    raised by one that refuses the certificate, Unreachable by one that
    cannot be talked to for now."""
    connection = Connection(address, port, scope, context, CONNECT_SECONDS)
    try:
        status, body = post_plist(connection, '/Discover', {})
        if status != 200:
            return None
        answer = plistlib.loads(body)
        if not isinstance(answer, dict):
            return None
        name = answer.get('ReceiverComputerName')
        model = answer.get('ReceiverModelName')
        if not isinstance(name, str) or not name.strip() or len(name) > 256:
            return None
        return name, model if isinstance(model, str) and len(model) <= 256 else None
    except (OSError, http.client.HTTPException) as error:
        if refuses_certificate(error):
            raise Unwilling(str(error)) from None
        raise Unreachable(str(error) or type(error).__name__) from None
    except ValueError:
        return None
    finally:
        connection.close()


def receiver_name(address, port, scope, context):
    identity = receiver_identity(address, port, scope, context)
    return identity[0] if identity else None


def find_receivers(browser, scope, context, wanted, seconds):
    """{name: (address, port)} of the receivers that answer with a name. Stops
    at the first whose name contains `wanted`, if that is given."""
    named = {}
    attempts = {}
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        for instance, (address, port) in browser.poll().items():
            if attempts.get(instance, 0) >= CONNECT_ATTEMPTS:
                continue
            try:
                name = receiver_name(address, port, scope, context)
            except Unwilling:
                attempts[instance] = CONNECT_ATTEMPTS
                logging.info('a receiver at %s refuses this computer\'s certificate: it '
                             'probably takes files from its contacts only', address)
                continue
            except Unreachable as error:
                attempts[instance] = attempts.get(instance, 0) + 1
                logging.info('a receiver at %s does not answer yet: %s', address, error)
                continue
            attempts[instance] = CONNECT_ATTEMPTS
            if name is None:
                logging.info('a receiver at %s is visible to its contacts only', address)
                continue
            logging.info('found %s', name)
            named[name] = (address, port)
            if wanted and wanted.lower() in name.lower():
                return {name: (address, port)}
            deadline = min(deadline, time.monotonic() + SETTLE_SECONDS)
    return named


def choose(named, wanted):
    if wanted:
        named = {name: where for name, where in named.items()
                 if wanted.lower() in name.lower()}
    if not named:
        raise SystemExit('no receiver found: on the iPhone set AirDrop to '
                         '"Everyone for 10 Minutes" and keep its screen on')
    if len(named) > 1:
        raise SystemExit('several receivers: pick one with --to: ' + ', '.join(sorted(named)))
    return next(iter(named.items()))


def ask_body(sender_name, files, transfer_id):
    """The request to send, with the keys an iPhone's own has, less its Apple
    identity and its preview picture."""
    return {
        'TransferID': {'id': transfer_id},
        'TransferType': {'files': {}},
        'SenderID': secrets.token_hex(6),
        'BundleID': 'com.apple.finder',
        'SenderComputerName': sender_name,
        'SenderModelName': DEFAULT_MODEL,
        'Items': [],
        'Files': [{
            'FileName': file.name,
            'FileType': UNIFORM_TYPES.get(os.path.splitext(file.name)[1].lower(),
                                          DEFAULT_UNIFORM_TYPE),
            'FileSize': file.size,
            'FileBomPath': f'./{file.name}',
            'FileIsDirectory': False,
            'ShouldConvertMediaFormats': False,
        } for file in files],
        'ConvertMediaFormats': False,
    }


def _blocks(path):
    with open(path, 'rb') as source:
        while block := source.read(IO_BLOCK_BYTES):
            yield block


def _members(files):
    # An iPhone's archive starts with the folder its files are named in.
    yield '.', cpio.DIRECTORY_MODE, 0, time.time(), ()
    for file in files:
        yield f'./{file.name}', cpio.FILE_MODE, file.size, file.mtime, _blocks(file.path)


def _archive(files, progress):
    for piece in cpio.write_archive(_members(files)):
        progress._watch.checkpoint()
        progress.note(len(piece))
        yield piece


def archive_body(files, progress):
    """The upload: the cpio archive of the files, in DVZip blocks."""
    return transfer.encode_dvzip(_archive(files, progress))


def archive_bytes(files):
    """How long the archive of these files is, which is what a transfer's
    progress counts up to."""
    return cpio.archive_length([('.', 0)] + [(f'./{file.name}', file.size) for file in files])


SENT, DECLINED, REFUSED = 'sent', 'declined', 'refused'


def deliver(address, port, scope, context, sender_name, files, watch=None):
    """Ask the receiver, and upload once its owner accepts. Returns SENT,
    DECLINED, or REFUSED for an upload the receiver did not take."""
    watch = watch or Watch()
    connection = Connection(address, port, scope, context, CONNECT_SECONDS)
    try:
        transfer_id = str(uuid.uuid4()).upper()
        connection.checkpoint = watch.checkpoint
        watch.connected(connection)
        watch.checkpoint()
        connection.connect()
        watch.checkpoint()
        connection.sock.settimeout(ASK_SECONDS)
        status, answer = post_plist(connection, '/Ask',
                                    ask_body(sender_name, files, transfer_id))
        logging.debug('answer to the ask: status %d, %s', status, outline(answer))
        watch.checkpoint()
        if status != 200:
            logging.warning('declined (status %d)', status)
            return DECLINED
        total = sum(file.size for file in files)
        logging.info('accepted; sending %d file(s), %.1f MB', len(files), total / MIB)
        watch.accepted(total)
        watch.checkpoint()
        connection.sock.settimeout(UPLOAD_IDLE_SECONDS)
        progress = Progress(total, watch)
        # The receiver ties the upload to what it accepted by the transfer's
        # identifier.
        connection.request('POST', '/Upload', body=archive_body(files, progress),
                           headers={**HEADERS, 'TotalBytes': str(total),
                                    'Content-Type': 'application/x-dvzip',
                                    'TransferID': transfer_id},
                           encode_chunked=True)
        watch.uploaded()
        response = connection.getresponse()
        response.read()
        watch.checkpoint()
        if response.status != 200:
            logging.warning('the receiver refused the upload (status %d)', response.status)
            return REFUSED
        logging.info('sent, at %.1f MB/s', progress.rate())
        return SENT
    finally:
        connection.close()


def send(address, port, scope, context, sender_name, files):
    """Whether the files went through."""
    return deliver(address, port, scope, context, sender_name, files) == SENT


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files', nargs='+', help='the files to send')
    parser.add_argument('--to', help='part of the receiver\'s name, when several answer')
    parser.add_argument('--name', default=socket.gethostname().split('.')[0],
                        help='the name the receiver sees (default: the computer name)')
    parser.add_argument('--search', type=int, default=DEFAULT_SEARCH_SECONDS,
                        help='how many seconds to look for receivers')
    parser.add_argument('--trace', action='store_true',
                        help='also print what the receiver answers')
    parser.add_argument('--no-beacon', action='store_true',
                        help='do not advertise over Bluetooth')
    parser.add_argument('--iface', default=DEFAULT_INTERFACE)
    parser.add_argument('--keys', default=identity.default_keys_dir(),
                        help='directory holding the TLS certificate and key')
    return parser.parse_args(argv)


def run(settings, files, context, scope):
    browser = mdns.Browser(SERVICE_TYPE, scope)
    try:
        named = find_receivers(browser, scope, context, settings.to, settings.search)
        if not named:
            logging.info('%d mDNS answers came in over AWDL', browser.heard)
    finally:
        browser.close()
    name, (address, port) = choose(named, settings.to)
    logging.info('asking %s; accept on the device', name)
    try:
        return send(address, port, scope, context, settings.name, files)
    except TimeoutError:
        logging.warning('%s did not answer in time', name)
    except (OSError, http.client.HTTPException) as error:
        logging.warning('the connection to %s broke: %s', name, error or type(error).__name__)
    return False


def main(argv=None):
    settings = parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if settings.trace else logging.INFO,
                        stream=sys.stdout,
                        format='%(asctime)s %(levelname)s %(message)s')
    files = outgoing(settings.files)
    try:
        context = client_context(settings.keys)
    except identity.IdentityError as error:
        raise SystemExit(str(error)) from None
    found = netif.wait_for_interface(settings.iface, INTERFACE_WAIT_SECONDS)
    if not found:
        raise SystemExit(f'interface {settings.iface} is not up: start '
                         'scripts\\awdl-run.ps1 first')
    scope, _ = found
    if settings.no_beacon:
        return 0 if run(settings, files, context, scope) else 1
    with Beacon():
        return 0 if run(settings, files, context, scope) else 1


if __name__ == '__main__':
    sys.exit(main())
