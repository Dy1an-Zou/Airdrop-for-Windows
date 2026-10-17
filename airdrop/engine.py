# SPDX-License-Identifier: MIT
"""The AirDrop receiver and sender as one process for a program to drive: it
reads one JSON command a line and writes one JSON event a line, and its log
goes to the error stream. It receives for as long as the AWDL adapter is up,
and looks for receivers only while told to, because looking wakes every Apple
device in Bluetooth range.

Commands: {"do": "look", "on": true}, {"do": "send", "id": ..., "peer": ...,
"files": [...]}, {"do": "cancel", "id": ...}, {"do": "answer", "id": ...,
"accept": true}, {"do": "set", "name": ..., "folder": ..., "ask": ...},
{"do": "quit"}. The end of the input is a quit as well."""
import argparse
import contextlib
import errno
import http.client
import itertools
import ipaddress
import json
import logging
import secrets
import socket
import sys
import threading
import time

import identity
import mdns
import netif
import receiver
import sender
import transfer

PROTOCOL = 1
LINK_POLL_SECONDS = 1
# A receiver that has not been announced for this long has left.
PEER_SILENCE_SECONDS = 20
# How often a receiver that gave no name yet is asked again, and how often
# one given on the command line is checked.
PROBE_PAUSE_SECONDS = 2
# One that will not talk to this computer is asked again less often: only
# its owner opening it to everyone changes the answer.
UNWILLING_PAUSE_SECONDS = 10
PROGRESS_GAP_SECONDS = 0.1
ASK_POLL_SECONDS = 0.25
# A sender that neither leaves nor gets an answer is given up on after this.
ASK_LONGEST_SECONDS = 300
# An accepted sender that uploads nothing is given up on after this.
UPLOAD_DUE_SECONDS = 30
BIND_RETRY_SECONDS = 5
LOOPBACK = ('::1', 0)


class Events:
    """Writes events for the driving program. `closed` is set when it has
    stopped reading."""

    def __init__(self, stream):
        self._stream = stream
        self._lock = threading.Lock()
        self.closed = threading.Event()

    def say(self, event, **fields):
        line = json.dumps({'event': event, **fields}, ensure_ascii=False)
        with self._lock:
            try:
                self._stream.write(line + '\n')
                self._stream.flush()
            except (OSError, ValueError):
                self.closed.set()


class Settings:
    """What the receiver reads on every request, so a change applies to the
    next one."""

    def __init__(self, args):
        self.name = args.name
        self.model = args.model
        self.dest = args.dest
        self.ask = args.ask
        self.port = args.port
        self.flags = args.flags
        self.max_receive_percent = args.max_receive_percent
        self.redact = False
        self.interface_index = 0

    def public(self):
        return {'name': self.name, 'folder': self.dest, 'ask': self.ask}


class Accepted:
    """One incoming transfer from its ask to its last file."""

    def __init__(self, key, total):
        self.key = key
        self.total = total
        self.folder = None
        self.uploading = False
        self.said = 0.0


def announced_files(ask):
    files = []
    for entry in ask.get('Files') or []:
        size = entry.get('FileSize')
        files.append({
            'name': transfer.safe_name(str(entry.get('FileName') or ''), 'file'),
            'size': size if isinstance(size, int) and size >= 0 else None,
            'type': str(entry.get('FileType') or ''),
        })
    return files


class Incoming(receiver.Watcher):
    """Turns what the receiver sees into events, and holds a sender's ask
    until the driving program answers it."""

    def __init__(self, events, settings):
        self.events = events
        self.settings = settings
        self._lock = threading.Lock()
        self._waiting = {}
        self._keys = (f'r{number}' for number in itertools.count(1))

    def _open(self, sender_name, model, files, links, waits):
        with self._lock:
            key = next(self._keys)
        sizes = [file['size'] for file in files]
        total = sum(sizes) if sizes and None not in sizes else None
        self.events.say('asked', id=key, sender=sender_name, model=model, files=files,
                        links=links, total=total, waits=waits)
        return Accepted(key, total)

    def asked(self, ask, address, left):
        kinds = ask.get('TransferType') or {}
        links = sum(1 for item in ask.get('Items') or [] if transfer.is_web_link(item)) \
            if 'links' in kinds else 0
        waits = self.settings.ask
        accepted = self._open(ask.get('SenderComputerName'), ask.get('SenderModelName'),
                              announced_files(ask), links, waits)
        how = self._decision(accepted.key, left) if waits else 'accepted'
        self.events.say('answered', id=accepted.key, how=how)
        if how != 'accepted':
            return None
        if not links:
            due = threading.Timer(UPLOAD_DUE_SECONDS, self._overdue, [accepted])
            due.daemon = True
            due.start()
        return accepted

    def _decision(self, key, left):
        slot = {'made': threading.Event(), 'accept': False}
        with self._lock:
            self._waiting[key] = slot
        try:
            deadline = time.monotonic() + ASK_LONGEST_SECONDS
            while time.monotonic() < deadline:
                if slot['made'].wait(ASK_POLL_SECONDS):
                    return 'accepted' if slot['accept'] else 'declined'
                if left():
                    return 'left'
            return 'expired'
        finally:
            with self._lock:
                del self._waiting[key]

    def answer(self, key, accept):
        with self._lock:
            slot = self._waiting.get(key)
        if slot:
            slot['accept'] = accept
            slot['made'].set()
        return bool(slot)

    def _overdue(self, accepted):
        if not accepted.uploading:
            accepted.uploading = True
            self.events.say('lost', id=accepted.key, reason='nothing arrived')

    def upload_started(self, accepted, tid, total):
        if accepted is None:
            # Asking first means nothing comes in that was not asked about.
            if self.settings.ask:
                return None
            accepted = self._open(None, None, [], 0, False)
            self.events.say('answered', id=accepted.key, how='accepted')
        elif accepted.uploading:
            return None
        accepted.uploading = True
        accepted.total = total or accepted.total
        accepted.folder = self.settings.dest
        self.events.say('incoming', id=accepted.key, done=0, total=accepted.total)
        return accepted

    def upload_progress(self, token, done):
        now = time.monotonic()
        if now - token.said < PROGRESS_GAP_SECONDS:
            return
        token.said = now
        # The archive is a little longer than the files in it.
        shown = min(done, token.total) if token.total else done
        self.events.say('incoming', id=token.key, done=shown, total=token.total)

    def stored(self, token, names):
        self.events.say('stored', id=token.key, folder=token.folder or self.settings.dest,
                        names=names, live=transfer.live_photos(names))

    def upload_failed(self, token, reason):
        self.events.say('lost', id=token.key, reason=reason)


class Receiving:
    """The receiver and its announcement, for as long as the link is up."""

    def __init__(self, events, settings, watcher, context, loopback):
        self.events = events
        self.settings = settings
        self.watcher = watcher
        self.context = context
        self.loopback = loopback
        self.server = None
        self.thread = None
        self.announcer = None
        self._said = None

    def _say(self, on, reason=None):
        if (on, reason) != self._said:
            self._said = (on, reason)
            self.events.say('receiving', on=on, reason=reason, port=self.settings.port)

    def start(self, index, address):
        """Whether the receiver runs now. A port held by another program is
        reported and tried again by the caller."""
        if self.server:
            return True
        self.settings.interface_index = index
        try:
            server = receiver.Receiver(self.settings.port, self.settings, self.watcher)
        except OSError as error:
            held = error.errno == errno.EADDRINUSE
            self._say(False, 'port_held' if held else str(error))
            return False
        self.settings.port = server.server_address[1]
        server.socket = self.context.wrap_socket(server.socket, server_side=True,
                                                 do_handshake_on_connect=False)
        self.server = server
        self.thread = threading.Thread(target=server.serve_forever, name='receiver',
                                       daemon=True)
        self.thread.start()
        if not self.loopback:
            service_id = secrets.token_hex(6)
            try:
                self.announcer = mdns.Announcer(
                    mdns.Service(receiver.SERVICE_TYPE, service_id, f'{service_id}.local',
                                 address, self.settings.port, f'flags={self.settings.flags}'),
                    index)
                self.announcer.start()
            except OSError as error:
                self.stop()
                self._say(False, str(error))
                return False
        logging.info('receiving as %s on port %d into %s', self.settings.name,
                     self.settings.port, self.settings.dest)
        self._say(True)
        return True

    def stop(self, reason=None):
        if self.announcer:
            self.announcer.stop()
            self.announcer = None
        if self.server:
            self.server.shutdown()
            self.thread.join()
            self.server.reset_sessions()
            self.server.server_close()
            self.server = None
            self._say(False, reason)


class Peer:
    def __init__(self, address, port):
        self.address = address
        self.port = port
        self.name = None
        self.model = None
        self.probing = False
        self.next_probe = 0.0
        self.shy = False  # answers only devices it knows


class Looking:
    """Finds receivers while told to: wakes them over Bluetooth, hears them
    announce themselves and asks each for its name."""

    def __init__(self, events, context, fixed):
        self.events = events
        self.context = context
        self.fixed = fixed  # receivers given on the command line, for a test
        self.wanted = threading.Event()
        self._lock = threading.Lock()
        self._peers = {}
        self._thread = None
        self._scope = None
        self._address = None
        self._stop = threading.Event()
        self._shy_said = 0

    def peer(self, key):
        with self._lock:
            peer = self._peers.get(key)
            return peer if peer and peer.name else None

    def link(self, scope, address=None):
        """Called with the adapter's index when the link comes up, and with
        None when it goes down."""
        self._halt()
        self._scope = scope
        self._address = address
        if scope is not None and self.wanted.is_set():
            self._begin()

    def look(self, on):
        if on == self.wanted.is_set():
            return
        if on:
            self.wanted.set()
            if self._scope is not None:
                self._begin()
        else:
            self.wanted.clear()
            self._halt()

    def _begin(self):
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, args=(self._scope, self._stop),
                                        name='looking', daemon=True)
        self._thread.start()

    def _halt(self):
        if not self._thread:
            return
        self._stop.set()
        self._thread.join()
        self._thread = None
        with self._lock:
            named = [key for key, peer in self._peers.items() if peer.name]
            self._peers.clear()
        for key in named:
            self.events.say('peer_gone', id=key)
        self._say_shy(0)
        self.events.say('looking', on=False)

    def _run(self, scope, stop):
        if self.fixed is not None:
            self.events.say('looking', on=True, beacon=False, why='test receivers')
            while not stop.wait(mdns.QUERY_PERIOD_S):
                self._reconcile(self.fixed, scope, stop)
            return
        try:
            browser = mdns.Browser(receiver.SERVICE_TYPE, scope)
        except OSError as error:
            logging.warning('cannot look for receivers: %s', error)
            self.events.say('looking', on=False, why=str(error))
            return
        try:
            with sender.Beacon(stop=stop) as beacon:
                if stop.is_set():
                    return
                self.events.say('looking', on=True, beacon=beacon.on, why=beacon.said)
                while not stop.is_set():
                    browser.poll()
                    self._reconcile(browser.instances.devices(PEER_SILENCE_SECONDS),
                                    scope, stop)
        finally:
            browser.close()

    def _reconcile(self, located, scope, stop):
        now = time.monotonic()
        gone, probes = [], []
        with self._lock:
            for key in [key for key in self._peers if key not in located]:
                if self._peers.pop(key).name:
                    gone.append(key)
            for key, (address, port) in located.items():
                if self.fixed is None and address == self._address:
                    continue
                if self.fixed is None:
                    try:
                        if not ipaddress.IPv6Address(address).is_link_local or not 1 <= port <= 65535:
                            continue
                    except (ValueError, TypeError):
                        continue
                peer = self._peers.get(key)
                if peer is None or (peer.address, peer.port) != (address, port):
                    if peer and peer.name:
                        gone.append(key)
                    peer = self._peers[key] = Peer(address, port)
                if not peer.name and not peer.probing and now >= peer.next_probe:
                    peer.probing = True
                    probes.append((key, peer))
            shy = sum(1 for peer in self._peers.values() if peer.shy)
        for key in gone:
            self.events.say('peer_gone', id=key)
        for key, peer in probes:
            threading.Thread(target=self._probe, args=(key, peer, scope, stop),
                             name='probe', daemon=True).start()
        self._say_shy(shy)

    def _say_shy(self, shy):
        if shy != self._shy_said:
            self._shy_said = shy
            self.events.say('unwilling', count=shy)

    def _probe(self, key, peer, scope, stop):
        try:
            found = sender.receiver_identity(peer.address, peer.port, scope, self.context)
        except sender.Unwilling as error:
            logging.info('a receiver at %s refuses this computer: %s', peer.address, error)
            found = None
        except sender.Unreachable as error:
            logging.info('a receiver at %s does not answer yet: %s', peer.address, error)
            found = False
        with self._lock:
            peer.probing = False
            # None is a receiver that keeps its name for those it knows, or
            # would not have our certificate at all.
            peer.shy = found is None
            peer.next_probe = time.monotonic() + (
                UNWILLING_PAUSE_SECONDS if peer.shy else PROBE_PAUSE_SECONDS)
            still_there = self._peers.get(key) is peer and not stop.is_set()
            if found and still_there:
                peer.name, peer.model = found
        if found and still_there:
            self.events.say('peer', id=key, name=peer.name, model=peer.model)


class SendWatch(sender.Watch):
    def __init__(self, events, key, total):
        self.events = events
        self.key = key
        self.total = total
        self.cancelled = False
        self.was_accepted = False
        self._connection = None
        self._said = 0.0

    def state(self, state, **more):
        self.events.say('send', id=self.key, state=state, total=self.total, **more)

    @property
    def reached(self):
        return self._connection is not None and self._connection.sock is not None

    def connected(self, connection):
        self._connection = connection
        if self.cancelled:
            self.cancel()

    def checkpoint(self):
        if self.cancelled:
            raise sender.Cancelled()

    def accepted(self, total):
        self.was_accepted = True
        self.state('sending', done=0)

    def progress(self, done):
        now = time.monotonic()
        if now - self._said >= PROGRESS_GAP_SECONDS:
            self._said = now
            self.state('sending', done=min(done, self.total))

    def uploaded(self):
        self.state('finishing', done=self.total)

    def cancel(self):
        self.cancelled = True
        connection = self._connection
        if connection:
            for sock in (connection.sock, getattr(connection, 'pending_socket', None)):
                if sock:
                    with contextlib.suppress(OSError):
                        sock.shutdown(socket.SHUT_RDWR)
                    with contextlib.suppress(OSError):
                        sock.close()


class Sending:
    def __init__(self, events, settings, looking, context):
        self.events = events
        self.settings = settings
        self.looking = looking
        self.context = context
        self._lock = threading.Lock()
        self._watches = {}
        self._threads = {}

    def start(self, key, peer_key, paths, scope):
        def failed(reason, detail=None):
            self.events.say('send', id=key, state='failed', reason=reason, detail=detail)

        # A rejected duplicate must not emit a terminal event for the active
        # operation, even when the duplicate's files or peer are malformed.
        with self._lock:
            if key in self._watches:
                self.events.say('error', operation='send', id=key, reason='duplicate transfer id')
                return
            if self._watches:
                failed('busy')
                return
        peer = self.looking.peer(peer_key)
        if not peer or scope is None:
            failed('gone')
            return
        try:
            files = sender.outgoing(paths)
        except sender.Unsendable as error:
            failed(error.kind, str(error))
            return
        except OSError as error:
            failed('unreadable', str(error))
            return
        watch = SendWatch(self.events, key, sender.archive_bytes(files))
        with self._lock:
            self._watches[key] = watch
            worker = threading.Thread(target=self._run,
                                      args=(watch, Peer(peer.address, peer.port), files, scope),
                                      name='send', daemon=True)
            self._threads[key] = worker
            worker.start()

    def cancel(self, key):
        with self._lock:
            watch = self._watches.get(key)
        if watch:
            watch.cancel()

    def stop(self):
        with self._lock:
            watches = list(self._watches.values())
            threads = list(self._threads.values())
        for watch in watches:
            watch.cancel()
        for thread in threads:
            thread.join(timeout=3)

    def _run(self, watch, peer, files, scope):
        watch.state('asking')
        try:
            watch.checkpoint()
            outcome = sender.deliver(peer.address, peer.port, scope, self.context,
                                     self.settings.name, files, watch)
            watch.checkpoint()
            watch.state(outcome, done=watch.total if outcome == sender.SENT else None)
        except sender.Cancelled:
            watch.state('cancelled')
        except TimeoutError:
            if watch.cancelled:
                watch.state('cancelled')
            else:
                watch.state('failed', reason='stalled' if watch.was_accepted else 'no_answer')
        except (OSError, http.client.HTTPException, ValueError) as error:
            if watch.cancelled:
                watch.state('cancelled')
            else:
                watch.state('failed', reason='broken' if watch.reached else 'unreachable',
                            detail=str(error) or type(error).__name__)
        finally:
            with self._lock:
                self._watches.pop(watch.key, None)
                self._threads.pop(watch.key, None)


class Engine:
    def __init__(self, args, events):
        self.events = events
        self.settings = Settings(args)
        self.alias = args.iface
        self.loopback = args.loopback
        self.context = receiver.tls_context(args.keys)
        self.incoming = Incoming(events, self.settings)
        self.receiving = Receiving(events, self.settings, self.incoming, self.context,
                                   args.loopback)
        self.looking = Looking(events, sender.client_context(args.keys),
                               dict(args.peer) if args.loopback else None)
        self.sending = Sending(events, self.settings, self.looking, self.looking.context)
        self.stopping = threading.Event()
        self.scope = None
        self._lock = threading.Lock()
        self._link_thread = threading.Thread(target=self._watch_link, name='link', daemon=True)

    def start(self):
        self.events.say('ready', protocol=PROTOCOL, **self.settings.public())
        self._link_thread.start()

    def _found(self):
        if self.loopback:
            return LOOPBACK[1], LOOPBACK[0]
        index = netif.interface_index(self.alias)
        address = netif.link_local_address(index) if index else None
        return (index, address) if address else None

    def _watch_link(self):
        current = None
        next_bind = 0.0
        while not self.stopping.is_set():
            found = self._found()
            if found != current:
                with self._lock:
                    self.receiving.stop()
                    self.sending.stop()
                    self.scope = found[0] if found else None
                    self.looking.link(self.scope, found[1] if found else None)
                current = found
                next_bind = 0.0
                self.events.say('link', up=bool(found))
            if found and not self.receiving.server and time.monotonic() >= next_bind:
                next_bind = time.monotonic() + BIND_RETRY_SECONDS
                self.receiving.start(*found)
            self.stopping.wait(LINK_POLL_SECONDS)

    def stop(self):
        self.stopping.set()
        self._link_thread.join()
        with self._lock:
            self.sending.stop()
            self.looking.look(False)
            self.receiving.stop()

    def set(self, command):
        refused = {}
        if 'name' in command:
            try:
                self.settings.name = receiver.valid_name(str(command['name']).strip())
            except argparse.ArgumentTypeError as error:
                refused['name'] = str(error)
        if 'folder' in command:
            try:
                self.settings.dest = receiver.valid_dir(str(command['folder']))
            except argparse.ArgumentTypeError as error:
                refused['folder'] = str(error)
        if 'ask' in command:
            self.settings.ask = bool(command['ask'])
        self.events.say('settings', refused=refused, **self.settings.public())

    def handle(self, command):
        """Carry out one command; False for the one that ends the engine."""
        if not isinstance(command, dict):
            self.events.say('error', reason='a command must be a JSON object')
            return True
        do = command.get('do')
        problem = None
        if do in ('send', 'cancel', 'answer'):
            key = command.get('id')
            if not isinstance(key, str) or not 1 <= len(key) <= 128 or any(ord(c) < 32 for c in key):
                problem = 'id must be a nonempty string of at most 128 characters'
        if do == 'send':
            peer = command.get('peer')
            paths = command.get('files')
            if not isinstance(peer, str) or not peer or len(peer) > 1024:
                problem = 'peer must be a discovered device id'
            elif not isinstance(paths, list) or len(paths) > sender.MAX_FILES or any(
                    not isinstance(path, str) or not path or '\0' in path for path in paths):
                problem = 'files must be a list of valid file paths'
        boolean_fields = {'look': ('on',), 'answer': ('accept',)}.get(do, ()) if isinstance(do, str) else ()
        if do == 'set' and 'ask' in command:
            boolean_fields = ('ask',)
        for field in boolean_fields:
            if type(command.get(field)) is not bool:
                problem = field + ' must be a boolean'
        if problem:
            self.events.say('error', operation=do, id=command.get('id') if isinstance(command.get('id'), str) else None,
                            reason=problem)
            return True
        if do == 'quit':
            return False
        if do == 'look':
            with self._lock:
                self.looking.look(command['on'])
        elif do == 'send':
            self.sending.start(command['id'], command['peer'], command['files'], self.scope)
        elif do == 'cancel':
            self.sending.cancel(str(command.get('id')))
        elif do == 'answer':
            self.incoming.answer(str(command.get('id')), bool(command.get('accept')))
        elif do == 'set':
            self.set(command)
        else:
            self.events.say('error', reason=f'unknown command {do!r}')
        return True


def fixed_peer(text):
    """A receiver to offer without looking: "[address]:port"."""
    address, _, port = text.rpartition(':')
    try:
        return f'{text}.fixed', (address.strip('[]'), int(port))
    except ValueError:
        raise argparse.ArgumentTypeError('"[address]:port"') from None


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--name', type=receiver.valid_name,
                        default=socket.gethostname().split('.')[0])
    parser.add_argument('--model', type=receiver.valid_model, default=receiver.DEFAULT_MODEL)
    parser.add_argument('--outdir', dest='dest', type=receiver.valid_dir,
                        default=receiver.valid_dir('~/Downloads'))
    parser.add_argument('--take-all', dest='ask', action='store_false',
                        help='store what anyone sends without asking')
    parser.add_argument('--max-receive-percent', type=receiver.valid_percent,
                        default=receiver.DEFAULT_MAX_RECEIVE_PERCENT)
    parser.add_argument('--iface', default=receiver.DEFAULT_INTERFACE)
    parser.add_argument('--port', type=int, default=receiver.DEFAULT_PORT)
    parser.add_argument('--flags', type=lambda text: int(text, 0), default=receiver.DEFAULT_FLAGS)
    parser.add_argument('--keys', default=identity.default_keys_dir())
    parser.add_argument('--loopback', action='store_true',
                        help='for a test without a radio: receive on this computer '
                             'alone and offer the receivers given with --peer')
    parser.add_argument('--peer', type=fixed_peer, action='append', default=[])
    parser.add_argument('--trace', action='store_true')
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        stream.reconfigure(encoding='utf-8')
    logging.basicConfig(level=logging.DEBUG if args.trace else logging.INFO,
                        stream=sys.stderr, format='%(asctime)s %(levelname)s %(message)s')
    events = Events(sys.stdout)
    try:
        engine = Engine(args, events)
    except identity.IdentityError as error:
        events.say('fatal', reason='no_certificate', detail=str(error))
        return 2
    engine.start()
    try:
        for line in sys.stdin:
            if not line.strip():
                continue
            try:
                command = json.loads(line)
                if not isinstance(command, dict):
                    raise ValueError
            except ValueError:
                events.say('error', reason='a command is one JSON object a line')
                continue
            if not engine.handle(command) or events.closed.is_set():
                break
    except KeyboardInterrupt:
        pass
    finally:
        engine.stop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
