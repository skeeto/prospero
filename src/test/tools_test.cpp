// Tools tests: argument validation, error texts, output cleaning, the
// file tools over the in-memory file system, and the shell environment

static Str argsshow(Arena *a, i32 tool, Str raw)
{
    Args r = parseargs(a, tool, raw, EDIT_REPLACE);
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
                                             "\"ignore_case\":1,\"literal\":\"true\"}", EDIT_REPLACE);
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
                                             "\\\"status\\\":\\\"completed\\\"}]\"}", EDIT_REPLACE);
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

        Args bad = parseargs(a, TOOL_READ, "{}", EDIT_REPLACE);
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
        expect(t, "tooldeny all but desktop", istr(a, tooldeny("read write edit "
               "shell grep glob todo_write jobs job_output job_kill task skill sessions "
               "send_message")),
               istr(a, TOOL_OFFMASK|TOOL_PLANMASK));
        expect(t, "tooldeny all", istr(a, tooldeny("read write edit shell grep glob "
               "todo_write jobs job_output job_kill task desktop skill sessions "
               "send_message")),
               istr(a, TOOL_PLANMASK));
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
                            "\"model\":\"\"}", EDIT_REPLACE);
        expect(t, "task args", concat(a, concat(a, concat(a, ta.desc, "|"), ta.prompt),
               concat(a, concat(a, "|", ta.role), concat(a, "|", istr(a, !ta.model.data)))),
               "Find it|Find the parser|explore|1");
        expect(t, "task no issues", istr(a, ta.issues.len), "0");
        Args tb = parseargs(a, TOOL_TASK, "{\"prompt\":\"  \"}", EDIT_REPLACE);
        expect(t, "task empty prompt", istr(a, tb.issues.len), "1");
        Args tc = parseargs(a, TOOL_TASK, "{\"description\":\"x\"}", EDIT_REPLACE);
        expect(t, "task no prompt", tc.issues.len ? tc.issues[0] : Str{},
               "prompt: required string is missing");
        // host and cwd: a cwd only with a host
        Args td = parseargs(a, TOOL_TASK, "{\"prompt\":\"p\",\"host\":\" deb \","
                            "\"cwd\":\"/home/me\"}", EDIT_REPLACE);
        expect(t, "task host", concat(a, concat(a, td.host, "|"), td.cwd), "deb|/home/me");
        expect(t, "task host no issues", istr(a, td.issues.len), "0");
        Args tf = parseargs(a, TOOL_TASK, "{\"prompt\":\"p\",\"cwd\":\"/x\"}", EDIT_REPLACE);
        expect(t, "task cwd without host", tf.issues.len ? tf.issues[0] : Str{},
               "cwd: only with host: here, a subagent works in your working directory");
        Args tg = parseargs(a, TOOL_TASK, "{\"prompt\":\"p\",\"host\":\"\"}", EDIT_REPLACE);
        expect(t, "task empty host", istr(a, !tg.host.data && !tg.issues.len), "1");

        // send_message: a target and some text, at most 16 KiB of it
        Args ma = parseargs(a, TOOL_MESSAGE, "{\"to\":\" k1 \",\"text\":\"hi\"}", EDIT_REPLACE);
        expect(t, "message args", concat(a, concat(a, ma.to, "|"), ma.text), "k1|hi");
        expect(t, "message no issues", istr(a, ma.issues.len), "0");
        Args mb = parseargs(a, TOOL_MESSAGE, "{\"to\":\"\",\"text\":\" \"}", EDIT_REPLACE);
        expect(t, "message empty", concat(a, concat(a, mb.issues[0], "|"), mb.issues[1]),
               "to: must not be empty|text: must not be empty");
        Buf big(a, MESSAGE_MAX + 64);
        print(&big, "{\"to\":\"k\",\"text\":\"");
        for (iz i = 0; i <= MESSAGE_MAX; i++) {
            putbyte(&big, 'x');
        }
        print(&big, "\"}");
        Args mc = parseargs(a, TOOL_MESSAGE, finish(&big), EDIT_REPLACE);
        expect(t, "message too long", mc.issues.len ? mc.issues[0] : Str{},
               "text: too long: at most 16 KiB");
        expect(t, "message alias", istr(a, find(unknowntool(a, "sendmessage", 0),
               "Did you mean \"send_message\"")>0), "1");
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

    // The final newline of a file written whole: content, whether a file
    // existed, its size and last bytes (null: unreadable), the suffix
    {
        struct {
            Str content;
            b32 existed;
            i64 size;
            Str tail;
            Str want;
        } cases[] = {
            {"x", 0, 0, {}, "\n"},                  // no line breaks: LF
            {"a\nb", 0, 0, {}, "\n"},
            {"a\r\nb", 0, 0, {}, "\r\n"},           // CRLF as the content's
            {"a\r\nb\nc\r\nd", 0, 0, {}, "\r\n"},   // mixed: as most are,
            {"a\nb\nc\r\nd", 0, 0, {}, "\n"},
            {"a\r\nb\nc", 0, 0, {}, "\r\n"},        // CRLF on a tie
            {"a\r\nb\r", 0, 0, {}, "\n"},           // a lone CR gains an LF
            {"a\nb\r", 0, 0, {}, "\n"},
            {"\r", 0, 0, {}, "\n"},
            {"", 0, 0, {}, ""},                     // empty stays empty
            {"\xef\xbb\xbf", 0, 0, {}, ""},
            {"\xef\xbb\xbfx", 0, 0, {}, "\n"},
            {"a\n", 0, 0, {}, ""},                  // already ended
            {"a\r\n", 0, 0, {}, ""},
            {"\n", 0, 0, {}, ""},
            {"a\0b", 0, 0, {}, ""},                 // binary
            {"x", 1, 0, "", "\n"},                  // an existing empty file
            {"x", 1, 0, {}, "\n"},
            {"x", 1, 3, "\xef\xbb\xbf", "\n"},      // or only a BOM
            {"a\r\nb", 1, 3, "\xef\xbb\xbf", "\r\n"},
            {"x", 1, 4, "\xbb\xbf\n", "\n"},        // a BOM and a newline
            {"x", 1, 5, "\xbf\r\n", "\r\n"},
            {"x", 1, 9, "y\n", "\n"},               // old ended: so does this,
            {"x", 1, 9, "y\r\n", "\r\n"},           // with no line breaks as it
            {"x", 1, 1, "\n", "\n"},
            {"x\r", 1, 9, "y\r\n", "\n"},
            {"a\nb", 1, 9, "\r\n", "\n"},           // else as the content
            {"a\r\nb", 1, 9, "y\n", "\r\n"},
            {"a\n", 1, 9, "yz", ""},
            {"x", 1, 9, "yz", ""},                  // old unended: as given
            {"x", 1, 9, "\xef\xbb\xbf", ""},        // a BOM's bytes, not a BOM
            {"a\r\nb", 1, 9, "y\r", ""},
            {"a\nb", 1, 1, "z", ""},
            {"", 1, 9, "y\n", ""},
            {"x", 1, 9, {}, "\n"},                  // unreadable: as new
            {"a\r\nb", 1, 9, {}, "\r\n"},
            {"x", 1, 9, "", "\n"},
            {"x", 0, 9, "y", "\n"},                 // no file, whatever else
        };
        for (iz i = 0; i < countof(cases); i++) {
            expect(t, concat(a, "finalnewline ", istr(a, i)),
                   finalnewline(cases[i].content, cases[i].existed, cases[i].size,
                                cases[i].tail), cases[i].want);
        }
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
        ToolOut o = toolread(os, a, "/w", "/w/r.txt", &r, 0);
        expect(t, "read numbered", o.text, "     1\tfirst\n     2\tsecond\n     3\tthird\n");
        r.offset = 2;
        r.limit  = 1;
        o = toolread(os, a, "/w", "/w/r.txt", &r, 0);
        expect(t, "read window", o.text,
               "     2\tsecond\n[Showing lines 2-2 of 3. To read more, call read with offset=3.]\n");
        r.offset = 9;
        o = toolread(os, a, "/w", "/w/r.txt", &r, 0);
        expect(t, "read past the end", o.text, "The file has only 3 lines, so offset 9 is past its end.");
        Args none = {};
        os_write(os, *a, "/w/bin", Str{(u8 *)"a\0b", 3});
        o = toolread(os, a, "/w", "/w/bin", &none, 0);
        expect(t, "read binary", istr(a, !o.ok && startswith(o.text, "This is a binary file")), "1");
        Buf longline(a, 5000);
        for (i32 i = 0; i < 2500; i++) putbyte(&longline, 'x');
        os_write(os, *a, "/w/long.txt", finish(&longline));
        o = toolread(os, a, "/w", "/w/long.txt", &none, 0);
        expect(t, "read long line", istr(a, endswith(o.text, "... [line truncated]\n")), "1");
        os_write(os, *a, "/w/note.txt", "x");
        o = toolread(os, a, "/w", "/w/nope.txt", &none, 0);
        expect(t, "read suggests", o.text, "File not found: nope.txt. Did you mean: note.txt");

        Args w = {};
        w.path    = "/w/sub/dir/out.txt";
        w.content = "a\nb\n";
        o = toolwrite(os, a, "/w", w.path, &w);
        expect(t, "write", o.text, "Created sub/dir/out.txt (4 bytes, 2 lines).");

        // A whole file written ends in a newline, unless it replaces one
        // that did not, and what was written is exactly what rewind finds
        struct {
            Str name;
            Str old;      // null: no such file
            Str content;
            Str want;     // on disk
            Str text;
        } writes[] = {
            {"new", {}, "int main(void) { return 0; }", "int main(void) { return 0; }\n",
             "Created f.c (29 bytes, 1 lines; added a final newline)."},
            {"new crlf", {}, "a\r\nb", "a\r\nb\r\n",
             "Created f.c (6 bytes, 2 lines; added a final newline)."},
            {"new ended", {}, "a\n", "a\n", "Created f.c (2 bytes, 1 lines)."},
            {"new empty", {}, "", "", "Created f.c (0 bytes, 0 lines)."},
            {"empty file", "", "x", "x\n",
             "Replaced f.c (2 bytes, 1 lines; added a final newline)."},
            {"old bom only", "\xef\xbb\xbf", "x", "x\n",
             "Replaced f.c (2 bytes, 1 lines; added a final newline)."},
            {"old bom unended", "\xef\xbb\xbfz", "x", "x",
             "Replaced f.c (1 bytes, 1 lines)."},
            {"old ended", "old\n", "a\nb", "a\nb\n",
             "Replaced f.c (4 bytes, 2 lines; added a final newline)."},
            {"old crlf", "\xef\xbb\xbfold\r\n", "x", "x\r\n",
             "Replaced f.c (3 bytes, 1 lines; added a final newline)."},
            {"old unended", "no newline", "still none", "still none",
             "Replaced f.c (10 bytes, 1 lines)."},
            {"old unended lf", "x", "a\nb", "a\nb", "Replaced f.c (3 bytes, 2 lines)."},
        };
        for (iz i = 0; i < countof(writes); i++) {
            Str name = concat(a, "write ", writes[i].name);
            os_remove(os, *a, "/w/f.c");
            if (writes[i].old.data) {
                os_write(os, *a, "/w/f.c", writes[i].old);
            }
            Args wa = {};
            wa.content = writes[i].content;
            o = toolwrite(os, a, "/w", "/w/f.c", &wa);
            expect(t, name, o.text, writes[i].text);
            expect(t, concat(a, name, " on disk"), vread(os, a, "/w/f.c"), writes[i].want);
            expect(t, concat(a, name, " wrote"), istr(a, o.ok && o.wrote.data &&
                   o.wrote==vread(os, a, "/w/f.c")), "1");
        }

        // A longer file replaced: its last bytes decide
        Buf big(a, 1<<16);
        for (i32 i = 0; i < 4096; i++) print(&big, "0123456789abcd\r\n");
        os_write(os, *a, "/w/big.txt", finish(&big));
        Args bw = {};
        bw.content = "small";
        o = toolwrite(os, a, "/w", "/w/big.txt", &bw);
        expect(t, "write over a long file", vread(os, a, "/w/big.txt"), "small\r\n");

        // edit with an empty old_string creates a file as write does
        Args ec = {};
        ec.path   = "/w/made.txt";
        ec.newstr = "fresh";
        o = tooledit(os, a, "/w", ec.path, &ec);
        expect(t, "edit creates ended", o.text,
               "Created made.txt (6 bytes, 1 lines; added a final newline).");
        expect(t, "edit created ended", vread(os, a, "/w/made.txt"), "fresh\n");
        expect(t, "edit created wrote", istr(a, o.wrote==vread(os, a, "/w/made.txt")), "1");

        // hashline: read shows the tag, and edit checks it
        os_write(os, *a, "/w/h.txt", "one\ntwo\n");
        Str tag = filetag(a, "one\ntwo\n");
        o = toolread(os, a, "/w", "/w/h.txt", &none, 1);
        expect(t, "read tagged", o.text, concat(a, concat(a, "[h.txt#", tag),
                                                "]\n     1\tone\n     2\ttwo\n"));
        os_write(os, *a, "/w/e.txt", "");
        o = toolread(os, a, "/w", "/w/e.txt", &none, 1);
        expect(t, "read tagged empty", o.text, concat(a, concat(a, "[e.txt#", filetag(a, "")),
                                                      "]\n(empty file)"));
        Str  hraw = concat(a, concat(a, "{\"path\":\"h.txt\",\"tag\":\"", tag),
                           "\",\"edits\":[{\"op\":\"replace\",\"line\":2,\"text\":\"TWO\"}]}");
        Args h    = parseargs(a, TOOL_EDIT, hraw, EDIT_HASHLINE);
        expect(t, "hashline args", istr(a, h.issues.len*10 + h.hedits.len), "1");
        o = toolhash(os, a, "/w", "/w/h.txt", &h);
        expect(t, "hashline edit", concat(a, istr(a, o.ok), vread(os, a, "/w/h.txt")), "1one\nTWO\n");
        o = toolhash(os, a, "/w", "/w/h.txt", &h);
        expect(t, "hashline stale", istr(a, !o.ok && find(o.text, "does not match h.txt")>0), "1");
        o = toolhash(os, a, "/w", "/w/nope.txt", &h);
        expect(t, "hashline missing", istr(a, endswith(o.text, "To create a file, use write.")), "1");
        Args hb = parseargs(a, TOOL_EDIT, "{\"path\":\"h\",\"tag\":\"x\",\"edits\":["
                            "{\"op\":\"move\",\"line\":1},{\"op\":\"insert\",\"line\":-1,\"text\":\"\"},"
                            "{\"op\":\"replace\",\"line\":3,\"end\":2,\"text\":\"\"},"
                            "{\"op\":\"insert\",\"line\":1}]}", EDIT_HASHLINE);
        expect(t, "hashline bad args", argserror(a, "edit", "{}", &hb),
               "Invalid arguments for tool \"edit\", so it was not run:\n"
               "- edits[0]: op must be replace, insert or delete\n"
               "- edits[1]: line must be 0 or more: the line to insert after\n"
               "- edits[2]: end must not be before line\n"
               "- edits[3]: text is required for replace and insert\n"
               "Arguments received: {}\nUpdate the arguments and call the tool again.");
        Args hc = parseargs(a, TOOL_EDIT, "{\"path\":\"h\",\"tag\":\"x\",\"edits\":"
                            "\"{\\\"op\\\":\\\"delete\\\",\\\"line\\\":\\\"2\\\"}\"}", EDIT_HASHLINE);
        expect(t, "hashline forgiving", istr(a, hc.issues.len*100 + hc.hedits.len*10 +
                                             hc.hedits[0].end), "12");
        // An empty file gains whole lines, so it ends in a newline
        Str  eraw = concat(a, concat(a, "{\"path\":\"e.txt\",\"tag\":\"", filetag(a, "")),
                           "\",\"edits\":[{\"op\":\"insert\",\"line\":0,\"text\":\"x\"}]}");
        Args he   = parseargs(a, TOOL_EDIT, eraw, EDIT_HASHLINE);
        o = toolhash(os, a, "/w", "/w/e.txt", &he);
        expect(t, "hashline into an empty file", concat(a, istr(a, o.ok), vread(os, a, "/w/e.txt")),
               "1x\n");
        expect(t, "hashline wrote", istr(a, o.wrote==vread(os, a, "/w/e.txt")), "1");

        // patch: every file is worked out before any changes
        os_write(os, *a, "/w/p.c", "int x = 1;\nint y = 2;\n");
        os_write(os, *a, "/w/gone.c", "bye\n");
        Args pa = parseargs(a, TOOL_EDIT, "{\"patch\":\"*** Begin Patch\\n*** Update File: p.c\\n"
                            "*** Move to: q.c\\n@@\\n int x = 1;\\n-int y = 2;\\n+int y = 3;\\n"
                            "*** Delete File: gone.c\\n*** Add File: new/n.c\\n+fresh\\n"
                            "*** End Patch\"}", EDIT_PATCH);
        expect(t, "patch args", istr(a, pa.issues.len*10 + pa.patch.files.len), "3");
        Str files[][2] = {{"/w/p.c", "/w/q.c"}, {"/w/gone.c", {}}, {"/w/new/n.c", {}}};
        for (iz i = 0; i < countof(files); i++) {
            pa.patch.files[i].file   = files[i][0];
            pa.patch.files[i].fileto = files[i][1];
        }
        PatchPlan plan = planpatch(os, a, "/w", &pa.patch);
        expect(t, "patch plan", plan.text, "Applied the patch: moved p.c to q.c, deleted gone.c, "
                                           "added new/n.c.");
        expect(t, "patch changes", istr(a, plan.changes.len), "4");
        expect(t, "patch move to", plan.changes[0].data, "int x = 1;\nint y = 3;\n");
        expect(t, "patch move from", istr(a, plan.changes[1].path=="/w/p.c" &&
                                             !plan.changes[1].data.data), "1");
        expect(t, "patch added ended", plan.changes[3].data, "fresh\n");
        expect(t, "patch nothing yet", vread(os, a, "/w/p.c"), "int x = 1;\nint y = 2;\n");
        pa.patch.files[1].file = "/w/none.c";
        plan = planpatch(os, a, "/w", &pa.patch);
        expect(t, "patch all or nothing", istr(a, !plan.ok &&
                                                  startswith(plan.text, "The patch was not applied, "
                                                  "and no file was changed: *** Delete File: File "
                                                  "not found: none.c")), "1");
        Args pb = parseargs(a, TOOL_EDIT, "{\"input\":\"*** Begin Patch\\n*** End Patch\"}", EDIT_PATCH);
        expect(t, "patch bad args", pb.issues.len ? pb.issues[0] : Str{},
               "patch: not a valid patch: it changes no files");
        Args pc = parseargs(a, TOOL_EDIT, "{\"path\":\"x\"}", EDIT_PATCH);
        expect(t, "patch missing", pc.issues.len ? pc.issues[0] : Str{},
               "patch: required string is missing");
    }

    // The shell
    {
        Str inherited[] = {"PATH=/bin", "GIT_DIR=/elsewhere", "git_dir=/x", "PAGER=less",
                           "HOME=/h", "=C:=C:\\x", "MY_KEY=sk-123", "my_key=lower",
                           "MY_KEYS=kept"};
        Str secret[] = {"MY_KEY"};
        Slice<Str> secrets = {secret, countof(secret), countof(secret)};
        Slice<Str> env = shellenv(a, {inherited, countof(inherited), countof(inherited)}, 0,
                                  secrets);
        Buf b(a, 4096);
        for (iz i = 0; i < env.len; i++) {
            print(&b, env[i]);
            putbyte(&b, '\n');
        }
        Str all = finish(&b);
        expect(t, "env strips GIT_DIR", istr(a, find(all, "GIT_DIR=")), "-1");
        expect(t, "env case on POSIX", istr(a, find(all, "git_dir=/x")>=0), "1");
        expect(t, "env pager", istr(a, find(all, "PAGER=cat\n")>=0 && find(all, "PAGER=less")<0), "1");
        expect(t, "env prospero", istr(a, find(all, "\nPROSPERO=1\n")>0), "1");
        expect(t, "env keeps the rest", istr(a, find(all, "HOME=/h\n")>=0), "1");
        expect(t, "env drops secrets", istr(a, find(all, "sk-123")<0 &&
               find(all, "my_key=lower\n")>=0 && find(all, "MY_KEYS=kept\n")>=0), "1");

        Slice<Str> wenv = shellenv(a, {inherited, countof(inherited), countof(inherited)}, 1,
                                   secrets);
        b32 gitdir = 0, drive = 0, utf8 = 0, key = 0;
        for (iz i = 0; i < wenv.len; i++) {
            gitdir |= equalfold(cut(wenv[i], '=').head, "git_dir");
            drive  |= wenv[i] == "=C:=C:\\x";
            utf8   |= wenv[i] == "PYTHONUTF8=1";
            key    |= equalfold(cut(wenv[i], '=').head, "my_key");
        }
        expect(t, "env windows", istr(a, !gitdir && drive && utf8 && !key), "1");

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
        te.shell     = "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
        te.shellkind = SHELL_PS;
        c = shellcmd(a, &te, "echo \"a b\"; exit 3");
        expect(t, "ps argv", istr(a, c.argv.len==8 && c.argv[0]==te.shell &&
               c.argv[5]=="Bypass" && c.argv[6]=="-Command" && !c.cmdline.len), "1");
        expect(t, "ps env", c.env, "PROSPERO_COMMAND=echo \"a b\"; exit 3");
        expect(t, "ps boot quoting", istr(a, find(c.argv[7], "\"")<0), "1");
        expect(t, "ps description", istr(a, find(shelldesc(a, &te), "no && or ||")>0), "1");
        te.shell     = "/usr/bin/pwsh";
        te.shellkind = SHELL_PWSH;
        te.windows   = 0;
        c = shellcmd(a, &te, "ls");
        expect(t, "pwsh argv", istr(a, c.argv.len==6 && c.argv[4]=="-Command"), "1");
        expect(t, "pwsh description", istr(a, find(shelldesc(a, &te), "PowerShell 7")>0 &&
               find(shelldesc(a, &te), "no &&")<0), "1");
        te.windows   = 1;
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

        // The edit tool in each format: its own parameters
        static Str const want[] = {
            "path old_string new_string replace_all ", "patch ", "path tag edits ",
        };
        for (i32 f = 0; f < EDIT_COUNT; f++) {
            ToolEnv ef = te;
            ef.edit = f;
            JsonDoc e  = jsonparse(a, tooljson(a, &ef));
            Json   *pp = jsonget(jsonget(jsonget(jsonat(e.root, TOOL_EDIT), "function"),
                                         "parameters"), "properties");
            Buf ps(a, 64);
            for (Json *p = pp->child; p; p = p->next) {
                print(&ps, p->key);
                putbyte(&ps, ' ');
            }
            expect(t, "edit schema", concat(a, istr(a, e.ok), finish(&ps)), concat(a, "1", want[f]));
        }
        {
            ToolEnv ef = te;
            ef.edit = EDIT_HASHLINE;
            JsonDoc e  = jsonparse(a, tooljson(a, &ef));
            Json   *ed = jsonget(jsonget(jsonget(jsonget(jsonat(e.root, TOOL_EDIT), "function"),
                                         "parameters"), "properties"), "edits");
            Json   *op = jsonget(jsonget(jsonget(ed, "items"), "properties"), "op");
            expect(t, "hashline ops", istr(a, jsonlen(jsonget(op, "enum"))), "3");
        }

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
        Json *props = jsonget(jsonget(task, "parameters"), "properties");
        expect(t, "task no hosts", istr(a, jsonget(props, "host")->type==JSON_MISSING &&
               jsonget(props, "cwd")->type==JSON_MISSING &&
               find(jsonstr(jsonget(task, "description")), "Hosts:")<0), "1");

        // With hosts: listed, and host and cwd offered
        te.hosts = "\n- deb: Linux (WSL) on this machine";
        JsonDoc h = jsonparse(a, tooljson(a, &te));
        Json *ht  = jsonget(jsonat(h.root, 2), "function");
        Json *hp  = jsonget(jsonget(ht, "parameters"), "properties");
        expect(t, "task hosts params", istr(a, jsonget(hp, "host")->type==JSON_OBJECT &&
               jsonget(hp, "cwd")->type==JSON_OBJECT), "1");
        Str hd = jsonstr(jsonget(ht, "description"));
        expect(t, "task hosts described", istr(a, endswith(hd, "\nHosts: a subagent runs "
               "here unless host names one of these. There it works under that host's "
               "own settings and permissions, in cwd, and only its report comes back; "
               "role and model are that host's.\n- deb: Linux (WSL) on this machine")), "1");
        te.hosts = {};
        Str sys = systemprompt(a, "/w", &te);
        expect(t, "system prompt for a role", istr(a, find(sys, "grep and glob")<0 &&
               find(sys, "another session")<0), "1");
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
        e.moved   = 1;
        e.elapsed = 7;
        o = shellresult(os, a, "/s/none.log", &e);
        expect(t, "moved by the user", o.text, "[the user moved it to the background "
               "after 7 seconds, so it continues as job 3. You will be told when it "
               "ends: do not poll it or wait for it. Use job_output to read more of "
               "its output, and job_kill to stop it]");
        e = {};
        e.timedout = 1;
        e.nojob    = 1;
        e.timeout  = 5;
        o = shellresult(os, a, "/s/none.log", &e);
        expect(t, "no room for a job", istr(a, find(o.text, "too many background jobs")>0), "1");
    }
}
