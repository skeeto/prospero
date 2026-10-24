// Door tests: a newcomer's request lines (door.cpp). The handing off
// itself, over real endpoints, is selftest_io's.

static Str doorshow(Arena *a, Str line)
{
    DoorReq r = doorparse(a, line);
    switch (r.kind) {
    case DOOR_RAISE: return concat(a, "raise:", r.token);
    case DOOR_GO:    return "go";
    }
    return "bad";
}

static void test_door(Test *t)
{
    Arena a = t->perm;
    expect(t, "door.raise", doorshow(&a, "{\"raise\":1,\"token\":\"abc_1.2-x\"}"),
           "raise:abc_1.2-x");
    expect(t, "door.notoken", doorshow(&a, "{\"raise\":1}"), "raise:");
    expect(t, "door.emptytoken", doorshow(&a, "{\"raise\":1,\"token\":\"\"}"), "raise:");
    expect(t, "door.go", doorshow(&a, "{\"go\":1}"), "go");
    expect(t, "door.space", doorshow(&a, "{\"raise\":1,\"token\":\"a b\"}"), "raise:");
    expect(t, "door.newline", doorshow(&a, "{\"raise\":1,\"token\":\"a\\nb\"}"), "raise:");
    expect(t, "door.nonascii", doorshow(&a, "{\"raise\":1,\"token\":\"\\u00e9\"}"), "raise:");
    expect(t, "door.nontext", doorshow(&a, "{\"raise\":1,\"token\":7}"), "raise:");
    Buf b(&a, 512);
    print(&b, "{\"raise\":1,\"token\":\"");
    for (i32 i = 0; i < DOOR_TOKEN+1; i++) putbyte(&b, 'x');
    print(&b, "\"}");
    expect(t, "door.long", doorshow(&a, finish(&b)), "raise:");
    expect(t, "door.zero", doorshow(&a, "{\"raise\":0}"), "bad");
    expect(t, "door.true", doorshow(&a, "{\"raise\":true}"), "bad");
    expect(t, "door.array", doorshow(&a, "[1]"), "bad");
    expect(t, "door.garbage", doorshow(&a, "GET / HTTP/1.1"), "bad");
    expect(t, "door.empty", doorshow(&a, ""), "bad");
}
