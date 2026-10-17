# SPDX-License-Identifier: MIT
"""Exercise real AirDrop HTTPS/CPIO transfer on ::1, without using any radio.

The receiver only accepts this self-test's request. Temporary keys and received
files are removed at exit; the report contains hashes, never the file contents.
Passing this check does not demonstrate iPhone discovery or wireless delivery.
"""
import argparse
import hashlib
import json
from pathlib import Path
import queue
import tempfile
import threading
import time

import engine
import sender


class Events:
    def __init__(self):
        self.closed = threading.Event()
        self.queue = queue.Queue()

    def say(self, event, **fields):
        self.queue.put({'event': event, **fields})

    def wait(self, event, seconds=30):
        deadline = time.monotonic() + seconds
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f'no {event} event')
            try:
                message = self.queue.get(timeout=remaining)
            except queue.Empty as error:
                raise TimeoutError(f'no {event} event') from error
            if message['event'] == event:
                return message


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def check(source):
    source = source.resolve(strict=True)
    if not source.is_file() or source.stat().st_size > 64 * 1024 * 1024:
        raise ValueError('choose one regular file no larger than 64 MiB')
    original_hash = digest(source)
    with tempfile.TemporaryDirectory(prefix='winawdl-loopback-') as temporary:
        root = Path(temporary)
        destination = root / 'received'
        destination.mkdir()
        keys = root / 'keys'
        args = engine.parse_args(['--loopback', '--port', '0', '--keys', str(keys),
                                  '--outdir', str(destination), '--name', 'Local self-test'])
        events = Events()
        receiver = engine.Engine(args, events)
        worker = None
        outcome = []
        try:
            receiver.start()
            events.wait('receiving')
            context = sender.client_context(str(keys))
            files = sender.outgoing([str(source)])

            def deliver():
                try:
                    outcome.append(sender.deliver('::1', receiver.settings.port, 0,
                                                   context, 'Local self-test', files))
                except Exception as error:
                    outcome.append({'error': str(error)})

            worker = threading.Thread(target=deliver, daemon=True)
            worker.start()
            request = events.wait('asked')
            receiver.handle({'do': 'answer', 'id': request['id'], 'accept': True})
            stored = events.wait('stored')
            worker.join(timeout=30)
            if worker.is_alive() or outcome != [sender.SENT]:
                raise RuntimeError(f'sender did not finish: {outcome}')
            if len(stored['names']) != 1 or stored['names'][0] != source.name:
                raise RuntimeError('receiver stored unexpected filenames')
            received = destination / source.name
            received_hash = digest(received)
            report = {'schema': 'winawdl.loopback.v1', 'transport': 'IPv6 ::1 HTTPS',
                      'source_name': source.name, 'bytes': source.stat().st_size,
                      'source_sha256': original_hash, 'received_sha256': received_hash,
                      'matched': received_hash == original_hash,
                      'confirmation_exercised': True, 'wireless_tested': False,
                      'iphone_transfer_verified': False}
            if not report['matched']:
                raise RuntimeError('received SHA-256 differs from source')
            return report
        finally:
            receiver.stop()
            if worker is not None:
                worker.join(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    report = check(args.source)
    text = json.dumps(report, indent=2, ensure_ascii=False)
    args.report.write_text(text + '\n', encoding='utf-8')
    print(text)


if __name__ == '__main__':
    main()
