// Containers made for sessions: Docker's or Podman's
//
// A container session runs in a container Prospero makes for it, from
// an image, reached as a remote by exec (deploy.cpp, Reach): the window
// makes the container, and connects to it as to [remote] exec = RUNTIME
// exec -i prospero-NAME, with deploy = auto, so that connecting puts an
// engine there over cat as for any exec remote, and its host's state
// in /prospero/state. Nothing is configured: the runtime is found on
// the PATH (ctrfind, [container] runtime choosing), and asked whether
// its daemon answers (ctrprobe). The container is made as
//
//   RUNTIME create --name prospero-NAME --label com.nullprogram.prospero=1
//       --label com.nullprogram.prospero.owner=OWNER --init
//       --network none [--cpus N] [--memory M] [ARGS...]
//       -v STATE:/prospero/state --tmpfs /prospero/state/run
//       [-v FOLDER:/work] -w /work --entrypoint sleep IMAGE 2147483647
//
// where sleep keeps it running (under --init, which reaps the shells'
// orphans and passes stop's SIGTERM on, so it stops at once), STATE is
// a directory here, so that the host's sessions and logs outlive the
// container, and the tmpfs holds its socket (os_endpoint), as a socket
// on a shared folder may not work. OWNER is a hash of this state
// directory, so a test's state never sees the user's containers. A
// folder is mounted at /work, or copied into it (cp FOLDER/. NAME:/work)
// before it starts, or /work starts empty. The image is pulled first
// when it is not here.
//
// The window keeps each in <state>/containers/NAME/: container.ini, as
// below, beside the state directory. Moving its last session to the
// Trash stops it, restoring one starts it again, and deleting its last
// session, emptying the Trash, or the Trash's purge removes it (rm -f)
// and its directory; the engine inside never knows. The sessions it
// held are kept in container.ini, so that the Trash shows them while
// it is stopped.
//
// This is free and unencumbered software released into the public domain.

enum : i64 {
    CTR_PROBETIME = 8*SEC,       // for version: a stopped daemon may hang
    CTR_STEPTIME  = 2*60*SEC,    // create, start, stop, rm
    CTR_PULLTIME  = 30*60*SEC,   // a large image, slowly
    CTR_COPYTIME  = 30*60*SEC,   // a large folder
};

enum : i32 {
    CTR_EMPTY,  // /work starts empty
    CTR_COPY,   // a folder copied into /work
    CTR_MOUNT,  // a folder mounted at /work
};

static Str ctrworkspaces[] = {"empty", "copy", "mount"};

static Str ctrlabel = "com.nullprogram.prospero";

// A container runtime, as found.
struct CtrRuntime {
    Str name;     // docker or podman
    Str path;     // the program, if found
    Str version;  // its daemon's, if it answers
    Str why;      // why it does not, else null
    b32 up;
};

// A session a container held, as last known.
struct CtrSess {
    Str sid;
    Str title;
};

// A container made for sessions, as container.ini keeps it.
struct Ctr {
    Str name;       // p-XXXXXXXX: its directory's, and prospero-NAME its own
    Str runtime;    // docker or podman
    Str program;    // that runtime's program
    Str image;
    i32 workspace;  // CTR_*
    Str folder;     // copied or mounted
    b32 network;
    i64 created;    // Unix epoch ms
    i64 trashed;    // Unix epoch ms it was stopped for the Trash, else 0
    Slice<CtrSess> sessions;
};

// The program of a runtime: docker or podman by name, auto (or empty)
// for whichever is found first, docker first, or a path. path is the
// PATH to search; macOS also gets the places its installers use, which
// a window's PATH may lack.
static CtrRuntime ctrfind(Os *os, Arena *perm, Arena scratch, Str setting, Str path)
{
    CtrRuntime r = {};
    setting = trim(setting);
    if (setting.len && setting!="auto" && setting!="docker" && setting!="podman") {
        r.name = setting=="podman" || endswith(setting, "podman") ? Str("podman")
                                                                    : Str("docker");
        r.path = os_stat(os, scratch, setting).type==STAT_FILE ? clone(perm, setting)
                                                                : Str{};
        return r;
    }
    if (!HOST_WINDOWS) {
        path = concat(&scratch, path, ":/usr/local/bin:/opt/homebrew/bin:"
                      "/Applications/Docker.app/Contents/Resources/bin:"
                      "/opt/podman/bin");
    }
    Str names[] = {"docker", "podman"};
    for (iz i = 0; i < countof(names); i++) {
        if (setting.len && setting!="auto" && setting!=names[i]) {
            continue;
        }
        Str exe = HOST_WINDOWS ? concat(&scratch, names[i], ".exe") : names[i];
        for (Str rest = path; rest.len;) {
            Cut c = cut(rest, HOST_WINDOWS ? ';' : ':');
            rest = c.tail;
            if (!c.head.len) {
                continue;
            }
            Arena a = scratch;
            Str   p = join(&a, c.head, exe);
            if (os_stat(os, a, p).type == STAT_FILE) {
                r.name = names[i];
                r.path = clone(perm, p);
                return r;
            }
        }
    }
    r.name = setting=="podman" ? Str("podman") : Str("docker");
    return r;
}

// The runtime's name for people: Docker or Podman.
static Str ctrtitle(Str runtime)
{
    return runtime=="podman" ? Str("Podman") : Str("Docker");
}

// Running the runtime's commands, one at a time, each with a deadline,
// through deploy's runner.
struct CtrRun {
    DeployConf conf;
    Deploy     d;
};

static CtrRun *newctrrun(Os *os, Arena *perm, Str localstate, Str envpath, i32 *cancel)
{
    CtrRun *r = alloc<CtrRun>(perm);
    r->conf.localstate = localstate;
    r->conf.cancel     = cancel;
    r->d.os   = os;
    r->d.conf = &r->conf;
    r->d.mb   = newmailbox(perm, 1<<16, 0, 0);
    r->d.env  = os_environ(os, perm);
    if (envpath.len) {
        r->d.env = envsetpath(perm, r->d.env, envpath, HOST_WINDOWS);
    }
    Str dir = join(perm, localstate, "containers");
    r->d.errpath = join(perm, dir, "run.err");
    return r->d.mb && os_mkdirs(os, *perm, dir) ? r : 0;
}

static DeployRun ctrrun(CtrRun *r, Arena *a, Slice<Str> argv, i64 timeout, Str input = {})
{
    return deployrun(&r->d, a, argv, timeout, input);
}

// Whether the runtime's daemon answers, and its version: Docker
// Desktop or a Podman machine may be installed but stopped.
static void ctrprobe(CtrRun *run, Arena *perm, Arena scratch, CtrRuntime *rt)
{
    rt->up      = 0;
    rt->version = {};
    rt->why     = {};
    if (!rt->path.len) {
        rt->why = concat(perm, rt->name, " is not installed");
        return;
    }
    Slice<Str> argv = {};
    argv = push(&scratch, argv, rt->path);
    argv = push(&scratch, argv, Str("version"));
    argv = push(&scratch, argv, Str("--format"));
    argv = push(&scratch, argv, Str("{{.Server.Version}}"));
    DeployRun r = ctrrun(run, &scratch, argv, CTR_PROBETIME);
    Str v = trim(r.out);
    if (r.code==0 && v.len && v.len<64) {
        rt->up      = 1;
        rt->version = clone(perm, v);
        return;
    }
    rt->why = concat(perm, ctrtitle(rt->name), " isn't running");
}

// prospero-NAME, the container's own name.
static Str ctrcname(Arena *a, Str name)
{
    return concat(a, "prospero-", name);
}

static Str ctrdir(Arena *a, Str localstate, Str name)
{
    return join(a, join(a, localstate, "containers"), name);
}

// This state directory's mark on its containers.
static Str ctrowner(Arena *a, Str localstate)
{
    Buf b(a, 16);
    printhex(&b, hash(localstate), 16);
    return finish(&b);
}

// A new container's name: p- and 8 hex digits.
static Str ctrnewname(Os *os, Arena *a)
{
    Buf b(a, 10);
    print(&b, "p-");
    printhex(&b, os_random(os), 8);
    return finish(&b);
}

// The exec command that reaches it, for a Reach.
static Str ctrexec(Arena *a, Ctr *c)
{
    Buf b(a, 64 + c->program.len + c->name.len);
    putbyte(&b, '"');
    print(&b, c->program);
    print(&b, "\" exec -i ");
    print(&b, ctrcname(a, c->name));
    return finish(&b);
}

// Where its host keeps its state, there.
static Str ctrstate = "/prospero/state";

// The command that makes it, as described at the top. cpus, memory and
// args are [container]'s.
static Slice<Str> ctrcreateargv(Arena *a, Ctr *c, Str owner, Str state, Str cpus,
                                Str memory, Slice<Str> args)
{
    Slice<Str> r = {};
    r = push(a, r, c->program);
    r = push(a, r, Str("create"));
    r = push(a, r, Str("--name"));
    r = push(a, r, ctrcname(a, c->name));
    r = push(a, r, Str("--label"));
    r = push(a, r, concat(a, ctrlabel, "=1"));
    r = push(a, r, Str("--label"));
    r = push(a, r, concat(a, concat(a, ctrlabel, ".owner="), owner));
    r = push(a, r, Str("--init"));
    if (!c->network) {
        r = push(a, r, Str("--network"));
        r = push(a, r, Str("none"));
    }
    if (trim(cpus).len) {
        r = push(a, r, Str("--cpus"));
        r = push(a, r, trim(cpus));
    }
    if (trim(memory).len) {
        r = push(a, r, Str("--memory"));
        r = push(a, r, trim(memory));
    }
    for (iz i = 0; i < args.len; i++) {
        r = push(a, r, args[i]);
    }
    r = push(a, r, Str("-v"));
    r = push(a, r, concat(a, state, concat(a, ":", ctrstate)));
    r = push(a, r, Str("--tmpfs"));
    r = push(a, r, concat(a, ctrstate, "/run"));
    if (c->workspace == CTR_MOUNT) {
        r = push(a, r, Str("-v"));
        r = push(a, r, concat(a, c->folder, ":/work"));
    }
    r = push(a, r, Str("-w"));
    r = push(a, r, Str("/work"));
    r = push(a, r, Str("--entrypoint"));
    r = push(a, r, Str("sleep"));
    r = push(a, r, c->image);
    r = push(a, r, Str("2147483647"));
    return r;
}

// container.ini's text.
static Str ctrini(Arena *a, Ctr *c)
{
    Buf b(a, 512);
    print(&b, "runtime = ");   print(&b, c->runtime);  putbyte(&b, '\n');
    print(&b, "program = ");   print(&b, c->program);  putbyte(&b, '\n');
    print(&b, "image = ");     print(&b, c->image);    putbyte(&b, '\n');
    print(&b, "workspace = "); print(&b, ctrworkspaces[c->workspace]); putbyte(&b, '\n');
    if (c->folder.len) {
        print(&b, "folder = ");  print(&b, c->folder);   putbyte(&b, '\n');
    }
    print(&b, "network = ");   print(&b, c->network ? Str("yes") : Str("no"));
    putbyte(&b, '\n');
    print(&b, "created = ");   print(&b, c->created);  putbyte(&b, '\n');
    print(&b, "trashed = ");   print(&b, c->trashed);  putbyte(&b, '\n');
    for (iz i = 0; i < c->sessions.len; i++) {
        print(&b, "session = ");
        print(&b, c->sessions[i].sid);
        putbyte(&b, ' ');
        for (iz k = 0; k < c->sessions[i].title.len; k++) {  // one line
            u8 ch = c->sessions[i].title[k];
            putbyte(&b, ch<0x20 ? (u8)' ' : ch);
        }
        putbyte(&b, '\n');
    }
    return finish(&b);
}

// Parse container.ini's text, for the container of a name; ok in name
// unless it names no image or program.
static Ctr ctrparse(Arena *a, Str name, Str text)
{
    Ctr c = {};
    for (Str rest = text; rest.len;) {
        Cut line = cut(rest, '\n');
        Cut kv   = cut(line.head, '=');
        rest = line.tail;
        Str k = trim(kv.head);
        Str v = trim(kv.tail);
        if (k == "runtime") {
            c.runtime = clone(a, v);
        } else if (k == "program") {
            c.program = clone(a, v);
        } else if (k == "image") {
            c.image = clone(a, v);
        } else if (k == "workspace") {
            c.workspace = v=="copy" ? CTR_COPY : v=="mount" ? CTR_MOUNT : CTR_EMPTY;
        } else if (k == "folder") {
            c.folder = clone(a, v);
        } else if (k == "network") {
            c.network = v == "yes";
        } else if (k == "created") {
            c.created = parseint(v).val;
        } else if (k == "trashed") {
            c.trashed = parseint(v).val;
        } else if (k == "session") {
            Cut     st = cut(v, ' ');
            CtrSess s  = {clone(a, trim(st.head)), clone(a, trim(st.tail))};
            if (s.sid.len) {
                c.sessions = push(a, c.sessions, s);
            }
        }
    }
    if (c.image.len && c.program.len) {
        c.name = clone(a, name);
    }
    return c;
}

static b32 ctrsave(Os *os, Arena scratch, Str localstate, Ctr *c)
{
    Str dir = ctrdir(&scratch, localstate, c->name);
    if (!os_mkdirs(os, scratch, dir)) {
        return 0;
    }
    Str path = join(&scratch, dir, "container.ini");
    Str part = concat(&scratch, path, ".part");
    Str text = ctrini(&scratch, c);  // before scratch goes by value
    return os_write(os, scratch, part, text) &&
           os_rename(os, scratch, part, path);
}

// Every container this state directory keeps, in no order.
static Slice<Ctr> ctrall(Os *os, Arena *perm, Arena scratch, Str localstate)
{
    Slice<Ctr> r   = {};
    Str        dir = join(&scratch, localstate, "containers");
    for (Dirent *d = os_list(os, &scratch, dir); d; d = d->next) {
        if (!d->isdir || !startswith(d->name, "p-")) {
            continue;
        }
        Arena tmp  = scratch;
        Str   path = join(&tmp, join(&tmp, dir, d->name), "container.ini");
        Read  rd   = os_read(os, &tmp, path, 1<<16);
        Ctr   c    = rd.err==READ_OK ? ctrparse(perm, d->name, rd.data) : Ctr{};
        if (c.name.len) {
            r = push(perm, r, c);
        }
    }
    return r;
}

// The last line a runtime said, for a message, else what it did.
static Str ctrsaid(Arena *a, DeployRun *r, Str doing)
{
    Str said = deploysaid(r);
    if (said.len) {
        return said;
    } else if (r->timeout) {
        return concat(a, doing, " took too long");
    } else if (r->err.len || r->code<0) {
        return r->err.len ? lastsaid(r->err) : concat(a, doing, " failed");
    }
    Buf b(a, 64 + doing.len);
    print(&b, doing);
    print(&b, " failed, exit status ");
    print(&b, (i64)r->code);
    return finish(&b);
}

// One of the runtime's commands on the container: start, stop, or rm
// -f, as a list of words after the program. Returns why it failed,
// else null.
static Str ctrdo(CtrRun *run, Arena *perm, Arena scratch, Ctr *c, Str cmd)
{
    Slice<Str> argv = {};
    argv = push(&scratch, argv, c->program);
    for (Str rest = cmd; rest.len;) {
        Cut w = cut(rest, ' ');
        rest = w.tail;
        argv = push(&scratch, argv, w.head);
    }
    argv = push(&scratch, argv, ctrcname(&scratch, c->name));
    DeployRun r = ctrrun(run, &scratch, argv, CTR_STEPTIME);
    return r.code==0 ? Str{} : clone(perm, ctrsaid(&scratch, &r, cmd));
}

// Remove the container, best effort, and its directory and its deploy
// record here. Returns why the container was not removed, else null;
// the directory goes either way (a file the container's root made in
// it, under a rootful Linux daemon, may stay).
static Str ctrremove(CtrRun *run, Arena *perm, Arena scratch, Ctr *c)
{
    Str why = ctrdo(run, perm, scratch, c, "rm -f");
    if (why.len && (find(why, "No such container")>=0 ||
                    find(why, "no such container")>=0)) {
        why = {};  // gone already
    }
    Str   localstate = run->conf.localstate;
    Reach l          = {};
    l.exec = ctrexec(&scratch, c);
    Str dest = linkdest(&scratch, l);
    static Str const exts[] = {".ini", ".err", ".lock"};
    for (i32 i = 0; i < countof(exts); i++) {
        Str path = deployfile(&scratch, localstate, dest, exts[i]);
        os_remove(run->d.os, scratch, path);  // path first: scratch goes by value
    }
    Str dir = ctrdir(&scratch, localstate, c->name);
    os_removetree(run->d.os, scratch, dir);
    return why;
}

// Progress of making one, for the frontend, as text.
typedef void (*CtrNote)(void *ctx, Str msg);

// Make the container and start it, as at the top: the image pulled if
// it is not here. On failure, whatever was made is removed again.
// Returns why it failed, else null; c is saved by then.
static Str ctrcreate(CtrRun *run, Arena *perm, Arena scratch, Ctr *c, Str cpus,
                     Str memory, Slice<Str> args, CtrNote note, void *ctx)
{
    Os *os = run->d.os;
    Str localstate = run->conf.localstate;
    Buf b(&scratch, 256);

    // The image, pulled if missing
    Slice<Str> argv = {};
    argv = push(&scratch, argv, c->program);
    argv = push(&scratch, argv, Str("image"));
    argv = push(&scratch, argv, Str("inspect"));
    argv = push(&scratch, argv, Str("--format"));
    argv = push(&scratch, argv, Str("{{.Os}}"));
    argv = push(&scratch, argv, c->image);
    DeployRun r = ctrrun(run, &scratch, argv, CTR_STEPTIME);
    if (r.code != 0) {
        b = Buf(&scratch, 256);
        print(&b, "pulling ");
        print(&b, c->image);
        note(ctx, finish(&b));
        Slice<Str> pull = {};
        pull = push(&scratch, pull, c->program);
        pull = push(&scratch, pull, Str("pull"));
        pull = push(&scratch, pull, c->image);
        DeployRun p = ctrrun(run, &scratch, pull, CTR_PULLTIME);
        if (p.code != 0) {
            Str said = ctrsaid(&scratch, &p, Str("pull"));
            return concat(perm, "cannot pull the image: ", said);
        }
        r = ctrrun(run, &scratch, argv, CTR_STEPTIME);
    }
    Str ios = trim(r.out);
    if (r.code==0 && ios.len && ios!="linux") {
        b = Buf(perm, 64 + ios.len);
        print(&b, "the image is for ");
        print(&b, ios);
        print(&b, ", and only Linux images can run an engine");
        return finish(&b);
    }

    // The container
    note(ctx, Str("creating the container"));
    Str dir   = ctrdir(&scratch, localstate, c->name);
    Str state = join(&scratch, dir, "state");
    if (!os_mkdirs(os, scratch, state) || !ctrsave(os, scratch, localstate, c)) {
        Str err = os_lasterr(os, &scratch);
        os_removetree(os, scratch, dir);
        return concat(perm, "cannot create its directory here: ", err);
    }
    Str real = os_realpath(os, &scratch, state);
    Str own  = ctrowner(&scratch, localstate);
    argv = ctrcreateargv(&scratch, c, own, real.len ? real : state, cpus, memory, args);
    r    = ctrrun(run, &scratch, argv, CTR_STEPTIME);
    if (r.code != 0) {
        Str why = ctrsaid(&scratch, &r, Str("create"));
        os_removetree(os, scratch, dir);
        return concat(perm, "cannot create the container: ", why);
    }

    // The folder into it, then started
    Str why = {};
    if (c->workspace == CTR_COPY) {
        b = Buf(&scratch, 256);
        print(&b, "copying ");
        print(&b, c->folder);
        print(&b, " to /work");
        note(ctx, finish(&b));
        Slice<Str> cp = {};
        cp = push(&scratch, cp, c->program);
        cp = push(&scratch, cp, Str("cp"));
        cp = push(&scratch, cp, join(&scratch, c->folder, "."));
        cp = push(&scratch, cp, concat(&scratch, ctrcname(&scratch, c->name), ":/work"));
        DeployRun p = ctrrun(run, &scratch, cp, CTR_COPYTIME);
        why = p.code==0 ? Str{} : concat(&scratch, "cannot copy the folder: ",
                                         ctrsaid(&scratch, &p, Str("cp")));
    }
    if (!why.len) {
        note(ctx, Str("starting it"));
        Str err = ctrdo(run, &scratch, carve(&scratch, 1<<20), c, "start");
        why = err.len ? concat(&scratch, "cannot start the container: ", err) : why;
    }
    if (why.len) {
        ctrremove(run, &scratch, carve(&scratch, 1<<20), c);
        return clone(perm, why);
    }
    return {};
}

// The images here, by REPOSITORY:TAG, but those with none, newest first
// as the runtime lists them.
static Slice<Str> ctrimages(CtrRun *run, Arena *perm, Arena scratch, Str program)
{
    Slice<Str> argv = {};
    argv = push(&scratch, argv, program);
    argv = push(&scratch, argv, Str("images"));
    argv = push(&scratch, argv, Str("--format"));
    argv = push(&scratch, argv, Str("{{.Repository}}:{{.Tag}}"));
    DeployRun  r   = ctrrun(run, &scratch, argv, CTR_PROBETIME*2);
    Slice<Str> out = {};
    for (Str rest = r.code==0 ? r.out : Str{}; rest.len;) {
        Cut l = cut(rest, '\n');
        rest = l.tail;
        Str s = trim(l.head);
        b32 seen = !s.len || find(s, "<none>")>=0;
        for (iz i = 0; !seen && i < out.len; i++) {
            seen = out[i] == s;
        }
        if (!seen) {
            out = push(perm, out, clone(perm, s));
        }
    }
    return out;
}

// The state of each of this state directory's containers that exists:
// NAME (without prospero-) and running, exited, created..., a line each.
static Str ctrstates(CtrRun *run, Arena *perm, Arena scratch, Str program, Str owner)
{
    Slice<Str> argv = {};
    argv = push(&scratch, argv, program);
    argv = push(&scratch, argv, Str("ps"));
    argv = push(&scratch, argv, Str("-a"));
    argv = push(&scratch, argv, Str("--filter"));
    argv = push(&scratch, argv, concat(&scratch, concat(&scratch, "label=", ctrlabel),
                                       concat(&scratch, ".owner=", owner)));
    argv = push(&scratch, argv, Str("--format"));
    argv = push(&scratch, argv, Str("{{.Names}} {{.State}}"));
    DeployRun r = ctrrun(run, &scratch, argv, CTR_PROBETIME*2);
    if (r.code != 0) {
        return {};
    }
    Buf b(perm, r.out.len + 1);
    for (Str rest = r.out; rest.len;) {
        Cut l = cut(rest, '\n');
        rest = l.tail;
        Str s = trim(l.head);
        if (startswith(s, "prospero-")) {
            print(&b, cuthead(s, 9));
            putbyte(&b, '\n');
        }
    }
    return finish(&b);
}

// A container's state in ctrstates' text, or null if it does not exist.
static Str ctrstateof(Str states, Str name)
{
    for (Str rest = states; rest.len;) {
        Cut l  = cut(rest, '\n');
        Cut ns = cut(l.head, ' ');
        rest = l.tail;
        if (ns.head == name) {
            return ns.tail.len ? ns.tail : Str("unknown");
        }
    }
    return {};
}
