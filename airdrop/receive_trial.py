"""One confirmed incoming transfer, ordinary-user trial, bounded to ten minutes.

JSON stdin accepts only answer/quit. No discovery wakeup, outbound send or take-all.
The administrator supervisor owns the adapter and its firewall rules.
"""
import argparse
import ctypes
import json
import logging
from pathlib import Path
import queue
import sys
import threading
import time
import engine


class TrialEvents(engine.Events):
    def __init__(self, stream):
        super().__init__(stream)
        self.stored_at = None

    def say(self, event, **fields):
        super().say(event, **fields)
        print(json.dumps({'event': event, **fields}, ensure_ascii=False), flush=True)
        if event == 'stored':
            self.stored_at = time.monotonic()


def answer(receiver, command, accepted):
    if command.get('do') != 'answer' or type(command.get('accept')) is not bool or not isinstance(command.get('id'), str):
        raise ValueError('Only answer with an id and a boolean accept, or quit, is allowed')
    if command['accept'] and accepted is not None:
        raise ValueError('This trial already accepted one transfer')
    if receiver.incoming.answer(command['id'], command['accept']) and command['accept']:
        return command['id']
    return accepted


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trial', type=Path, required=True)
    parser.add_argument('--outdir', type=Path, required=True)
    parser.add_argument('--keys', type=Path, required=True)
    args = parser.parse_args()
    if ctypes.windll.shell32.IsUserAnAdmin():
        raise RuntimeError('File receiver must run without administrator rights')
    base = args.trial
    ready = base.with_name(base.stem + '-ready.json')
    stop = base.with_name(base.stem + '-stop.signal')
    done = base.with_name(base.stem + '-runtime.json')
    record = json.loads(ready.read_text(encoding='utf-8'))
    if record['interface'] != 'awdl0' or record['port'] != 8771 or Path(record['stop_file']) != stop:
        raise RuntimeError('Unexpected trial readiness record')
    # The supervisor writes its report before installing the driver, too.
    # Existence alone therefore means neither completion nor restoration.
    state = json.loads(base.read_text(encoding='utf-8')) if base.exists() else {}
    if stop.exists() or done.exists() or state.get('restored') or state.get('error'):
        raise RuntimeError('Trial already ended')
    args.outdir.mkdir(parents=True, exist_ok=False)
    args.keys.mkdir(parents=True, exist_ok=False)
    event_path = base.with_name(base.stem + '-receiver.jsonl')
    log_path = base.with_name(base.stem + '-receiver.log')
    commands = queue.Queue()
    def read_input():
        for line in sys.stdin:
            commands.put(line)
        commands.put('{"do":"quit"}')
    threading.Thread(target=read_input, daemon=True).start()
    with event_path.open('x', encoding='utf-8') as events_file, log_path.open('x', encoding='utf-8') as log:
        logging.basicConfig(stream=log, level=logging.INFO)
        events = TrialEvents(events_file)
        receiver = None
        try:
            settings = engine.parse_args(['--name', 'Airdrop for Windows', '--iface', 'awdl0', '--port', '8771',
                '--outdir', str(args.outdir), '--keys', str(args.keys), '--max-receive-percent', '1'])
            receiver = engine.Engine(settings, events)
            receiver.start()
            accepted = None
            deadline = time.monotonic() + 600
            while time.monotonic() < deadline and not done.exists() and not stop.exists():
                if events.stored_at is not None and time.monotonic() - events.stored_at > 2:
                    break
                try:
                    command = json.loads(commands.get(timeout=.25))
                    if not isinstance(command, dict): raise ValueError('JSON object required')
                    if command.get('do') == 'quit': break
                    accepted = answer(receiver, command, accepted)
                except queue.Empty:
                    pass
                except (ValueError, TypeError) as error:
                    events.say('error', reason=str(error))
        finally:
            try:
                if receiver is not None: receiver.stop()
            finally:
                stop.touch(exist_ok=True)
    return 0


if __name__ == '__main__': raise SystemExit(main())
