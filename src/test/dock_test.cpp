// Dock layout tests: the presets, the layout file's lines both ways,
// what is refused or dropped in reading them, and the names a saved
// layout may have.

static Str dockstr(Arena *a, DockLayout *l)
{
    Buf b(a, 512);
    dockwrite(&b, l);
    return finish(&b);
}

// A layout from its lines, as the layout file gives them, then checked.
static DockLayout dockread(Str text, b32 *ok)
{
    DockLayout l = {};
    for (Str rest = text; rest.len;) {
        Cut line = cut(rest, '\n');
        Cut kv   = cut(line.head, '=');
        rest = line.tail;
        if (kv.ok) {
            dockline(&l, kv.head, kv.tail);
        }
    }
    *ok = dockdone(&l);
    return l;
}

// Read, then written again: "!" if refused.
static Str dockagain(Arena *a, Str text)
{
    b32        ok = 0;
    DockLayout l  = dockread(text, &ok);
    return ok ? dockstr(a, &l) : Str("!");
}

static void test_dock(Test *t)
{
    Arena a = t->perm;

    // Standard is the layout the window always had
    DockLayout std = dockpreset(PRESET_STANDARD);
    expect(t, "dock.standard", dockstr(&a, &std),
           "Shown=Sessions,Transcript,Prompt,Todo,Plan,Jobs,Log\n"
           "Split=X,0.75\n"
           "Split=X,0.22\n"
           "Split=Y,0.75\n"
           "Tabs=Sessions\n"
           "Tabs=Todo\n"
           "Split=Y,0.72\n"
           "Center=Transcript\n"
           "Tabs=Prompt\n"
           "Split=Y,0.5\n"
           "Tabs=*Jobs,Log,Help,Settings,Hosts\n"
           "Tabs=Plan,Changes\n");

    // Compact keeps the left column's width, with no right column
    DockLayout compact = dockpreset(PRESET_COMPACT);
    expect(t, "dock.compact", dockstr(&a, &compact),
           "Shown=Sessions,Transcript,Prompt,Todo,Plan,Jobs\n"
           "Split=X,0.165\n"
           "Split=Y,0.75\n"
           "Tabs=Sessions\n"
           "Tabs=*Todo,Jobs,Log\n"
           "Split=Y,0.72\n"
           "Center=*Transcript,Plan,Changes,Help,Settings,Hosts\n"
           "Tabs=Prompt\n");

    // Focus shows four panes, and is home to the rest above the prompt
    DockLayout focus = dockpreset(PRESET_FOCUS);
    expect(t, "dock.focus", dockstr(&a, &focus),
           "Shown=Transcript,Prompt,Todo,Plan\n"
           "Split=Y,0.72\n"
           "Center=*Transcript,Todo,Plan,Changes,Sessions,Jobs,Log,Help,Settings,Hosts\n"
           "Tabs=Prompt\n");

    // Every preset is whole, places every pane but the metrics once,
    // and reads back as written; only Focus hides Help and the rest
    u32 all = (1u<<PANE_COUNT) - 1;
    for (i32 i = 0; i < PRESET_COUNT; i++) {
        DockLayout l    = dockpreset(i);
        Str        text = dockstr(&a, &l);
        Str        name = concat(&a, "dock.preset.", presetnames[i]);
        i32        tabs = 0;
        for (i32 p = 0; p < l.nparts; p++) {
            tabs += l.parts[p].ntabs;
        }
        expect(t, concat(&a, name, ".bad"), istr(&a, l.bad), "0");
        expect(t, concat(&a, name, ".placed"), istr(&a, dockplaced(&l)),
               istr(&a, all & ~(1u<<PANE_METRICS)));
        expect(t, concat(&a, name, ".once"), istr(&a, tabs), istr(&a, PANE_COUNT-1));
        expect(t, concat(&a, name, ".again"), dockagain(&a, text), text);
        expect(t, concat(&a, name, ".keep"), istr(&a, !!l.keep),
               i==PRESET_FOCUS ? Str("0") : Str("1"));
        b32 done = dockdone(&l);
        expect(t, concat(&a, name, ".done"), istr(&a, done), "1");
        expect(t, concat(&a, name, ".pruned"), dockstr(&a, &l), text);
    }

    // A pane floating at its place, which is kept within the dock space
    Str floats = "Shown=Transcript,Metrics,Help\n"
                 "Center=Transcript\n"
                 "Float=Metrics,0.3,0.2,0.4,0.5\n"
                 "Float=Help,0.9,-1,0.5,2\n";
    expect(t, "dock.floats", dockagain(&a, floats),
           "Shown=Transcript,Help,Metrics\n"
           "Center=Transcript\n"
           "Float=Help,0.5,0,0.5,1\n"
           "Float=Metrics,0.3,0.2,0.4,0.5\n");

    // An empty center, shares clamped, and the front marked
    expect(t, "dock.emptycenter", dockagain(&a,
               "Shown=\nSplit=X,1.5\nCenter=\nSplit=Y,-2\nTabs=Todo,*Jobs\nTabs=Log\n"),
           "Shown=\nSplit=X,0.95\nCenter=\nSplit=Y,0.05\nTabs=Todo,*Jobs\nTabs=Log\n");

    // Unknown keys and panes are skipped, a pane placed again too, and
    // a leaf left empty is dropped with its split
    expect(t, "dock.unknown", dockagain(&a,
               "Shown=Transcript,Gizmo\n"
               "Zoom=2\n"
               "Split=X,0.3\n"
               "Tabs=Gizmo,*Widget\n"
               "Split=Y,0.6\n"
               "Center=Transcript,Prompt\n"
               "Tabs=Prompt,Todo\n"
               "Float=Todo,0.1,0.1,0.5,0.5\n"
               "Float=Gizmo,0.1,0.1,0.5,0.5\n"),
           "Shown=Transcript\n"
           "Split=Y,0.6\n"
           "Center=Transcript,Prompt\n"
           "Tabs=Todo\n");
    expect(t, "dock.extrafields", dockagain(&a,
               "Split=Y,0.6,1\nCenter=Transcript\nTabs=Prompt\n"
               "Float=Help,0.1,0.2,0.3,0.4,9\n"),
           "Shown=\nSplit=Y,0.6\nCenter=Transcript\nTabs=Prompt\n"
           "Float=Help,0.1,0.2,0.3,0.4\n");

    // What is refused
    expect(t, "dock.empty", dockagain(&a, ""), "!");
    expect(t, "dock.shownonly", dockagain(&a, "Shown=Transcript\n"), "!");
    expect(t, "dock.nochild", dockagain(&a, "Split=X,0.5\nCenter=Transcript\n"), "!");
    expect(t, "dock.tworoots", dockagain(&a, "Center=Transcript\nTabs=Prompt\n"), "!");
    expect(t, "dock.nocenter", dockagain(&a, "Split=X,0.5\nTabs=Sessions\nTabs=Prompt\n"), "!");
    expect(t, "dock.twocenters", dockagain(&a, "Split=X,0.5\nCenter=Sessions\nCenter=Prompt\n"), "!");
    expect(t, "dock.axis", dockagain(&a, "Split=Z,0.5\nCenter=Sessions\nTabs=Prompt\n"), "!");
    expect(t, "dock.share", dockagain(&a, "Split=X,half\nCenter=Sessions\nTabs=Prompt\n"), "!");
    expect(t, "dock.noshare", dockagain(&a, "Split=X\nCenter=Sessions\nTabs=Prompt\n"), "!");
    expect(t, "dock.float", dockagain(&a, "Center=Sessions\nFloat=Help,0.1,0.2,0.3\n"), "!");

    // Too many parts for a layout
    Buf many(&a, 1024);
    for (i32 i = 0; i < DOCK_PARTS/2; i++) {
        print(&many, "Split=X,0.5\nTabs=\n");
    }
    print(&many, "Center=Transcript\n");
    expect(t, "dock.many", dockagain(&a, finish(&many)), "!");

    // The front of a leaf, of the panes shown
    DockPart *jobs = std.parts + 9;
    u32       log  = 1u << PANE_LOG;
    expect(t, "dock.front", istr(&a, dockfront(jobs, all)), istr(&a, PANE_JOBS));
    expect(t, "dock.fronthidden", istr(&a, dockfront(jobs, all & ~(1u<<PANE_JOBS))),
           istr(&a, PANE_LOG));
    expect(t, "dock.frontlate", istr(&a, dockfront(jobs, log | 1u<<PANE_HOSTS)),
           istr(&a, PANE_LOG));
    expect(t, "dock.frontnone", istr(&a, dockfront(jobs, 1u<<PANE_PLAN)), "-1");
    expect(t, "dock.frontfirst", istr(&a, dockfront(std.parts + 10, all)),
           istr(&a, PANE_PLAN));

    // The subtrees, as the window builds them
    expect(t, "dock.skip", istr(&a, dockskip(&std, 1)), "8");
    expect(t, "dock.skipleaf", istr(&a, dockskip(&std, 4)), "5");
    expect(t, "dock.centered", istr(&a, dockcentered(&std, 1)*10 + dockcentered(&std, 8)),
           "10");

    // Names
    expect(t, "dock.name", docknamewhy("Review 2"), "");
    expect(t, "dock.nameempty", docknamewhy(trim(Str("   "))), "A layout needs a name");
    expect(t, "dock.namepreset", docknamewhy("standard"), "A built-in layout has that name");
    expect(t, "dock.namefocus", docknamewhy("FOCUS"), "A built-in layout has that name");
    expect(t, "dock.namecontrol", docknamewhy("a\tb"), "A name cannot have control characters");
    expect(t, "dock.namehash", docknamewhy("C## notes"), "A name cannot have ##");
    expect(t, "dock.namehash1", docknamewhy("C# notes"), "");
    Str long64 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    expect(t, "dock.name64", docknamewhy(long64), "");
    expect(t, "dock.name65", docknamewhy(concat(&a, long64, "x")), "A name is at most 64 bytes");
}
