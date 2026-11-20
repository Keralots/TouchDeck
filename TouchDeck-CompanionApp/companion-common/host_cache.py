"""Cached hostname -> IPv4 lookups for the deck and panel addresses.

Windows caches no mDNS answers and takes ~2.3 s per .local lookup, so the
command listener and the stream loop must never resolve on every packet.
"""

import ipaddress
import socket
import threading
import time

TTL = 60.0         # a good answer is refreshed in the background after this
RETRY = 10.0       # a failed one sooner
MIN_REFRESH = 5.0  # forced lookups (sender IP not known) at most this often

_lock = threading.Lock()
_cache = {}  # host -> (ip or None, monotonic time of the lookup)
_busy = set()


def _is_ip(host):
    try:
        ipaddress.IPv4Address(host)
        return True
    except ValueError:
        return False


def _lookup(host):
    try:
        ip = socket.gethostbyname(host)
    except OSError:
        ip = None
    with _lock:
        _cache[host] = (ip, time.monotonic())
        _busy.discard(host)
    return ip


def resolve(host, fresh=False):
    """IPv4 address of host, or None. Only a first (or forced) lookup blocks."""
    if not host or _is_ip(host):
        return host or None
    now = time.monotonic()
    with _lock:
        entry = _cache.get(host)
        if entry is not None:
            ip, at = entry
            age = now - at
            if fresh and age >= MIN_REFRESH:
                pass  # forced lookup below
            elif age < (TTL if ip else RETRY) or host in _busy:
                return ip
            elif not fresh:
                _busy.add(host)
                threading.Thread(target=_lookup, args=(host,), daemon=True,
                                 name="resolve-" + host).start()
                return ip
    return _lookup(host)


def clear():
    with _lock:
        _cache.clear()
        _busy.clear()
