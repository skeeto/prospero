// A small HTTP router: path patterns with parameters and wildcards,
// middleware, and a few helpers for responses.

'use strict';

const METHODS = ['GET', 'HEAD', 'POST', 'PUT', 'PATCH', 'DELETE', 'OPTIONS'];

function splitPath(path) {
  const q = path.indexOf('?');
  const bare = q >= 0 ? path.slice(0, q) : path;
  return bare.split('/').filter((s) => s.length > 0);
}

function parseQuery(path) {
  const q = path.indexOf('?');
  const out = {};
  if (q < 0) {
    return out;
  }
  for (const pair of path.slice(q + 1).split('&')) {
    if (!pair) {
      continue;
    }
    const eq = pair.indexOf('=');
    const key = decodeURIComponent(eq >= 0 ? pair.slice(0, eq) : pair);
    const value = eq >= 0 ? decodeURIComponent(pair.slice(eq + 1)) : '';
    if (key in out) {
      out[key] = [].concat(out[key], value);
    } else {
      out[key] = value;
    }
  }
  return out;
}

// A pattern such as /users/:id/posts/*rest becomes a list of parts.
function compile(pattern) {
  const parts = splitPath(pattern).map((seg) => {
    if (seg.startsWith(':')) {
      return { kind: 'param', name: seg.slice(1) };
    } else if (seg.startsWith('*')) {
      return { kind: 'rest', name: seg.slice(1) || 'rest' };
    }
    return { kind: 'literal', text: seg };
  });
  let score = 0;
  for (const p of parts) {
    score += p.kind === 'literal' ? 3 : p.kind === 'param' ? 2 : 1;
  }
  return { pattern, parts, score };
}

function match(route, segs) {
  const params = {};
  for (let i = 0; i < route.parts.length; i++) {
    const p = route.parts[i];
    if (p.kind === 'rest') {
      params[p.name] = segs.slice(i).join('/');
      return params;
    }
    if (i >= segs.length) {
      return null;
    }
    if (p.kind === 'literal') {
      if (p.text !== segs[i]) {
        return null;
      }
    } else {
      params[p.name] = decodeURIComponent(segs[i]);
    }
  }
  return route.parts.length === segs.length ? params : null;
}

class Router {
  constructor() {
    this.routes = [];
    this.middleware = [];
    this.fallback = (req, res) => notFound(res);
  }

  use(fn) {
    this.middleware.push(fn);
    return this;
  }

  add(method, pattern, handler) {
    if (!METHODS.includes(method)) {
      throw new Error(`unknown method: ${method}`);
    }
    const route = compile(pattern);
    route.method = method;
    route.handler = handler;
    this.routes.push(route);
    // Most specific first, so that /users/me wins over /users/:id
    this.routes.sort((a, b) => b.score - a.score);
    return this;
  }

  get(pattern, handler) {
    return this.add('GET', pattern, handler);
  }

  post(pattern, handler) {
    return this.add('POST', pattern, handler);
  }

  find(method, path) {
    const segs = splitPath(path);
    let allowed = [];
    for (const route of this.routes) {
      const params = match(route, segs);
      if (params === null) {
        continue;
      }
      if (route.method === method || (method === 'HEAD' && route.method === 'GET')) {
        return { route, params, allowed: null };
      }
      allowed.push(route.method);
    }
    return { route: null, params: null, allowed };
  }

  handle(req, res) {
    const found = this.find(req.method, req.url);
    req.params = found.params || {};
    req.query = parseQuery(req.url);
    let index = 0;
    const next = (err) => {
      if (err) {
        return serverError(res, err);
      }
      if (index < this.middleware.length) {
        const fn = this.middleware[index++];
        return fn(req, res, next);
      }
      if (found.route) {
        return found.route.handler(req, res);
      }
      if (found.allowed.length > 0) {
        return methodNotAllowed(res, found.allowed);
      }
      return this.fallback(req, res);
    };
    return next();
  }
}

function send(res, status, body, type) {
  const text = typeof body === 'string' ? body : JSON.stringify(body);
  res.statusCode = status;
  res.setHeader('Content-Type', type || 'application/json; charset=utf-8');
  res.setHeader('Content-Length', Buffer.byteLength(text));
  res.end(text);
}

function notFound(res) {
  send(res, 404, { error: 'not found' });
}

function methodNotAllowed(res, allowed) {
  res.setHeader('Allow', Array.from(new Set(allowed)).join(', '));
  send(res, 405, { error: 'method not allowed' });
}

function serverError(res, err) {
  const message = err && err.message ? err.message : String(err);
  send(res, 500, { error: message });
}

function redirect(res, location, status) {
  res.setHeader('Location', location);
  send(res, status || 302, '', 'text/plain');
}

module.exports = { Router, compile, match, parseQuery, send, redirect };
