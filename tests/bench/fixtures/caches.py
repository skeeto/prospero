"""Caches: least recently used, time to live, and a memoizing decorator."""

import functools
import time


class _Node:
    __slots__ = ("key", "value", "prev", "next", "expires")

    def __init__(self, key, value, expires=None):
        self.key = key
        self.value = value
        self.prev = None
        self.next = None
        self.expires = expires


class LRUCache:
    """A doubly linked list in recency order, plus a dict to find nodes."""

    def __init__(self, capacity):
        if capacity < 1:
            raise ValueError("capacity must be at least 1")
        self.capacity = capacity
        self.nodes = {}
        self.head = _Node(None, None)  # most recent after this
        self.tail = _Node(None, None)  # least recent before this
        self.head.next = self.tail
        self.tail.prev = self.head
        self.hits = 0
        self.misses = 0

    def __len__(self):
        return len(self.nodes)

    def __contains__(self, key):
        return key in self.nodes

    def _unlink(self, node):
        node.prev.next = node.next
        node.next.prev = node.prev

    def _push_front(self, node):
        node.next = self.head.next
        node.prev = self.head
        self.head.next.prev = node
        self.head.next = node

    def get(self, key, default=None):
        node = self.nodes.get(key)
        if node is None:
            self.misses += 1
            return default
        self.hits += 1
        self._unlink(node)
        self._push_front(node)
        return node.value

    def put(self, key, value):
        node = self.nodes.get(key)
        if node is not None:
            node.value = value
            self._unlink(node)
            self._push_front(node)
            return
        node = _Node(key, value)
        self.nodes[key] = node
        self._push_front(node)
        if len(self.nodes) > self.capacity:
            oldest = self.tail.prev
            self._unlink(oldest)
            del self.nodes[oldest.key]

    def pop(self, key, default=None):
        node = self.nodes.pop(key, None)
        if node is None:
            return default
        self._unlink(node)
        return node.value

    def keys(self):
        out = []
        node = self.head.next
        while node is not self.tail:
            out.append(node.key)
            node = node.next
        return out

    def hit_rate(self):
        total = self.hits + self.misses
        if total == 0:
            return 0.0
        return self.hits / total


class TTLCache(LRUCache):
    """An LRU cache whose entries also expire after ttl seconds."""

    def __init__(self, capacity, ttl, clock=time.monotonic):
        super().__init__(capacity)
        self.ttl = ttl
        self.clock = clock

    def get(self, key, default=None):
        node = self.nodes.get(key)
        if node is not None and node.expires <= self.clock():
            self._unlink(node)
            del self.nodes[key]
            node = None
        if node is None:
            self.misses += 1
            return default
        return super().get(key, default)

    def put(self, key, value):
        super().put(key, value)
        self.nodes[key].expires = self.clock() + self.ttl

    def purge(self):
        """Drop every expired entry; returns how many went."""
        now = self.clock()
        gone = [k for k, n in self.nodes.items() if n.expires <= now]
        for key in gone:
            self._unlink(self.nodes[key])
            del self.nodes[key]
        return len(gone)


def memoize(maxsize=128, ttl=None):
    """Cache a function's results by its arguments."""
    def decorate(fn):
        if ttl is None:
            cache = LRUCache(maxsize)
        else:
            cache = TTLCache(maxsize, ttl)
        missing = object()

        @functools.wraps(fn)
        def wrapper(*args, **kwargs):
            key = (args, tuple(sorted(kwargs.items())))
            value = cache.get(key, missing)
            if value is missing:
                value = fn(*args, **kwargs)
                cache.put(key, value)
            return value

        wrapper.cache = cache
        wrapper.clear = lambda: cache.__init__(maxsize) if ttl is None else cache.__init__(maxsize, ttl)
        return wrapper
    return decorate


def backoff(attempt, base=0.5, cap=30.0):
    """Seconds to wait before a retry: doubling, up to cap."""
    if attempt < 1:
        return 0.0
    return min(cap, base * (2 ** (attempt - 1)))


def retry(fn, attempts=5, sleep=time.sleep, retry_on=(OSError,)):
    for attempt in range(1, attempts + 1):
        try:
            return fn()
        except retry_on:
            if attempt == attempts:
                raise
            sleep(backoff(attempt))
