// FsPool tests: inline jobs where no thread starts, and in manual mode
// (fsbegin, fsrun) queue order, expiry, holds, stuck workers and their
// marks, deadlines, abandoned jobs and quitting, on the fake clock

// What finish and drop saw, as "ID:ANSWER;", and what an abandon in a
// finish returned.
struct FsLog {
    u8      text[8192];
    iz      len;
    FsPool *p;
    i64     abandon;   // the id fsabandon is called for in its finish
    i64     returns;   // the id whose worker returns (fsrun) in its finish
    i32     abandoned; // what it returned, +1 (0: not called)
    i32     drops;
};

static Str fslogged(FsLog *l)
{
    Str s = {l->text, l->len};
    l->len = 0;
    return s;
}

static void fslogput(FsLog *l, Str s)
{
    iz n = s.len<(iz)sizeof(l->text)-l->len ? s.len : (iz)sizeof(l->text)-l->len;
    copybytes(l->text+l->len, s.data, n);
    l->len += n;
}

static void fstestfinish(void *owner, FsJob *j, Arena scratch)
{
    FsLog *l = (FsLog *)owner;
    Buf    b(&scratch, 256);
    print(&b, j->put.id);
    putbyte(&b, ':');
    print(&b, j->err.len ? j->err : j->out);
    putbyte(&b, ';');
    fslogput(l, finish(&b));
    if (l->abandon && l->abandon==j->put.id) {
        l->abandoned = fsabandon(l->p, j->put.id) + 1;
    }
    for (i32 k = 0; l->returns && l->returns==j->put.id && k<FS_THREADS; k++) {
        FsJob *q = l->p->workers[k].job;
        if (q && q->put.id==j->put.id && q->state==FSJ_RUNNING) {
            fsrun(l->p, k);
        }
    }
}

static void fstestdrop(Os *, void *own)
{
    ((FsLog *)own)->drops++;
}

// Each path given, then "ran ARG", in the worker's own arena.
static void fstestrun(FsCall *c)
{
    for (iz i = 0; i < c->paths.len; i++) {
        if (!fsat(c, c->paths[i], 1)) {
            c->err = fsstuck;
            return;
        }
    }
    c->out = concat(c->mem, "ran ", c->arg);
}

static FsPut fstestput(Arena *a, i64 id, Str arg, Str path, FsLog *own = 0)
{
    FsPut put = {};
    put.id  = id;
    put.run = fstestrun;
    put.arg = arg;
    if (path.len) {
        put.paths = push(a, put.paths, path);
    }
    put.own  = own;
    put.drop = own ? fstestdrop : 0;
    return put;
}

// Queue a job, "+" if taken, else "-WHY".
static Str fstestqueue(FsPool *p, Arena *a, i64 id, Str path, b32 exact = 0)
{
    FsPut put = fstestput(a, id, path, path);
    put.exact = exact;
    Str why = {};
    return fsput(p, &put, &why, *a) ? Str("+") : concat(a, "-", why);
}

// Every completion posted, delivered.
static void fstestdeliver(FsPool *p, Arena scratch)
{
    Mail m = mailtake(p->done);
    for (Record r; (r = mailnext(&m)).ok;) {
        fsdone(p, r, scratch);
    }
}

static Str fstestmarks(FsPool *p, Arena *a)
{
    Buf b(a, 256);
    for (i32 i = 0; i < FS_MARKS; i++) {
        if (p->marks[i].path.len) {
            print(&b, p->marks[i].path);
            print(&b, p->marks[i].exact ? Str("=|") : Str("|"));
        }
    }
    return finish(&b);
}

static FsPool *fstestpool(Os *os, Arena *a, FsLog *l)
{
    *l = {};
    FsPool *p = fsnew(os, a, newmailbox(a, 1<<16, 0, 0), 77, l, fstestfinish);
    p->manual = 1;
    l->p = p;
    return p;
}

static void testfsinline(Test *t, Arena a)
{
    Os   *os = newos(&a, 1<<16);
    FsLog l  = {};
    FsPool *p = fsnew(os, &a, 0, 77, &l, fstestfinish);
    l.p = p;
    FsPut put = fstestput(&a, 5, "x", "/a/x");
    Str   why = {};
    b32   ok  = fsput(p, &put, &why, a);
    expect(t, "fs inline", concat(&a, istr(&a, ok), fslogged(&l)), "15:ran x;");
    expect(t, "fs inline frees its slot",
           istr(&a, fsheld(p, 5)*100 + p->nqueued*10 + p->threads), "0");
    put = fstestput(&a, 6, "y", {});
    ok  = fsput(p, &put, &why, a);
    expect(t, "fs inline again", concat(&a, istr(&a, ok), fslogged(&l)), "16:ran y;");

    // No event to be made: inline all the same
    testnoevent = 1;
    p   = fsnew(os, &a, 0, 77, &l, fstestfinish);
    l.p = p;
    put = fstestput(&a, 7, "z", "/a/z");
    ok  = fsput(p, &put, &why, a);
    testnoevent = 0;
    expect(t, "fs inline, no event", concat(&a, istr(&a, ok), fslogged(&l)), "17:ran z;");
}

static void testfsqueue(Test *t, Arena a)
{
    Os     *os = newos(&a, 1<<16);
    FsLog   l  = {};
    FsPool *p  = fstestpool(os, &a, &l);

    // In order, whatever order they end in
    fstestqueue(p, &a, 1, "/q/a");
    fstestqueue(p, &a, 2, "/q/b");
    fstestqueue(p, &a, 3, "/q/c");
    i32 w0 = fsbegin(p);
    i32 w1 = fsbegin(p);
    i32 w2 = fsbegin(p);
    expect(t, "fs begin", concat(&a, concat(&a, istr(&a, w0), istr(&a, w1)), istr(&a, w2)),
           "012");
    expect(t, "fs begun", istr(&a, fsbegin(p)), "-1");
    fsrun(p, w2);
    fsrun(p, w0);
    fsrun(p, w1);
    fstestdeliver(p, a);
    expect(t, "fs answers, as they end", fslogged(&l),
           "3:ran /q/c;1:ran /q/a;2:ran /q/b;");
    fstestqueue(p, &a, 4, "/q/d");
    fstestqueue(p, &a, 5, "/q/e");
    i32 wa = fsbegin(p);
    i32 wb = fsbegin(p);
    fsrun(p, wa);
    fsrun(p, wb);
    fstestdeliver(p, a);
    expect(t, "fs queue order", fslogged(&l), "4:ran /q/d;5:ran /q/e;");

    // FS_QUEUE wait at most, and expire at FS_STUCK as busy
    Buf took(&a, 256);
    for (i32 i = 0; i < FS_QUEUE+1; i++) {
        os->now += FS_MS;
        Str r = fstestqueue(p, &a, 100+i, "/q/many");
        if (i == FS_QUEUE) {
            print(&took, r);
        } else if (!(r == "+")) {
            print(&took, "?");
        }
    }
    expect(t, "fs queue full", finish(&took), concat(&a, "-", fsbusy));
    os->now += FS_STUCK - (i64)FS_QUEUE*FS_MS;
    fstick(p, a, os->now);
    Str got = fslogged(&l);
    expect(t, "fs expired", concat(&a, istr(&a, find(got, "100:the file system is busy")),
                                   istr(&a, p->nqueued)), "063");
    expect(t, "fs next, queued", istr(&a, fsnext(p) - os->now), istr(&a, FS_MS));
    os->now += (i64)FS_QUEUE*FS_MS;
    fstick(p, a, os->now);
    got = fslogged(&l);
    expect(t, "fs all expired", istr(&a, p->nqueued), "0");
    expect(t, "fs last expired", istr(&a, find(got, "163:the file system is busy")>0), "1");
    expect(t, "fs next, none", istr(&a, fsnext(p)==I64MAX), "1");
}

static void testfsstuck(Test *t, Arena a)
{
    Os     *os = newos(&a, 1<<16);
    FsLog   l  = {};
    FsPool *p  = fstestpool(os, &a, &l);

    // A job never answered, and a listing
    fstestqueue(p, &a, 1, "/m/dead");
    fstestqueue(p, &a, 2, "/m/dir", 1);
    i32 dead = fsbegin(p);
    i32 dir  = fsbegin(p);
    FsCall c = {};
    c.w   = &p->workers[dead];
    c.job = c.w->job;
    fsat(&c, "/m/dead", 1);
    c.w   = &p->workers[dir];
    c.job = c.w->job;
    fsat(&c, "/m/dir/", 1);
    os->now += FS_STUCK;
    fstick(p, a, os->now);
    expect(t, "fs stuck", fslogged(&l),
           concat(&a, concat(&a, concat(&a, "1:", fsstuck), concat(&a, ";2:", fsstuck)), ";"));
    expect(t, "fs marks", fstestmarks(p, &a), "/m/dead|/m/dir=|");
    expect(t, "fs gone", istr(&a, fsgone(&c)), "1");

    // Refused at once, below the mark too, a sibling taken; a listing's
    // mark refuses only its folder
    Buf got(&a, 256);
    print(&got, fstestqueue(p, &a, 3, "/m/dead"));
    print(&got, fstestqueue(p, &a, 4, "/m/dead/x"));
    print(&got, fstestqueue(p, &a, 5, "/m/sib"));
    print(&got, fstestqueue(p, &a, 6, "/m/dir"));
    print(&got, fstestqueue(p, &a, 7, "/m/dir/f"));
    print(&got, fstestqueue(p, &a, 8, {}));  // no path, never refused
    FsPut mixed = fstestput(&a, 9, "mixed", "/m/dead");
    mixed.paths = push(&a, mixed.paths, Str("/m/sib2"));
    Str why = {};
    print(&got, fsput(p, &mixed, &why, a) ? Str("+") : Str("-"));
    expect(t, "fs refused", finish(&got),
           concat(&a, concat(&a, concat(&a, "-", fsstuck), concat(&a, "-", fsstuck)),
                  concat(&a, concat(&a, "+-", fsstuck), "+++")));
    expect(t, "fs refused, finishing nothing", fslogged(&l), "");

    // Once: no second finish, no more marks, and no deadline counted again
    os->now += FS_STUCK;
    fstick(p, a, os->now);
    got = Buf(&a, 256);
    Str ended = fslogged(&l);
    expect(t, "fs stuck once", istr(&a, find(ended, "1:")<0 && find(ended, "2:")<0), "1");
    expect(t, "fs marks kept", fstestmarks(p, &a), "/m/dead|/m/dir=|");

    // The stuck call returns: nothing finished, the marks gone
    fsrun(p, dead);
    fsrun(p, dir);
    fstestdeliver(p, a);
    expect(t, "fs returned late", fslogged(&l), "");
    expect(t, "fs unmarked", fstestmarks(p, &a), "");
    expect(t, "fs taken again", fstestqueue(p, &a, 10, "/m/dead"), "+");

    // The stuck call returns while fstick's finishes run: marks cleared
    // there, the worker neither stuck nor busy, its completion nothing
    i32 again = fsbegin(p);
    c.w   = &p->workers[again];
    c.job = c.w->job;
    fsat(&c, "/m/dead", 1);
    l.returns = 10;
    os->now += FS_STUCK;
    fstick(p, a, os->now);
    l.returns = 0;
    expect(t, "fs returned in the finish", fslogged(&l), concat(&a, concat(&a, "10:", fsstuck), ";"));
    expect(t, "fs returned, unmarked", fstestmarks(p, &a), "");
    expect(t, "fs returned, idle",
           istr(&a, p->workers[again].stuck*10 + (p->workers[again].job!=0)), "0");
    fstestdeliver(p, a);
    expect(t, "fs returned, nothing more", fslogged(&l), "");
    expect(t, "fs returned, taken again", fstestqueue(p, &a, 11, "/m/dead"), "+");
}

// A full pool, a job declared stuck and another queued: no deadline in
// the past. Busy workers and a queued job: the earliest beat+FS_SLOW.
static void testfsnext(Test *t, Arena a)
{
    Os     *os = newos(&a, 1<<16);
    FsLog   l  = {};
    FsPool *p  = fstestpool(os, &a, &l);
    for (i32 i = 0; i < FS_THREADS; i++) {
        fstestqueue(p, &a, 1+i, concat(&a, "/n/", istr(&a, i)));
        fsbegin(p);
        os->now += 10*FS_MS;
    }
    os->now += FS_STUCK;
    fstick(p, a, os->now);
    fstestqueue(p, &a, 20, "/n/more");
    expect(t, "fs full", istr(&a, fsbegin(p)), "-1");
    expect(t, "fs next past stuck", istr(&a, fsnext(p) > os->now), "1");
    expect(t, "fs next, the queued job", istr(&a, fsnext(p) - os->now), istr(&a, FS_STUCK));

    FsPool *q = fstestpool(os, &a, &l);
    i64     t0 = os->now;
    for (i32 i = 0; i < 3; i++) {
        fstestqueue(q, &a, 1+i, concat(&a, "/b/", istr(&a, i)));
        fsbegin(q);
        os->now += 100*FS_MS;
    }
    fstestqueue(q, &a, 9, "/b/queued");
    expect(t, "fs next, slow", istr(&a, fsnext(q) - t0), istr(&a, FS_SLOW));
    os->now = t0 + FS_SLOW;
    fstick(q, a, os->now);
    expect(t, "fs next, slow passed", istr(&a, fsnext(q) - t0), istr(&a, FS_SLOW + 100*FS_MS));
    expect(t, "fs holding", istr(&a, q->workers[0].holding*10 + q->workers[1].holding), "10");
}

// Marks and holds in Windows' spellings, and a link's path held, then
// refused, while its job hangs past realpath.
static void testfspaths(Test *t, Arena a)
{
    Os     *os = newos(&a, 1<<16);
    FsLog   l  = {};
    FsPool *p  = fstestpool(os, &a, &l);
    p->win = 1;
    fstestqueue(p, &a, 1, "C:/Link/f.txt");
    i32    k = fsbegin(p);
    FsCall c = {};
    c.w   = &p->workers[k];
    c.job = c.w->job;
    fsat(&c, "C:/Link/f.txt", 1);
    fsat(&c, "c:\\Data\\real\\f.txt");
    os->now += FS_STUCK;
    fstick(p, a, os->now);
    fslogged(&l);
    expect(t, "fs windows marks", fstestmarks(p, &a), "C:/Link/f.txt|C:/Data/real/f.txt|");
    Buf got(&a, 64);
    print(&got, fstestqueue(p, &a, 2, "C:/DATA/Real/f.txt"));
    print(&got, fstestqueue(p, &a, 3, "C:/Data/real2.txt"));
    print(&got, istr(&a, fsat(&c, "C:\\data\\real\\f.txt\\x")));
    expect(t, "fs windows refused", finish(&got), concat(&a, concat(&a, "-", fsstuck), "+0"));

    FsPool *q = fstestpool(os, &a, &l);
    fstestqueue(q, &a, 1, "/l/link.txt");
    k = fsbegin(q);
    c = {};
    c.w   = &q->workers[k];
    c.job = c.w->job;
    fsat(&c, "/l/link.txt", 1);
    fsat(&c, "/t/target.txt");
    os->now += FS_SLOW;
    fstick(q, a, os->now);
    got = Buf(&a, 64);
    print(&got, fstestqueue(q, &a, 2, "/l/link.txt"));
    print(&got, fstestqueue(q, &a, 3, "/t/target.txt"));
    print(&got, fstestqueue(q, &a, 4, "/elsewhere"));
    i32 other = fsbegin(q);
    print(&got, istr(&a, fsbegin(q)));
    expect(t, "fs held", finish(&got), "+++-1");
    expect(t, "fs held, another goes", istr(&a, other>=0 && q->workers[other].job->put.id==4), "1");
    os->now += FS_STUCK - FS_SLOW;
    fstick(q, a, os->now);
    Str ends = fslogged(&l);
    expect(t, "fs held, then not responding", ends,
           concat(&a, concat(&a, concat(&a, "1:", fsstuck), concat(&a, ";2:", fsstuck)),
                  concat(&a, concat(&a, ";3:", fsstuck), ";")));
    expect(t, "fs the link refused", fstestqueue(q, &a, 5, "/l/link.txt"),
           concat(&a, "-", fsstuck));

    // A worker that beats again lets the jobs it held go
    FsPool *r = fstestpool(os, &a, &l);
    fstestqueue(r, &a, 1, "/s/slow");
    k = fsbegin(r);
    c = {};
    c.w   = &r->workers[k];
    c.job = c.w->job;
    fsat(&c, "/s/slow", 1);
    os->now += FS_SLOW;
    fstick(r, a, os->now);
    fstestqueue(r, &a, 2, "/s/slow");
    expect(t, "fs held behind a slow one", istr(&a, fsbegin(r)), "-1");
    fsbeat(&c);
    Mail m = mailtake(r->done);
    Record poke = mailnext(&m);
    expect(t, "fs beat pokes", istr(&a, (i64)poke.id), istr(&a, FS_THREADS + k));
    expect(t, "fs no longer held", istr(&a, fsbegin(r)>=0), "1");
}

static void testfsabandon(Test *t, Arena a)
{
    Os     *os = newos(&a, 1<<16);
    FsLog   l  = {};
    FsPool *p  = fstestpool(os, &a, &l);
    FsLog   own = {};

    // Running: the pool drops it when it returns, finishing nothing
    FsPut put = fstestput(&a, 1, "a", "/x/a", &own);
    Str   why = {};
    fsput(p, &put, &why, a);
    i32 k = fsbegin(p);
    expect(t, "fs abandon running", istr(&a, fsabandon(p, 1)), "1");
    expect(t, "fs abandoned gone", istr(&a, p->workers[k].job->gone), "1");
    fsrun(p, k);
    fstestdeliver(p, a);
    expect(t, "fs abandoned dropped", concat(&a, fslogged(&l), istr(&a, own.drops)), "1");

    // Queued: the caller frees it
    put = fstestput(&a, 2, "b", "/x/b", &own);
    fsput(p, &put, &why, a);
    expect(t, "fs abandon queued", istr(&a, fsabandon(p, 2)*10 + fsheld(p, 2)), "0");

    // In fsdone's finish: the caller frees it; in fstick's, the pool
    l.abandon = 3;
    put = fstestput(&a, 3, "c", "/x/c", &own);
    fsput(p, &put, &why, a);
    k = fsbegin(p);
    fsrun(p, k);
    fstestdeliver(p, a);
    expect(t, "fs abandon in fsdone's finish",
           concat(&a, fslogged(&l), istr(&a, l.abandoned*10 + own.drops)), "3:ran c;11");
    l.abandon = 4;
    put = fstestput(&a, 4, "d", "/x/d", &own);
    fsput(p, &put, &why, a);
    k = fsbegin(p);
    os->now += FS_STUCK;
    fstick(p, a, os->now);
    expect(t, "fs abandon in fstick's finish", istr(&a, l.abandoned*10 + own.drops), "21");
    fsrun(p, k);
    fstestdeliver(p, a);
    expect(t, "fs dropped once it returns", istr(&a, own.drops), "2");
    l.abandon = 5;
    put = fstestput(&a, 5, "e", "/x/e", &own);
    fsput(p, &put, &why, a);
    os->now += FS_STUCK;
    fstick(p, a, os->now);
    expect(t, "fs abandon an expired one",
           concat(&a, fslogged(&l), istr(&a, l.abandoned*10 + own.drops)),
           concat(&a, concat(&a, "4:", fsstuck), concat(&a, concat(&a, ";5:", fsbusy), ";23")));

    // Abandoned, then stuck: marked, but nothing finished; dropped and
    // unmarked once it returns
    l.abandon = 0;
    put = fstestput(&a, 6, "f", "/x/f", &own);
    fsput(p, &put, &why, a);
    k = fsbegin(p);
    FsCall c = {};
    c.w   = &p->workers[k];
    c.job = c.w->job;
    fsat(&c, "/x/f", 1);
    fsabandon(p, 6);
    os->now += FS_STUCK;
    fstick(p, a, os->now);
    expect(t, "fs abandoned, then stuck",
           concat(&a, fslogged(&l), fstestmarks(p, &a)), "/x/f|");
    fsrun(p, k);
    fstestdeliver(p, a);
    expect(t, "fs abandoned, then stuck, returns",
           concat(&a, concat(&a, fslogged(&l), fstestmarks(p, &a)), istr(&a, own.drops)), "4");
}

static void testfsquit(Test *t, Arena a)
{
    Os     *os = newos(&a, 1<<16);
    FsLog   l  = {};
    FsPool *p  = fstestpool(os, &a, &l);
    fstestqueue(p, &a, 1, "/z/running");
    fstestqueue(p, &a, 2, "/z/done");
    i32 running = fsbegin(p);
    i32 done    = fsbegin(p);
    fstestqueue(p, &a, 3, "/z/queued");
    fsrun(p, done);  // posted, not yet delivered
    fsquit(p, "stopping", a);
    expect(t, "fs quit", fslogged(&l), "1:stopping;2:ran /z/done;3:stopping;");
    fsrun(p, running);
    fstestdeliver(p, a);
    expect(t, "fs quit, later", fslogged(&l), "");
    expect(t, "fs quit refuses", fstestqueue(p, &a, 4, "/z/new"), "-stopping");
}

static void testfsstallparse(Test *t, Arena a)
{
    Os     *os = newos(&a, 1<<16);
    FsLog   l  = {};
    FsPool *p  = fstestpool(os, &a, &l);
    fsstallparse(p, &a, "a=5;b=70000");
    fsstallparse(p, &a, "x;=5;c=0;d=700000;e=abc;f=7;");
    Buf b(&a, 128);
    for (i32 i = 0; i < p->nstalls; i++) {
        print(&b, p->stalls[i].text);
        putbyte(&b, '=');
        print(&b, (i64)p->stalls[i].ms);
        putbyte(&b, ' ');
    }
    expect(t, "fs stalls", finish(&b), "a=5 b=70000 f=7 ");
    expect(t, "fs stuck ms", concat(&a, concat(&a, istr(&a, fsstuckparse("50")/FS_MS), " "),
                                    concat(&a, concat(&a, istr(&a, fsstuckparse("999999")/FS_MS),
                                                      " "), istr(&a, fsstuckparse("2000x")))),
           "100 600000 0");
    expect(t, "fs stuck ms, in range", istr(&a, fsstuckparse("2000")), istr(&a, 2000*FS_MS));
}

static void test_fswork(Test *t)
{
    testfsinline(t, t->perm);
    testfsqueue(t, t->perm);
    testfsstuck(t, t->perm);
    testfsnext(t, t->perm);
    testfspaths(t, t->perm);
    testfsabandon(t, t->perm);
    testfsquit(t, t->perm);
    testfsstallparse(t, t->perm);
}
