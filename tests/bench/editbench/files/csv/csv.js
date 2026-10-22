// CSV reading and writing per RFC 4180, with a choice of delimiter,
// quoted fields across lines, and a header row turned into objects.

'use strict';

const DEFAULTS = { delimiter: ',', quote: '"', header: true, trim: false };

function parse(text, options = {}) {
  const opts = Object.assign({}, DEFAULTS, options);
  const rows = [];
  let row = [];
  let field = '';
  let quoted = false;
  let i = 0;
  if (text.charCodeAt(0) === 0xfeff) {
    i = 1; // a byte order mark
  }
  while (i < text.length) {
    const c = text[i];
    if (quoted) {
      if (c === opts.quote) {
        if (text[i + 1] === opts.quote) {
          field += opts.quote;
          i += 2;
          continue;
        }
        quoted = false;
        i += 1;
        continue;
      }
      field += c;
      i += 1;
      continue;
    }
    if (c === opts.quote && field.length === 0) {
      quoted = true;
    } else if (c === opts.delimiter) {
      row.push(finishField(field, opts));
      field = '';
    } else if (c === '\n' || c === '\r') {
      row.push(finishField(field, opts));
      rows.push(row);
      row = [];
      field = '';
      if (c === '\r' && text[i + 1] === '\n') {
        i += 1;
      }
    } else {
      field += c;
    }
    i += 1;
  }
  if (quoted) {
    throw new Error('unterminated quoted field');
  }
  if (field.length > 0 || row.length > 0) {
    row.push(finishField(field, opts));
    rows.push(row);
  }
  return opts.header ? toObjects(rows) : rows;
}

function finishField(field, opts) {
  return opts.trim ? field.trim() : field;
}

function toObjects(rows) {
  if (rows.length === 0) {
    return [];
  }
  const names = uniqueNames(rows[0]);
  const out = [];
  for (let r = 1; r < rows.length; r++) {
    const row = rows[r];
    if (row.length === 1 && row[0] === '') {
      continue; // a blank line
    }
    const obj = {};
    for (let c = 0; c < names.length; c++) {
      obj[names[c]] = c < row.length ? row[c] : '';
    }
    out.push(obj);
  }
  return out;
}

// Header names made unique: a second "name" becomes "name_2".
function uniqueNames(header) {
  const seen = new Map();
  return header.map((name) => {
    const base = name || 'column';
    const n = (seen.get(base) || 0) + 1;
    seen.set(base, n);
    return n === 1 ? base : `${base}_${n}`;
  });
}

function needsQuotes(text, opts) {
  return text.includes(opts.delimiter) || text.includes(opts.quote) ||
    text.includes('\n') || text.includes('\r') || text !== text.trim();
}

function formatField(value, opts) {
  if (value === null || value === undefined) {
    return '';
  }
  const text = String(value);
  if (!needsQuotes(text, opts)) {
    return text;
  }
  const doubled = text.split(opts.quote).join(opts.quote + opts.quote);
  return opts.quote + doubled + opts.quote;
}

function stringify(records, options = {}) {
  const opts = Object.assign({}, DEFAULTS, options);
  if (records.length === 0) {
    return '';
  }
  const lines = [];
  let columns = null;
  if (!Array.isArray(records[0])) {
    columns = options.columns || Object.keys(records[0]);
    lines.push(columns.map((c) => formatField(c, opts)).join(opts.delimiter));
  }
  for (const rec of records) {
    const cells = columns ? columns.map((c) => rec[c]) : rec;
    lines.push(cells.map((v) => formatField(v, opts)).join(opts.delimiter));
  }
  return lines.join('\r\n') + '\r\n';
}

function sniff(sample) {
  const candidates = [',', ';', '\t', '|'];
  let best = ',';
  let bestScore = -1;
  const lines = sample.split(/\r?\n/).slice(0, 10).filter((l) => l.length > 0);
  for (const d of candidates) {
    const counts = lines.map((l) => l.split(d).length - 1);
    if (counts.length === 0 || counts[0] === 0) {
      continue;
    }
    const steady = counts.every((n) => n === counts[0]);
    const score = (steady ? 100 : 0) + counts[0];
    if (score > bestScore) {
      best = d;
      bestScore = score;
    }
  }
  return best;
}

module.exports = { parse, stringify, sniff, formatField };
