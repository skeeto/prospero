// Containers made for sessions: the pure parts, and container.ini kept
// in a state directory (the runtime's commands run in tests/gui.py's
// containers mode)

static void test_container(Test *t)
{
    Arena  a = t->perm;
    CfgEnv v = newcfgenv(&a);

    // Finding a runtime on the PATH, docker before podman
    os_write(v.os, a, "/u/bin/podman", "x");
    os_write(v.os, a, "/d/bin/docker", "x");
    CtrRuntime rt = ctrfind(v.os, &a, v.scratch, "auto", "/none:/u/bin");
    expect(t, "ctrfind podman", concat(&a, rt.name, concat(&a, " ", rt.path)),
           "podman /u/bin/podman");
    rt = ctrfind(v.os, &a, v.scratch, "", "/u/bin:/d/bin");
    expect(t, "ctrfind docker first", rt.path, "/d/bin/docker");
    rt = ctrfind(v.os, &a, v.scratch, "podman", "/d/bin:/u/bin");
    expect(t, "ctrfind named", rt.path, "/u/bin/podman");
    rt = ctrfind(v.os, &a, v.scratch, "docker", "/u/bin");
    expect(t, "ctrfind missing", concat(&a, rt.name, concat(&a, ":", rt.path)), "docker:");
    rt = ctrfind(v.os, &a, v.scratch, "/u/bin/podman", "");
    expect(t, "ctrfind path", concat(&a, rt.name, concat(&a, ":", rt.path)),
           "podman:/u/bin/podman");

    // The command that makes one
    Ctr c = {};
    c.name    = "p-0011aabb";
    c.runtime = "docker";
    c.program = "/d/bin/docker";
    c.image   = "debian:12";
    Slice<Str> args = {};
    args = push(&a, args, Str("--cap-add=SYS_PTRACE"));
    Slice<Str> argv = ctrcreateargv(&a, &c, "f00d", "/s/state", " 2 ", "", args);
    expect(t, "ctrcreate empty", joinwords(&a, argv),
           "/d/bin/docker|create|--name|prospero-p-0011aabb|"
           "--label|com.nullprogram.prospero=1|"
           "--label|com.nullprogram.prospero.owner=f00d|--init|--network|none|"
           "--cpus|2|--cap-add=SYS_PTRACE|-v|/s/state:/prospero/state|"
           "--tmpfs|/prospero/state/run|-v|/work|-w|/work|--entrypoint|sleep|debian:12|"
           "2147483647");
    c.workspace = CTR_MOUNT;
    c.folder    = "/h/src";
    c.network   = 1;
    argv = ctrcreateargv(&a, &c, "f00d", "/s/state", {}, "4g", {});
    expect(t, "ctrcreate mount", joinwords(&a, argv),
           "/d/bin/docker|create|--name|prospero-p-0011aabb|"
           "--label|com.nullprogram.prospero=1|"
           "--label|com.nullprogram.prospero.owner=f00d|--init|--memory|4g|"
           "-v|/s/state:/prospero/state|--tmpfs|/prospero/state/run|"
           "-v|/h/src:/work|-w|/work|--entrypoint|sleep|debian:12|2147483647");
    c.workspace = CTR_COPY;
    argv = ctrcreateargv(&a, &c, "f00d", "/s/state", {}, {}, {});
    expect(t, "ctrcreate copy, a volume", istr(&a, find(joinwords(&a, argv), "|-v|/work|-w|")>0),
           "1");

    // Reached as an exec remote, its words quoted
    expect(t, "ctrexec", ctrexec(&a, &c), "\"/d/bin/docker\" exec -i prospero-p-0011aabb");
    Reach l = {};
    l.exec = ctrexec(&a, &c);
    expect(t, "ctrexec argv", joinwords(&a, linkargv(&a, {}, l, "x")),
           "/d/bin/docker|exec|-i|prospero-p-0011aabb|/bin/sh|-c|cd; x");

    // container.ini, kept and read back
    c.created = 1793546320000;
    c.trashed = 1793546330000;
    CtrSess ss[] = {{"20261101-150000-abcd", "Fuzz\nthe parser"}, {"20261101-150100-ef01", ""}};
    c.sessions = {ss, countof(ss), countof(ss)};
    expect(t, "ctrsave", istr(&a, ctrsave(v.os, v.scratch, "/st", &c)), "1");
    expect(t, "ctrini", cfgfile(v.os, "/st/containers/p-0011aabb/container.ini"),
           "runtime = docker\nprogram = /d/bin/docker\nimage = debian:12\n"
           "workspace = copy\nfolder = /h/src\nnetwork = yes\n"
           "created = 1793546320000\ntrashed = 1793546330000\n"
           "session = 20261101-150000-abcd Fuzz the parser\n"
           "session = 20261101-150100-ef01 \n");
    os_write(v.os, a, "/st/containers/p-bad/container.ini", "image = x\n");  // no program
    os_write(v.os, a, "/st/containers/other/container.ini", "image = x\nprogram = y\n");
    Slice<Ctr> all = ctrall(v.os, &a, v.scratch, "/st");
    expect(t, "ctrall", istr(&a, all.len), "1");
    Ctr *b = all.len ? &all[0] : &c;
    expect(t, "ctrall back", ctrini(&a, b), ctrini(&a, &c));
    expect(t, "ctrall sessions", concat(&a, b->sessions[0].title, b->sessions[1].sid),
           "Fuzz the parser20261101-150100-ef01");

    // ps's states, by name
    Str states = "p-1 running\np-22 exited\n";
    expect(t, "ctrstateof", ctrstateof(states, "p-22"), "exited");
    expect(t, "ctrstateof none", istr(&a, !ctrstateof(states, "p-2").data), "1");

    expect(t, "ctrowner", istr(&a, ctrowner(&a, "/st").len), "16");
    expect(t, "ctrowner differs", istr(&a, ctrowner(&a, "/st")!=ctrowner(&a, "/su")), "1");
    expect(t, "ctrnewname", istr(&a, ctrnewname(v.os, &a).len), "10");

    // A subagent's: t-, labelled a task's, its /out mounted, and never
    // listed with the window's
    Ctr k = {};
    k.name    = "t-00c0ffee";
    k.program = "docker";
    k.image   = "alpine";
    k.out     = "/sc/containers/t-00c0ffee";
    argv = ctrcreateargv(&a, &k, "f00d", "/s/state", {}, {}, {});
    expect(t, "ctrcreate task", joinwords(&a, argv),
           "docker|create|--name|prospero-t-00c0ffee|"
           "--label|com.nullprogram.prospero=1|"
           "--label|com.nullprogram.prospero.owner=f00d|"
           "--label|com.nullprogram.prospero.task=1|--init|--network|none|"
           "-v|/s/state:/prospero/state|--tmpfs|/prospero/state/run|-v|/work|"
           "-v|/sc/containers/t-00c0ffee:/out|-w|/work|--entrypoint|sleep|alpine|"
           "2147483647");
    expect(t, "ctrsave task", istr(&a, ctrsave(v.os, v.scratch, "/st", &k)), "1");
    expect(t, "ctrini out", istr(&a, find(cfgfile(v.os, "/st/containers/t-00c0ffee/container.ini"),
                                          "\nout = /sc/containers/t-00c0ffee\n")>0), "1");
    expect(t, "ctrall no task", istr(&a, ctrall(v.os, &a, v.scratch, "/st").len), "1");
    expect(t, "ctrnewname task", takehead(ctrnewname(v.os, &a, "t-"), 2), "t-");

    // [container] images, by name as compared
    Slice<Str> imgs = ctrimagelist(&a, " debian:12, alpine\tdocker.io/library/ubuntu:24.04,,");
    expect(t, "ctrimagelist", joinwords(&a, imgs), "debian:12|alpine|docker.io/library/ubuntu:24.04");
    expect(t, "ctrimagekey", ctrimagekey(&a, "docker.io/library/alpine"), "alpine:latest");
    expect(t, "ctrimagekey registry", ctrimagekey(&a, "localhost:5000/fuzz"),
           "localhost:5000/fuzz:latest");
    expect(t, "ctrimagekey digest", ctrimagekey(&a, "alpine@sha256:ab"), "alpine@sha256:ab");
    Str set = "debian:12, alpine, docker.io/library/ubuntu:24.04";
    expect(t, "ctrimagelisted", istr(&a, ctrimagelisted(a, set, "alpine:latest")*100 +
                                         ctrimagelisted(a, set, "ubuntu:24.04")*10 +
                                         ctrimagelisted(a, set, "debian:13")), "110");

    // A subagent's container's sessions, kept: each copied whole, its
    // first record without remote, so a subagent of its parent here;
    // never over a session here, nor a dot folder
    Str kept = "/st/containers/t-0000beef/state/sessions";
    Str dirs[] = {"/20261102-120000-abcd", "/20261102-120000-abcd/out",
                  "/20261102-120001-dupe", "/.part"};
    for (i32 i = 0; i < countof(dirs); i++) {
        os_mkdirs(v.os, a, concat(&a, kept, dirs[i]));
    }
    os_mkdirs(v.os, a, "/st/sessions/20261102-120001-dupe");
    os_write(v.os, a, concat(&a, kept, "/20261102-120000-abcd/events.jsonl"),
             "{\"sid\":\"20261102-120000-abcd\",\"ev\":\"session\",\"cwd\":\"/work\","
             "\"parent\":\"20261102-110000-0001\",\"call\":\"c1\",\"depth\":1,"
             "\"remote\":1,\"via\":\"exec\",\"relay\":\"ab\"}\n{\"ev\":\"turn.end\"}\n");
    os_write(v.os, a, concat(&a, kept, "/20261102-120000-abcd/out/x.txt"), "x");
    os_write(v.os, a, concat(&a, kept, "/20261102-120001-dupe/events.jsonl"), "theirs\n");
    os_write(v.os, a, concat(&a, kept, "/.part/events.jsonl"), "{}\n");
    os_write(v.os, a, "/st/sessions/20261102-120001-dupe/events.jsonl", "mine\n");
    ctrkeep(v.os, v.scratch, "/st", "t-0000beef");
    expect(t, "ctrkeep log", cfgfile(v.os, "/st/sessions/20261102-120000-abcd/events.jsonl"),
           "{\"sid\":\"20261102-120000-abcd\",\"ev\":\"session\",\"cwd\":\"/work\","
           "\"parent\":\"20261102-110000-0001\",\"call\":\"c1\",\"depth\":1,"
           "\"kept\":\"t-0000beef\"}\n"
           "{\"ev\":\"turn.end\"}\n");
    expect(t, "ctrkeep files", cfgfile(v.os, "/st/sessions/20261102-120000-abcd/out/x.txt"), "x");
    expect(t, "ctrkeep not over", cfgfile(v.os, "/st/sessions/20261102-120001-dupe/events.jsonl"),
           "mine\n");
    expect(t, "ctrkeep dot", cfgfile(v.os, "/st/sessions/.part/events.jsonl"), "(missing)");

    // [container] chooses what runs: an included file's is ignored
    os_write(v.os, a, "/c/team.ini", "[container]\nargs = --privileged\n");
    os_write(v.os, a, "/c/x.ini", "include = team.ini\n[container]\nimages = debian:12\n");
    Config *cfg = newconfig(v.os, &a, v.scratch, "/c/x.ini", 0);
    expect(t, "container.own", cfgget(cfg, "container", "", "images", 0), "debian:12");
    expect(t, "container.shared", cfgget(cfg, "container", "", "args", 0), "");
    expect(t, "container.diags", inidiags(&a, cfg->ini),
           "/c/team.ini:2:8: error: a [container] setting in an included file is "
           "ignored, since it chooses what runs: set it in your own file");
    cfgfree(cfg);
}
