# SPDX-License-Identifier: MIT
"""The TLS identity this receiver presents: a self-signed certificate, which
is all a sender needs when the receiver is visible to everyone."""
import os
import shutil
import subprocess

CERTIFICATE = 'certificate.self-signed.pem'
KEY = 'key.self-signed.pem'
VALID_DAYS = '365'
# Passed explicitly because some Windows builds of OpenSSL point at a default
# configuration file that does not exist. Senders do not look at the name.
REQUEST_CONFIG = '''[req]
distinguished_name = subject
prompt = no
[subject]
CN = AirDrop receiver
'''
# Git for Windows ships OpenSSL without putting it on PATH.
GIT_OPENSSL = (('mingw64', 'bin', 'openssl.exe'), ('usr', 'bin', 'openssl.exe'))


class IdentityError(Exception):
    pass


def default_keys_dir():
    base = os.environ.get('LOCALAPPDATA') or os.path.expanduser('~')
    return os.path.join(base, 'winawdl', 'keys')


def find_openssl():
    found = shutil.which('openssl')
    if found:
        return found
    git = shutil.which('git')
    if git:
        root = os.path.dirname(os.path.dirname(os.path.realpath(git)))
        for parts in GIT_OPENSSL:
            candidate = os.path.join(root, *parts)
            if os.path.isfile(candidate):
                return candidate
    raise IdentityError('OpenSSL not found: put openssl on PATH or install Git for Windows')


def ensure_self_signed(keys_dir):
    """Return the certificate and key paths, creating the pair on first use."""
    certificate = os.path.join(keys_dir, CERTIFICATE)
    key = os.path.join(keys_dir, KEY)
    if os.path.isfile(certificate) and os.path.isfile(key):
        return certificate, key
    os.makedirs(keys_dir, exist_ok=True)
    config = os.path.join(keys_dir, 'request.cnf')
    with open(config, 'w', encoding='ascii') as out:
        out.write(REQUEST_CONFIG)
    try:
        run = subprocess.run(
            [find_openssl(), 'req', '-config', config, '-newkey', 'rsa:2048',
             '-nodes', '-x509', '-days', VALID_DAYS, '-keyout', key,
             '-out', certificate],
            capture_output=True, text=True)
    finally:
        os.unlink(config)
    if run.returncode != 0:
        raise IdentityError(f'openssl could not create a certificate: {run.stderr.strip()}')
    return certificate, key
