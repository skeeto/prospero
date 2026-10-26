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
}
