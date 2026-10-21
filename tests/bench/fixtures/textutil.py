"""Text utilities: wrapping, justifying, word statistics and slugs."""

import re
import unicodedata

WORD = re.compile(r"[A-Za-z0-9']+")
SENTENCE_END = re.compile(r"[.!?]+[\"')\]]*\s+")


def words(text):
    return WORD.findall(text)


def word_counts(text, fold=True):
    counts = {}
    for word in words(text):
        key = word.lower() if fold else word
        counts[key] = counts.get(key, 0) + 1
    return counts


def top_words(text, n=10, stop=()):
    counts = word_counts(text)
    for word in stop:
        counts.pop(word, None)
    ranked = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))
    return ranked[:n]


def sentences(text):
    parts = SENTENCE_END.split(text.strip())
    return [p.strip() for p in parts if p.strip()]


def wrap(text, width=72):
    """Greedy word wrap; a word longer than width gets a line of its own."""
    lines = []
    line = []
    length = 0
    for word in text.split():
        extra = len(word) + (1 if line else 0)
        if line and length + extra > width:
            lines.append(" ".join(line))
            line = [word]
            length = len(word)
        else:
            line.append(word)
            length += extra
    if line:
        lines.append(" ".join(line))
    return lines


def justify(line, width):
    """Spread the words of a line to fill width exactly."""
    parts = line.split()
    if len(parts) < 2:
        return line.ljust(width)
    gaps = len(parts) - 1
    spaces = width - sum(len(p) for p in parts)
    if spaces < gaps:
        return " ".join(parts)
    each, extra = divmod(spaces, gaps)
    out = []
    for i, part in enumerate(parts[:-1]):
        out.append(part)
        out.append(" " * (each + (1 if i < extra else 0)))
    out.append(parts[-1])
    return "".join(out)


def fill(text, width=72):
    lines = wrap(text, width)
    if not lines:
        return ""
    body = [justify(line, width) for line in lines[:-1]]
    body.append(lines[-1])
    return "\n".join(body)


def indent(text, prefix="    ", blank=False):
    out = []
    for line in text.splitlines():
        if line.strip() or blank:
            out.append(prefix + line)
        else:
            out.append(line)
    return "\n".join(out)


def dedent(text):
    lines = text.splitlines()
    margins = [len(l) - len(l.lstrip()) for l in lines if l.strip()]
    if not margins:
        return text
    cut = min(margins)
    return "\n".join(l[cut:] if l.strip() else l for l in lines)


def slugify(text, sep="-", limit=60):
    text = unicodedata.normalize("NFKD", text)
    text = text.encode("ascii", "ignore").decode("ascii")
    text = re.sub(r"[^A-Za-z0-9]+", sep, text).strip(sep).lower()
    if len(text) > limit:
        text = text[:limit].rstrip(sep)
    return text


def truncate(text, limit, ellipsis="..."):
    if len(text) <= limit:
        return text
    keep = limit - len(ellipsis)
    if keep <= 0:
        return ellipsis[:limit]
    cut = text.rfind(" ", 0, keep + 1)
    if cut < keep // 2:
        cut = keep
    return text[:cut].rstrip() + ellipsis


def reading_time(text, per_minute=220):
    count = len(words(text))
    minutes = count / per_minute
    if minutes < 1:
        return "under a minute"
    return f"{round(minutes)} min read"


def columns(rows, gap=2):
    """Align rows of cells into columns, each as wide as its widest cell."""
    if not rows:
        return ""
    widths = [0] * max(len(r) for r in rows)
    for row in rows:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], len(str(cell)))
    out = []
    for row in rows:
        cells = [str(c).ljust(widths[i]) for i, c in enumerate(row)]
        out.append((" " * gap).join(cells).rstrip())
    return "\n".join(out)


def ngrams(text, n=2):
    ws = [w.lower() for w in words(text)]
    return [tuple(ws[i:i + n]) for i in range(len(ws) - n + 1)]


def similarity(a, b):
    """Jaccard similarity of the word sets of two texts."""
    sa = set(w.lower() for w in words(a))
    sb = set(w.lower() for w in words(b))
    if not sa and not sb:
        return 1.0
    return len(sa & sb) / len(sa | sb)
