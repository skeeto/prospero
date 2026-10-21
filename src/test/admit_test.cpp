// Admission tests: limits, first come first served, the exceptions for a
// loaded model, lingering and patience

static Str admgostr(Arena *a, AdmPoint *p, i64 now)
{
    return istr(a, (i64)admgo(p, a, now));
}

static void test_admit(Test *t)
{
    Arena *a = &t->perm;
    i64    S = 1000000;

    // A router with --models-max 1
    {
        Adm       adm = {};
        AdmPoint *p   = admpoint(&adm, a, "http://r/");
        expect(t, "admpoint finds", istr(a, admpoint(&adm, a, "http://r/") == p), "1");
        p->maxmodels = 1;
        admenqueue(p, a, 1, "A", 4, 1, 0);
        expect(t, "the first goes", admgostr(a, p, 0), "1");
        admenqueue(p, a, 2, "B", 4, 1, 0);
        expect(t, "another model waits", admgostr(a, p, 0), "0");
        expect(t, "its place", istr(a, admpos(p, 2)), "1");
        expect(t, "the queue", istr(a, admlen(p)), "1");
        admenqueue(p, a, 3, "A", 4, 1, 1*S);
        expect(t, "the loaded model passes", admgostr(a, p, 1*S), "3");
        admdone(p, "A", 2*S, 0);
        expect(t, "in use while one runs", admgostr(a, p, 2*S), "0");
        admdone(p, "A", 3*S, 0);
        expect(t, "then the other model", admgostr(a, p, 3*S), "2");
        expect(t, "nobody waits", istr(a, admnext(p, 3*S)), "0");

        // A lingering model keeps its place a while for its session
        admenqueue(p, a, 4, "A", 4, 1, 3*S);
        admdone(p, "B", 4*S, 1);
        expect(t, "lingering", admgostr(a, p, 4*S), "0");
        expect(t, "admnext at its end", istr(a, admnext(p, 4*S)),
               istr(a, 4*S + ADM_LINGER));
        expect(t, "and after", admgostr(a, p, 4*S+ADM_LINGER), "4");
        admdone(p, "A", 20*S, 1);

        // Cooled at the end of a turn, it lingers no more
        admenqueue(p, a, 5, "B", 4, 1, 20*S);
        expect(t, "lingering A", admgostr(a, p, 20*S), "0");
        admcool(p, "A");
        expect(t, "cooled", admgostr(a, p, 20*S), "5");
        admdone(p, "B", 21*S, 0);
    }

    // Patience: a waiter is passed only so long
    {
        Adm       adm = {};
        AdmPoint *p   = admpoint(&adm, a, "http://r/");
        p->maxmodels = 1;
        admenqueue(p, a, 1, "A", 4, 1, 0);
        expect(t, "patience.first", admgostr(a, p, 0), "1");
        admenqueue(p, a, 2, "B", 4, 1, 0);
        admenqueue(p, a, 3, "A", 4, 1, ADM_PATIENCE-1);
        expect(t, "passes within", admgostr(a, p, ADM_PATIENCE-1), "3");
        admenqueue(p, a, 4, "A", 4, 1, ADM_PATIENCE);
        expect(t, "not after", admgostr(a, p, ADM_PATIENCE), "0");
        admdone(p, "A", ADM_PATIENCE+1, 1);
        admdone(p, "A", ADM_PATIENCE+2, 1);
        expect(t, "lingering ignored past patience",
               admgostr(a, p, ADM_PATIENCE+2), "2");
        expect(t, "then in line", admgostr(a, p, ADM_PATIENCE+2), "0");
        expect(t, "behind it", istr(a, admpos(p, 4)), "1");
        expect(t, "admnext for its own patience",
               istr(a, admnext(p, ADM_PATIENCE+2)), istr(a, 2*ADM_PATIENCE));
    }

    // Models that take no place, each model's cap, the request limit
    {
        Adm       adm = {};
        AdmPoint *p   = admpoint(&adm, a, "http://r/");
        p->maxmodels = 1;
        admenqueue(p, a, 1, "A", 2, 1, 0);
        admenqueue(p, a, 2, "remote", 2, 0, 0);
        admenqueue(p, a, 3, "A", 2, 1, 0);
        admenqueue(p, a, 4, "A", 2, 1, 0);
        expect(t, "cap.1", admgostr(a, p, 0), "1");
        expect(t, "uncounted beside", admgostr(a, p, 0), "2");
        expect(t, "cap.2", admgostr(a, p, 0), "3");
        expect(t, "at the cap", admgostr(a, p, 0), "0");
        admdone(p, "A", 1, 0);
        expect(t, "under the cap", admgostr(a, p, 1), "4");

        AdmPoint *q = admpoint(&adm, a, "http://q/");
        q->maxreq = 1;
        admenqueue(q, a, 5, "A", 4, 0, 0);
        admenqueue(q, a, 6, "B", 4, 0, 0);
        expect(t, "maxreq.1", admgostr(a, q, 0), "5");
        expect(t, "maxreq.full", admgostr(a, q, 0), "0");
        admdone(q, "A", 1, 1);
        expect(t, "maxreq.next", admgostr(a, q, 1), "6");
    }

    // An uncounted model passes a waiter blocked on a model place, even
    // past its patience, but not under a request limit
    {
        Adm       adm = {};
        AdmPoint *p   = admpoint(&adm, a, "http://r/");
        p->maxmodels = 1;
        admenqueue(p, a, 1, "A", 4, 1, 0);
        expect(t, "remote.first", admgostr(a, p, 0), "1");
        admenqueue(p, a, 2, "B", 4, 1, 0);
        admenqueue(p, a, 3, "remote", 4, 0, ADM_PATIENCE);
        expect(t, "remote passes", admgostr(a, p, ADM_PATIENCE), "3");
        admenqueue(p, a, 4, "remote", 4, 0, ADM_PATIENCE);
        p->maxreq = 8;
        expect(t, "remote waits under maxreq", admgostr(a, p, ADM_PATIENCE), "0");
    }

    // A full KV pool halves a model's concurrency, once per event, then
    // it rises by one each calm period
    {
        Adm       adm = {};
        AdmPoint *p   = admpoint(&adm, a, "http://r/");
        for (u64 id = 1; id <= 4; id++) {
            admenqueue(p, a, id, "A", 8, 1, 0);
            admgo(p, a, 0);
        }
        i32 epoch = admlane(p, 0, "A")->epoch;
        expect(t, "kv.shrink", istr(a, admshrink(p, "A", epoch, 1*S, 10*S)), "2");
        admdone(p, "A", 1*S, 0);
        expect(t, "kv.same event", istr(a, admshrink(p, "A", epoch, 1*S, 10*S)), "0");
        admdone(p, "A", 1*S, 0);
        admdone(p, "A", 1*S, 0);
        admdone(p, "A", 1*S, 0);
        for (u64 id = 5; id <= 7; id++) {
            admenqueue(p, a, id, "A", 8, 1, 2*S);
        }
        expect(t, "kv.first", admgostr(a, p, 2*S), "5");
        expect(t, "kv.second", admgostr(a, p, 2*S), "6");
        expect(t, "kv.limited", admgostr(a, p, 2*S), "0");
        admdone(p, "A", 12*S, 0, 10*S);  // calm: one more at once
        expect(t, "kv.rises", istr(a, admlane(p, 0, "A")->most), "3");
        expect(t, "kv.third", admgostr(a, p, 12*S), "7");
    }

    // Leaving the queue, and no limits at all
    {
        Adm       adm = {};
        AdmPoint *p   = admpoint(&adm, a, "http://r/");
        p->maxmodels = 1;
        admenqueue(p, a, 1, "A", 4, 1, 0);
        admgo(p, a, 0);
        admenqueue(p, a, 2, "B", 4, 1, 0);
        admenqueue(p, a, 3, "C", 4, 1, 0);
        admleave(p, 2);
        expect(t, "left", istr(a, admpos(p, 2)), "0");
        expect(t, "moved up", istr(a, admpos(p, 3)), "1");
        p->maxmodels = 0;
        expect(t, "unlimited", admgostr(a, p, 0), "3");
        expect(t, "recycled", istr(a, p->spare != 0), "1");
    }
}
