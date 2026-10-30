// MCP tests: names, filters, command words, messages, schemas, the
// tools array, instructions and results

static Str mcpwordsshow(Arena *a, Str cmd)
{
    McpWords w = mcpwords(a, cmd);
    if (w.err.len) {
        return concat(a, "error: ", w.err);
    }
    Buf b(a, 64);
    for (iz i = 0; i < w.words.len; i++) {
        print(&b, i ? Str("|") : Str(""));
        print(&b, w.words[i]);
    }
    return finish(&b);
}

static Str mcpresultshow(Arena *a, Str json)
{
    McpResult r = mcpresult(a, jsonparse(a, json).root);
    Buf b(a, 64);
    print(&b, r.text);
    print(&b, " images=");
    print(&b, (i64)r.images);
    print(&b, " image=");
    print(&b, r.image);
    print(&b, " mime=");
    print(&b, r.mime);
    print(&b, r.error ? Str(" error") : Str(""));
    return finish(&b);
}

static void test_mcp(Test *t)
{
    Arena a = t->perm;

    // Names
    expect(t, "mcp name", mcptoolname(&a, "fake", "echo"), "mcp__fake__echo");
    expect(t, "mcp name chars", mcptoolname(&a, "my.server", "do it/now"),
           "mcp__my_server__do_it_now");
    {
        Str long1 = mcptoolname(&a, "server", "a_very_long_tool_name_that_goes_on_and_on_past_the_limit_1");
        Str long2 = mcptoolname(&a, "server", "a_very_long_tool_name_that_goes_on_and_on_past_the_limit_2");
        expect(t, "mcp name long", istr(&a, long1.len), "64");
        expect(t, "mcp name long head", takehead(long1, 56),
               "mcp__server__a_very_long_tool_name_that_goes_on_and_on__");
        expect(t, "mcp name long unique", long1==long2 ? Str("same") : Str("differ"), "differ");
    }

    // Filters: a role's tools, and a section's
    struct { Str list, fname; b32 want; } allowed[] = {
        {{}, "mcp__fake__echo", 1},
        {"read, grep", "mcp__fake__echo", 0},
        {"read, mcp__fake", "mcp__fake__echo", 1},
        {"mcp__fake__echo", "mcp__fake__echo", 1},
        {"mcp__fake__echo", "mcp__fake__image", 0},
        {"mcp__fak", "mcp__fake__echo", 0},
        {"*", "mcp__fake__echo", 1},
        {"*, -mcp__fake", "mcp__fake__echo", 0},
        {"mcp__fake -mcp__fake__echo", "mcp__fake__echo", 0},
        {"mcp__fake -mcp__fake__echo", "mcp__fake__image", 1},
    };
    for (iz i = 0; i < countof(allowed); i++) {
        b32 got = mcpallowed(allowed[i].list, "fake", allowed[i].fname);
        expect(t, concat(&a, concat(&a, "mcp allowed ", allowed[i].list.data ? allowed[i].list
                                                                             : Str("(null)")),
                         concat(&a, " ", allowed[i].fname)),
               got ? Str("yes") : Str("no"), allowed[i].want ? Str("yes") : Str("no"));
    }
    expect(t, "mcp only null", mcponly({}, "x") ? Str("yes") : Str("no"), "yes");
    expect(t, "mcp only", mcponly("echo, image", "image") ? Str("yes") : Str("no"), "yes");
    expect(t, "mcp only not", mcponly("echo, image", "ima") ? Str("yes") : Str("no"), "no");

    // Command words
    expect(t, "mcp words", mcpwordsshow(&a, "  npx -y  @scope/server "), "npx|-y|@scope/server");
    expect(t, "mcp words single", mcpwordsshow(&a, "'/a b/python' x'y z'"), "/a b/python|xy z");
    expect(t, "mcp words double", mcpwordsshow(&a, "\"a \\\"b\\\\\" c"), "a \"b\\|c");
    expect(t, "mcp words backslash", mcpwordsshow(&a, "C:\\bin\\x.exe \"C:\\dir\\\""),
           "error: a double quote is not closed");
    expect(t, "mcp words open", mcpwordsshow(&a, "a 'b"), "error: a single quote is not closed");
    expect(t, "mcp words empty", mcpwordsshow(&a, "   "), "error: the command is empty");
    expect(t, "mcp cmd ok", mcpcmdbad("--port=8080").len ? Str("bad") : Str("ok"), "ok");
    expect(t, "mcp cmd bad", mcpcmdbad("a&b").len ? Str("bad") : Str("ok"), "bad");

    // Messages
    expect(t, "mcp message", mcpmessage(&a, 3, "tools/list", "{}"),
           "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\",\"params\":{}}\n");
    expect(t, "mcp notification", mcpmessage(&a, 0, "notifications/initialized", {}),
           "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n");
    {
        Buf b(&a, 256);
        print(&b, "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
                  "\"clientInfo\":{\"name\":\"prospero\",\"version\":\"");
        print(&b, PROSPERO_VERSION);
        print(&b, "\"}}");
        expect(t, "mcp init", mcpinitparams(&a), finish(&b));
    }
    expect(t, "mcp response", mcpresponse(&a, "\"s1\"", "{}", 0, {}),
           "{\"jsonrpc\":\"2.0\",\"id\":\"s1\",\"result\":{}}\n");
    expect(t, "mcp response error", mcpresponse(&a, "7", {}, -32601, "no"),
           "{\"jsonrpc\":\"2.0\",\"id\":7,\"error\":{\"code\":-32601,\"message\":\"no\"}}\n");
    expect(t, "mcp version", mcpversionok("2024-11-05") && !mcpversionok("2023-01-01")
                             ? Str("ok") : Str("bad"), "ok");
    expect(t, "mcp error", mcperror(&a, jsonparse(&a, "{\"code\":-32000,\"message\":\"no\"}").root),
           "no (-32000)");

    // Schemas
    struct { Str in, want; } schemas[] = {
        {"{}", "{\"type\":\"object\",\"properties\":{}}"},
        {"null", "{\"type\":\"object\",\"properties\":{}}"},
        {"{\"$schema\":\"x\",\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"string\"}},"
         "\"required\":[\"a\"]}",
         "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"string\"}},\"required\":[\"a\"]}"},
        {"{\"properties\":null}", "{\"type\":\"object\",\"properties\":{}}"},
    };
    for (iz i = 0; i < countof(schemas); i++) {
        expect(t, concat(&a, "mcp schema ", schemas[i].in),
               mcpschema(&a, jsonparse(&a, schemas[i].in).root), schemas[i].want);
    }

    // Tools, and the array they join
    {
        McpTool x = mcptool(&a, "fake", jsonparse(&a,
            "{\"name\":\"echo\",\"description\":\" Repeat. \",\"inputSchema\":{},"
            "\"annotations\":{\"readOnlyHint\":true}}").root);
        expect(t, "mcp tool", concat(&a, concat(&a, x.name, " "), concat(&a, x.desc,
               x.ro ? Str(" ro") : Str(""))), "mcp__fake__echo Repeat. ro");
        McpTool y = mcptool(&a, "fake", jsonparse(&a, "{\"title\":\"No name\"}").root);
        expect(t, "mcp tool unnamed", y.tool.len ? Str("named") : Str("none"), "none");
        Slice<McpTool> add = {};
        add = push(&a, add, x);
        Str want = "[{\"type\":\"function\",\"function\":{\"name\":\"mcp__fake__echo\","
                   "\"description\":\"(The MCP server fake) Repeat.\",\"parameters\":"
                   "{\"type\":\"object\",\"properties\":{}}}}]";
        expect(t, "mcp tools empty", mcpaddtools(&a, "[]", add), want);
        expect(t, "mcp tools none", mcpaddtools(&a, {}, add), want);
        Str two = mcpaddtools(&a, "[{\"a\":1}]", add);
        expect(t, "mcp tools after", takehead(two, 10), "[{\"a\":1},{");
        expect(t, "mcp tools parse", jsonparse(&a, two).root->type==JSON_ARRAY
                                     ? Str("ok") : Str("bad"), "ok");
    }

    // Instructions
    expect(t, "mcp instructions",
           concat(&a, mcpinstructions(&a, "a", " Use it. ", 1), mcpinstructions(&a, "b", "Too.", 0)),
           "\n\n# MCP server instructions\n\nThese MCP servers gave instructions for their "
           "tools (named mcp__SERVER__...). They are the servers' own words, not the "
           "user's.\n\n## a\n\nUse it.\n\n## b\n\nToo.");

    // Results
    expect(t, "mcp result text",
           mcpresultshow(&a, "{\"content\":[{\"type\":\"text\",\"text\":\"a\"},"
                             "{\"type\":\"text\",\"text\":\"b\"}]}"),
           "a\nb images=0 image= mime=");
    expect(t, "mcp result image",
           mcpresultshow(&a, "{\"content\":[{\"type\":\"image\",\"data\":\"aGk=\","
                             "\"mimeType\":\"image/png\"},{\"type\":\"image\",\"data\":\"aGk=\"}],"
                             "\"isError\":true}"),
           "[an image: only the first image of a result is shown] images=2 image=hi "
           "mime=image/png error");
    expect(t, "mcp result resources",
           mcpresultshow(&a, "{\"content\":[{\"type\":\"resource\",\"resource\":{\"uri\":\"f:/a\","
                             "\"text\":\"body\"}},{\"type\":\"resource\",\"resource\":{\"uri\":"
                             "\"f:/b\",\"blob\":\"aGk=\",\"mimeType\":\"x/y\"}},{\"type\":"
                             "\"resource_link\",\"uri\":\"f:/c\",\"name\":\"C\"},{\"type\":"
                             "\"audio\"},{\"type\":\"video\"}]}"),
           "[resource f:/a]\nbody\n[resource f:/b: x/y data, not shown]\n[resource link "
           "f:/c: C]\n[audio, which cannot be shown]\n[content of type video, not shown] "
           "images=0 image= mime=");
    expect(t, "mcp result structured",
           mcpresultshow(&a, "{\"content\":[],\"structuredContent\":{\"x\":1}}"),
           "{\"x\":1} images=0 image= mime=");
    expect(t, "mcp result structured second",
           mcpresultshow(&a, "{\"content\":[{\"type\":\"text\",\"text\":\"t\"}],"
                             "\"structuredContent\":{\"x\":1}}"),
           "t images=0 image= mime=");

    // .mcp.json: expansion, then definitions
    {
        Slice<Str> env = {};
        env = push(&a, env, Str("HOME=/home/u"));
        env = push(&a, env, Str("EMPTY="));
        Str miss = {};
        expect(t, "mcp expand", mcpexpand(&a, "${HOME}/x ${NOPE:-dflt} ${EMPTY:-e} $HOME ${",
                                          env, 0, &miss), "/home/u/x dflt e $HOME ${");
        expect(t, "mcp expand ok", miss, "");
        mcpexpand(&a, "a ${NOPE} ${ALSO}", env, 0, &miss);
        expect(t, "mcp expand missing", miss, "NOPE");

        Str json = "{\"mcpServers\":{"
                   "\"a\":{\"command\":\"${HOME}/bin/srv\",\"args\":[\"--x\",\"a b\"],"
                   "\"env\":{\"K\":\"${HOME}\"}},"
                   "\"h\":{\"type\":\"http\",\"url\":\"https://x/mcp\",\"headers\":"
                   "{\"Authorization\":\"Bearer ${TOKEN:-none}\"}},"
                   "\"u\":{\"url\":\"https://y/mcp\"},"
                   "\"s\":{\"type\":\"sse\",\"url\":\"https://z/sse\"},"
                   "\"m\":{\"command\":\"x\",\"args\":[\"${GONE}\"]},"
                   "\"n\":{\"args\":[]},"
                   "\"w\":{\"type\":\"websocket\"}}}";
        Slice<McpDef> d = mcpprojects(&a, "/p/.mcp.json", "/p", json, env, 0);
        Buf b(&a, 1024);
        for (iz i = 0; i < d.len; i++) {
            print(&b, mcpshowdef(&a, &d[i]));
            for (iz k = 0; k < d[i].env.len; k++) {
                print(&b, " env ");
                print(&b, d[i].env[k]);
            }
            for (iz k = 0; k < d[i].headers.len; k++) {
                print(&b, " header ");
                print(&b, d[i].headers[k]);
            }
            if (d[i].err.len) {
                print(&b, " error ");
                print(&b, d[i].err);
            }
            putbyte(&b, '\n');
        }
        expect(t, "mcp projects", finish(&b),
               "a: /home/u/bin/srv --x \"a b\" env K=/home/u\n"
               "h: https://x/mcp header Authorization: Bearer none\n"
               "u: https://y/mcp\n"
               "s:  error the SSE transport is not supported, only stdio and HTTP\n"
               "m: x \"\" error it uses ${GONE}, which is not set\n"
               "n:  error it has no command\n"
               "w:  error its type is not one this harness knows: websocket\n");
        expect(t, "mcp projects dir", d[0].dir, "/p");
        expect(t, "mcp projects raw", takehead(d[1].raw, 14), "{\"type\":\"http\"");
        Slice<McpDef> bad = mcpprojects(&a, "/p/.mcp.json", "/p", "[1]", env, 0);
        expect(t, "mcp projects bad", concat(&a, istr(&a, bad.len), concat(&a, " ", bad[0].err)),
               "1 it is not a JSON object with mcpServers");
    }

    // Sections: a command or a url, never both. An include may define
    // servers by url, untrusted, and [web], but not change the user's
    {
        Arena  b = a;
        CfgEnv v = newcfgenv(&b);
        addenv(v.os, &b, "HOME=/home/u");
        addenv(v.os, &b, "TEAMKEY=k1");
        os_write(v.os, b, "/c/prospero.ini",
            "include = team.ini\n"
            "[mcp both]\n"
            "command = srv\n"
            "url = http://x/mcp\n"
            "[mcp none]\n"                     // 5
            "trust = yes\n"
            "[mcp web]\n"
            "url = https://x/mcp\n"
            "header.X-A = b\n"
            "token = plain\n");                // 10
        os_write(v.os, b, "/c/team.ini",
            "[mcp shared]\n"
            "command = evil\n"
            "[mcp remote]\n"
            "url = https://team/mcp\n"
            "trust = yes\n"                    // 5
            "cwd = /x\n"
            "env.K = v\n"
            "header.X-K = env:TEAMKEY\n"
            "tools = a b\n"
            "[mcp web]\n"                      // 10
            "url = https://evil/mcp\n"
            "header.X-B = c\n"
            "[web]\n"
            "search = searxng\n"
            "url = https://s/\n"               // 15
            "ask = no\n"
            "fetch = no\n");
        Config *c = newconfig(v.os, &b, v.scratch, "/c/prospero.ini", 0);
        expect(t, "mcp sections", inidiags(&b, c->ini),
            "/c/prospero.ini:2:2: warning: mcp both has both a command and a url: "
            "set one of them\n"
            "/c/prospero.ini:5:2: warning: mcp none has neither a command nor a url\n"
            "/c/prospero.ini:10:9: warning: a plaintext secret: write env:NAME and "
            "set the variable, or file:PATH, so that this file can be shared\n"
            "/c/team.ini:1:2: warning: mcp shared has neither a command nor a url\n"
            "/c/team.ini:2:11: error: [mcp] command in an included file is ignored, "
            "since it chooses what runs: set it in your own file (an included file "
            "may give a server by its url)\n"
            "/c/team.ini:5:9: error: [mcp] trust in an included file is ignored, "
            "since it lets the agent use the server's tools without asking: set it "
            "in your own file\n"
            "/c/team.ini:6:7: error: [mcp] cwd in an included file is ignored, "
            "since it chooses what runs: set it in your own file (an included file "
            "may give a server by its url)\n"
            "/c/team.ini:7:9: error: [mcp] env.NAME in an included file is ignored, "
            "since it chooses what runs: set it in your own file (an included file "
            "may give a server by its url)\n"
            "/c/team.ini:11:1: error: ignored, as your own file has [mcp web], "
            "which an included file cannot change\n"
            "/c/team.ini:12:1: error: ignored, as your own file has [mcp web], "
            "which an included file cannot change\n"
            "/c/team.ini:16:7: error: [web] ask = false in an included file is "
            "ignored, since it lets the agent search and read unasked: set it in "
            "your own file");
        expect(t, "mcp shared command", cfgget(c, "mcp", "shared", "command", 0), "");
        expect(t, "mcp remote url", cfgget(c, "mcp", "remote", "url", 0), "https://team/mcp");
        expect(t, "mcp remote trust", cfgget(c, "mcp", "remote", "trust", 0), "false");
        expect(t, "mcp remote cwd", cfgget(c, "mcp", "remote", "cwd", 0), "");
        expect(t, "mcp remote tools", cfgget(c, "mcp", "remote", "tools", 0), "a b");
        Slice<CfgPair> env = cfgpairs(c, &b, "mcp", "remote", "env.");
        Slice<CfgPair> hdr = cfgpairs(c, &b, "mcp", "remote", "header.");
        expect(t, "mcp remote env", istr(&b, env.len), "0");
        expect(t, "mcp remote header", hdr.len==1 ? concat(&b, hdr[0].name, hdr[0].value)
                                                  : Str{}, "X-Kk1");
        expect(t, "mcp web sealed", cfgget(c, "mcp", "web", "url", 0), "https://x/mcp");
        hdr = cfgpairs(c, &b, "mcp", "web", "header.");
        expect(t, "mcp web sealed header", istr(&b, hdr.len), "1");
        expect(t, "web search shared", cfgget(c, "web", "", "search", 0), "searxng");
        expect(t, "web url shared", cfgget(c, "web", "", "url", 0), "https://s/");
        expect(t, "web ask shared", cfgget(c, "web", "", "ask", 0), "true");
        expect(t, "web fetch shared", cfgget(c, "web", "", "fetch", 0), "no");

        // The user's own [web] shuts out the include's
        os_write(v.os, b, "/c/own.ini",
            "include = team.ini\n"
            "[web]\n"
            "search = exa\n");
        c = newconfig(v.os, &b, v.scratch, "/c/own.ini", 0);
        expect(t, "web sealed search", cfgget(c, "web", "", "search", 0), "exa");
        expect(t, "web sealed url", cfgget(c, "web", "", "url", 0), "");
        expect(t, "web sealed fetch", cfgget(c, "web", "", "fetch", 0), "true");
    }

    // Signing in: PKCE (RFC 7636's own example), encodings, challenges
    expect(t, "mcp pkce",
           mcpchallenge(&a, "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"),
           "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    expect(t, "mcp base64url", base64url(&a, "\xfb\xff\xfe"), "-__-");
    expect(t, "mcp base64url pad", base64url(&a, "ab"), "YWI");
    expect(t, "mcp pct", pctencode(&a, "a b/c?d=é~"), "a%20b%2Fc%3Fd%3D%C3%A9~");
    expect(t, "mcp pct back", pctdecode(&a, "a%20b+c%2fd%zz%4"), "a b c/d%zz%4");
    expect(t, "mcp query", mcpquery(&a, "?code=a%2Bb&state=x+y", "code"), "a+b");
    expect(t, "mcp query 2", mcpquery(&a, "code=1&state=x+y", "state"), "x y");
    expect(t, "mcp query empty", mcpquery(&a, "code=&state=1", "code").data ? Str("set") : Str("null"), "set");
    expect(t, "mcp query none", mcpquery(&a, "code=1", "state").data ? Str("set") : Str("null"), "null");
    {
        Str h = "Bearer realm=\"x\", error=\"invalid_token\", "
                "resource_metadata=\"https://h/.well-known/oauth-protected-resource/mcp\", scope=a";
        expect(t, "mcp challenge", mcpchallengeparam(&a, h, "resource_metadata"),
               "https://h/.well-known/oauth-protected-resource/mcp");
        expect(t, "mcp challenge bare", mcpchallengeparam(&a, h, "scope"), "a");
        expect(t, "mcp challenge none", mcpchallengeparam(&a, h, "metadata").data ? Str("set") : Str("null"), "null");
    }
    expect(t, "mcp origin", urlorigin("https://h:8080/a/b?q"), "https://h:8080");
    expect(t, "mcp path", urlpath("https://h:8080/a/b/?q"), "/a/b");
    expect(t, "mcp path root", urlpath("https://h"), "");
    {
        Slice<Str> u = mcpprmurls(&a, "https://h/v1/mcp", {});
        expect(t, "mcp prm urls", istr(&a, u.len), "2");
        expect(t, "mcp prm 0", u[0], "https://h/.well-known/oauth-protected-resource/v1/mcp");
        expect(t, "mcp prm 1", u[1], "https://h/.well-known/oauth-protected-resource");
        Slice<Str> m = mcpasmurls(&a, "https://h/tenant");
        expect(t, "mcp asm 0", m[0], "https://h/.well-known/oauth-authorization-server/tenant");
        expect(t, "mcp asm 2", m[2], "https://h/tenant/.well-known/openid-configuration");
        m = mcpasmurls(&a, "https://h/");
        expect(t, "mcp asm root", m[0], "https://h/.well-known/oauth-authorization-server");
    }
}
