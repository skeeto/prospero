"""A small double-entry ledger: accounts, balanced transactions, reports.

Amounts are integers in cents, so that sums are exact. Every transaction
has two or more postings whose amounts sum to zero.
"""

import datetime
import json


class LedgerError(Exception):
    pass


ACCOUNT_KINDS = ("asset", "liability", "equity", "income", "expense")

# Kinds whose balance is normally positive when debited
DEBIT_NORMAL = ("asset", "expense")


class Account:
    def __init__(self, name, kind, parent=None):
        if kind not in ACCOUNT_KINDS:
            raise LedgerError(f"unknown account kind: {kind}")
        self.name = name
        self.kind = kind
        self.parent = parent
        self.children = []
        if parent is not None:
            parent.children.append(self)

    def path(self):
        parts = []
        node = self
        while node is not None:
            parts.append(node.name)
            node = node.parent
        return ":".join(reversed(parts))

    def depth(self):
        n = 0
        node = self.parent
        while node is not None:
            n += 1
            node = node.parent
        return n


class Posting:
    def __init__(self, account, amount, memo=""):
        self.account = account
        self.amount = amount
        self.memo = memo


class Transaction:
    def __init__(self, date, description, postings):
        self.date = date
        self.description = description
        self.postings = list(postings)

    def total(self):
        return sum(p.amount for p in self.postings)

    def check(self):
        if len(self.postings) < 2:
            raise LedgerError("a transaction needs at least two postings")
        if self.total() != 0:
            raise LedgerError(f"unbalanced by {self.total()} cents: {self.description}")
        seen = set()
        for p in self.postings:
            if p.account.path() in seen:
                raise LedgerError(f"account posted twice: {p.account.path()}")
            seen.add(p.account.path())


class Ledger:
    def __init__(self):
        self.accounts = {}
        self.transactions = []

    def open(self, path, kind):
        """Open an account by its full path, creating its parents."""
        parent = None
        names = path.split(":")
        for i, name in enumerate(names):
            key = ":".join(names[:i + 1])
            account = self.accounts.get(key)
            if account is None:
                account = Account(name, kind, parent)
                self.accounts[key] = account
            parent = account
        return parent

    def account(self, path):
        account = self.accounts.get(path)
        if account is None:
            raise LedgerError(f"no such account: {path}")
        return account

    def post(self, date, description, entries):
        """entries: (path, amount) pairs; amounts in cents."""
        postings = [Posting(self.account(path), amount) for path, amount in entries]
        txn = Transaction(date, description, postings)
        txn.check()
        self.transactions.append(txn)
        return txn

    def balance(self, path, until=None):
        """The balance of an account and everything under it."""
        prefix = path + ":"
        total = 0
        for txn in self.transactions:
            if until is not None and txn.date > until:
                continue
            for p in txn.postings:
                full = p.account.path()
                if full == path or full.startswith(prefix):
                    total += p.amount
        return total

    def normal_balance(self, path, until=None):
        """The balance with the sign its kind usually shows."""
        account = self.account(path)
        amount = self.balance(path, until)
        if account.kind in DEBIT_NORMAL:
            return amount
        return -amount

    def trial_balance(self, until=None):
        """Each top-level account's balance; the sum is always zero."""
        rows = []
        for key, account in sorted(self.accounts.items()):
            if account.parent is None:
                rows.append((key, self.balance(key, until)))
        return rows

    def between(self, start, end):
        return [t for t in self.transactions if start <= t.date < end]

    def income_statement(self, start, end):
        income = 0
        expense = 0
        for txn in self.between(start, end):
            for p in txn.postings:
                if p.account.kind == "income":
                    income -= p.amount
                elif p.account.kind == "expense":
                    expense += p.amount
        return {"income": income, "expense": expense, "net": income - expense}


def format_cents(amount, width=12):
    sign = "-" if amount < 0 else ""
    whole, cents = divmod(abs(amount), 100)
    text = f"{sign}{whole:,}.{cents:02d}"
    return text.rjust(width)


def report(ledger, until=None):
    lines = []
    for key in sorted(ledger.accounts):
        account = ledger.accounts[key]
        amount = ledger.normal_balance(key, until)
        if amount == 0:
            continue
        indent = "  " * account.depth()
        lines.append(f"{indent}{account.name:<30}{format_cents(amount)}")
    return "\n".join(lines)


def monthly(ledger, path, year):
    """The change in an account's balance in each month of a year."""
    out = []
    for month in range(1, 13):
        start = datetime.date(year, month, 1)
        if month == 12:
            end = datetime.date(year + 1, 1, 1)
        else:
            end = datetime.date(year, month + 1, 1)
        change = 0
        for txn in ledger.between(start, end):
            for p in txn.postings:
                if p.account.path() == path:
                    change += p.amount
        out.append((start.strftime("%b"), change))
    return out


def dump(ledger):
    data = {
        "accounts": [[k, a.kind] for k, a in sorted(ledger.accounts.items())],
        "transactions": [
            {
                "date": t.date.isoformat(),
                "description": t.description,
                "postings": [[p.account.path(), p.amount] for p in t.postings],
            }
            for t in ledger.transactions
        ],
    }
    return json.dumps(data, indent=2)


def load(text):
    data = json.loads(text)
    ledger = Ledger()
    for path, kind in data["accounts"]:
        ledger.open(path, kind)
    for t in data["transactions"]:
        date = datetime.date.fromisoformat(t["date"])
        ledger.post(date, t["description"], [(p, a) for p, a in t["postings"]])
    return ledger
