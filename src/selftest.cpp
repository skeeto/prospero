// Platform integration tests, run against the real platform layer
// $ prospero --selftest
//
// Covers what the unit tests cannot: threads, locks, events and
// mailboxes under real contention, timing, the file system, and reserved
// memory. Progress goes to standard error, and the result is non-zero on
// failure.
//
// Detached threads may still touch shared state just after their last
// report, so such state lives in perm, which outlives the tests, and
// never in scratch.
//
// This is free and unencumbered software released into the public domain.

struct Selftest {
    Os *os;
    i32 checks;
    i32 failed;
};

static void say(Os *os, Str msg)
{
    os_print(os, 2, "selftest: ");
    os_print(os, 2, msg);
    os_print(os, 2, "\n");
}

// Count a check, reporting it if it failed.
static b32 check(Selftest *t, b32 ok, Str what)
{
    t->checks++;
    if (!ok) {
        t->failed++;
        os_print(t->os, 2, "selftest: FAIL ");
        os_print(t->os, 2, what);
        os_print(t->os, 2, "\n");
    }
    return ok;
}

// Report a measurement: "label value unit".
static void report(Os *os, Arena scratch, Str label, f64 value,
                   i32 decimals, Str unit)
{
    Buf b(&scratch, 128);
    print(&b, label);
    putbyte(&b, ' ');
    printfloat(&b, value, decimals);
    print(&b, unit);
    say(os, finish(&b));
}

static f64 millis(i64 us)
{
    return (f64)us / 1000;
}

// Threads that count down as they finish, waking a waiter at zero.
struct Group {
    Event done;
    i32   left;
};

static void leave(Group *g)
{
    if (!__atomic_sub_fetch(&g->left, 1, __ATOMIC_ACQ_REL)) {
        os_signal(&g->done);
    }
}

// Wait up to ms milliseconds for every thread to leave.
static b32 waitgroup(Os *os, Group *g, i32 ms)
{
    i64 deadline = os_ticks(os) + (i64)ms*1000;
    while (__atomic_load_n(&g->left, __ATOMIC_ACQUIRE)) {
        i64 left = deadline - os_ticks(os);
        if (left <= 0) {
            return 0;
        }
        os_wait(&g->done, (i32)(left/1000 + 1));
    }
    return 1;
}


// Clocks, entropy, environment

static void testbasics(Selftest *t, Arena scratch)
{
    Os *os = t->os;

    i64 prev = os_ticks(os);
    b32 mono = 1;
    for (i32 i = 0; i < 100000; i++) {
        i64 now = os_ticks(os);
        mono &= now >= prev;
        prev = now;
    }
    check(t, mono, "os_ticks is monotonic");

    i64 now = os_time(os);
    check(t, now > 1767225600000, "os_time is after 2026-01-01");
    check(t, now < 4102444800000, "os_time is before 2100-01-01");

    u64 seen[16] = {};
    b32 distinct = 1;
    for (i32 i = 0; i < countof(seen); i++) {
        seen[i] = os_random(os);
        for (i32 j = 0; j < i; j++) {
            distinct &= seen[i] != seen[j];
        }
    }
    check(t, distinct, "os_random values differ");

    Str path = os_getenv(os, &scratch, "PATH");
    check(t, path.data && path.len, "os_getenv finds PATH");
    Str none = os_getenv(os, &scratch, "PROSPERO_SELFTEST_UNSET_X9");
    check(t, !none.data, "os_getenv returns null when unset");
    Str nul = os_getenv(os, &scratch, "PATH\0X");
    check(t, !nul.data, "os_getenv of a name with a null byte");

    Str exe = os_exepath(os, &scratch);
    if (check(t, exe.len>0, "os_exepath is not empty")) {
        Stat s = os_stat(os, scratch, exe);
        check(t, s.type==STAT_FILE && s.size>0, "os_exepath names a file");
        Buf b(&scratch, 64);
        print(&b, "exe ");
        print(&b, exe);
        say(os, finish(&b));
    }
    say(os, "clocks, entropy, environment done");
}


// Events

struct Pinger {
    Os   *os;
    Event target;
    Event idle;  // never signaled: waiting on it sleeps
    Lock  lock;
    i64   sent;  // ticks just before signaling, under lock
};

static void pinger(void *arg)
{
    Pinger *p = (Pinger *)arg;
    os_wait(&p->idle, 20);
    os_lock(&p->lock);
    p->sent = os_ticks(p->os);
    os_unlock(&p->lock);
    os_signal(&p->target);
}

static void testevents(Selftest *t, Arena *perm, Arena scratch)
{
    Os     *os = t->os;
    Pinger *p  = alloc<Pinger>(perm);
    p->os = os;
    os_lockinit(&p->lock);
    if (!check(t, os_eventinit(&p->target), "os_eventinit") ||
            !check(t, os_eventinit(&p->idle), "os_eventinit")) {
        return;
    }
    Event *e = &p->target;

    check(t, !os_wait(e, 0), "an event starts unsignaled");
    os_signal(e);
    os_signal(e);
    check(t, os_wait(e, 0), "a signaled event wakes");
    check(t, !os_wait(e, 0), "signals coalesce and reset on wake");

    i64 t0 = os_ticks(os);
    b32 r  = os_wait(e, 50);
    i64 dt = os_ticks(os) - t0;
    check(t, !r, "os_wait(50) times out");
    check(t, dt>=30000 && dt<=200000, "os_wait(50) returns within 30-200 ms");
    report(os, scratch, "os_wait(50) timed out after", millis(dt), 1, " ms");

    t0 = os_ticks(os);
    if (!check(t, os_thread(os, pinger, p), "os_thread")) {
        return;
    }
    r = os_wait(e, 2000);
    i64 woke = os_ticks(os);
    os_lock(&p->lock);
    i64 sent = p->sent;
    os_unlock(&p->lock);
    check(t, r, "a signal from another thread wakes the waiter");
    check(t, woke-t0 >= 10000, "the waiter slept until the signal");
    check(t, sent && woke-sent<100000, "the waiter wakes within 100 ms");
    report(os, scratch, "cross-thread wake latency", millis(woke - sent), 3,
           " ms");
}

struct Waiters {
    Event  event;
    Group *group;
    i32    woken;  // atomic
};

static void waiter(void *arg)
{
    Waiters *w = (Waiters *)arg;
    if (os_wait(&w->event, 5000)) {
        __atomic_add_fetch(&w->woken, 1, __ATOMIC_ACQ_REL);
    }
    leave(w->group);
}

// Like a Win32 auto-reset event, one signal wakes exactly one of several
// waiters, and the others keep waiting.
static void testwakeone(Selftest *t, Arena *perm)
{
    enum { NWAIT = 4 };
    Os      *os   = t->os;
    Group   *g    = alloc<Group>(perm);
    Waiters *w    = alloc<Waiters>(perm);
    Event    idle = {};
    if (!check(t, os_eventinit(&g->done) && os_eventinit(&w->event) &&
                  os_eventinit(&idle), "os_eventinit")) {
        return;
    }
    w->group = g;

    g->left = NWAIT;
    i32 started = 0;
    for (i32 i = 0; i < NWAIT; i++) {
        if (check(t, os_thread(os, waiter, w), "os_thread")) {
            started++;
        } else {
            leave(g);
        }
    }
    os_wait(&idle, 50);  // let them all block

    b32 one = 1;
    for (i32 n = 1; n <= started; n++) {
        os_signal(&w->event);
        for (i32 i = 0; i < 200; i++) {
            if (__atomic_load_n(&w->woken, __ATOMIC_ACQUIRE) >= n) break;
            os_wait(&idle, 10);
        }
        os_wait(&idle, 30);  // time for any extra waiter to wake
        one &= __atomic_load_n(&w->woken, __ATOMIC_ACQUIRE) == n;
    }
    check(t, one, "each signal wakes one of several waiters");
    check(t, waitgroup(os, g, 1000), "every waiter wakes");
}


// Locks

struct Counter {
    Lock   lock;
    Lock   gate;   // held while workers start, so that they all contend
    Group *group;
    i64    count;  // under lock
    i32    iters;
};

static void lockworker(void *arg)
{
    Counter *c = (Counter *)arg;
    os_lock(&c->gate);
    os_unlock(&c->gate);
    for (i32 i = 0; i < c->iters; i++) {
        os_lock(&c->lock);
        c->count++;
        os_unlock(&c->lock);
    }
    leave(c->group);
}

static void testlock(Selftest *t, Arena *perm, Arena scratch)
{
    enum { NTHREADS = 8, ITERS = 100000 };
    Os      *os = t->os;
    Group   *g  = alloc<Group>(perm);
    Counter *c  = alloc<Counter>(perm);
    if (!check(t, os_eventinit(&g->done), "os_eventinit")) {
        return;
    }
    os_lockinit(&c->lock);
    os_lockinit(&c->gate);
    c->group = g;
    c->iters = ITERS;

    g->left = NTHREADS;
    os_lock(&c->gate);
    for (i32 i = 0; i < NTHREADS; i++) {
        if (!check(t, os_thread(os, lockworker, c), "os_thread")) {
            leave(g);
        }
    }
    i64 t0 = os_ticks(os);
    os_unlock(&c->gate);
    b32 done = waitgroup(os, g, 60000);
    i64 dt   = os_ticks(os) - t0;
    check(t, done, "lock workers finish");

    os_lock(&c->lock);
    i64 count = c->count;
    os_unlock(&c->lock);
    check(t, !done || count==(i64)NTHREADS*ITERS, "no increment is lost");
    report(os, scratch, "lock: 8 threads x 100000 increments in",
           millis(dt), 1, " ms");
}


// Mailbox stress: many producers, one consumer

enum : u32 {
    STRESS_DATA = 1,
    STRESS_DONE = 2,  // payload: the producer's count of dropped records
};

struct Producer {
    Mailbox *mbox;
    i32      index;
    i32      count;
    b32      block;
};

static iz stresslen(i32 index, i32 seq)
{
    return 4 + (seq*31 + index*17)%251;
}

static u8 stressbyte(i32 index, i32 seq, iz i)
{
    return (u8)(seq*7 + index*13 + i);
}

static void producer(void *arg)
{
    Producer *p = (Producer *)arg;
    u8        buf[256];
    i32       dropped = 0;
    for (i32 seq = 0; seq < p->count; seq++) {
        iz len = stresslen(p->index, seq);
        copybytes(buf, &seq, 4);
        for (iz i = 4; i < len; i++) {
            buf[i] = stressbyte(p->index, seq, i);
        }
        Str rec = {buf, len};
        u64 id  = (u64)p->index;
        dropped += !mailpost(p->mbox, STRESS_DATA, id, rec, p->block);
    }
    // Always blocking, so never lost: the consumer stops after all of these
    Str done = {(u8 *)&dropped, 4};
    mailpost(p->mbox, STRESS_DONE, (u64)p->index, done, 1);
}

// With block clear, the consumer naps after each take so that the small
// buffer overflows. Every record must arrive intact and in order per
// producer, and a gap in a producer's sequence must follow a take that
// reported overflow since that producer's previous record.
static void teststress(Selftest *t, Arena *perm, Arena scratch, b32 block)
{
    enum { NPROD = 8, COUNT = 20000 };
    Os      *os   = t->os;
    Mailbox *m    = newmailbox(perm, block ? 1<<14 : 1<<12, 0, 0);
    Event    idle = {};
    if (!check(t, m && os_eventinit(&idle), "newmailbox")) {
        return;
    }

    i64 t0      = os_ticks(os);
    i32 started = 0;
    for (i32 i = 0; i < NPROD; i++) {
        Producer *p = alloc<Producer>(perm);
        p->mbox  = m;
        p->index = i;
        p->count = COUNT;
        p->block = block;
        started += check(t, os_thread(os, producer, p), "os_thread");
    }

    i32 next[NPROD]   = {};  // expected sequence number
    i32 ovseen[NPROD] = {};  // overflow takes before its last record
    i32 ndone     = 0;
    i32 takes     = 0;
    i32 overflows = 0;  // takes that reported overflow
    i64 records   = 0;
    i64 dropped   = 0;
    b32 order     = 1;
    b32 intact    = 1;
    while (ndone < started) {
        if (!mailwait(m, 10000)) {
            check(t, 0, "mailbox consumer is woken");
            return;
        }
        Mail mail = mailtake(m);
        takes++;
        for (Record r = mailnext(&mail); r.ok; r = mailnext(&mail)) {
            if (r.id >= NPROD) {
                intact = 0;
                continue;
            }
            i32 p = (i32)r.id;
            if (r.type == STRESS_DONE) {
                intact &= r.data.len == 4;
                i32 d = 0;
                copybytes(&d, r.data.data, 4);
                dropped += d;
                ndone++;
                continue;
            }
            if (r.data.len < 4) {
                intact = 0;
                continue;
            }
            i32 seq = 0;
            copybytes(&seq, r.data.data, 4);
            if (seq != next[p]) {
                order &= seq>next[p] && overflows>ovseen[p];
            }
            next[p]   = seq + 1;
            ovseen[p] = overflows;
            intact &= r.type==STRESS_DATA && r.data.len==stresslen(p, seq);
            for (iz i = 4; intact && i < r.data.len; i++) {
                intact &= r.data[i] == stressbyte(p, seq, i);
            }
            records++;
        }
        overflows += mail.overflow;
        if (!block) {
            os_wait(&idle, 1);
        }
    }
    i64 dt = os_ticks(os) - t0;

    i64 total = (i64)started * COUNT;
    check(t, intact, "mailbox records arrive intact");
    check(t, order,
          "mailbox keeps per-producer order, gaps only after overflow");
    check(t, records+dropped == total, "mailbox accounts for every record");
    if (block) {
        check(t, !dropped && !overflows, "blocking posts never drop");
    } else {
        check(t, dropped && overflows, "non-blocking posts overflow");
    }

    Buf b(&scratch, 128);
    print(&b, block ? Str("mailbox, blocking: ") : "mailbox, dropping: ");
    print(&b, records);
    print(&b, " delivered, ");
    print(&b, dropped);
    print(&b, " dropped, ");
    print(&b, (i64)takes);
    print(&b, " takes (");
    print(&b, (i64)overflows);
    print(&b, " overflowed) in ");
    printfloat(&b, millis(dt), 1);
    print(&b, " ms");
    say(os, finish(&b));
}

struct Bigpost {
    Mailbox *mbox;
    Event    idle;
};

static void bigposter(void *arg)
{
    Bigpost *p = (Bigpost *)arg;
    os_wait(&p->idle, 20);
    u8  buf[128] = {};
    Str big      = {buf, countof(buf)};
    mailpost(p->mbox, 1, 0, big, 0);
}

// A record too big for the mailbox still wakes a consumer blocked on it
// while empty, which must learn of the drop.
static void testmaildrop(Selftest *t, Arena *perm)
{
    Os      *os = t->os;
    Bigpost *p  = alloc<Bigpost>(perm);
    p->mbox = newmailbox(perm, 64, 0, 0);
    if (!check(t, p->mbox && os_eventinit(&p->idle), "newmailbox") ||
            !check(t, os_thread(os, bigposter, p), "os_thread")) {
        return;
    }
    check(t, mailwait(p->mbox, 5000), "a dropped record wakes the consumer");
    Mail mail = mailtake(p->mbox);
    check(t, mail.overflow && !mail.len, "the consumer learns of the drop");
}


// Files

static Str tempdir(Os *os, Arena *a)
{
    Str vars[] = {"TMPDIR", "TEMP", "TMP"};
    for (iz i = 0; i < countof(vars); i++) {
        Str dir = os_getenv(os, a, vars[i]);
        for (; dir.len > 1; dir.len--) {
            u8 c = dir[dir.len-1];
            if (c!='/' && c!='\\') break;
        }
        if (dir.len) {
            return dir;
        }
    }
    return "/tmp";
}

// Remove a directory tree that holds no symlinks.
static b32 removeall(Os *os, Arena scratch, Str path)
{
    for (Dirent *d = os_list(os, &scratch, path); d; d = d->next) {
        Arena tmp = scratch;
        Str   sub = join(&tmp, path, d->name);
        if (d->isdir) {
            removeall(os, tmp, sub);
        } else {
            os_remove(os, tmp, sub);
        }
    }
    return os_remove(os, scratch, path);
}

static b32 direntless(Dirent **a, Dirent **b)
{
    return compare((*a)->name, (*b)->name) < 0;
}

// Sorted listing: "name:size" for files, "name/" for directories.
static Str listshow(Os *os, Arena *a, Str dir)
{
    iz n = 0;
    Dirent *head = os_list(os, a, dir);
    for (Dirent *d = head; d; d = d->next) {
        n++;
    }
    Dirent **all = alloc<Dirent *>(a, n);
    n = 0;
    for (Dirent *d = head; d; d = d->next) {
        all[n++] = d;
    }
    sort(all, n, direntless, *a);

    Buf b(a, 128);
    for (iz i = 0; i < n; i++) {
        if (i) putbyte(&b, ' ');
        print(&b, all[i]->name);
        if (all[i]->isdir) {
            putbyte(&b, '/');
        } else {
            putbyte(&b, ':');
            print(&b, all[i]->size);
        }
    }
    return finish(&b);
}

static b32 readis(Os *os, Arena scratch, Str path, Str want)
{
    Read r = os_read(os, &scratch, path, (iz)1<<30);
    return !r.err && r.data.data && r.data==want && r.size==want.len;
}

static void testfiles(Selftest *t, Arena scratch, Str dir)
{
    Os   *os = t->os;
    Arena a  = scratch;  // reset by each section

    Str sub   = join(&scratch, dir, "sub/deeper");
    Str fa    = join(&scratch, dir, "a.txt");
    Str fb    = join(&scratch, dir, "b.txt");
    Str empty = join(&scratch, dir, "empty");
    Str big   = join(&scratch, dir, "big.bin");
    Str gone  = join(&scratch, dir, "missing");
    Str wide  = join(&scratch, dir, "na\xc3\xafve-\xe6\x97\xa5"
                                    "\xe6\x9c\xac.txt");

    check(t, os_stat(os, scratch, dir).type==STAT_MISSING,
          "a fresh directory name");
    check(t, os_mkdirs(os, scratch, sub), "os_mkdirs creates parents");
    check(t, os_mkdirs(os, scratch, sub),
          "os_mkdirs accepts an existing directory");
    check(t, os_stat(os, scratch, sub).type==STAT_DIR,
          "os_stat on a directory");

    // Atomic replacement leaves nothing else behind
    check(t, os_write(os, scratch, fa, "hello"), "os_write creates");
    check(t, readis(os, scratch, fa, "hello"), "os_read reads back");
    check(t, os_write(os, scratch, fa, "replaced!"), "os_write replaces");
    check(t, readis(os, scratch, fa, "replaced!"),
          "os_read reads the replacement");
    a = scratch;
    check(t, listshow(os, &a, dir)=="a.txt:9 sub/",
          "os_write leaves no temporary files");

    // Size cap and errors
    a = scratch;
    Read r = os_read(os, &a, fa, 8);
    check(t, r.err==READ_TOOBIG && r.size==9 && !r.data.data,
          "os_read reports READ_TOOBIG");
    check(t, os_read(os, &a, fa, 9).err==READ_OK,
          "os_read reads up to the cap");
    check(t, os_read(os, &a, gone, 99).err==READ_NOTFOUND,
          "os_read of a missing file");
    check(t, os_read(os, &a, join(&a, gone, "x"), 99).err==READ_NOTFOUND,
          "os_read under a missing directory");
    check(t, os_read(os, &a, dir, 99).err==READ_NOTFILE,
          "os_read of a directory");
    check(t, os_read(os, &a, join(&a, fa, "x"), 99).err==READ_NOTFOUND,
          "os_read under a file");

    // A null byte would cut a path short at the system call, so that it
    // named a.txt, or the directory: it names nothing instead.
    a = scratch;
    Str nul    = concat(&a, fa, "\0/../nul.txt");
    Str nuldir = concat(&a, dir, "\0");
    check(t, os_stat(os, a, nul).type==STAT_MISSING,
          "os_stat of a path with a null byte");
    check(t, os_read(os, &a, nul, 99).err==READ_NOTFOUND,
          "os_read of a path with a null byte");
    check(t, !os_write(os, a, nul, "x"),
          "os_write to a path with a null byte");
    check(t, !os_mkdirs(os, a, nuldir),
          "os_mkdirs of a path with a null byte");
    check(t, !os_list(os, &a, nuldir), "os_list of a path with a null byte");
    check(t, !os_rename(os, a, nul, fb) && !os_rename(os, a, fa, nul),
          "os_rename with a null byte");
    check(t, !os_remove(os, a, nul), "os_remove of a path with a null byte");
    check(t, os_logopen(os, a, nul) == -1,
          "os_logopen of a path with a null byte");
    check(t, readis(os, a, fa, "replaced!") &&
             os_stat(os, a, fb).type==STAT_MISSING,
          "a path with a null byte touches no other file");

    // Empty, large, and non-ASCII files
    check(t, os_write(os, scratch, empty, ""), "os_write of an empty file");
    a = scratch;
    r = os_read(os, &a, empty, 0);
    check(t, !r.err && r.data.data && !r.data.len,
          "os_read of an empty file");

    a = scratch;
    iz  bigsize = ((iz)3<<20) + 7;  // beyond one Win32 I/O chunk
    Str bigdata = {allocbytes(&a, bigsize), bigsize};
    for (iz i = 0; i < bigsize; i++) {
        bigdata[i] = (u8)(i ^ i>>11);
    }
    check(t, os_write(os, a, big, bigdata), "os_write of 3 MiB");
    check(t, readis(os, a, big, bigdata), "os_read of 3 MiB");

    check(t, os_write(os, scratch, wide, "\xe2\x82\xac"),
          "os_write to a non-ASCII name");
    check(t, readis(os, scratch, wide, "\xe2\x82\xac"),
          "os_read from a non-ASCII name");

    a = scratch;
    Str want = "a.txt:9 big.bin:3145735 empty:0 "
               "na\xc3\xafve-\xe6\x97\xa5\xe6\x9c\xac.txt:3 sub/";
    check(t, listshow(os, &a, dir)==want, "os_list");

    // Times are Unix epoch milliseconds
    Stat s   = os_stat(os, scratch, fa);
    i64  now = os_time(os);
    check(t, s.type==STAT_FILE && s.size==9, "os_stat on a file");
    check(t, s.mtime>now-3600000 && s.mtime<now+3600000,
          "os_stat mtime is near os_time");
    a = scratch;
    i64 listed = 0;
    for (Dirent *d = os_list(os, &a, dir); d; d = d->next) {
        listed = d->name=="a.txt" ? d->mtime : listed;
    }
    check(t, listed == s.mtime, "os_list and os_stat agree on mtime");
    check(t, os_stat(os, scratch, gone).type==STAT_MISSING,
          "os_stat on a missing path");

    // Rename over an existing file
    check(t, os_write(os, scratch, fb, "old"), "os_write");
    check(t, os_rename(os, scratch, fa, fb), "os_rename replaces");
    check(t, readis(os, scratch, fb, "replaced!"),
          "os_rename moves the contents");
    check(t, os_stat(os, scratch, fa).type==STAT_MISSING,
          "os_rename removes the source");
    check(t, !os_rename(os, scratch, gone, fa),
          "os_rename of a missing file fails");

    // A file in the way of a directory
    check(t, !os_mkdirs(os, scratch, fb), "os_mkdirs over a file fails");
    a = scratch;
    check(t, !os_mkdirs(os, a, join(&a, fb, "x")),
          "os_mkdirs under a file fails");

    check(t, os_remove(os, scratch, fb), "os_remove");
    check(t, !os_remove(os, scratch, fb), "os_remove of a missing file fails");
    check(t, os_stat(os, scratch, fb).type==STAT_MISSING, "os_remove removes");
    say(os, "files done");
}

static void testlog(Selftest *t, Arena scratch, Str dir)
{
    Os *os   = t->os;
    Str path = join(&scratch, dir, "events.jsonl");

    iz log = os_logopen(os, scratch, path);
    if (!check(t, log != -1, "os_logopen")) {
        return;
    }
    check(t, os_logwrite(os, log, "one\n"), "os_logwrite");
    check(t, os_logwrite(os, log, "two\n"), "os_logwrite");
    check(t, os_logsync(os, log), "os_logsync");
    check(t, os_stat(os, scratch, path).size==8, "os_logwrite is unbuffered");
    check(t, os_logtrunc(os, log, 4), "os_logtrunc");
    check(t, os_logwrite(os, log, "three\n"), "os_logwrite after truncation");
    os_logclose(os, log);

    // Reopened logs append, and handles append after one another
    iz l1 = os_logopen(os, scratch, path);
    iz l2 = os_logopen(os, scratch, path);
    if (!check(t, l1!=-1 && l2!=-1, "os_logopen again")) {
        return;
    }
    os_logwrite(os, l1, "four\n");
    os_logwrite(os, l2, "five\n");
    os_logwrite(os, l1, "six\n");
    os_logclose(os, l1);
    os_logclose(os, l2);
    check(t, readis(os, scratch, path, "one\nthree\nfour\nfive\nsix\n"),
          "log contents after truncation and reopening");

    Str bad = join(&scratch, dir, "missing/events.jsonl");
    check(t, os_logopen(os, scratch, bad) == -1,
          "os_logopen fails without a directory");
    say(os, "logs done");
}


// Reserved memory

static void testarena(Selftest *t, Arena scratch)
{
    Os     *os   = t->os;
    iz      size = (iz)256 << 20;
    Region *r    = os_reserve(os, size + ((iz)16<<20));
    if (!check(t, r!=0, "os_reserve of 272 MiB")) {
        return;
    }
    Arena g = newarena(r);
    check(t, g.end-g.beg < size, "a reservation starts mostly uncommitted");
    check(t, fits(&g, size), "fits counts uncommitted space");

    i64 t0 = os_ticks(os);
    u8 *p  = allocbytes(&g, size);
    for (iz i = 0; i < size; i += 4096) {
        p[i] = (u8)(i >> 12);
    }
    b32 ok = 1;
    for (iz i = 0; i < size; i += 4096) {
        ok &= p[i] == (u8)(i >> 12);
    }
    i64 dt = os_ticks(os) - t0;
    check(t, ok, "committed memory holds data");
    check(t, r->committed >= size, "the region commits on demand");
    os_release(os, r);
    report(os, scratch, "256 MiB committed and touched in", millis(dt), 1,
           " ms");
}


static i32 selftest(Os *os, Arena *perm, Arena scratch)
{
    Selftest t = {};
    t.os = os;
    i64 t0 = os_ticks(os);

    testbasics(&t, scratch);
    testevents(&t, perm, scratch);
    testwakeone(&t, perm);
    testlock(&t, perm, scratch);
    teststress(&t, perm, scratch, 1);
    teststress(&t, perm, scratch, 0);
    testmaildrop(&t, perm);

    Arena a    = scratch;
    Buf   name(&a, 64);
    print(&name, "prospero-selftest-");
    printhex(&name, os_random(os), 16);
    Str dir = join(&a, tempdir(os, &a), finish(&name));
    say(os, dir);
    testfiles(&t, a, dir);
    testlog(&t, a, dir);
    removeall(os, a, dir);
    check(&t, os_stat(os, a, dir).type==STAT_MISSING,
          "temporary directory removed");

    testarena(&t, scratch);

    Buf b(&scratch, 128);
    print(&b, (i64)t.checks);
    print(&b, " checks, ");
    print(&b, (i64)t.failed);
    print(&b, " failed, ");
    printfloat(&b, millis(os_ticks(os) - t0), 0);
    print(&b, " ms");
    say(os, finish(&b));
    return t.failed ? 1 : 0;
}
