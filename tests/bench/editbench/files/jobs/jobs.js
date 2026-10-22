// A job queue: a binary heap by priority and time, a pool of workers
// with a concurrency limit, and retries with exponential backoff.

'use strict';

class Heap {
  constructor(less) {
    this.items = [];
    this.less = less;
  }

  get size() {
    return this.items.length;
  }

  peek() {
    return this.items[0];
  }

  push(item) {
    const a = this.items;
    a.push(item);
    let i = a.length - 1;
    while (i > 0) {
      const parent = (i - 1) >> 1;
      if (!this.less(a[i], a[parent])) {
        break;
      }
      [a[i], a[parent]] = [a[parent], a[i]];
      i = parent;
    }
  }

  pop() {
    const a = this.items;
    if (a.length === 0) {
      return undefined;
    }
    const top = a[0];
    const last = a.pop();
    if (a.length > 0) {
      a[0] = last;
      let i = 0;
      for (;;) {
        const l = 2 * i + 1;
        const r = l + 1;
        let best = i;
        if (l < a.length && this.less(a[l], a[best])) {
          best = l;
        }
        if (r < a.length && this.less(a[r], a[best])) {
          best = r;
        }
        if (best === i) {
          break;
        }
        [a[i], a[best]] = [a[best], a[i]];
        i = best;
      }
    }
    return top;
  }
}

function backoffMs(attempt, base, cap) {
  const delay = base * Math.pow(2, attempt - 1);
  return Math.min(delay, cap);
}

let nextId = 1;

class Queue {
  constructor(options = {}) {
    this.concurrency = options.concurrency || 2;
    this.maxAttempts = options.maxAttempts || 3;
    this.baseDelay = options.baseDelay || 100;
    this.maxDelay = options.maxDelay || 10000;
    this.now = options.now || (() => Date.now());
    this.timer = options.timer || setTimeout;
    this.waiting = new Heap((x, y) =>
      x.priority !== y.priority ? x.priority > y.priority : x.runAt < y.runAt);
    this.running = new Map();
    this.done = [];
    this.failed = [];
    this.listeners = {};
  }

  on(event, fn) {
    (this.listeners[event] = this.listeners[event] || []).push(fn);
    return this;
  }

  emit(event, payload) {
    for (const fn of this.listeners[event] || []) {
      fn(payload);
    }
  }

  add(fn, options = {}) {
    const job = {
      id: nextId++,
      fn,
      priority: options.priority || 0,
      runAt: this.now() + (options.delay || 0),
      attempts: 0,
      error: null,
    };
    this.waiting.push(job);
    this.emit('added', job);
    this.pump();
    return job.id;
  }

  ready() {
    const top = this.waiting.peek();
    return top !== undefined && top.runAt <= this.now();
  }

  pump() {
    while (this.running.size < this.concurrency && this.ready()) {
      const job = this.waiting.pop();
      this.start(job);
    }
    const top = this.waiting.peek();
    if (top !== undefined && this.running.size < this.concurrency) {
      const wait = Math.max(0, top.runAt - this.now());
      this.timer(() => this.pump(), wait);
    }
  }

  start(job) {
    job.attempts += 1;
    this.running.set(job.id, job);
    this.emit('start', job);
    let result;
    try {
      result = Promise.resolve(job.fn(job.attempts));
    } catch (err) {
      result = Promise.reject(err);
    }
    result.then(
      (value) => this.finish(job, null, value),
      (err) => this.finish(job, err, undefined),
    );
  }

  finish(job, err, value) {
    this.running.delete(job.id);
    if (err === null) {
      job.value = value;
      this.done.push(job);
      this.emit('done', job);
    } else if (job.attempts < this.maxAttempts) {
      job.error = err;
      job.runAt = this.now() + backoffMs(job.attempts, this.baseDelay, this.maxDelay);
      this.waiting.push(job);
      this.emit('retry', job);
    } else {
      job.error = err;
      this.failed.push(job);
      this.emit('failed', job);
    }
    this.pump();
    if (this.idle()) {
      this.emit('drain', this.stats());
    }
  }

  idle() {
    return this.running.size === 0 && this.waiting.size === 0;
  }

  stats() {
    return {
      waiting: this.waiting.size,
      running: this.running.size,
      done: this.done.length,
      failed: this.failed.length,
    };
  }
}

module.exports = { Heap, Queue, backoffMs };
