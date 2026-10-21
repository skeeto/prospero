// Tools tests: argument validation, error texts, output cleaning, the
// file tools over the in-memory file system, and the shell environment

static Str argsshow(Arena *a, i32 tool, Str raw)
{
    Args r = parseargs(a, tool, raw);
    Buf  b(a, 256);
    for (iz i = 0; i < r.issues.len; i++) {
        print(&b, r.issues[i]);
        print(&b, "; ");
    }
    if (r.issues.len) {
        return finish(&b);
    }
    switch (tool) {
    case TOOL_READ:
        print(&b, r.path);
        putbyte(&b, ' ');
        print(&b, r.offset);
        putbyte(&b, ' ');
        print(&b, r.limit);
        break;
    case TOOL_EDIT:
        print(&b, r.path);
        putbyte(&b, ' ');
        print(&b, (i64)r.replaceall);
        break;
    case TOOL_SHELL:
        print(&b, r.command);
        putbyte(&b, ' ');
        print(&b, r.timeout);
        break;
    case TOOL_WRITE:
        print(&b, r.path);
        putbyte(&b, '=');
        print(&b, r.content);
        break;
    }
    return finish(&b);
}

static Str vread(Os *os, Arena *a, Str path)
{
    Read r = os_read(os, a, path, 1<<20);
    return r.err ? Str("(missing)") : r.data;
}

static Str editrun(Os *os, Arena *a, Str path, Str oldstr, Str newstr, b32 all)
{
    Args args = {};
    args.path       = path;
    args.oldstr     = oldstr;
    args.newstr     = newstr;
    args.replaceall = all;
    ToolOut o = tooledit(os, a, "/w", path, &args);
    return o.ok ? Str("ok") : o.text;
}

static void test_tools(Test *t)
{
    Arena *a = &t->perm;

    // Validation and coercion
    {
        expect(t, "toolfind", istr(a, toolfind("edit")*10 + (toolfind("bash")==TOOL_NONE)), "21");
        expect(t, "read coerced", argsshow(a, TOOL_READ, "{\"path\":\"x\",\"offset\":\"10\",\"limit\":5.0}"),
               "x 10 5");
        expect(t, "read null is absent", argsshow(a, TOOL_READ, "{\"path\":\"x\",\"offset\":null}"),
               "x 0 0");
        expect(t, "read bad offset", argsshow(a, TOOL_READ, "{\"path\":\"x\",\"offset\":\"ten\"}"),
               "offset: must be an integer; ");
        expect(t, "read missing path", argsshow(a, TOOL_READ, "{}"),
               "path: required string is missing; ");
        expect(t, "read empty path", argsshow(a, TOOL_READ, "{\"path\":\"\"}"),
               "path: must not be empty; ");
        {
            // Paths are bounded, before the guard ever looks at them
            Buf b(a, 1<<14);
            print(&b, "{\"path\":\"");
            for (i32 i = 0; i < 255; i++) {
                print(&b, "a/");
            }
            print(&b, "f\"}");
            Str ok = finish(&b);
            expect(t, "path of 256 names", istr(a, argsshow(a, TOOL_READ, ok).len > 500), "1");
            Buf c(a, 1<<14);
            print(&c, "{\"path\":\"");
            for (i32 i = 0; i < 128; i++) {
                print(&c, "a/b\\\\");
            }
            print(&c, "f\"}");
            expect(t, "path of 257 names", argsshow(a, TOOL_READ, finish(&c)),
                   "path: too many components: at most 256; ");
            Buf d(a, 1<<14);
            print(&d, "{\"path\":\"");
            for (i32 i = 0; i < 4097; i++) {
                putbyte(&d, 'x');
            }
            print(&d, "\"}");
            expect(t, "path of 4097 bytes", argsshow(a, TOOL_WRITE, finish(&d)),
                   "content: required string is missing; path: too long: at most 4096 bytes; ");
        }
        expect(t, "read extra keys ignored", argsshow(a, TOOL_READ, "{\"path\":\"x\",\"why\":1}"),
               "x 0 0");
        expect(t, "edit bool coercions",
               argsshow(a, TOOL_EDIT, "{\"path\":\"p\",\"old_string\":\"a\",\"new_string\":\"b\","
                                       "\"replace_all\":\"yes\"}"), "p 1");
        expect(t, "edit bad bool",
               argsshow(a, TOOL_EDIT, "{\"path\":\"p\",\"old_string\":\"a\",\"new_string\":\"b\","
                                       "\"replace_all\":\"maybe\"}"), "replace_all: must be true or false; ");
        expect(t, "shell default timeout", argsshow(a, TOOL_SHELL, "{\"command\":\"ls\"}"), "ls -1");
        expect(t, "shell clamped", argsshow(a, TOOL_SHELL, "{\"command\":\"ls\",\"timeout\":7200}"),
               "ls 600");
        expect(t, "job timeout", argsshow(a, TOOL_SHELL, "{\"command\":\"ls\",\"timeout\":7200,"
                                                          "\"background\":true}"), "ls 7200");
        expect(t, "job clamped", argsshow(a, TOOL_SHELL, "{\"command\":\"ls\",\"timeout\":9999,"
                                                          "\"background\":\"yes\"}"), "ls 7200");
        expect(t, "grep pattern", argsshow(a, TOOL_GREP, "{\"pattern\":\"\"}"),
               "pattern: must not be empty; ");
        expect(t, "grep no pattern", argsshow(a, TOOL_GREP, "{\"path\":\"x\"}"),
               "pattern: required string is missing; ");
        {
            Args g = parseargs(a, TOOL_GREP, "{\"pattern\":\"a\",\"path\":\"\","
                                             "\"ignore_case\":1,\"literal\":\"true\"}");
            expect(t, "grep args", istr(a, !g.issues.len && !g.path.data && g.ignorecase &&
                                           g.literal), "1");
        }
        expect(t, "glob pattern", argsshow(a, TOOL_GLOB, "{\"pattern\":\" \"}"),
               "pattern: must not be empty; ");
        expect(t, "todo missing", argsshow(a, TOOL_TODO, "{}"), "todos: required array is missing; ");
        expect(t, "todo not array", argsshow(a, TOOL_TODO, "{\"todos\":5}"),
               "todos: must be an array of {content, status} objects; ");
        expect(t, "todo bad items",
               argsshow(a, TOOL_TODO, "{\"todos\":[{\"content\":\"\",\"status\":\"done\"},"
                                      "{\"content\":\"x\",\"status\":\"later\"},7]}"),
               "todos[0]: content must be a non-empty string; todos[1]: status must be "
               "pending, in_progress or done; todos[2]: must be an object with content and "
               "status; ");
        {
            Args d = parseargs(a, TOOL_TODO, "{\"todos\":\"[{\\\"content\\\":\\\"x\\\","
                                             "\\\"status\\\":\\\"completed\\\"}]\"}");
            expect(t, "todo as a string", istr(a, d.todos.len*10 + d.todos[0].status), "12");
        }
        expect(t, "job_output job", argsshow(a, TOOL_JOBOUT, "{\"from\":\"middle\",\"lines\":9999}"),
               "job: a job number is required; lines: must be 1 to 2000; from: must be head "
               "or tail; ");
        expect(t, "job_kill job", argsshow(a, TOOL_JOBKILL, "{\"job\":\"x\"}"),
               "job: must be an integer; job: a job number is required; ");
        expect(t, "shell zero rejected",
               istr(a, startswith(argsshow(a, TOOL_SHELL, "{\"command\":\"ls\",\"timeout\":0}"),
                                  "timeout: 0 is not allowed")), "1");
        expect(t, "shell blank command", argsshow(a, TOOL_SHELL, "{\"command\":\"  \"}"),
               "command: must not be empty; ");
        expect(t, "write object content", argsshow(a, TOOL_WRITE, "{\"path\":\"p\",\"content\":{}}"),
               "content: must be a string; ");
        expect(t, "write number content", argsshow(a, TOOL_WRITE, "{\"path\":\"p\",\"content\":42}"),
               "p=42");
        expect(t, "not an object", argsshow(a, TOOL_WRITE, "[1]"),
               "the arguments are not a valid JSON object; ");

        Args bad = parseargs(a, TOOL_READ, "{}");
        Str  err = argserror(a, "read", "{}", &bad);
        expect(t, "argserror says how", istr(a, find(err, "Invalid arguments for tool \"read\"")==0 &&
                                                find(err, "call the tool again")>0), "1");
        expect(t, "unknown tool alias", istr(a, find(unknowntool(a, "functions.bash", 0),
                                                     "Did you mean \"shell\"")>0), "1");
        expect(t, "unknown tool plain", istr(a, find(unknowntool(a, "frobnicate", 0), "Did you")), "-1");

        // A role's tools: the rest are unknown, and never suggested
        u32 deny = tooldeny("read, grep  glob fly");
        expect(t, "tooldeny", istr(a, deny), istr(a, (((u32)1<<TOOL_COUNT) - 1) &
               ~((u32)1<<TOOL_READ | (u32)1<<TOOL_GREP | (u32)1<<TOOL_GLOB)));
        expect(t, "tooldeny all", istr(a, tooldeny("read write edit shell grep glob "
               "todo_write jobs job_output job_kill task")), "0");
        expect(t, "tooldeny none", istr(a, tooldeny("")), istr(a, ((u32)1<<TOOL_COUNT) - 1));
        expect(t, "unknown for the role", unknowntool(a, "write", deny),
               "The tool \"write\" is not available in this session, so nothing "
               "was run. The tools are: read, grep, glob.");
        expect(t, "no alias outside the role", unknowntool(a, "bash", deny),
               "No tool named \"bash\" is available, so nothing was run. The "
               "tools are: read, grep, glob.");

        // task: description is forgiven, the prompt is not
        Args ta = parseargs(a, TOOL_TASK, "{\"description\":\" Find it \","
                            "\"prompt\":\"Find the parser\",\"role\":\"explore\","
                            "\"model\":\"\"}");
        expect(t, "task args", concat(a, concat(a, concat(a, ta.desc, "|"), ta.prompt),
               concat(a, concat(a, "|", ta.role), concat(a, "|", istr(a, !ta.model.data)))),
               "Find it|Find the parser|explore|1");
        expect(t, "task no issues", istr(a, ta.issues.len), "0");
        Args tb = parseargs(a, TOOL_TASK, "{\"prompt\":\"  \"}");
        expect(t, "task empty prompt", istr(a, tb.issues.len), "1");
        Args tc = parseargs(a, TOOL_TASK, "{\"description\":\"x\"}");
        expect(t, "task no prompt", tc.issues.len ? tc.issues[0] : Str{},
               "prompt: required string is missing");
    }

    // Output cleaning
    {
        expect(t, "ansi", cleanoutput(a, "\x1b[1;31mred\x1b[0m \x1b]0;title\x07ok\n"), "red ok\n");
        expect(t, "progress", cleanoutput(a, "10%\r20%\r30%\ndone\n"), "30%\ndone\n");
        expect(t, "crlf", cleanoutput(a, "a\r\nb\r\n"), "a\nb\n");
        expect(t, "utf8head", utf8head("a\xc3\xa9z", 2), "a");
        expect(t, "utf8tail", utf8tail("a\xc3\xa9z", 2), "z");
        expect(t, "countlines", istr(a, countlines("a\nb")*10 + countlines("a\nb\n")), "22");
    }

    // The file tools
    {
        Os *os = newos(a, 1<<20);
        os_write(os, *a, "/w/f.txt", "alpha\nbeta  \ngamma\nalpha\n");
        expect(t, "edit unique", editrun(os, a, "/w/f.txt", "beta", "BETA", 0), "ok");
        expect(t, "edit result", vread(os, a, "/w/f.txt"), "alpha\nBETA  \ngamma\nalpha\n");
        Str multi = editrun(os, a, "/w/f.txt", "alpha", "x", 0);
        expect(t, "edit ambiguous", istr(a, find(multi, "matches 2 places")>0 &&
                                            find(multi, "(lines 1, 4)")>0), "1");
        expect(t, "edit replace_all", editrun(os, a, "/w/f.txt", "alpha", "x", 1), "ok");
        expect(t, "edit all result", vread(os, a, "/w/f.txt"), "x\nBETA  \ngamma\nx\n");
        expect(t, "edit trailing whitespace", editrun(os, a, "/w/f.txt", "BETA\ngamma\n", "b\n", 0), "ok");
        expect(t, "edit trailing result", vread(os, a, "/w/f.txt"), "x\nb\nx\n");
        Str miss = editrun(os, a, "/w/f.txt", "nothing here", "y", 0);
        expect(t, "edit not found", istr(a, startswith(miss, "old_string was not found")), "1");
        expect(t, "edit same", istr(a, startswith(editrun(os, a, "/w/f.txt", "x", "x", 0),
                                                  "old_string and new_string are the same")), "1");

        os_write(os, *a, "/w/q.txt", "say \xe2\x80\x9chello\xe2\x80\x9d \xe2\x80\x94 bye\n");
        expect(t, "edit typographic", editrun(os, a, "/w/q.txt", "\"hello\" - bye", "hi", 0), "ok");
        expect(t, "edit typographic result", vread(os, a, "/w/q.txt"), "say hi\n");

        os_write(os, *a, "/w/crlf.txt", "\xef\xbb\xbfone\r\ntwo\r\nthree\r\n");
        expect(t, "edit crlf", editrun(os, a, "/w/crlf.txt", "one\ntwo\n", "1\n2\n", 0), "ok");
        expect(t, "edit crlf kept", vread(os, a, "/w/crlf.txt"), "\xef\xbb\xbf" "1\r\n2\r\nthree\r\n");

        expect(t, "edit creates", editrun(os, a, "/w/new.txt", "", "fresh\n", 0), "ok");
        expect(t, "edit created", vread(os, a, "/w/new.txt"), "fresh\n");
        expect(t, "edit missing", istr(a, startswith(editrun(os, a, "/w/none.txt", "a", "b", 0),
                                                     "File not found: none.txt")), "1");

        Args r = {};
        os_write(os, *a, "/w/r.txt", "first\r\nsecond\nthird");
        ToolOut o = toolread(os, a, "/w", "/w/r.txt", &r);
        expect(t, "read numbered", o.text, "     1\tfirst\n     2\tsecond\n     3\tthird\n");
        r.offset = 2;
        r.limit  = 1;
        o = toolread(os, a, "/w", "/w/r.txt", &r);
        expect(t, "read window", o.text,
               "     2\tsecond\n[Showing lines 2-2 of 3. To read more, call read with offset=3.]\n");
        r.offset = 9;
        o = toolread(os, a, "/w", "/w/r.txt", &r);
        expect(t, "read past the end", o.text, "The file has only 3 lines, so offset 9 is past its end.");
        Args none = {};
        os_write(os, *a, "/w/bin", Str{(u8 *)"a\0b", 3});
        o = toolread(os, a, "/w", "/w/bin", &none);
        expect(t, "read binary", istr(a, !o.ok && startswith(o.text, "This is a binary file")), "1");
        Buf longline(a, 5000);
        for (i32 i = 0; i < 2500; i++) putbyte(&longline, 'x');
        os_write(os, *a, "/w/long.txt", finish(&longline));
        o = toolread(os, a, "/w", "/w/long.txt", &none);
        expect(t, "read long line", istr(a, endswith(o.text, "... [line truncated]\n")), "1");
        os_write(os, *a, "/w/note.txt", "x");
        o = toolread(os, a, "/w", "/w/nope.txt", &none);
        expect(t, "read suggests", o.text, "File not found: nope.txt. Did you mean: note.txt");

        Args w = {};
        w.path    = "/w/sub/dir/out.txt";
        w.content = "a\nb\n";
        o = toolwrite(os, a, "/w", w.path, &w);
        expect(t, "write", o.text, "Created sub/dir/out.txt (4 bytes, 2 lines).");
    }

    // The shell
    {
        Str inherited[] = {"PATH=/bin", "GIT_DIR=/elsewhere", "git_dir=/x", "PAGER=less",
                           "HOME=/h", "=C:=C:\\x"};
        Slice<Str> env = shellenv(a, {inherited, countof(inherited), countof(inherited)}, 0);
        Buf b(a, 4096);
        for (iz i = 0; i < env.len; i++) {
            print(&b, env[i]);
            putbyte(&b, '\n');
        }
        Str all = finish(&b);
        expect(t, "env strips GIT_DIR", istr(a, find(all, "GIT_DIR=")), "-1");
        expect(t, "env case on POSIX", istr(a, find(all, "git_dir=/x")>=0), "1");
        expect(t, "env pager", istr(a, find(all, "PAGER=cat\n")>=0 && find(all, "PAGER=less")<0), "1");
        expect(t, "env harness", istr(a, find(all, "\nHARNESS=1\n")>0), "1");
        expect(t, "env keeps the rest", istr(a, find(all, "HOME=/h\n")>=0), "1");

        Slice<Str> wenv = shellenv(a, {inherited, countof(inherited), countof(inherited)}, 1);
        b32 gitdir = 0, drive = 0, utf8 = 0;
        for (iz i = 0; i < wenv.len; i++) {
            gitdir |= equalfold(cut(wenv[i], '=').head, "git_dir");
            drive  |= wenv[i] == "=C:=C:\\x";
            utf8   |= wenv[i] == "PYTHONUTF8=1";
        }
        expect(t, "env windows", istr(a, !gitdir && drive && utf8), "1");

        ToolEnv te = {};
        te.shell     = "C:\\bb\\busybox.exe";
        te.shellkind = SHELL_BUSYBOX;
        te.windows   = 1;
        ShellCmd c = shellcmd(a, &te, "ls -l");
        expect(t, "busybox argv", istr(a, c.argv.len==4 && c.argv[1]=="sh" && c.argv[3]=="ls -l"), "1");
        te.shell     = "C:\\Windows\\System32\\cmd.exe";
        te.shellkind = SHELL_CMD;
        c = shellcmd(a, &te, "dir \"a b\" & echo %X%");
        expect(t, "cmd line", c.cmdline,
               "\"C:\\Windows\\System32\\cmd.exe\" /d /s /c \"dir \"a b\" & echo %X%\"");
        expect(t, "cmd description", istr(a, find(shelldesc(a, &te), "No POSIX shell")>0), "1");
        te.jobtimeout = 90;
        te.jobmax     = 3600;
        Str desc = shelldesc(a, &te);
        expect(t, "job limits described", istr(a, find(desc, "stopped after 90 seconds")>0 &&
               find(desc, "at most 60 minutes")>0), "1");
    }

    // Tools JSON: byte-stable, and valid
    {
        ToolEnv te = {};
        te.shell = "/bin/sh";
        Str     j1 = tooljson(a, &te);
        Str     j2 = tooljson(a, &te);
        JsonDoc d  = jsonparse(a, j1);
        expect(t, "tooljson stable", istr(a, j1 == j2), "1");
        expect(t, "tooljson valid", istr(a, d.ok*100 + jsonlen(d.root)), istr(a, 100+TOOL_COUNT));
        Json *todo = jsonget(jsonget(jsonat(d.root, TOOL_TODO), "function"), "parameters");
        Json *item = jsonget(jsonget(jsonget(todo, "properties"), "todos"), "items");
        expect(t, "todo schema", jsonstr(jsonget(item, "type")), "object");

        // A role's tools only, and the task tool's roles
        te.deny     = tooldeny("read glob task");
        te.roles    = "\n- explore: reads";
        te.roleenum = "\"enum\":[\"explore\",\"general\"]";
        JsonDoc r   = jsonparse(a, tooljson(a, &te));
        Buf     names(a, 128);
        for (Json *f = r.root->child; f; f = f->next) {
            print(&names, jsonstr(jsonget(jsonget(f, "function"), "name")));
            putbyte(&names, ' ');
        }
        expect(t, "tooljson for a role", istr(a, r.ok), "1");
        expect(t, "tooljson role tools", finish(&names), "read glob task ");
        Json *task = jsonget(jsonat(r.root, 2), "function");
        Json *role = jsonget(jsonget(jsonget(task, "parameters"), "properties"), "role");
        expect(t, "task roles enum", istr(a, jsonlen(jsonget(role, "enum"))), "2");
        expect(t, "task roles described", istr(a, find(jsonstr(jsonget(task, "description")),
               "Roles:\n- explore: reads")>0), "1");
        Str sys = systemprompt(a, "/w", &te);
        expect(t, "system prompt for a role", istr(a, find(sys, "grep and glob")<0 &&
               find(sys, "todo_write")<0 && find(sys, "subagents")>0), "1");
    }

    // Todo lists
    {
        Slice<TodoItem> old = {}, now = {};
        TodoItem x = {"write", TODO_ACTIVE}, y = {"test", TODO_PENDING};
        old = push(a, old, x);
        old = push(a, old, y);
        expect(t, "todoshow", todoshow(a, old),
               "Todo list, 0 of 2 done:\n[>] write (in progress)\n[ ] test\n");
        expect(t, "todoleft", todoleft(a, old), "[>] write\n[ ] test\n");
        expect(t, "todoshow empty", todoshow(a, now), "The todo list is empty now.");
        expect(t, "todo same", istr(a, todoprogress(old, old)), "0");
        x.status = TODO_DONE;
        y.status = TODO_ACTIVE;
        now = push(a, now, x);
        now = push(a, now, y);
        expect(t, "todo progress", istr(a, todoprogress(old, now)), "1");
        expect(t, "todo left none", istr(a, !todoleft(a, Slice<TodoItem>{}).data), "1");
        Slice<TodoItem> fewer = {};
        fewer = push(a, fewer, old[0]);
        expect(t, "todo fewer", istr(a, todoprogress(old, fewer)), "1");
        Slice<TodoItem> swapped = {};
        TodoItem z = {"test", TODO_ACTIVE}, w = {"write", TODO_PENDING};
        swapped = push(a, swapped, w);
        swapped = push(a, swapped, z);
        expect(t, "todo new active", istr(a, todoprogress(old, swapped)), "1");
    }

    // Job output
    {
        Os *os = newos(a, 1<<20);
        Buf b(a, 1024);
        for (i32 i = 1; i <= 30; i++) {
            print(&b, "line ");
            print(&b, (i64)i);
            print(&b, i==5 ? Str("\x1b[1mbold\x1b[0m\r\n") : Str("\n"));
        }
        os_write(os, *a, "/s/out.log", finish(&b));
        JobLines jl = joblines(os, a, "/s/out.log", JOBLINES_TAIL, 2, 0);
        expect(t, "joblines tail", jl.text, "line 29\nline 30\n");
        expect(t, "joblines tail more", istr(a, jl.before*10 + jl.after), "10");
        jl = joblines(os, a, "/s/out.log", JOBLINES_HEAD, 2, 0);
        expect(t, "joblines head", jl.text, "line 1\nline 2\n");
        expect(t, "joblines head more", istr(a, jl.first*100 + jl.before*10 + jl.after), "101");
        jl = joblines(os, a, "/s/out.log", JOBLINES_AT, 2, 5);
        expect(t, "joblines at", jl.text, "line 5bold\nline 6\n");
        expect(t, "joblines at first", istr(a, jl.first), "5");
        jl = joblines(os, a, "/s/out.log", JOBLINES_AT, 2, 99);
        expect(t, "joblines past", istr(a, jl.count*10 + jl.before), "1");
        jl = joblines(os, a, "/s/none.log", JOBLINES_TAIL, 2, 0);
        expect(t, "joblines none", istr(a, jl.count + jl.text.len), "0");

        Str tail = jobtail(os, a, "/s/out.log");
        expect(t, "jobtail", istr(a, startswith(tail, "line 11\n") &&
               endswith(tail, "line 30\n")), "1");

        ShellEnd e = {};
        e.job     = 3;
        e.timeout = 120;
        ToolOut o = shellresult(os, a, "/s/none.log", &e);
        expect(t, "moved to a job", o.text, "[still running after 2 minutes, so it "
               "continues in the background as job 3. You will be told when it ends: "
               "do not poll it or wait for it. Use job_output to read more of its "
               "output, and job_kill to stop it]");
        e = {};
        e.timedout = 1;
        e.nojob    = 1;
        e.timeout  = 5;
        o = shellresult(os, a, "/s/none.log", &e);
        expect(t, "no room for a job", istr(a, find(o.text, "too many background jobs")>0), "1");
    }
}
