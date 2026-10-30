// Eval: tasks run through the engine, to compare models and edit formats
//
//   prospero eval [options] SUITE...
//   prospero eval --report [-c FILE] FILE...
//
// A suite is a directory of tasks, one INI file each (NAME.ini), with
// settings for all of them in suite.ini, which a task's own override
// (a task's prompt or prompt_file, either one, over suite.ini's).
// Each run of a task gets a directory of its own under a temporary one,
// and in it work/, the agent's working directory: a copy of the task's
// fixture, changed by its patch and then its setup command. The agent
// gets the prompt in a session of the engine in this process, under the
// task's limits of time and model requests, unattended, so that asks
// are denied, and with the project's instruction files only, not the
// user's own. The run passes if its turn ended done, every file of
// expect is in work/ byte for byte, and the check command exits 0.
// Nothing of a suite, nor any repository, is ever changed: the agent
// works on copies. A fixture may hold a .git directory, which is
// copied, but no .git file: a worktree's or submodule's, which names
// another repository's git directory, would have the copy share it.
//
// A task's keys, with paths relative to the file that sets them:
//
//   prompt         what the agent is asked, on one line
//   prompt_file    a file holding it, instead
//   fixture        a directory copied as the working directory (or none)
//   patch          a patch in the edit tool's format (*** Begin Patch)
//                  applied to that copy: a mutation to undo, say
//   setup          a command run there before the agent starts
//   expect         a directory whose files must be in work/ afterward
//   check          a command run in work/ afterward: exit status 0 passes
//   timeout        seconds the run may take, not counting waits in the
//                  endpoint's queue (600)
//   steps          model requests at most (100)
//   tools          the tools offered, as a role's tools are (all)
//   role           a [role] of the configuration
//   check_timeout  seconds a setup or check command may take (300)
//
// Commands run in the agent's shell, as its commands do (without the
// variables that endpoints' keys come from), with PROSPERO_EVAL_SUITE
// set to the suite's directory and PROSPERO_EVAL_TASK to the task's
// name. Symbolic links and file modes are not copied: a check runs its
// scripts with sh, not by their names.
//
// Every model (-m, each MODEL or ENDPOINT/MODEL) runs every task in each
// edit format (-f, by default the model's own), n times (-n), with up
// to j runs at once (-j). Their model requests wait their turn at each
// endpoint under its limits ([endpoint] max_concurrent, max_models and
// [model] max_concurrent, see admit.cpp), as any session's do, and that
// waiting is not the run's time. Runs go model by model, so that a
// router with one model loaded at a time swaps models rarely.
//
// Each run appends one JSON line to the results file (-o), whose fields
// are the run's identity (suite, task, endpoint, model, format, own: the
// model's own format, rep), its outcome (pass; why its turn ended, with
// timeout for the eval's cancel at its time limit; err; check, the
// verdict of expect and check alone; expect, the first file that did
// not match; check_exit and check_out), what it took (steps, calls,
// edits, reads; errors by kind, from tool.end's fail: miss, an edit
// that did not apply, args, unknown, loop and other; edit_fails, edit
// calls that failed for any of those, as a patch that does not parse
// fails as args, and a replacement not found as miss; denied by the
// guard; repairs, calls in which the harness forgave something, and
// repaired, how often it forgave each thing, from tool.end's repair;
// nudges and nudged by kind, retries and retried by class, compactions;
// tokens in, out, cached and written; wall_ms and queued_ms), the
// session's sid, and the diff of work/ as the agent left it. All of it
// is counted from the logs of the run's sessions, its subagents'
// included. A run that could not be made has error instead: its patch
// or setup failed, the model never answered (no response, and the turn
// ended in an error, or at its time limit after retries), or a request
// went to a fallback, which the sessions are told not to use. A run
// found in the file already, with no error, is skipped, so an eval
// stopped part way goes on where it stopped. After EVAL_SILENT runs of
// a model in a row (or -j, if more) were never answered, its runs not
// started are left out, and the eval exits 1.
//
// The summary, after the runs and for --report, is a table by model and
// format, what was forgiven, nudged and retried, the runs that failed,
// and for each model run in more than one format, the format to give
// it. Formats are compared on the runs that each of them made of the
// same tasks and repetitions alone, task by task, by sign tests (exact
// McNemar tests with one run a task): the pass rate, then failed edits,
// steps and tokens, the first that differs beyond chance (p < 0.05)
// deciding. A format better so than every other is given, else the
// model keeps its own, as its own runs or the configuration have it.
//
// The engine takes these from new for each run: edit_format, tools,
// steps, instructions "project", fallback 0 and temp "ask", as the
// runs' directories are in the temporary directory (engine.cpp, cmdnew),
// and close with forget lets each session go once its run is over.
//
// This is free and unencumbered software released into the public domain.

static Str evalusage =
"usage: prospero eval [options] SUITE...\n"
"       prospero eval --report [-c FILE] FILE...\n"
"options:\n"
"  -m MODEL,...   models, each MODEL or ENDPOINT/MODEL (default: the\n"
"                 configured model)\n"
"  -e ENDPOINT    endpoint URL or configured name, for models without one\n"
"  -f FORMAT,...  edit formats: replace, patch, hashline, or all\n"
"                 (default: each model's own)\n"
"  -n N           runs of each task, model and format (default 1)\n"
"  -j N           runs at once (default 4)\n"
"  -t TASK,...    only these tasks\n"
"  -o FILE        results, one JSON line a run, appended; runs it holds\n"
"                 already are skipped (default: eval.jsonl)\n"
"  -c FILE        configuration file\n"
"  --state DIR    session storage (default: a temporary directory)\n"
"  --keep         keep the runs' directories\n";

enum : i64 {
    EVAL_TIMEOUT = 600,        // seconds of a run, by default
    EVAL_STEPS   = 100,        // model requests of a run, by default
    EVAL_CMDTIME = 300,        // seconds of a setup or check command
    EVAL_CANCEL  = 60,         // seconds for a cancelled turn to end
    EVAL_FILEMAX = 64 << 20,   // largest file copied, compared or diffed
    EVAL_DIFFMAX = 64 << 10,   // bytes of a run's diff kept
    EVAL_OUTMAX  = 1 << 20,    // bytes of a command's output kept
    EVAL_OUTTAIL = 2 << 10,    // of which a failed command's record shows
    EVAL_LCSMAX  = 1 << 22,    // cells of a line diff's table, at most
    EVAL_CONTEXT = 3,          // lines around a diff's changes
    EVAL_SILENT  = 3,          // runs in a row unanswered drop a model,
                               // or as many as run at once, if more
};


// Suites

struct EvalTask {
    Str suite;     // the suite's name: its directory's
    Str suitedir;  // absolute
    Str name;      // the task file's, without .ini
    Str prompt;
    Str fixture;   // absolute directories, or null
    Str expect;
    Str patch;     // the patch's text, or null
    Str setup;     // commands, or null
    Str check;
    Str tools;     // null for every tool
    Str role;
    i64 timeout;   // seconds
    i64 steps;
    i64 cmdtime;   // seconds
};

static Str evalkeys[] = {
    "prompt", "prompt_file", "fixture", "patch", "setup", "expect", "check",
    "timeout", "steps", "tools", "role", "check_timeout",
};

// A setting's value, trimmed, or null if unset or empty.
static Str evalget(Ini *ini, Str key)
{
    Str v = trim(iniget(ini, "", "", key));
    return v.len ? v : Str{};
}

// A path setting, resolved against the file that set it.
static Str evalpath(Arena *perm, Ini *ini, Str key)
{
    IniEntry *e = inilookup(ini, "", "", key);
    Str       v = e ? trim(e->value) : Str{};
    return v.len ? inipath(perm, inidir(e->file->path), v) : Str{};
}

// A positive number of seconds or steps, dflt if unset; -1 if invalid.
static i64 evalnum(Ini *ini, Str key, i64 dflt, i64 max)
{
    Str v = evalget(ini, key);
    if (!v.data) {
        return dflt;
    }
    Int n = parseint(v);
    return n.ok && n.val>0 && n.val<=max ? n.val : -1;
}

static b32 evaltaskless(EvalTask *a, EvalTask *b)
{
    return compare(a->name, b->name) < 0;
}

// Report a problem with a suite on standard error.
static void evalcomplain(Os *os, Arena scratch, Str file, Str msg)
{
    Buf b(&scratch, 256 + file.len + msg.len);
    print(&b, "prospero: ");
    print(&b, file);
    print(&b, ": ");
    print(&b, msg);
    putbyte(&b, '\n');
    os_print(os, 2, finish(&b));
}

// Report what is wrong in one file of an Ini: its reader's diagnostics,
// and keys a task does not have, or in a section.
static void evalcheckini(Os *os, Arena scratch, Ini *ini, Str path)
{
    for (iz i = 0; i < ini->diags.len; i++) {
        if (ini->diags[i].file == path) {
            Buf b(&scratch, 256);
            print(&b, "prospero: ");
            print(&b, ini->diags[i]);
            putbyte(&b, '\n');
            os_print(os, 2, finish(&b));
        }
    }
    for (IniSection *s = ini->sections; s; s = s->next) {
        for (IniEntry *e = s->keys; e; e = e->next) {
            if (e->file->path != path) {
                continue;
            }
            b32 known = 0;
            for (iz k = 0; k < countof(evalkeys); k++) {
                known |= e->key == evalkeys[k];
            }
            if (s->kind.len) {
                Str msg = concat(&scratch, "a task has no sections, so this "
                                 "key is ignored: ", e->key);
                evalcomplain(os, scratch, path, msg);
            } else if (!known) {
                Str msg = concat(&scratch, "unknown key, ignored: ", e->key);
                evalcomplain(os, scratch, path, msg);
            }
        }
    }
}

// The first .git under dir that is a file, not a directory, or null if
// none. Such a file (a worktree's, or a submodule's) names the git
// directory of another repository, which a copy would share.
static Str evalgitfile(Os *os, Arena *perm, Arena scratch, Str dir)
{
    for (Dirent *d = os_list(os, &scratch, dir); d; d = d->next) {
        Arena t    = scratch;
        Str   path = join(&t, dir, d->name);
        if (!d->isdir && d->name==".git") {
            return clone(perm, path);
        } else if (d->isdir) {
            Str r = evalgitfile(os, perm, t, path);
            if (r.data) {
                return r;
            }
        }
    }
    return {};
}

// A suite's settings for every task: suite.ini, or none.
struct EvalSuiteIni {
    Str path;
    Str text;  // null if there is none
};

// Load a task file over the suite's settings. Returns 0, reported, if
// it is not a task that can run.
static b32 evaltask(Os *os, Arena *perm, Arena scratch, EvalSuiteIni *suite,
                    EvalTask *t, Str path)
{
    Read rd = os_read(os, perm, path, 1<<20);
    if (rd.err) {
        evalcomplain(os, scratch, path, "cannot read the task file");
        return 0;
    }
    Ini *ini = alloc<Ini>(perm);
    if (suite->text.data) {
        iniparse(ini, perm, suite->path, suite->text);
    }
    iniparse(ini, perm, path, rd.data.data ? rd.data : Str{(u8 *)"", 0});
    evalcheckini(os, scratch, ini, path);

    // The prompt from the task's own file, if it sets prompt or
    // prompt_file, over suite.ini's; one file may set only one of them
    IniEntry *pe = inilookup(ini, "", "", "prompt");
    IniEntry *pf = inilookup(ini, "", "", "prompt_file");
    pe = pe && trim(pe->value).len ? pe : 0;
    pf = pf && trim(pf->value).len ? pf : 0;
    if (pe && pf && pe->file==pf->file) {
        evalcomplain(os, scratch, pe->file->path,
                     "both prompt and prompt_file are set: set one of them");
        return 0;
    }
    b32 byfile = pf && (!pe || pf->file->path==path);
    Str pfile  = byfile ? evalpath(perm, ini, "prompt_file") : Str{};
    t->prompt  = byfile ? Str{} : evalget(ini, "prompt");
    t->fixture = evalpath(perm, ini, "fixture");
    t->expect  = evalpath(perm, ini, "expect");
    t->setup   = evalget(ini, "setup");
    t->check   = evalget(ini, "check");
    t->tools   = evalget(ini, "tools");
    t->role    = evalget(ini, "role");
    t->timeout = evalnum(ini, "timeout", EVAL_TIMEOUT, 7*24*3600);
    t->steps   = evalnum(ini, "steps", EVAL_STEPS, STEP_HARD);
    t->cmdtime = evalnum(ini, "check_timeout", EVAL_CMDTIME, 7*24*3600);
    Str patch  = evalpath(perm, ini, "patch");

    if (pfile.data) {
        Read p = os_read(os, perm, pfile, 1<<20);
        if (p.err) {
            Str msg = concat(&scratch, "cannot read ", pfile);
            evalcomplain(os, scratch, path, msg);
            return 0;
        }
        t->prompt = trim(p.data);
    }
    if (patch.data) {
        Read p = os_read(os, perm, patch, 16<<20);
        if (p.err) {
            Str msg = concat(&scratch, "cannot read ", patch);
            evalcomplain(os, scratch, path, msg);
            return 0;
        }
        t->patch = p.data.data ? p.data : Str{(u8 *)"", 0};
        Patch pt = parsepatch(&scratch, t->patch);
        if (pt.err.data) {
            Str msg = concat(&scratch, concat(&scratch, patch, ": "), pt.err);
            evalcomplain(os, scratch, path, msg);
            return 0;
        }
    }
    Str dirs[] = {t->fixture, t->expect};
    for (iz i = 0; i < countof(dirs); i++) {
        if (dirs[i].data && os_stat(os, scratch, dirs[i]).type!=STAT_DIR) {
            Str msg = concat(&scratch, "not a directory: ", dirs[i]);
            evalcomplain(os, scratch, path, msg);
            return 0;
        }
    }
    Str git = t->fixture.data ? evalgitfile(os, perm, scratch, t->fixture) : Str{};
    if (git.data) {
        Str msg = concat(&scratch, git, " is a file, which points at the git "
                         "directory of another repository (a worktree's or a "
                         "submodule's), so git in a copy of the fixture would "
                         "change that repository: make the fixture a clone, or "
                         "a copy without git");
        evalcomplain(os, scratch, path, msg);
        return 0;
    }
    if (!t->prompt.len) {
        evalcomplain(os, scratch, path, "the task has no prompt");
        return 0;
    } else if (t->timeout<0 || t->cmdtime<0) {
        evalcomplain(os, scratch, path, "a timeout must be a whole number of seconds above 0");
        return 0;
    } else if (t->steps < 0) {
        evalcomplain(os, scratch, path, "steps must be 1 to 300");
        return 0;
    } else if (t->tools.data && unknowntools(t->tools).len) {
        Str msg = concat(&scratch, "no such tool: ", unknowntools(t->tools));
        evalcomplain(os, scratch, path, msg);
        return 0;
    } else if (!t->expect.data && !t->check.data) {
        evalcomplain(os, scratch, path, "the task has neither expect nor check, "
                     "so nothing can tell whether it passed");
        return 0;
    }
    return 1;
}

// The tasks of a suite directory, in order of name, those named in only
// if it is given. Returns 0, reported, if the suite cannot run.
static b32 evalsuite(Os *os, Arena *perm, Arena scratch, Str dir,
                     Slice<Str> only, Slice<EvalTask> *out)
{
    Str real = os_realpath(os, perm, dir);
    if (!real.len || os_stat(os, scratch, real).type!=STAT_DIR) {
        evalcomplain(os, scratch, dir, "not a suite directory");
        return 0;
    }
    real = inipath(perm, {}, real);
    EvalSuiteIni suite = {};
    suite.path = join(perm, real, "suite.ini");
    if (os_stat(os, scratch, suite.path).type == STAT_FILE) {
        Read rd = os_read(os, perm, suite.path, 1<<20);
        if (rd.err) {
            evalcomplain(os, scratch, suite.path, "cannot read it");
            return 0;
        }
        suite.text = rd.data.data ? rd.data : Str{(u8 *)"", 0};
        Arena t   = scratch;
        Ini  *ini = alloc<Ini>(&t);
        iniparse(ini, &t, suite.path, suite.text);
        evalcheckini(os, t, ini, suite.path);
    }

    Slice<EvalTask> tasks = {};
    for (Dirent *d = os_list(os, perm, real); d; d = d->next) {
        if (d->isdir || !endswith(d->name, ".ini") || d->name=="suite.ini") {
            continue;
        }
        Str name = cuttail(d->name, 4);
        b32 want = !only.len;
        for (iz i = 0; i < only.len; i++) {
            want |= only[i] == name;
        }
        if (!want) {
            continue;
        }
        EvalTask t = {};
        t.suite    = basename(real);
        t.suitedir = real;
        t.name     = name;
        if (!evaltask(os, perm, scratch, &suite, &t, join(perm, real, d->name))) {
            return 0;
        }
        tasks = push(perm, tasks, t);
    }
    sort(tasks.data, tasks.len, evaltaskless, scratch);
    for (iz i = 0; i < tasks.len; i++) {
        *out = push(perm, *out, tasks[i]);
    }
    return 1;
}


// Directories

// Copy a directory tree, but for any .git if nogit. Otherwise a .git
// file is not copied but refused (evalgitfile): it would share the
// repository it names. Returns null on success, else the path that
// could not be copied.
static Str evalcopy(Os *os, Arena *perm, Arena scratch, Str from, Str to, b32 nogit)
{
    if (!os_mkdirs(os, scratch, to)) {
        return clone(perm, to);
    }
    for (Dirent *d = os_list(os, &scratch, from); d; d = d->next) {
        Arena t   = scratch;
        Str   src = join(&t, from, d->name);
        Str   dst = join(&t, to, d->name);
        if (nogit && d->name==".git") {
            continue;
        } else if (!d->isdir && d->name==".git") {
            return clone(perm, src);
        } else if (d->isdir) {
            Str bad = evalcopy(os, perm, t, src, dst, nogit);
            if (bad.data) {
                return bad;
            }
            continue;
        }
        Read rd = os_read(os, &t, src, EVAL_FILEMAX);
        if (rd.err || !os_write(os, t, dst, rd.data.data ? rd.data : Str{(u8 *)"", 0})) {
            return clone(perm, src);
        }
    }
    return {};
}

// Relative paths of the files under dir, as rel/name, but for anything
// named .git if nogit.
static Slice<Str> evalfiles(Os *os, Arena *a, Str dir, Str rel, b32 nogit,
                            Slice<Str> list)
{
    for (Dirent *d = os_list(os, a, dir); d; d = d->next) {
        Str name = rel.len ? concat(a, concat(a, rel, "/"), d->name) : d->name;
        if (nogit && d->name==".git") {
            continue;
        } else if (!d->isdir) {
            list = push(a, list, name);
        } else {
            list = evalfiles(os, a, join(a, dir, d->name), name, nogit, list);
        }
    }
    return list;
}

static b32 evalstrless(Str *a, Str *b)
{
    return compare(*a, *b) < 0;
}

// The first file of expect that work does not hold byte for byte, as
// "PATH is missing" or "PATH differs", or null if none.
static Str evalexpect(Os *os, Arena *perm, Arena scratch, Str expect, Str work)
{
    Slice<Str> files = evalfiles(os, &scratch, expect, {}, 0, {});
    sort(files.data, files.len, evalstrless, scratch);
    for (iz i = 0; i < files.len; i++) {
        Arena t    = scratch;
        Read  want = os_read(os, &t, join(&t, expect, files[i]), EVAL_FILEMAX);
        Read  got  = os_read(os, &t, join(&t, work, files[i]), EVAL_FILEMAX);
        if (got.err == READ_NOTFOUND) {
            return concat(perm, files[i], " is missing");
        } else if (want.err || got.err || want.data!=got.data) {
            return concat(perm, files[i], " differs");
        }
    }
    return {};
}

// Apply a task's patch to the working directory, refusing any file
// outside it, with paths as Windows has them if win. Returns null on
// success, else why not.
static Str evalpatch(Os *os, Arena *perm, Arena scratch, Str text, Str work, b32 win)
{
    Patch p = parsepatch(&scratch, text);
    if (p.err.data) {
        return concat(perm, "the patch is invalid: ", p.err);
    }
    Str root = normpath(&scratch, work, ".", win);
    for (iz i = 0; i < p.files.len; i++) {
        PatchFile *f = &p.files[i];
        f->file   = normpath(&scratch, root, f->path, win);
        f->fileto = f->to.data ? normpath(&scratch, root, f->to, win) : Str{};
        if (!within(root, f->file, win) || (f->fileto.data && !within(root, f->fileto, win))) {
            return concat(perm, "the patch names a file outside the working "
                          "directory: ", f->path);
        }
    }
    PatchPlan plan = planpatch(os, &scratch, root, &p, win);
    if (!plan.ok) {
        return clone(perm, plan.text);
    }
    for (iz i = 0; i < plan.changes.len; i++) {
        FileChange *c   = &plan.changes[i];
        Str         dir = parentdir(c->path);
        b32 ok = !c->data.data ? os_remove(os, scratch, c->path) :
                 (!dir.len || os_mkdirs(os, scratch, dir)) &&
                 os_write(os, scratch, c->path, c->data);
        if (!ok) {
            return concat(perm, "the patch could not change ", c->path);
        }
    }
    return {};
}


// Diffs

struct EvalText {
    Slice<Str> lines;  // each with its newline, the last maybe without
    b32        binary;
};

static EvalText evaltext(Arena *a, Str data)
{
    EvalText r = {};
    for (iz i = 0; i<data.len && i<8192 && !r.binary; i++) {
        r.binary = !data[i];
    }
    for (Str rest = data; rest.len && !r.binary;) {
        iz n = 0;
        for (; n<rest.len && rest[n]!='\n'; n++) {}
        n += n < rest.len;
        r.lines = push(a, r.lines, takehead(rest, n));
        rest    = cuthead(rest, n);
    }
    return r;
}

// An edit script: for each line kept, removed or added, ' ', '-' or '+'
// with its line in the old text (i) and in the new (j).
struct EvalOp {
    iz i;
    iz j;
    u8 kind;
};

// The edit script from old to now: common ends trimmed, then the longest
// common subsequence of the middle if its table is small enough, else
// the middle replaced whole. The table stays in the arena.
static Slice<EvalOp> evalscript(Arena *a, Slice<Str> old, Slice<Str> now)
{
    iz pre = 0;
    for (; pre<old.len && pre<now.len && old[pre]==now[pre]; pre++) {}
    iz suf = 0;
    for (; suf<old.len-pre && suf<now.len-pre &&
           old[old.len-1-suf]==now[now.len-1-suf]; suf++) {}
    iz n = old.len - pre - suf;
    iz m = now.len - pre - suf;

    i32 *len = 0;  // common lines of the middles' tails, n+1 by m+1
    if (n && m && (n+1) <= EVAL_LCSMAX/(m+1)) {
        len = alloc<i32>(a, (n+1)*(m+1));
        for (iz i = n-1; i >= 0; i--) {
            for (iz j = m-1; j >= 0; j--) {
                i32 *v = len + i*(m+1) + j;
                if (old[pre+i] == now[pre+j]) {
                    *v = v[m+2] + 1;
                } else {
                    *v = v[m+1]>v[1] ? v[m+1] : v[1];
                }
            }
        }
    }

    Slice<EvalOp> r = {};
    for (iz k = 0; k < pre; k++) {
        r = push(a, r, EvalOp{k, k, ' '});
    }
    if (len) {
        iz i = 0, j = 0;
        while (i<n || j<m) {
            i32 *v = len + i*(m+1) + j;
            if (i<n && j<m && old[pre+i]==now[pre+j]) {
                r = push(a, r, EvalOp{pre+i, pre+j, ' '});
                i++;
                j++;
            } else if (j==m || (i<n && v[m+1]>=v[1])) {
                r = push(a, r, EvalOp{pre+i, pre+j, '-'});
                i++;
            } else {
                r = push(a, r, EvalOp{pre+i, pre+j, '+'});
                j++;
            }
        }
    } else {
        for (iz k = 0; k < n; k++) {
            r = push(a, r, EvalOp{pre+k, pre, '-'});
        }
        for (iz k = 0; k < m; k++) {
            r = push(a, r, EvalOp{pre+n, pre+k, '+'});
        }
    }
    for (iz k = 0; k < suf; k++) {
        r = push(a, r, EvalOp{pre+n+k, pre+m+k, ' '});
    }
    return r;
}

static void evalline(Buf *b, u8 kind, Str line)
{
    putbyte(b, kind);
    if (line.len && line[line.len-1]=='\n') {
        print(b, line);
    } else {
        print(b, line);
        print(b, "\n\\ No newline at end of file\n");
    }
}

// A hunk's range, as "start,count", where an empty range's start is the
// line before it.
static void evalrange(Buf *b, iz start, iz count)
{
    print(b, (i64)(count ? start+1 : start));
    if (count != 1) {
        putbyte(b, ',');
        print(b, (i64)count);
    }
}

// A unified diff of one file, old or now null for one added or removed.
static void evaldiff(Buf *b, Arena scratch, Str path, Str old, Str now)
{
    EvalText x = evaltext(&scratch, old);
    EvalText y = evaltext(&scratch, now);
    if (x.binary || y.binary) {
        print(b, "Binary files ");
        print(b, old.data ? concat(&scratch, "a/", path) : Str("/dev/null"));
        print(b, " and ");
        print(b, now.data ? concat(&scratch, "b/", path) : Str("/dev/null"));
        print(b, " differ\n");
        return;
    }
    print(b, "--- ");
    print(b, old.data ? concat(&scratch, "a/", path) : Str("/dev/null"));
    print(b, "\n+++ ");
    print(b, now.data ? concat(&scratch, "b/", path) : Str("/dev/null"));
    putbyte(b, '\n');

    Slice<EvalOp> ops = evalscript(&scratch, x.lines, y.lines);
    iz            ctx = EVAL_CONTEXT;
    for (iz k = 0; k < ops.len;) {
        if (ops[k].kind == ' ') {
            k++;
            continue;
        }
        // A hunk: from context before this change to context after the
        // last change no more than twice the context away
        iz beg = k - ctx;
        beg = beg<0 ? 0 : beg;
        iz end = k;
        for (iz gap = 0; end<ops.len && gap<=2*ctx; end++) {
            gap = ops[end].kind==' ' ? gap+1 : 0;
        }
        for (; end>k && ops[end-1].kind==' '; end--) {}
        end = end+ctx<ops.len ? end+ctx : ops.len;

        iz ostart = ops[beg].i, nstart = ops[beg].j, on = 0, nn = 0;
        for (iz h = beg; h < end; h++) {
            on += ops[h].kind != '+';
            nn += ops[h].kind != '-';
        }
        print(b, "@@ -");
        evalrange(b, ostart, on);
        print(b, " +");
        evalrange(b, nstart, nn);
        print(b, " @@\n");
        for (iz h = beg; h < end; h++) {
            EvalOp o = ops[h];
            evalline(b, o.kind, o.kind=='+' ? y.lines[o.j] : x.lines[o.i]);
        }
        k = end;
    }
}

// The changes from start to work, as a unified diff of each file that
// differs, in order of path, .git directories aside, cut at max bytes.
static Str evaltreediff(Os *os, Arena *perm, Arena scratch, Str start, Str work, iz max)
{
    Slice<Str> a = evalfiles(os, &scratch, start, {}, 1, {});
    Slice<Str> b = evalfiles(os, &scratch, work, {}, 1, {});
    sort(a.data, a.len, evalstrless, scratch);
    sort(b.data, b.len, evalstrless, scratch);

    Buf out(perm, 1024);
    for (iz i = 0, j = 0; (i<a.len || j<b.len) && out.len<max;) {
        i32 c = i==a.len ? 1 : j==b.len ? -1 : compare(a[i], b[j]);
        Str path = c<=0 ? a[i] : b[j];
        Arena t  = scratch;
        Read  x  = c<=0 ? os_read(os, &t, join(&t, start, path), EVAL_FILEMAX) : Read{};
        Read  y  = c>=0 ? os_read(os, &t, join(&t, work, path), EVAL_FILEMAX) : Read{};
        i += c <= 0;
        j += c >= 0;
        if (x.err || y.err) {
            print(&out, "(");
            print(&out, path);
            print(&out, " could not be read)\n");
            continue;
        }
        Str old = c>0 ? Str{} : x.data.data ? x.data : Str{(u8 *)"", 0};
        Str now = c<0 ? Str{} : y.data.data ? y.data : Str{(u8 *)"", 0};
        if (c==0 && old==now) {
            continue;
        }
        evaldiff(&out, t, path, old, now);
    }
    Str r = finish(&out);
    if (r.len > max) {
        r = concat(perm, utf8head(r, max), "\n[the diff is cut here]\n");
    }
    return r;
}


// What a run took, from the logs of its sessions

// How often each of some kinds of thing happened.
struct EvalCount {
    Str what;
    i64 n;
};

static Slice<EvalCount> evalcount(Arena *perm, Slice<EvalCount> c, Str what, i64 n)
{
    for (iz i = 0; i < c.len; i++) {
        if (c[i].what == what) {
            c[i].n += n;
            return c;
        }
    }
    EvalCount e = {clone(perm, what), n};
    return push(perm, c, e);
}

// Count each phrase of a call's repair: those of callrepair, separated
// by "; ", and of editrepair, by ", ".
static Slice<EvalCount> evalfixes(Arena *perm, Slice<EvalCount> c, Str repair)
{
    for (Str rest = repair; rest.len;) {
        iz at = 0;
        for (; at<rest.len && !((rest[at]==';' || rest[at]==',') &&
                                at+1<rest.len && rest[at+1]==' '); at++) {}
        Str phrase = trim(takehead(rest, at));
        if (phrase.len) {
            c = evalcount(perm, c, phrase, 1);
        }
        rest = cuthead(rest, at<rest.len ? at+2 : at);
    }
    return c;
}

struct EvalStats {
    Str              format;  // the run's own session's edit format
    Str              other;   // a model other than its own that its own
                              // session asked (a fallback), else null
    Slice<Str>       sids;    // its sessions: its own, then its subagents'
    Slice<EvalCount> fixes;   // repairs, by what was forgiven
    Slice<EvalCount> nudged;  // nudges, by kind
    Slice<EvalCount> retried; // retries, by class of failure
    i64        steps;
    i64        calls;
    i64        edits;
    i64        reads;
    i64        errors[5];  // EVAL_MISS ...
    i64        editfails;  // edit calls that failed, for any of those
    i64        denied;
    i64        repairs;
    i64        nudges;
    i64        retries;
    i64        compactions;
    i64        in;
    i64        out;
    i64        cached;
    i64        written;
};

enum : i32 {
    EVAL_MISS,     // an edit that did not apply
    EVAL_ARGS,     // invalid arguments
    EVAL_UNKNOWN,  // no such tool
    EVAL_LOOP,     // not run: the same calls again
    EVAL_OTHER,    // any other failure
};

static Str evalerrnames[] = {"miss", "args", "unknown", "loop", "other"};

static Str evallogpath(Arena *a, Str statedir, Str sid)
{
    return join(a, join(a, join(a, statedir, "sessions"), sid), "events.jsonl");
}

// Count what the logs of a run's sessions hold: its own, then each that
// a task call of one of them started. Edit calls are known by their ids
// from tool.start, in a map in perm, which a failed one's tool.end finds.
static EvalStats evalstats(Os *os, Arena *perm, Arena scratch, Str statedir, Str sid)
{
    EvalStats r   = {};
    Str       own = {};  // the model of the run's own session
    r.format = editnames[EDIT_REPLACE];
    r.sids   = push(perm, r.sids, sid);
    for (iz n = 0; n < r.sids.len; n++) {
        Arena      t     = scratch;
        Read       rd    = os_read(os, &t, evallogpath(&t, statedir, r.sids[n]), LOG_MAX);
        Slice<Str> kids  = {};
        Map<b32>  *edits = 0;
        for (Str rest = rd.data; rest.len;) {
            Cut   c = cut(rest, '\n');
            Arena u = t;
            EvRec e = evparse(&u, c.head);
            Json *j = e.root;
            rest = c.tail;
            switch (e.ev) {
            case EV_SESSION:
                if (!n) {
                    r.format = editnames[editformat(jsonstr(jsonget(j, "edit_format")))];
                    own      = clone(perm, jsonstr(jsonget(j, "model")));
                }
                break;
            case EV_ATTEMPT: {
                Str model = jsonstr(jsonget(j, "model"));
                if (!n && own.len && model.len && model!=own && !r.other.data) {
                    r.other = clone(perm, model);
                }
            } break;
            case EV_ASSISTANT: {
                Json *usage = jsonget(j, "usage");
                r.steps++;
                r.in      += jsonint(jsonget(usage, "in"), 0);
                r.out     += jsonint(jsonget(usage, "out"), 0);
                r.cached  += jsonint(jsonget(usage, "cached"), 0);
                r.written += jsonint(jsonget(usage, "written"), 0);
            } break;
            case EV_TOOLSTART: {
                Str name = jsonstr(jsonget(j, "name"));
                r.calls++;
                r.edits += name == "edit";
                r.reads += name == "read";
                if (name == "edit") {
                    Str call = clone(perm, jsonstr(jsonget(j, "call")));
                    *upsert(&edits, call, perm) = 1;
                }
            } break;
            case EV_TOOLEND: {
                Str status = jsonstr(jsonget(j, "status"));
                Str fail   = jsonstr(jsonget(j, "fail"));
                Str child  = jsonstr(jsonget(j, "child"));
                r.denied  += status == "denied";
                Str repair = jsonstr(jsonget(j, "repair"));
                r.repairs += repair.len > 0;
                r.fixes    = evalfixes(perm, r.fixes, repair);
                if (status == "error") {
                    i32 k = EVAL_OTHER;
                    for (i32 i = 0; i < EVAL_OTHER; i++) {
                        k = fail==evalerrnames[i] ? i : k;
                    }
                    r.errors[k]++;
                    Str call = jsonstr(jsonget(j, "call"));
                    r.editfails += upsert(&edits, call, (Arena *)0) != 0;
                }
                if (child.len) {
                    kids = push(&t, kids, child);
                }
            } break;
            case EV_RETRY:
                r.retries++;
                r.retried = evalcount(perm, r.retried, jsonstr(jsonget(j, "class")), 1);
                break;
            case EV_NUDGE:
                r.nudges++;
                r.nudged = evalcount(perm, r.nudged, jsonstr(jsonget(j, "kind")), 1);
                break;
            case EV_COMPACT: r.compactions++; break;
            }
        }
        // The log is done with: its subagents' ids into perm, which t
        // does not overlap, as scratch is the caller's other arena
        for (iz i = 0; i < kids.len; i++) {
            r.sids = push(perm, r.sids, clone(perm, kids[i]));
        }
    }
    return r;
}


// Running

struct EvalModel {
    Str endpoint;  // a configured name or a URL
    Str model;
    Str own;       // its configured edit format (evalowns)
    i32 silent;    // its latest runs in a row that it never answered
    b32 dropped;   // so many that its runs left are not made
};

enum : i32 {
    RUN_TODO,
    RUN_SETUP,  // its setup command runs
    RUN_TURN,   // the agent works
    RUN_CHECK,  // its check command runs
    RUN_DONE,
};

struct EvalRun {
    EvalTask  *task;
    EvalModel *model;
    i32        format;    // EDIT_*, or -1 for the model's own
    i32        rep;       // from 1
    i32        state;     // RUN_*
    i64        id;        // its new command's, and its commands' records
    Str        base;      // its directory, holding:
    Str        work;      // the agent's working directory
    Str        start;     // work as the agent got it
    Str        sid;       // its own session's
    i64        lastseq;   // its latest durable record seen
    i64        began;     // ticks: new sent
    i64        ended;     // ticks: its turn ended
    i64        queued;    // ticks waiting at the endpoint
    i64        queuedat;  // ticks: waiting since then, 0 if not
    i64        cancelat;  // ticks: cancelled at its time limit, 0 if not
    Str        why;       // how its turn ended
    Str        err;
    Str        error;     // why it could not be made, null if it could
    Str        missed;    // expect's first file not as expected
    // Its setup or check command
    Proc      *proc;
    u64        procid;    // its records': its own, so none of setup's
                          // is taken for check's
    i64        procat;    // ticks: started
    i64        exitat;    // ticks: exited, 0 if not yet
    i32        code;
    b32        eof;
    b32        killed;    // at its time limit
    b32        checked;   // the check command ran
    i32        checkcode; // and exited so, -1 if killed
    Str        checkout;  // the end of its output, if it failed
};

struct Eval {
    Os               *os;
    Engine           *e;
    Mailbox          *mbox;     // the engine's records and the commands'
    Region           *spare;    // temporaries of reading logs and trees
    ToolEnv           te;       // the agent's shell, for commands
    Slice<Str>        env;
    Slice<Str>        secrets;  // variables the commands do not get
    Str               root;     // the runs' directories are under it
    Str               statedir;
    iz                out;      // the results file, as a log
    b32               keep;
    i32               jobs;     // runs at once
    Slice<EvalRun *>  runs;     // to run, in order
    iz                next;     // the next to start
    EvalRun         **live;     // running, jobs of them, null when free
    i32               nlive;
    i32               done;
    i32               silent;   // runs in a row unanswered that drop a model
    i64               skipped;  // runs not made, their models dropped
    i64               nextid;
};

static Str evalformat(EvalRun *run)
{
    return run->format<0 ? Str{} : editnames[run->format];
}

static void evalsend(Eval *ev, Arena scratch, i32 cmd, Str sid, b32 forget)
{
    Buf   b(&scratch, 256);
    EvOut w = cmdbegin(&b, cmd, 0);
    putstrkey(&w.json, "sid", sid);
    if (forget) {
        putintkey(&w.json, "forget", 1);
    }
    enginecmd(ev->e, cuttail(evend(&w), 1));
}

// Write a run's line of results and report it, and free its place.
static void evalfinish(Eval *ev, Arena scratch, EvalRun *run)
{
    Os       *os   = ev->os;
    EvalTask *t    = run->task;
    Arena     a    = scratch;
    EvalStats st   = {};
    Str       dif  = {};
    b32       chk  = 0;
    b32       mute = 0;  // the model never answered
    if (!run->error.data) {
        st  = evalstats(os, &a, newarena(ev->spare), ev->statedir, run->sid);
        dif = evaltreediff(os, &a, newarena(ev->spare), run->start, run->work, EVAL_DIFFMAX);
        chk = !run->missed.data && (!t->check.data || run->checkcode==0);
        // What measures the endpoint or the configuration, rather than
        // the model, is no run: it is left out, and made again next time.
        // A model too slow for the time limit, but answering, is a fail.
        mute = !st.steps && (run->why=="error" || (run->why=="timeout" && st.retries));
        if (st.other.data) {
            Buf b(&a, 256);
            print(&b, "a request went to another model, ");
            print(&b, st.other);
            print(&b, ", a fallback, so the run measured that one");
            run->error = finish(&b);
        } else if (mute) {
            Str err = run->err.len ? run->err : run->why;
            run->error = concat(&a, "the model never answered: ", err);
        }
    }
    b32 pass = chk && run->why=="done";

    Buf     b(&a, 4096 + 2*dif.len + 2*run->checkout.len);
    JsonOut w = {&b};
    putobject(&w);
    putstrkey(&w, "suite", t->suite);
    putstrkey(&w, "task", t->name);
    putstrkey(&w, "endpoint", run->model->endpoint);
    putstrkey(&w, "model", run->model->model);
    putstrkey(&w, "format", run->error.data ? evalformat(run) : st.format);
    putkey(&w, "own");
    putbool(&w, run->format < 0);
    putintkey(&w, "rep", run->rep);
    if (run->error.data) {
        putstrkey(&w, "error", run->error);
    } else {
        putkey(&w, "pass");
        putbool(&w, pass);
        putstrkey(&w, "why", run->why);
        if (run->err.len) {
            putstrkey(&w, "err", run->err);
        }
        putkey(&w, "check");
        putbool(&w, chk);
        if (run->missed.data) {
            putstrkey(&w, "expect", run->missed);
        }
        if (run->checked) {
            putintkey(&w, "check_exit", run->checkcode);
        }
        if (run->checkout.len) {
            putstrkey(&w, "check_out", run->checkout);
        }
        putintkey(&w, "steps", st.steps);
        putintkey(&w, "calls", st.calls);
        putintkey(&w, "edits", st.edits);
        putintkey(&w, "reads", st.reads);
        putkey(&w, "errors");
        putobject(&w);
        for (i32 i = 0; i < countof(evalerrnames); i++) {
            putintkey(&w, evalerrnames[i], st.errors[i]);
        }
        putend(&w);
        putintkey(&w, "edit_fails", st.editfails);
        putintkey(&w, "denied", st.denied);
        putintkey(&w, "repairs", st.repairs);
        putkey(&w, "repaired");
        putobject(&w);
        for (iz i = 0; i < st.fixes.len; i++) {
            putintkey(&w, st.fixes[i].what, st.fixes[i].n);
        }
        putend(&w);
        putintkey(&w, "nudges", st.nudges);
        putkey(&w, "nudged");
        putobject(&w);
        for (iz i = 0; i < st.nudged.len; i++) {
            putintkey(&w, st.nudged[i].what, st.nudged[i].n);
        }
        putend(&w);
        putintkey(&w, "retries", st.retries);
        putkey(&w, "retried");
        putobject(&w);
        for (iz i = 0; i < st.retried.len; i++) {
            putintkey(&w, st.retried[i].what, st.retried[i].n);
        }
        putend(&w);
        putintkey(&w, "compactions", st.compactions);
        putkey(&w, "tokens");
        putobject(&w);
        putintkey(&w, "in", st.in);
        putintkey(&w, "out", st.out);
        putintkey(&w, "cached", st.cached);
        putintkey(&w, "written", st.written);
        putend(&w);
        putintkey(&w, "wall_ms", (run->ended - run->began)/MS);
        putintkey(&w, "queued_ms", run->queued/MS);
        putstrkey(&w, "sid", run->sid);
        putstrkey(&w, "diff", dif);
    }
    putend(&w);
    putbyte(&b, '\n');
    if (!os_logwrite(os, ev->out, finish(&b))) {
        os_print(os, 2, "prospero: cannot write the results file\n");
    }

    ev->done++;
    Buf p(&a, 512);
    print(&p, "eval: [");
    print(&p, (i64)ev->done);
    putbyte(&p, '/');
    print(&p, (i64)ev->runs.len);
    print(&p, "] ");
    print(&p, run->model->model);
    putbyte(&p, ' ');
    print(&p, run->error.data ? evalformat(run) : st.format);
    putbyte(&p, ' ');
    print(&p, t->name);
    print(&p, " #");
    print(&p, (i64)run->rep);
    print(&p, ": ");
    if (run->error.data) {
        print(&p, "error: ");
        print(&p, run->error);
    } else {
        print(&p, pass ? Str("pass") : Str("FAIL"));
        if (run->why != "done") {
            print(&p, " (");
            print(&p, run->why);
            print(&p, ")");
        }
        print(&p, ", ");
        print(&p, st.steps);
        print(&p, st.steps==1 ? Str(" step, ") : Str(" steps, "));
        i64 tenths = (run->ended - run->began - run->queued) / (100*MS);
        print(&p, tenths/10);
        putbyte(&p, '.');
        print(&p, tenths%10);
        print(&p, " s");
    }
    putbyte(&p, '\n');
    os_print(os, 2, finish(&p));

    // A model whose endpoint answers nothing gets no more runs, which
    // would each wait out their time for nothing
    EvalModel *m = run->model;
    m->silent = mute ? m->silent+1 : run->error.data ? m->silent : 0;
    if (m->silent>=ev->silent && !m->dropped) {
        m->dropped = 1;
        Buf q(&a, 512);
        print(&q, "eval: ");
        print(&q, m->model);
        print(&q, ": ");
        print(&q, (i64)m->silent);
        print(&q, " runs in a row got no answer (");
        print(&q, run->error);
        print(&q, "), so its runs not yet started are left out; an eval "
                  "run again makes them\n");
        os_print(os, 2, finish(&q));
    }

    if (!ev->keep) {
        removeall(os, scratch, run->base);
    }
    run->state = RUN_DONE;
    for (i32 i = 0; i < ev->jobs; i++) {
        if (ev->live[i] == run) {
            ev->live[i] = 0;
            ev->nlive--;
        }
    }
}

static void evalfail(Eval *ev, Arena scratch, EvalRun *run, Str error)
{
    run->error = error;
    evalfinish(ev, scratch, run);
}

// Start a command of the run's in the agent's shell, its output into
// base/log. Returns 0 if it could not start.
static b32 evalspawn(Eval *ev, Arena scratch, EvalRun *run, Str command, Str log)
{
    EvalTask  *t   = run->task;
    ShellCmd   sc  = shellcmd(&scratch, &ev->te.shell, ev->te.windows, command);
    Slice<Str> env = shellenv(&scratch, ev->env, ev->te.windows, ev->secrets);
    if (sc.env.len) {
        env = push(&scratch, env, sc.env);
    }
    env = push(&scratch, env, concat(&scratch, "PROSPERO_EVAL_SUITE=", t->suitedir));
    env = push(&scratch, env, concat(&scratch, "PROSPERO_EVAL_TASK=", t->name));
    Spawn sp = {};
    sp.argv     = sc.argv;
    sp.cmdline  = sc.cmdline;
    sp.cwd      = run->work;
    sp.env      = env;
    sp.outpath  = join(&scratch, run->base, log);
    sp.maxbytes = EVAL_OUTMAX;
    sp.mbox     = ev->mbox;
    sp.id       = (u64)run->id<<2 | (run->state==RUN_SETUP ? 1 : 2);
    run->procid = sp.id;
    run->proc   = os_spawn(ev->os, &sp, scratch);
    run->procat = os_ticks(ev->os);
    run->exitat = 0;
    run->eof    = 0;
    run->killed = 0;
    return run->proc != 0;
}

// The end of a command's output, cleaned, for a record.
static Str evaltail(Os *os, Arena *perm, Arena scratch, Str path)
{
    Read rd = os_read(os, &scratch, path, EVAL_OUTMAX + 4096);
    Str  s  = trim(cleanoutput(&scratch, rd.data));
    return clone(perm, utf8tail(s, EVAL_OUTTAIL));
}

// Give the agent its turn: work as it now is is the start, then a
// session with the task's prompt and limits.
static void evalbegin(Eval *ev, Arena scratch, Arena *perm, EvalRun *run)
{
    EvalTask *t   = run->task;
    Str       bad = evalcopy(ev->os, perm, scratch, run->work, run->start, 1);
    if (bad.data) {
        evalfail(ev, scratch, run, concat(perm, "cannot copy ", bad));
        return;
    }
    Buf   b(&scratch, 1024 + t->prompt.len);
    EvOut w = cmdbegin(&b, CMD_NEW, run->id);
    putstrkey(&w.json, "cwd", run->work);
    putstrkey(&w.json, "model", run->model->model);
    putstrkey(&w.json, "endpoint", run->model->endpoint);
    if (t->role.data) {
        putstrkey(&w.json, "role", t->role);
    }
    if (run->format >= 0) {
        putstrkey(&w.json, "edit_format", editnames[run->format]);
    }
    if (t->tools.data) {
        putstrkey(&w.json, "tools", t->tools);
    }
    putintkey(&w.json, "steps", t->steps);
    putstrkey(&w.json, "instructions", "project");
    putintkey(&w.json, "fallback", 0);
    putstrkey(&w.json, "temp", "ask");
    putstrkey(&w.json, "prompt", t->prompt);
    enginecmd(ev->e, cuttail(evend(&w), 1));
    run->state = RUN_TURN;
    run->began = os_ticks(ev->os);
}

// Make the run's directory and start it.
static void evalstart(Eval *ev, Arena scratch, Arena *perm, EvalRun *run)
{
    Os       *os = ev->os;
    EvalTask *t  = run->task;
    run->id    = ++ev->nextid;
    Buf name(perm, 64);
    printpad(&name, run->id, 4);
    run->base  = join(perm, join(perm, ev->root, "runs"), finish(&name));
    run->work  = join(perm, run->base, "work");
    run->start = join(perm, run->base, "start");
    for (i32 i = 0; i < ev->jobs; i++) {
        if (!ev->live[i]) {
            ev->live[i] = run;
            ev->nlive++;
            break;
        }
    }

    Str bad = t->fixture.data ? evalcopy(os, perm, scratch, t->fixture, run->work, 0)
                              : os_mkdirs(os, scratch, run->work) ? Str{} : run->work;
    if (bad.data) {
        evalfail(ev, scratch, run, concat(perm, "cannot copy ", bad));
        return;
    }
    // The work directory's real path, as the session will have it
    Str real = os_realpath(os, perm, run->work);
    run->work = real.len ? real : run->work;
    if (t->patch.data) {
        Str why = evalpatch(os, perm, scratch, t->patch, run->work, HOST_WINDOWS);
        if (why.data) {
            evalfail(ev, scratch, run, why);
            return;
        }
    }
    if (t->setup.data) {
        run->state = RUN_SETUP;
        if (!evalspawn(ev, scratch, run, t->setup, "setup.log")) {
            Str err = os_lasterr(os, &scratch);
            evalfail(ev, scratch, run, concat(perm, "the setup command could not start: ", err));
        }
        return;
    }
    evalbegin(ev, scratch, perm, run);
}

// Its turn ended: close its sessions, then check what it left.
static void evalturnend(Eval *ev, Arena scratch, Arena *perm, EvalRun *run, Json *j)
{
    Os       *os  = ev->os;
    EvalTask *t   = run->task;
    i64       now = os_ticks(os);
    Str       why = jsonstr(jsonget(j, "why"));
    run->ended = now;
    if (run->queuedat) {
        run->queued  += now - run->queuedat;
        run->queuedat = 0;
    }
    run->why = run->cancelat && why=="cancel" ? Str("timeout") : clone(perm, why);
    run->err = clone(perm, jsonstr(jsonget(j, "err")));
    if (run->cancelat && why=="cancel") {
        Buf b(perm, 128);
        print(&b, "the run took longer than its limit of ");
        print(&b, t->timeout);
        print(&b, " s");
        run->err = finish(&b);
    }

    // Every session of the run leaves memory once idle
    EvalStats st = evalstats(os, &scratch, newarena(ev->spare), ev->statedir, run->sid);
    for (iz i = 0; i < st.sids.len; i++) {
        evalsend(ev, scratch, CMD_CLOSE, st.sids[i], 1);
    }

    if (t->expect.data) {
        run->missed = evalexpect(os, perm, scratch, t->expect, run->work);
    }
    if (t->check.data) {
        run->state = RUN_CHECK;
        if (!evalspawn(ev, scratch, run, t->check, "check.log")) {
            Str err = os_lasterr(os, &scratch);
            evalfail(ev, scratch, run, concat(perm, "the check command could not start: ", err));
        }
        return;
    }
    evalfinish(ev, scratch, run);
}

// A command of the run's ended and drained, or its drain was given up:
// kill what it left, and go on.
static void evalprocdone(Eval *ev, Arena scratch, Arena *perm, EvalRun *run)
{
    Os       *os  = ev->os;
    EvalTask *t   = run->task;
    os_kill(os, run->proc, 1);
    os_procfree(os, run->proc);
    run->proc = 0;
    b32 setup = run->state == RUN_SETUP;
    Str log   = join(&scratch, run->base, setup ? Str("setup.log") : Str("check.log"));
    if (setup && (run->killed || run->code)) {
        Buf b(&scratch, 256);
        print(&b, "the setup command ");
        if (run->killed) {
            print(&b, "took longer than ");
            print(&b, t->cmdtime);
            print(&b, " s");
        } else {
            print(&b, "exited with status ");
            print(&b, (i64)run->code);
        }
        Str out = evaltail(os, perm, scratch, log);
        if (out.len) {
            print(&b, ": ");
            print(&b, out);
        }
        evalfail(ev, scratch, run, clone(perm, finish(&b)));
        return;
    } else if (setup) {
        evalbegin(ev, scratch, perm, run);
        return;
    }
    run->checked   = 1;
    run->checkcode = run->killed ? -1 : run->code;
    if (run->checkcode) {
        run->checkout = evaltail(os, perm, scratch, log);
    }
    evalfinish(ev, scratch, run);
}

static EvalRun *evalbysid(Eval *ev, Str sid)
{
    for (i32 i = 0; sid.len && i < ev->jobs; i++) {
        EvalRun *run = ev->live[i];
        if (run && run->state==RUN_TURN && run->sid==sid) {
            return run;
        }
    }
    return 0;
}

static EvalRun *evalbyid(Eval *ev, i64 id)
{
    for (i32 i = 0; i < ev->jobs; i++) {
        EvalRun *run = ev->live[i];
        if (run && run->id==id) {
            return run;
        }
    }
    return 0;
}

// One record from the engine.
static void evalrecord(Eval *ev, Arena scratch, Arena *perm, Str line)
{
    Arena t  = scratch;
    EvRec r  = evparse(&t, line);
    Json *j  = r.root;
    if (r.ev == EV_REPLY) {
        // To a new: the run's session, or why it has none
        EvalRun *run = evalbyid(ev, jsonint(jsonget(j, "id"), 0));
        if (run && run->state==RUN_TURN && !run->sid.data) {
            if (!jsonint(jsonget(j, "ok"), 0)) {
                Str err = concat(perm, "the engine refused the run: ",
                                 jsonstr(jsonget(j, "err")));
                evalfail(ev, t, run, err);
            } else {
                run->sid = clone(perm, r.sid);
            }
        }
        return;
    }

    EvalRun *run = evalbysid(ev, r.sid);
    if (!run) {
        return;
    }
    run->lastseq = r.seq ? r.seq : run->lastseq;
    if (r.ev == EV_LIVE) {
        // Time in the endpoint's queue is not the run's
        i64 now    = os_ticks(ev->os);
        b32 queued = jsonstr(jsonget(j, "state")) == "queued";
        if (queued && !run->queuedat) {
            run->queuedat = now;
        } else if (!queued && run->queuedat) {
            run->queued  += now - run->queuedat;
            run->queuedat = 0;
        }
    } else if (r.ev == EV_TURNEND) {
        evalturnend(ev, t, perm, run, j);
    }
}

// Records lost to a full mailbox: each run's own come back from its log,
// all but its queue's comings and goings, which are not logged.
static void evalresync(Eval *ev, Arena scratch, Arena *perm)
{
    for (i32 i = 0; i < ev->jobs; i++) {
        EvalRun *run = ev->live[i];
        if (!run || run->state!=RUN_TURN || !run->sid.data) {
            continue;
        }
        // Each line into scratch: handling it may use the spare region
        Arena  lines = newarena(ev->spare);
        Str    path  = evallogpath(&scratch, ev->statedir, run->sid);
        Replay rp    = logreplay(ev->os, &lines, scratch, path, run->sid,
                                 run->lastseq, LOG_MAX);
        Slice<Str> got = {};
        for (iz k = 0; k < rp.lines.len; k++) {
            got = push(&scratch, got, clone(&scratch, rp.lines[k]));
        }
        for (iz k = 0; k<got.len && run->state==RUN_TURN; k++) {
            evalrecord(ev, scratch, perm, got[k]);
        }
    }
}

// Deadlines: a turn past its time, a cancelled turn that did not end, a
// command past its time or its drain. Returns the next, 0 if none.
static i64 evaltick(Eval *ev, Arena scratch, Arena *perm)
{
    i64 now  = os_ticks(ev->os);
    i64 next = 0;
    for (i32 i = 0; i < ev->jobs; i++) {
        EvalRun *run = ev->live[i];
        if (!run) {
            continue;
        }
        i64 at = 0;
        if (run->proc && !run->exitat) {
            at = run->procat + run->task->cmdtime*SEC;
            if (now>=at && !run->killed) {
                os_kill(ev->os, run->proc, 1);
                run->killed = 1;
            }
            at = run->killed ? 0 : at;
        } else if (run->proc) {
            at = run->exitat + DRAIN;
            if (now >= at) {
                evalprocdone(ev, scratch, perm, run);
                continue;
            }
        } else if (run->state==RUN_TURN && run->sid.data && run->cancelat) {
            at = run->cancelat + (i64)EVAL_CANCEL*SEC;
            if (now >= at) {
                evalsend(ev, scratch, CMD_CLOSE, run->sid, 1);
                evalfail(ev, scratch, run, "the turn did not end after it was cancelled");
                continue;
            }
        } else if (run->state==RUN_TURN && run->sid.data && !run->queuedat) {
            at = run->began + run->queued + run->task->timeout*SEC;
            if (now >= at) {
                evalsend(ev, scratch, CMD_CANCEL, run->sid, 0);
                run->cancelat = now;
                at = now + (i64)EVAL_CANCEL*SEC;
            }
        }
        next = at && (!next || at<next) ? at : next;
    }
    return next;
}

// A process record for a run's command.
static void evalproc(Eval *ev, Arena scratch, Arena *perm, Record r)
{
    EvalRun *run = evalbyid(ev, (i64)(r.id>>2));
    if (!run || !run->proc || run->procid!=r.id) {
        return;  // a command whose drain was given up
    }
    if (r.type == PROC_EXIT) {
        run->exitat = os_ticks(ev->os);
        run->code   = get32(r.data);
    } else if (r.type == PROC_EOF) {
        run->eof = 1;
    }
    if (run->exitat && run->eof) {
        evalprocdone(ev, scratch, perm, run);
    }
}


// Summary

// A run with a result, as the format recommendation compares them.
struct EvalRec {
    Str key;     // suite, task and repetition: what pairs runs
    Str task;    // suite and task: what a sign test counts by
    b32 own;     // in the model's own format
    i64 pass;
    i64 fails;   // failed edits
    i64 steps;
    i64 tokens;  // in and out
};

struct EvalCell {
    EvalCell *next;
    Str       endpoint;
    Str       model;
    Str       format;
    i64       runs;      // with a result: errors aside
    i64       pass;
    i64       errors;    // runs that could not be made
    i64       steps;     // the rest are sums
    i64       calls;
    i64       fails;     // failed edit calls, of any kind
    i64       bad;       // invalid calls: args, unknown, loop
    i64       denied;
    i64       repairs;
    i64       retries;
    i64       in;
    i64       cached;
    i64       out;
    i64       ms;        // of the runs' own time, queueing aside
    Slice<EvalCount> fixes;   // what repairs forgave
    Slice<EvalCount> nudged;  // nudges by kind
    Slice<EvalCount> retried; // retries by class
    Slice<EvalRec>   recs;    // its runs, in order
    i32       order;     // its model's, by first appearance
};

static i32 evalformatrank(Str format)
{
    for (i32 i = 0; i < EDIT_COUNT; i++) {
        if (format == editnames[i]) {
            return i;
        }
    }
    return EDIT_COUNT;
}

// Thousands separated: 13,898.
static void evalthousands(Buf *b, i64 v)
{
    if (v < 0) {
        putbyte(b, '-');
        v = -v;
    }
    if (v >= 1000) {
        evalthousands(b, v/1000);
        putbyte(b, ',');
        printpad(b, v%1000, 3);
    } else {
        print(b, v);
    }
}

// A sum's mean over n, with a fixed number of decimals, or with none
// in thousands (evalthousands). Sums are not negative.
static void evalmean(Buf *b, i64 sum, i64 n, i32 decimals)
{
    i64 scale = 1;
    for (i32 i = 0; i < decimals; i++) {
        scale *= 10;
    }
    i64 x = n ? (sum*scale*2 + n) / (2*n) : 0;  // rounded
    if (!decimals) {
        evalthousands(b, x);
        return;
    }
    print(b, x / scale);
    putbyte(b, '.');
    printpad(b, x % scale, decimals);
}

// The two-sided p value of a sign test: of the wins+losses pairs that
// differ, the chance that a split at least this uneven comes of no
// difference at all, 2 P(X <= min(wins, losses)) for X binomial with
// p 1/2, at most 1. Exact: the sum of binomial coefficients is scaled
// down as it grows, and 2^-n applied last, so that nothing overflows.
static f64 evalsign(i64 wins, i64 losses)
{
    i64 n = wins + losses;
    i64 k = wins<losses ? wins : losses;
    f64 term = 1, sum = 1;  // C(n, i) and the sum so far, over 2^shift
    i64 shift = 0;
    for (i64 i = 0; i < k; i++) {
        term = term * (f64)(n - i) / (f64)(i + 1);
        sum += term;
        if (sum > 0x1p500) {
            term  *= 0x1p-500;
            sum   *= 0x1p-500;
            shift += 500;
        }
    }
    for (i64 e = shift - n + 1; e; e += e<0 ? 1 : -1) {
        sum = e<0 ? sum/2 : sum*2;
    }
    return sum<1 ? sum : 1;
}

// A p value as the summary prints it: to three decimals, or < 0.001.
static void evalp(Buf *b, f64 p)
{
    i64 x = (i64)(p*1000 + 0.5);
    i32 d = 3;
    if (x >= 1000) {
        print(b, "p = 1");
    } else if (x < 1) {
        print(b, "p < 0.001");
    } else {
        for (; x%10 == 0; x /= 10, d--) {}
        print(b, "p = 0.");
        printpad(b, x, d);
    }
}

// What tells two formats apart, in order: the pass rate (higher), then
// failed edits, steps and tokens (fewer).
enum : i32 {
    CRIT_PASS,
    CRIT_FAILS,
    CRIT_STEPS,
    CRIT_TOKENS,
    CRIT_COUNT,
};

static Str evalcritnames[] = {"the pass rates", "failed edits", "steps", "tokens"};
static Str evaldeciders[]  = {
    "the pass rate decides", "fewer failed edits decide", "fewer steps decide",
    "fewer tokens decide",
};

// A format's runs among those that every format of its model made:
// sums over them all, and by task.
struct EvalArm {
    EvalCell *cell;
    i64       sum[CRIT_COUNT];
    i64      *bytask;  // ntasks of CRIT_COUNT sums
};

// Two formats compared task by task, each criterion by a sign test over
// the tasks whose sums differ. The first criterion whose p is below
// 0.05 decides, in favor of whichever was better on more tasks.
struct EvalVerdict {
    i32 crit;  // what decides, CRIT_COUNT if nothing does
    b32 x;     // the first format is the better
    i64 wins[CRIT_COUNT];    // tasks on which the first is better
    i64 losses[CRIT_COUNT];  // and worse
    f64 p[CRIT_COUNT];
};

static EvalVerdict evalverdict(EvalArm *x, EvalArm *y, iz ntasks)
{
    EvalVerdict r = {};
    r.crit = CRIT_COUNT;
    for (i32 c = 0; c < CRIT_COUNT; c++) {
        for (iz i = 0; i < ntasks; i++) {
            i64 d = x->bytask[i*CRIT_COUNT+c] - y->bytask[i*CRIT_COUNT+c];
            d = c==CRIT_PASS ? d : -d;
            r.wins[c]   += d > 0;
            r.losses[c] += d < 0;
        }
        r.p[c] = evalsign(r.wins[c], r.losses[c]);
        if (r.crit==CRIT_COUNT && r.p[c]<0.05) {
            r.crit = c;
            r.x    = r.wins[c] > r.losses[c];
        }
    }
    return r;
}

// Whether arm a comes before b by their sums: the order to list them,
// not a verdict.
static b32 evalarmless(EvalArm *a, EvalArm *b)
{
    for (i32 c = 0; c < CRIT_COUNT; c++) {
        if (a->sum[c] != b->sum[c]) {
            return c==CRIT_PASS ? a->sum[c]>b->sum[c] : a->sum[c]<b->sum[c];
        }
    }
    return 0;  // the sort is stable: the usual order stays
}

// Why one format is better than another, or that neither is.
static void evalwhy(Buf *b, EvalVerdict *v, b32 flip)
{
    i32 last = v->crit<CRIT_COUNT ? v->crit : CRIT_COUNT;
    if (v->crit == CRIT_COUNT) {
        print(b, "no measurable difference (");
    }
    for (i32 c = 0; c < last; c++) {
        if (c) {
            print(b, c+1<last || v->crit==CRIT_COUNT ? Str(", ") : Str(" and "));
        }
        print(b, evalcritnames[c]);
        print(b, v->crit==CRIT_COUNT ? Str(" ") : Str(" ("));
        evalp(b, v->p[c]);
        print(b, v->crit==CRIT_COUNT ? Str("") : Str(")"));
    }
    if (v->crit == CRIT_COUNT) {
        print(b, ")");
        return;
    } else if (last) {
        print(b, " cannot be told apart, and ");
    }
    i32 c = v->crit;
    print(b, evaldeciders[c]);
    print(b, " (better on ");
    print(b, flip ? v->losses[c] : v->wins[c]);
    print(b, ", worse on ");
    print(b, flip ? v->wins[c] : v->losses[c]);
    print(b, " tasks, ");
    evalp(b, v->p[c]);
    print(b, ")");
}

static b32 evalrowless(EvalCell **a, EvalCell **b)
{
    if ((*a)->order != (*b)->order) {
        return (*a)->order < (*b)->order;
    }
    return evalformatrank((*a)->format) < evalformatrank((*b)->format);
}

static void evalcellname(Buf *b, EvalCell *c, b32 endpoints)
{
    print(b, c->model);
    if (endpoints) {
        print(b, " (");
        print(b, c->endpoint);
        print(b, ")");
    }
}

// A failed run, as a line of the list of them.
static void evalfailure(Buf *b, Json *j)
{
    Str why    = jsonstr(jsonget(j, "why"));
    Str expect = jsonstr(jsonget(j, "expect"));
    print(b, "- ");
    print(b, jsonstr(jsonget(j, "model")));
    putbyte(b, ' ');
    print(b, jsonstr(jsonget(j, "format")));
    putbyte(b, ' ');
    print(b, jsonstr(jsonget(j, "task")));
    print(b, " #");
    print(b, jsonint(jsonget(j, "rep"), 0));
    print(b, ": ");
    if (why != "done") {
        print(b, "ended ");
        print(b, why);
        Str err = jsonstr(jsonget(j, "err"));
        if (err.len) {
            print(b, " (");
            print(b, utf8head(err, 120));
            print(b, ")");
        }
    } else if (expect.len) {
        print(b, expect);
    } else {
        print(b, "the check exited with status ");
        print(b, jsonint(jsonget(j, "check_exit"), 0));
    }
    putbyte(b, '\n');
}

// The format to give one model run in more than one, fs, from the runs
// that every one of them made, by the same tasks and repetitions: one
// that does measurably better than each of the others (evalverdict),
// else the model's own format, from its own runs or the configuration
// (owns), kept.
static void evalchoose(Buf *b, Arena scratch, Slice<EvalCell *> fs, Slice<EvalModel> owns,
                       b32 endpoints)
{
    EvalCell *c   = fs[0];
    Str       own = {};
    for (iz i = 0; i < owns.len; i++) {
        if (owns[i].endpoint==c->endpoint && owns[i].model==c->model) {
            own = owns[i].own;
        }
    }
    i64 total = 0, passed = 0;
    for (iz f = 0; f < fs.len; f++) {
        for (iz i = 0; i < fs[f]->recs.len; i++) {
            own = fs[f]->recs[i].own ? fs[f]->format : own;
        }
        total  += fs[f]->runs;
        passed += fs[f]->pass;
    }

    // The runs each format made, the first of a key; those of keys
    // that every format has; and their tasks
    Map<EvalRec *> **made = alloc<Map<EvalRec *> *>(&scratch, fs.len);
    for (iz f = 0; f < fs.len; f++) {
        for (iz i = 0; i < fs[f]->recs.len; i++) {
            EvalRec **r = upsert(&made[f], fs[f]->recs[i].key, &scratch);
            *r = *r ? *r : &fs[f]->recs[i];
        }
    }
    Slice<EvalRec *> shared = {};
    for (iz i = 0; i < fs[0]->recs.len; i++) {
        EvalRec *r   = &fs[0]->recs[i];
        b32      all = *upsert(&made[0], r->key, (Arena *)0) == r;
        for (iz f = 1; all && f<fs.len; f++) {
            all = upsert(&made[f], r->key, (Arena *)0) != 0;
        }
        if (all) {
            shared = push(&scratch, shared, r);
        }
    }
    Map<iz> *taskidx = 0;
    iz       ntasks  = 0;
    for (iz i = 0; i < shared.len; i++) {
        iz *k = upsert(&taskidx, shared[i]->task, &scratch);
        *k = *k ? *k : ++ntasks;  // from 1
    }
    Slice<EvalArm> arms = {};
    for (iz f = 0; f < fs.len; f++) {
        EvalArm arm = {};
        arm.cell   = fs[f];
        arm.bytask = alloc<i64>(&scratch, ntasks*CRIT_COUNT);
        for (iz i = 0; i < shared.len; i++) {
            EvalRec *r = *upsert(&made[f], shared[i]->key, (Arena *)0);
            i64     *t = arm.bytask + (*upsert(&taskidx, r->task, (Arena *)0)-1)*CRIT_COUNT;
            i64      v[CRIT_COUNT] = {r->pass, r->fails, r->steps, r->tokens};
            for (i32 k = 0; k < CRIT_COUNT; k++) {
                t[k]       += v[k];
                arm.sum[k] += v[k];
            }
        }
        arms = push(&scratch, arms, arm);
    }
    sort(arms.data, arms.len, evalarmless, scratch);

    // The winner: better than each other format
    iz win = -1;
    for (iz w = 0; shared.len && w<arms.len && win<0; w++) {
        b32 all = 1;
        for (iz o = 0; all && o<arms.len; o++) {
            EvalVerdict v = evalverdict(&arms[w], &arms[o], ntasks);
            all = o==w || (v.crit<CRIT_COUNT && v.x);
        }
        win = all ? w : -1;
    }
    if (win > 0) {
        EvalArm first = arms[win];  // listed first
        for (iz i = win; i > 0; i--) {
            arms[i] = arms[i-1];
        }
        arms[0] = first;
        win = 0;
    }

    print(b, "\n");
    evalcellname(b, c, endpoints);
    print(b, ": ");
    if (!passed) {
        print(b, "none, since no run passed\n");
    } else if (!shared.len) {
        print(b, "no task was run in every format, so nothing compares them\n");
    } else if (win == 0) {
        print(b, arms[0].cell->format);
        print(b, arms[0].cell->format==own ? Str(", its own format already\n")
                                           : Str("\n"));
    } else if (own.len) {
        print(b, "keep ");
        print(b, own);
        print(b, ", its own format: none did measurably better at this many runs\n");
    } else {
        print(b, "keep its own format: none did measurably better at this many runs\n");
    }
    if (!shared.len) {
        return;
    }
    print(b, "\n");
    for (iz i = 0; i < arms.len; i++) {
        EvalArm *r = &arms[i];
        print(b, "- ");
        print(b, r->cell->format);
        print(b, ": ");
        print(b, r->sum[CRIT_PASS]);
        putbyte(b, '/');
        print(b, (i64)shared.len);
        print(b, " passed, ");
        evalmean(b, r->sum[CRIT_FAILS], shared.len, 2);
        print(b, " failed edits, ");
        evalmean(b, r->sum[CRIT_STEPS], shared.len, 1);
        print(b, " steps and ");
        evalmean(b, r->sum[CRIT_TOKENS], shared.len, 0);
        print(b, " tokens a run\n");
    }
    if (!passed) {
        return;
    }

    print(b, "\nOn the ");
    print(b, (i64)shared.len);
    print(b, shared.len==1 ? Str(" run") : Str(" runs"));
    print(b, " of each format with the same tasks and repetitions, by the ");
    print(b, (i64)ntasks);
    print(b, ntasks==1 ? Str(" task") : Str(" tasks"));
    print(b, " (sign tests");
    i64 left = total - (i64)(shared.len*fs.len);
    if (left) {
        print(b, "; ");
        print(b, left);
        print(b, left==1 ? Str(" run") : Str(" runs"));
        print(b, " that not every format made left out");
    }
    print(b, "):\n\n");
    iz from = win==0 ? 1 : arms.len;  // the winner's comparisons, or all
    for (iz i = 0; i < from; i++) {
        for (iz k = i+1; k < arms.len; k++) {
            EvalVerdict v    = evalverdict(&arms[i], &arms[k], ntasks);
            b32         flip = v.crit<CRIT_COUNT && !v.x;
            print(b, "- ");
            print(b, arms[flip ? k : i].cell->format);
            print(b, v.crit<CRIT_COUNT ? Str(" over ") : Str(" and "));
            print(b, arms[flip ? i : k].cell->format);
            print(b, ": ");
            evalwhy(b, &v, flip);
            print(b, ".\n");
        }
    }
    if (win==0 && arms[0].cell->format!=own) {
        print(b, "\n    [model ");
        print(b, c->model);
        print(b, "]\n    edit_format = ");
        print(b, arms[0].cell->format);
        putbyte(b, '\n');
    }
}

// The summary of results lines: a table by model and format, the runs
// that failed, and the format each model should get, with the numbers
// behind it. owns has the models' own formats where known.
static Str evalsummary(Arena *a, Arena scratch, Slice<Str> lines, Slice<EvalModel> owns)
{
    EvalCell  *cells = 0;
    EvalCell **tail  = &cells;
    Buf        fails(a, 1024);
    i64        nfail = 0;
    Str        firstep = {};
    b32        endpoints = 0;
    i32        nmodels = 0;
    for (iz i = 0; i < lines.len; i++) {
        Arena t = scratch;  // nothing else goes in scratch meanwhile
        Json *j = jsonparse(&t, lines[i]).root;
        if (j->type != JSON_OBJECT) {
            continue;
        }
        Str ep     = jsonstr(jsonget(j, "endpoint"));
        Str model  = jsonstr(jsonget(j, "model"));
        Str format = jsonstr(jsonget(j, "format"));
        firstep    = firstep.data ? firstep : clone(a, ep);
        endpoints |= ep != firstep;
        EvalCell *c = cells;
        for (; c && !(c->endpoint==ep && c->model==model && c->format==format); c = c->next) {}
        if (!c) {
            c = alloc<EvalCell>(a);
            c->endpoint = clone(a, ep);
            c->model    = clone(a, model);
            c->format   = clone(a, format);
            c->order    = nmodels;
            for (EvalCell *d = cells; d; d = d->next) {
                c->order = d->endpoint==ep && d->model==model ? d->order : c->order;
            }
            nmodels += c->order == nmodels;
            *tail = c;
            tail  = &c->next;
        }
        if (jsonget(j, "error")->type == JSON_STRING) {
            c->errors++;
            continue;
        }
        Json *errs  = jsonget(j, "errors");
        Json *tok   = jsonget(j, "tokens");
        b32   pass  = jsonbool(jsonget(j, "pass"), 0);
        i64   miss  = jsonint(jsonget(errs, "miss"), 0);
        i64   efail = jsonint(jsonget(j, "edit_fails"), miss);  // older lines
        c->runs++;
        c->pass    += pass;
        c->steps   += jsonint(jsonget(j, "steps"), 0);
        c->calls   += jsonint(jsonget(j, "calls"), 0);
        c->fails   += efail;
        c->bad     += jsonint(jsonget(errs, "args"), 0) +
                      jsonint(jsonget(errs, "unknown"), 0) +
                      jsonint(jsonget(errs, "loop"), 0);
        c->denied  += jsonint(jsonget(j, "denied"), 0);
        c->repairs += jsonint(jsonget(j, "repairs"), 0);
        c->retries += jsonint(jsonget(j, "retries"), 0);
        c->in      += jsonint(jsonget(tok, "in"), 0);
        c->cached  += jsonint(jsonget(tok, "cached"), 0);
        c->out     += jsonint(jsonget(tok, "out"), 0);
        c->ms      += jsonint(jsonget(j, "wall_ms"), 0) - jsonint(jsonget(j, "queued_ms"), 0);
        for (Json *k = jsonget(j, "repaired")->child; k; k = k->next) {
            c->fixes = evalcount(a, c->fixes, k->key, jsonint(k, 0));
        }
        for (Json *k = jsonget(j, "nudged")->child; k; k = k->next) {
            c->nudged = evalcount(a, c->nudged, k->key, jsonint(k, 0));
        }
        for (Json *k = jsonget(j, "retried")->child; k; k = k->next) {
            c->retried = evalcount(a, c->retried, k->key, jsonint(k, 0));
        }
        if (!pass && nfail++<50) {
            evalfailure(&fails, j);
        }

        EvalRec r = {};
        Buf     k(a, 128);
        print(&k, jsonstr(jsonget(j, "suite")));
        putbyte(&k, '\t');
        print(&k, jsonstr(jsonget(j, "task")));
        r.task = finish(&k);
        putbyte(&k, '\t');
        print(&k, jsonint(jsonget(j, "rep"), 0));
        r.key    = finish(&k);
        r.own    = jsonbool(jsonget(j, "own"), 0);
        r.pass   = pass;
        r.fails  = efail;
        r.steps  = jsonint(jsonget(j, "steps"), 0);
        r.tokens = jsonint(jsonget(tok, "in"), 0) + jsonint(jsonget(tok, "out"), 0);
        c->recs  = push(a, c->recs, r);
    }

    // Rows by model, each model's formats in their usual order
    Slice<EvalCell *> rows = {};
    for (EvalCell *c = cells; c; c = c->next) {
        rows = push(&scratch, rows, c);
    }
    sort(rows.data, rows.len, evalrowless, scratch);

    Buf b(a, 4096);
    print(&b, "| model | format | runs | pass | steps | calls | failed edits "
              "| bad calls | denials | repairs | tokens in | cached | tokens out "
              "| retries | time s |\n");
    print(&b, "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    i64 errors = 0;
    for (iz i = 0; i < rows.len; i++) {
        EvalCell *c = rows[i];
        errors += c->errors;
        if (!c->runs) {
            continue;
        }
        print(&b, "| ");
        evalcellname(&b, c, endpoints);
        print(&b, " | ");
        print(&b, c->format);
        print(&b, " | ");
        print(&b, c->runs);
        print(&b, " | ");
        evalmean(&b, c->pass*100, c->runs, 1);
        print(&b, "% (");
        print(&b, c->pass);
        print(&b, ") | ");
        evalmean(&b, c->steps, c->runs, 1);
        print(&b, " | ");
        evalmean(&b, c->calls, c->runs, 1);
        print(&b, " | ");
        print(&b, c->fails);
        print(&b, " | ");
        print(&b, c->bad);
        print(&b, " | ");
        print(&b, c->denied);
        print(&b, " | ");
        print(&b, c->repairs);
        print(&b, " | ");
        evalmean(&b, c->in, c->runs, 0);
        print(&b, " | ");
        evalmean(&b, c->cached, c->runs, 0);
        print(&b, " | ");
        evalmean(&b, c->out, c->runs, 0);
        print(&b, " | ");
        print(&b, c->retries);
        print(&b, " | ");
        evalmean(&b, c->ms, c->runs*1000, 1);
        print(&b, " |\n");
    }
    print(&b, "\nSteps, calls, tokens and time are means a run; failed edits "
              "(edit calls that failed, whether the change did not apply or "
              "the call was invalid), bad calls (invalid arguments, unknown "
              "tools, repeated calls), denials, repairs and retries are "
              "totals. Time leaves out waits in the endpoint's queue.\n");
    if (errors) {
        print(&b, "\n");
        print(&b, errors);
        print(&b, errors==1 ? Str(" run could not be made")
                            : Str(" runs could not be made"));
        print(&b, ": the results file has why, and an eval run again makes "
                  "them again.\n");
    }
    b32 forgave = 0;
    for (iz i = 0; i < rows.len; i++) {
        EvalCell *c = rows[i];
        if (!c->fixes.len && !c->nudged.len && !c->retried.len) {
            continue;
        }
        if (!forgave) {
            print(&b, "\nWhat the harness forgave (repairs), nudged and retried, "
                      "by kind:\n\n");
            forgave = 1;
        }
        print(&b, "- ");
        evalcellname(&b, c, endpoints);
        putbyte(&b, ' ');
        print(&b, c->format);
        putbyte(&b, ':');
        Slice<EvalCount> kinds[] = {c->fixes, c->nudged, c->retried};
        Str              heads[] = {"repaired", "nudged", "retried"};
        for (i32 n = 0, any = 0; n < countof(kinds); n++) {
            for (iz k = 0; k < kinds[n].len; k++, any = 1) {
                print(&b, k ? Str(", ") : any ? Str("; ") : Str(" "));
                if (!k) {
                    print(&b, heads[n]);
                    putbyte(&b, ' ');
                }
                print(&b, kinds[n][k].n);
                print(&b, " x ");
                print(&b, kinds[n][k].what);
            }
        }
        putbyte(&b, '\n');
    }
    if (nfail) {
        print(&b, "\nRuns that failed");
        if (nfail > 50) {
            print(&b, " (the first 50 of ");
            print(&b, nfail);
            print(&b, ")");
        }
        print(&b, ":\n\n");
        print(&b, finish(&fails));
    }

    // The format for each model run in more than one
    b32 heading = 0;
    for (EvalCell *c = cells; c; c = c->next) {
        b32 first = 1;
        for (EvalCell *d = cells; d != c; d = d->next) {
            first &= !(d->endpoint==c->endpoint && d->model==c->model);
        }
        Slice<EvalCell *> fs = {};  // the model's formats, in their usual order
        for (i32 rank = 0; first && rank<=EDIT_COUNT; rank++) {
            for (EvalCell *d = cells; d; d = d->next) {
                if (d->runs && d->endpoint==c->endpoint && d->model==c->model &&
                        evalformatrank(d->format)==rank) {
                    fs = push(&scratch, fs, d);
                }
            }
        }
        if (fs.len < 2) {
            continue;
        }
        if (!heading) {
            print(&b, "\n## Edit format by model\n");
            heading = 1;
        }
        evalchoose(&b, scratch, fs, owns, endpoints);
    }
    return finish(&b);
}

// Read results files into lines, for the summary.
static Slice<Str> evallines(Os *os, Arena *a, Slice<Str> paths, b32 *ok)
{
    Slice<Str> r = {};
    *ok = 1;
    for (iz i = 0; i < paths.len; i++) {
        Read rd = os_read(os, a, paths[i], (iz)1<<30);
        if (rd.err) {
            *ok = 0;
            continue;
        }
        for (Str rest = rd.data; rest.len;) {
            Cut c = cut(rest, '\n');
            if (trim(c.head).len) {
                r = push(a, r, c.head);
            }
            rest = c.tail;
        }
    }
    return r;
}

// Each model of results lines, with its own edit format as the
// configuration has it, for the summary.
static Slice<EvalModel> evalowns(Arena *a, Arena scratch, Slice<Str> lines, Config *cfg)
{
    Slice<EvalModel> r = {};
    for (iz i = 0; i < lines.len; i++) {
        Arena t = scratch;
        Json *j = jsonparse(&t, lines[i]).root;
        Str   e = jsonstr(jsonget(j, "endpoint"));
        Str   m = jsonstr(jsonget(j, "model"));
        b32   seen = 0;
        for (iz k = 0; k < r.len; k++) {
            seen |= r[k].endpoint==e && r[k].model==m;
        }
        if (!seen && m.len) {
            EvalModel x = {};
            x.endpoint = clone(a, e);
            x.model    = clone(a, m);
            Str q      = concat(&t, concat(&t, e, "/"), m);
            Str own    = cfgsplitmodel(cfg->ini, q).ok ? q : m;  // as on endpoint e
            x.own      = editnames[editformat(cfgget(cfg, "model", own, "edit_format", 0))];
            r = push(a, r, x);
        }
    }
    return r;
}

// A run's key in a results file: suite, task, endpoint, model, format
// ("" for the model's own) and repetition.
static Str evalkey(Arena *a, Str suite, Str task, Str ep, Str model, Str format, i64 rep)
{
    Buf b(a, 256);
    Str parts[] = {suite, task, ep, model, format};
    for (iz i = 0; i < countof(parts); i++) {
        print(&b, parts[i]);
        putbyte(&b, '\t');
    }
    print(&b, rep);
    return finish(&b);
}


// The command

// Comma-separated words.
static Slice<Str> evalwords(Arena *a, Slice<Str> list, Str s)
{
    for (Str rest = s; rest.len;) {
        Str w = cfgnextword(&rest);
        if (w.len) {
            list = push(a, list, w);
        }
    }
    return list;
}

static i32 evalmain(Os *os, Arena perm, Arena scratch, Str *args, i32 nargs)
{
    Str        config = {}, state = {}, endpoint = {}, outpath = "eval.jsonl";
    Slice<Str> models = {}, formats = {}, only = {}, suites = {};
    i64        reps   = 1, jobs = 4;
    b32        keep   = 0, report = 0;
    for (i32 i = 2; i < nargs; i++) {
        Str a = args[i];
        b32 more = i+1 < nargs;
        if (a=="-m" && more) {
            models = evalwords(&perm, models, args[++i]);
        } else if (a=="-e" && more) {
            endpoint = args[++i];
        } else if (a=="-f" && more) {
            formats = evalwords(&perm, formats, args[++i]);
        } else if (a=="-t" && more) {
            only = evalwords(&perm, only, args[++i]);
        } else if ((a=="-n" || a=="-j") && more) {
            Int n = parseint(args[++i]);
            if (!n.ok || n.val<1 || n.val>1000) {
                os_print(os, 2, "prospero: -n and -j take a number from 1 to 1000\n");
                return 2;
            } else if (a == "-n") {
                reps = n.val;
            } else {
                jobs = n.val;
            }
        } else if (a=="-o" && more) {
            outpath = args[++i];
        } else if (a=="-c" && more) {
            config = args[++i];
        } else if (a=="--state" && more) {
            state = args[++i];
        } else if (a == "--keep") {
            keep = 1;
        } else if (a == "--report") {
            report = 1;
        } else if (a.len && a[0]!='-') {
            suites = push(&perm, suites, a);
        } else {
            os_print(os, 2, evalusage);
            return 2;
        }
    }
    if (!suites.len) {
        os_print(os, 2, evalusage);
        return 2;
    }
    if (report) {
        b32        ok    = 0;
        Slice<Str> lines = evallines(os, &perm, suites, &ok);
        if (!ok) {
            os_print(os, 2, "prospero: cannot read a results file\n");
            return 2;
        }
        // The configuration, for the models' own formats, fetches nothing
        Config *cfg = cliconfig(os, &perm, scratch, config, state, CFG_NETNONE);
        if (!cfg) {
            return 2;
        }
        Slice<EvalModel> owns = evalowns(&perm, scratch, lines, cfg);
        os_print(os, 1, evalsummary(&perm, scratch, lines, owns));
        return 0;
    }

    // Formats: -1 for each model's own
    Slice<i32> fmts = {};
    for (iz i = 0; i < formats.len; i++) {
        if (formats[i] == "all") {
            for (i32 f = 0; f < EDIT_COUNT; f++) fmts = push(&perm, fmts, f);
        } else if (equalfold(editnames[editformat(formats[i])], formats[i])) {
            fmts = push(&perm, fmts, editformat(formats[i]));
        } else {
            Str msg = concat(&scratch, concat(&scratch, "prospero: no such edit format: ",
                             formats[i]), "\n");
            os_print(os, 2, msg);
            return 2;
        }
    }
    if (!fmts.len) {
        fmts = push(&perm, fmts, -1);
    }

    Slice<EvalTask> tasks = {};
    for (iz i = 0; i < suites.len; i++) {
        if (!evalsuite(os, &perm, scratch, suites[i], only, &tasks)) {
            return 2;
        }
    }
    if (!tasks.len) {
        os_print(os, 2, "prospero: no tasks to run\n");
        return 2;
    }

    // Before the engine owns the configuration: the models and endpoints
    Eval ev = {};
    ev.os     = os;
    ev.keep   = keep;
    ev.jobs   = (i32)jobs;
    ev.silent = ev.jobs>EVAL_SILENT ? ev.jobs : (i32)EVAL_SILENT;
    Str root = tempdir(os, &perm);
    Buf name(&perm, 32);
    print(&name, "prospero-eval-");
    printhex(&name, os_random(os), 8);
    ev.root     = join(&perm, root, finish(&name));
    ev.statedir = state.len ? state : join(&perm, ev.root, "state");
    Config *cfg = cliconfig(os, &perm, scratch, config, ev.statedir, CFG_NETWAIT);
    if (!cfg) {
        return 2;
    }
    CfgDefaults dflt  = cfgdefaults(cfg);
    Str         defep = endpoint.len ? endpoint : dflt.endpoint;
    if (!models.len) {
        Str m = dflt.model;
        if (!m.len) {
            os_print(os, 2, "prospero: no model: give one with -m, or set model "
                            "in the configuration\n");
            return 2;
        }
        models = push(&perm, models, m);
    }
    Slice<EvalModel> ms = {};
    for (iz i = 0; i < models.len; i++) {
        EvalModel m = {};
        Cut       c = cut(models[i], '/');
        m.endpoint = defep;
        m.model    = models[i];
        if (c.ok && c.tail.len && trim(cfgget(cfg, "endpoint", c.head, "url", 0)).len) {
            m.endpoint = c.head;
            m.model    = c.tail;
        }
        if (!parseurl(m.endpoint).ok && !trim(cfgget(cfg, "endpoint", m.endpoint, "url", 0)).len) {
            Str msg = concat(&scratch, concat(&scratch, "prospero: no usable endpoint for ",
                             models[i]), ": give a URL or a configured name with -e\n");
            os_print(os, 2, msg);
            return 2;
        }
        ms = push(&perm, ms, m);
    }

    // Runs already in the results file are not made again
    Map<b32> *done  = 0;
    i64       dones = 0;
    if (os_stat(os, scratch, outpath).type == STAT_FILE) {
        b32        ok    = 0;
        Slice<Str> paths = {};
        paths = push(&scratch, paths, outpath);
        Arena      t     = scratch;
        Slice<Str> lines = evallines(os, &t, paths, &ok);
        for (iz i = 0; i < lines.len; i++) {
            Arena u = t;
            Json *j = jsonparse(&u, lines[i]).root;
            if (j->type!=JSON_OBJECT || jsonget(j, "error")->type==JSON_STRING) {
                continue;
            }
            b32 own = jsonbool(jsonget(j, "own"), 0);
            Str key = evalkey(&perm, jsonstr(jsonget(j, "suite")), jsonstr(jsonget(j, "task")),
                              jsonstr(jsonget(j, "endpoint")), jsonstr(jsonget(j, "model")),
                              own ? Str{} : jsonstr(jsonget(j, "format")),
                              jsonint(jsonget(j, "rep"), 0));
            *upsert(&done, key, &perm) = 1;
        }
    }
    for (iz m = 0; m < ms.len; m++) {
        for (i64 r = 1; r <= reps; r++) {
            for (iz t = 0; t < tasks.len; t++) {
                for (iz f = 0; f < fmts.len; f++) {
                    Arena u   = scratch;
                    Str   fmt = fmts[f]<0 ? Str{} : editnames[fmts[f]];
                    Str   key = evalkey(&u, tasks[t].suite, tasks[t].name,
                                        ms[m].endpoint, ms[m].model, fmt, r);
                    if (upsert(&done, key, (Arena *)0)) {
                        dones++;
                        continue;
                    }
                    EvalRun *run = alloc<EvalRun>(&perm);
                    run->task   = &tasks[t];
                    run->model  = &ms[m];
                    run->format = fmts[f];
                    run->rep    = (i32)r;
                    ev.runs = push(&perm, ev.runs, run);
                }
            }
        }
    }

    ev.out = os_logopen(os, scratch, outpath);
    if (ev.out < 0) {
        os_print(os, 2, "prospero: cannot open the results file\n");
        return 2;
    }
    {
        Buf b(&scratch, 256);
        print(&b, "eval: ");
        print(&b, (i64)ev.runs.len);
        print(&b, ev.runs.len==1 ? Str(" run to make") : Str(" runs to make"));
        if (dones) {
            print(&b, ", ");
            print(&b, dones);
            print(&b, " made before");
        }
        print(&b, ", results in ");
        print(&b, outpath);
        print(&b, ", runs in ");
        print(&b, ev.root);
        putbyte(&b, '\n');
        os_print(os, 2, finish(&b));
    }

    if (!ev.runs.len) {
        os_logclose(os, ev.out);
        b32        ok    = 0;
        Slice<Str> paths = {};
        paths = push(&perm, paths, outpath);
        Slice<Str>       lines = evallines(os, &perm, paths, &ok);
        Slice<EvalModel> owns  = evalowns(&perm, scratch, lines, cfg);
        os_print(os, 1, evalsummary(&perm, scratch, lines, owns));
        return 0;
    }
    Str sessions = join(&perm, ev.statedir, "sessions");
    Str runs     = join(&perm, ev.root, "runs");
    if (!os_mkdirs(os, scratch, sessions) || !os_mkdirs(os, scratch, runs)) {
        os_print(os, 2, "prospero: cannot create the eval's directories\n");
        return 2;
    }
    b32 big   = sizeof(void *) == 8;
    ev.spare  = os_reserve(os, (iz)1<<(big ? 32 : 26));
    ev.mbox   = ev.spare ? newmailbox(&perm, big ? 16<<20 : 2<<20, 0, 0) : 0;
    ev.e      = ev.mbox ? newengine(os, &perm, cfg, ev.statedir, ev.mbox) : 0;
    ev.live   = alloc<EvalRun *>(&perm, ev.jobs);
    if (!ev.e) {
        os_print(os, 2, "prospero: cannot start the engine\n");
        return 2;
    }
    ev.te      = ev.e->te;
    ev.env     = envsetpath(&perm, ev.e->env, enginepath(ev.e, &perm, HOST_WINDOWS),
                            HOST_WINDOWS);  // as configured when it began
    ev.secrets = cfgsecrets(cfg, &perm);  // before the engine owns cfg
    if (!enginestart(ev.e)) {
        os_print(os, 2, "prospero: cannot start the engine\n");
        return 2;
    }

    for (;;) {
        for (; ev.next<ev.runs.len && ev.nlive<ev.jobs; ev.next++) {
            EvalRun *run = ev.runs[ev.next];
            if (run->model->dropped) {
                ev.skipped++;
                continue;
            }
            evalstart(&ev, scratch, &perm, run);
        }
        if (!ev.nlive && ev.next==ev.runs.len) {
            break;
        }
        i64 at = evaltick(&ev, scratch, &perm);
        if (!ev.nlive) {
            continue;  // the tick finished the last ones
        }
        i64 now = os_ticks(os);
        i32 ms  = -1;
        if (at) {
            i64 wait = (at - now + MS - 1) / MS;
            ms = wait<0 ? 0 : wait>3600000 ? 3600000 : (i32)wait;
        }
        mailwait(ev.mbox, ms);
        Mail m = mailtake(ev.mbox);
        for (Record r; (r = mailnext(&m)).ok;) {
            switch (r.type) {
            case MAIL_OUT:
                evalrecord(&ev, scratch, &perm, r.data);
                break;
            case PROC_EXIT:
            case PROC_EOF:
                evalproc(&ev, scratch, &perm, r);
                break;
            }
        }
        if (m.overflow) {
            evalresync(&ev, scratch, &perm);
        }
    }

    // Stop the engine, then sum up all the results file holds
    enginecmd(ev.e, "{\"cmd\":\"quit\",\"id\":0}");
    for (b32 quit = 0; !quit;) {
        mailwait(ev.mbox, -1);
        Mail m = mailtake(ev.mbox);
        for (Record r; (r = mailnext(&m)).ok;) {
            quit |= r.type == MAIL_QUIT;
        }
    }
    os_logclose(os, ev.out);
    if (!keep) {
        removeall(os, scratch, ev.root);
        if (os_stat(os, scratch, ev.root).type != STAT_MISSING) {
            os_print(os, 2, concat(&scratch, concat(&scratch, "prospero: could not "
                     "remove all of ", ev.root), "\n"));
        }
    }

    b32        ok    = 0;
    Slice<Str> paths = {};
    paths = push(&perm, paths, outpath);
    Slice<Str>       lines = evallines(os, &perm, paths, &ok);
    Slice<EvalModel> owns  = evalowns(&perm, scratch, lines, cfg);  // the engine is done
    os_print(os, 1, evalsummary(&perm, scratch, lines, owns));
    if (ev.skipped) {
        Buf b(&scratch, 256);
        print(&b, "eval: ");
        print(&b, ev.skipped);
        print(&b, ev.skipped==1 ? Str(" run was") : Str(" runs were"));
        print(&b, " left out, their models never answering; an eval run "
                  "again makes them\n");
        os_print(os, 2, finish(&b));
        return 1;
    }
    return 0;
}
