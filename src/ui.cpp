// ImGui user interface: the transcript widget with its tool rows, ask
// cards and compaction and rewind cards, the session's bar and info, the
// sessions sidebar with subagents under their parents and each host's
// sessions under it, the todo list and background jobs panels, the
// liveness strip and the prompt, the settings window, the Hosts panel,
// the Output window, the new session, rewind and close dialogs, the
// menu bar and the dock layout of panes it shows and hides, the metrics
// overlay, the theme's colors, and the scheduler's view of ImGui
//
// Tool rows carry an action at the right of their header: Undo on a
// write's or edit's (a rewind from its call on), Rewind to here on the
// message that began a turn, a link to a task's subagent, and Output on
// a call with a spill file, which the Output window shows as its host
// gives it. A rewind asks first, listing the files as the view's
// snapshots predict them.
//
// A host's countdown to its next try, like the strip's clocks, declares
// its deadline only while drawn.
//
// All ImDrawList code lives here, since its signatures shift between
// ImGui releases. uiscan and uideadlines read ImGui's internals after
// each frame to tell the scheduler what it is waiting on, and drawhash
// fingerprints the frame's draw data.
//
// Every color comes from the palette in effect (g->pal, theme.cpp), and
// ImGui's own from its stock style for the palette with a few replaced
// (themestyle). A change of theme recolors both at once.
//
// Nothing here may make a frame differ from the last without declaring
// why: a value drawn from the clock needs a deadline for when it next
// changes, or the staleness probe reports it. The strip's clocks and an
// ask's countdown declare theirs only while drawn, so a session out of
// sight costs no frames.
//
// This is free and unencumbered software released into the public domain.

static ImVec2 v2(f32 x, f32 y)
{
    ImVec2 r;
    r.x = x;
    r.y = y;
    return r;
}

static char *cstr0(Arena *a, Str s)  // null-terminated copy for ImGui
{
    char *r = alloc<char>(a, s.len+1);
    copybytes(r, s.data, s.len);
    return r;
}

static void text(Str s)
{
    ImGui::TextUnformatted(s.cdata, s.cdata+s.len);
}

// Push an ID made from a string, which may be empty.
static void pushstr(Str s)
{
    if (s.len) {
        ImGui::PushID(s.cdata, s.cdata+s.len);
    } else {
        ImGui::PushID("##empty");
    }
}

// Remember where the last item went, for the scripted checklist.
static void itemrect(Gui *g, i32 which)
{
    ImVec2 a = ImGui::GetItemRectMin();
    ImVec2 z = ImGui::GetItemRectMax();
    g->rects[which] = {a.x, a.y, z.x, z.y};
}

static b32 checkbox(char const *label, b32 *v)
{
    bool b = *v;
    b32  r = ImGui::Checkbox(label, &b);
    *v = b;
    return r;
}


// Fonts and measurement

static f32 stylesize(Fonts *f, i32 style)
{
    f32 base = f->size;
    switch ((style & STYLE_HMASK) >> STYLE_HSHIFT) {
    case 1: return base * 1.60f;
    case 2: return base * 1.35f;
    case 3: return base * 1.17f;
    case 4: return base * 1.05f;
    }
    return base;
}

static ImFont *stylefont(Fonts *f, i32 style)
{
    b32 bold   = style&(STYLE_BOLD|STYLE_HEADER|STYLE_HMASK);
    b32 italic = style&STYLE_ITALIC;
    if (style & STYLE_CODE) {
        return bold ? f->monobold : f->mono;
    }
    return bold&&italic ? f->bolditalic : bold ? f->bold :
           italic ? f->italic : f->prose;
}

// Baked fonts per style, looked up once per frame: switching sizes
// otherwise costs a hash lookup per character. Slots are only valid in
// the frame that filled them, since ImGui may discard unused bakes.
static ImFontBaked *stylebaked(Fonts *f, i32 style)
{
    i32 key = style & (STYLE_BOLD|STYLE_ITALIC|STYLE_CODE|STYLE_HEADER|STYLE_HMASK);
    u32 i   = (u32)key*0x9e3779b1u >> 27;
    for (i32 n = 0; n < countof(f->baked); n++, i = (i+1)%countof(f->baked)) {
        BakedSlot *s = &f->baked[i];
        if (s->gen==f->gen && s->key==key) {
            return s->baked;
        } else if (s->gen != f->gen) {
            s->gen   = f->gen;
            s->key   = key;
            s->baked = stylefont(f, style)->GetFontBaked(stylesize(f, style));
            return s->baked;
        }
    }
    return stylefont(f, style)->GetFontBaked(stylesize(f, style));  // full
}

static f32 fontadvance(void *ctx, i32 style, i32 cp)
{
    Fonts *f = (Fonts *)ctx;
    return stylebaked(f, style)->GetCharAdvance((ImWchar)cp);
}

static FontMetrics fontmetrics(void *ctx, i32 style)
{
    Fonts      *f = (Fonts *)ctx;
    FontMetrics r = {};
    f32 size = stylesize(f, style);
    r.lineh  = (f32)(i32)(size*1.3f + 0.5f);
    r.ascent = (f32)(i32)(stylebaked(f, style)->Ascent + (r.lineh - size)/2 + 0.5f);
    return r;
}


// Draw-data hashing

// Hash everything that reaches the screen: sizes, vertices, indices,
// and each command's clip rectangle, texture and ranges. Per-list
// hashes, by owner window, explain a stale frame.
static u64 drawhash(ImDrawData *dd, ListHash *lists, i32 *nlists, i32 cap,
                    u64 *header)
{
    Hash64 h = {};
    f32 hdr[] = {
        dd->DisplayPos.x, dd->DisplayPos.y, dd->DisplaySize.x, dd->DisplaySize.y,
        dd->FramebufferScale.x, dd->FramebufferScale.y,
    };
    hashbytes(&h, hdr, sizeof(hdr));
    Hash64 hh = {};
    hashbytes(&hh, hdr, sizeof(hdr));
    *header = hashfinal(&hh);
    *nlists = 0;
    for (i32 i = 0; i < dd->CmdLists.Size; i++) {
        ImDrawList *dl = dd->CmdLists[i];
        Hash64 lh = {};
        hashbytes(&lh, dl->VtxBuffer.Data, (iz)dl->VtxBuffer.Size*(iz)sizeof(ImDrawVert));
        hashbytes(&lh, dl->IdxBuffer.Data, (iz)dl->IdxBuffer.Size*(iz)sizeof(ImDrawIdx));
        for (i32 c = 0; c < dl->CmdBuffer.Size; c++) {
            ImDrawCmd *cmd = &dl->CmdBuffer[c];
            hashbytes(&lh, &cmd->ClipRect, sizeof(cmd->ClipRect));
            hashword(&lh, (u64)(uz)cmd->TexRef._TexData);
            hashword(&lh, (u64)cmd->TexRef._TexID);
            hashword(&lh, (u64)cmd->ElemCount<<32 | cmd->VtxOffset);
            hashword(&lh, (u64)cmd->IdxOffset);
            hashword(&lh, (u64)(uz)cmd->UserCallback);
        }
        u64 lv = hashfinal(&lh);
        hashword(&h, lv);
        if (*nlists < cap) {
            lists[*nlists].owner = dl->_OwnerName;
            lists[*nlists].hash  = lv;
            (*nlists)++;
        }
    }
    return hashfinal(&h);
}


// The scheduler's view of ImGui

static b32 modifierkey(i32 k)
{
    return (k>=ImGuiKey_LeftCtrl && k<=ImGuiKey_RightSuper) ||
           (k>=ImGuiKey_ReservedForModCtrl && k<=ImGuiKey_ReservedForModSuper);
}

// A window's content size as the next frame will compute it from this
// one's layout, as ImGui's CalcWindowContentSizes does.
static b32 contentmoves(ImGuiWindow *w)
{
    if (w->Collapsed || w->Hidden) {
        return 0;
    }
    f32 x = w->ContentSizeExplicit.x!=0 ? w->ContentSizeExplicit.x
          : (f32)(i64)(w->DC.CursorMaxPos.x - w->DC.CursorStartPos.x);
    f32 y = w->ContentSizeExplicit.y!=0 ? w->ContentSizeExplicit.y
          : (f32)(i64)(w->DC.CursorMaxPos.y - w->DC.CursorStartPos.y);
    return x!=w->ContentSize.x || y!=w->ContentSize.y;
}

// Reasons the frame just rendered needs a successor.
static u32 uiscan()
{
    ImGuiContext &c  = *GImGui;
    ImGuiIO      &io = c.IO;
    u32 busy = 0;
    for (i32 i = 0; i < 5; i++) {
        busy |= io.MouseDown[i] ? (u32)BUSY_MOUSE : 0;
    }
    for (i32 k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
        if (k>=ImGuiKey_MouseLeft && k<=ImGuiKey_MouseWheelY) continue;
        if (!modifierkey(k) && ImGui::IsKeyDown((ImGuiKey)k)) {
            busy |= BUSY_KEY;
            break;
        }
    }
    if (c.DimBgRatio>0 && c.DimBgRatio<1) busy |= BUSY_DIM;
    if (c.NavWindowingTarget) busy |= BUSY_NAVWIN;
    if (c.NavWindowingTargetAnim && c.NavWindowingHighlightAlpha>0) busy |= BUSY_NAVWIN;
    if (c.DragDropActive) busy |= BUSY_DRAG;
    if (c.InputEventsQueue.Size) busy |= BUSY_QUEUE;
    if (c.DockContext.Requests.Size) busy |= BUSY_DOCK;
    for (i32 i = 0; i < c.Windows.Size; i++) {
        ImGuiWindow *w = c.Windows[i];
        if (!w->Active && !w->WasActive) continue;
        // A window measuring itself before it appears (new windows,
        // reopened popups and tooltips, a tab about to be selected) is
        // drawn next frame. Windows hidden for good (other dock tabs,
        // clipped children) and auto-fitting ones are not pending:
        // the latter change the hash as they go.
        if (w->HiddenFramesCannotSkipItems>0 || w->HiddenFramesForRenderOnly>0) {
            busy |= BUSY_HIDDEN;
        }
        if (w->ScrollTarget.x<FLT_MAX || w->ScrollTarget.y<FLT_MAX) {
            busy |= BUSY_SCROLL;
        }
        // Content grown or shrunk this frame shows in the scrollbar only
        // next frame, even when nothing visible moved now: a line added
        // below a scrolled window's bottom edge.
        if (w->Active && contentmoves(w)) {
            busy |= BUSY_LAYOUT;
        }
    }
    for (i32 i = 0; i < c.TabBars.GetMapSize(); i++) {
        ImGuiTabBar *tb = c.TabBars.TryGetMapData(i);
        if (tb && (tb->ScrollingAnim!=tb->ScrollingTarget || tb->ReorderRequestTabId)) {
            busy |= BUSY_TABS;
        }
    }
    return busy;
}

// Timers ImGui runs without any event: tooltips, the caret, the
// settings save. Declares a deadline for each pending one.
static void uideadlines(Gui *g, Sched *s)
{
    ImGuiContext &c  = *GImGui;
    ImGuiIO      &io = c.IO;
    ImGuiStyle   &st = c.Style;
    i64 now = s->now;

    // Hover delays restart whenever the mouse moves, or whenever what is
    // under it changes without moving it (a resize, a scroll): splitters
    // light up after WINDOWS_RESIZE_FROM_EDGES_FEEDBACK_TIMER, 0.04 s.
    if (io.MouseDelta.x!=0 || io.MouseDelta.y!=0 || c.HoveredId!=g->lasthover) {
        g->lastmove  = now;
        g->lasthover = c.HoveredId;
    }
    if (g->lastmove) {
        i64 slack = 20000;
        f32 delays[] = {
            0.04f,
            st.HoverStationaryDelay,
            st.HoverStationaryDelay + st.HoverDelayShort,
            st.HoverStationaryDelay + st.HoverDelayNormal,
        };
        for (iz i = 0; i < countof(delays); i++) {
            i64 t = g->lastmove + (i64)(delays[i]*1e6f) + slack;
            if (t > now) {
                schedat(s, t, i>1 ? DUE_TOOLTIP : DUE_HOVER);
            }
        }
    }

    // The caret blinks: on for 0.8 s, off for 0.4 s, after 0.3 s held on
    // following each edit. Blinking stops 10 s after the last key.
    if (c.ActiveId && c.InputTextState.ID==c.ActiveId && io.ConfigInputTextCursorBlink) {
        f32 a  = c.InputTextState.CursorAnim;
        f32 dt = 0;
        if (a < 0) {
            dt = 0.8f - a;
        } else {
            f32 r = ImFmod(a, 1.2f);
            dt = r<0.8f ? 0.8f-r : 1.2f-r;
        }
        schedat(s, now + (i64)(dt*1e6f) + 5000, DUE_CARET);
        schedat(s, g->lastkey + CARET_STOP, DUE_CARET);
    }

    if (c.SettingsDirtyTimer > 0) {
        schedat(s, now + (i64)(c.SettingsDirtyTimer*1e6f) + 1000, DUE_SETTINGS);
    }
}


// Themes

// ImGui's colors for a palette: its stock dark or light style, with the
// palette's panels, text, fields, selected rows and bars.
static void themestyle(ImGuiStyle *st, Palette *p)
{
    if (p->dark) {
        ImGui::StyleColorsDark(st);
    } else {
        ImGui::StyleColorsLight(st);
    }
    ImVec4 *c = st->Colors;
    c[ImGuiCol_WindowBg]      = ImGui::ColorConvertU32ToFloat4(p->panel);
    c[ImGuiCol_Text]          = ImGui::ColorConvertU32ToFloat4(p->paneltext);
    c[ImGuiCol_TextDisabled]  = ImGui::ColorConvertU32ToFloat4(p->paneldim);
    c[ImGuiCol_FrameBg]       = ImGui::ColorConvertU32ToFloat4(p->frame);
    c[ImGuiCol_Header]        = ImGui::ColorConvertU32ToFloat4(p->selrow);
    c[ImGuiCol_HeaderHovered] = ImGui::ColorConvertU32ToFloat4(p->rowhover);
    c[ImGuiCol_HeaderActive]  = ImGui::ColorConvertU32ToFloat4(p->rowpress);
    c[ImGuiCol_TabHovered]    = ImGui::ColorConvertU32ToFloat4(p->rowhover);
    c[ImGuiCol_Button]        = ImGui::ColorConvertU32ToFloat4(p->btn);
    c[ImGuiCol_ButtonHovered] = ImGui::ColorConvertU32ToFloat4(p->btnhover);
    c[ImGuiCol_ButtonActive]  = ImGui::ColorConvertU32ToFloat4(p->btnpress);
    c[ImGuiCol_PlotHistogram] = ImGui::ColorConvertU32ToFloat4(p->bar);
    c[ImGuiCol_TextLink]      = ImGui::ColorConvertU32ToFloat4(p->link);
    c[ImGuiCol_PopupBg]       = ImGui::ColorConvertU32ToFloat4(p->panel);
}

// Put the theme in effect: the one configured, or the system's. A change
// recolors everything in one presented frame, and the window's frame
// follows where the platform can make it do so; no change, no frame.
static void applytheme(Gui *g)
{
    Palette *p = themepalette(g->themeset, g->sysdark);
    if (p == g->pal) {
        return;
    }
    g->pal = p;
    themestyle(&g->basestyle, p);  // where a new DPI scale starts
    ImGuiStyle &st = ImGui::GetStyle();
    for (i32 i = 0; i < ImGuiCol_COUNT; i++) {
        st.Colors[i] = g->basestyle.Colors[i];
    }
    g->titleok = os_guitheme(g->os, p->dark);
    g->woke |= WOKE_FORCE;
    schedforce(&g->sched);
}

// The configured theme: THEME_*, or -1 for an unknown name, which means
// the system's.
static void settheme(Gui *g, i32 theme)
{
    g->themeset = theme<0 ? THEME_SYSTEM : theme;
    applytheme(g);
}

// Show reasoning as [ui] thinking says: THINK_*, or -1 for an unknown
// name, which means fold. Every view at once, in one frame.
static void setthinking(Gui *g, i32 mode)
{
    mode = mode<0 ? THINK_FOLD : mode;
    b32 changed = g->thinkset != mode;
    g->thinkset = mode;
    for (GuiSession *s = g->sessions; s; s = s->next) {
        changed |= s->view && viewthink(s->view, mode);
    }
    if (changed) {
        g->thinkhidshown = 0;
        g->woke |= WOKE_FORCE;
        schedforce(&g->sched);
    }
}

// Fold tool rows as [ui] tools says: TOOLS_*, or -1 for an unknown name,
// which means auto. Every view at once, in one frame.
static void settools(Gui *g, i32 mode)
{
    mode = mode<0 ? TOOLS_AUTO : mode;
    b32 changed = g->toolset != mode;
    g->toolset = mode;
    for (GuiSession *s = g->sessions; s; s = s->next) {
        changed |= s->view && viewtools(s->view, mode);
    }
    if (changed) {
        g->woke |= WOKE_FORCE;
        schedforce(&g->sched);
    }
}

// A session's own toggles, which the session bar, their shortcuts and the
// View menu work: every tool row open, or all reasoning shown, whatever
// the settings say, until turned off again, when the settings fold them
// as before but for rows the user opened or closed. The configuration is
// left alone. A null session is the one shown.
static void setsessiontools(Gui *g, GuiSession *s, b32 on)
{
    s = s ? s : g->sess;
    if (s && s->view && viewshowtools(s->view, on)) {
        scheddirty(&g->sched);
    }
}

static void setsessionthink(Gui *g, GuiSession *s, b32 on)
{
    s = s ? s : g->sess;
    if (s && s->view && viewshowthink(s->view, on)) {
        g->thinkhidshown = 0;
        scheddirty(&g->sched);
    }
}

// Whether the toggles are on, for a menu's check marks.
static b32 sessiontools(Gui *g)
{
    return g->sess && g->sess->view && g->sess->view->showtools;
}

static b32 sessionthink(Gui *g)
{
    return g->sess && g->sess->view && g->sess->view->showthink;
}

// Find in the transcript (Cmd/Ctrl+F, and Edit > Find): the find bar
// opens over the transcript with its field focused, its text selected.
static void gui_find_open(Gui *g)
{
    setpane(g, PANE_TRANSCRIPT, 1);
    g->findopen  = 1;
    g->findfocus = 1;
    scheddirty(&g->sched);
}

static void gui_find_close(Gui *g)
{
    g->findopen   = 0;
    g->findactive = 0;
    scheddirty(&g->sched);
}

// The next match (dir 1) or the previous (-1), wrapping around, shown
// and opened if folded.
static void gui_find_next(Gui *g, i32 dir)
{
    ViewSearch *s = g->search;
    iz          n = s->hits.len;
    if (!g->findopen || !n || s->v!=g->view) {
        return;
    }
    iz cur = s->cur<0 ? (dir>0 ? 0 : n-1) : (s->cur + dir + n) % n;
    s->cur      = (i32)cur;
    g->findjump = 1;
    g->findwant = 0;
    searchreveal(s, &s->hits[cur]);
    scheddirty(&g->sched);
}


// Transcript

// A color at a fraction of its opacity.
static u32 fade(u32 col, f32 alpha)
{
    u32 a = (u32)((f32)(col >> IM_COL32_A_SHIFT & 0xff) * alpha);
    return (col & ~IM_COL32_A_MASK) | a<<IM_COL32_A_SHIFT;
}

// A piece's color, in a code block or elsewhere.
static u32 piececolor(Palette *pal, Piece *p, b32 codeblock)
{
    if (p->hl) {
        return pal->hl[p->hl];
    } else if (codeblock) {
        return pal->codetext;
    } else if (p->style & STYLE_LINK) {
        return pal->link;
    } else if (p->style & STYLE_DIM) {
        return pal->reasoning;
    } else if (p->style & STYLE_CODE) {
        return pal->code;
    }
    return pal->text;
}

static Str rolename(i32 role)
{
    switch (role) {
    case VIEW_USER:      return "You";
    case VIEW_ASSISTANT: return "Agent";
    case VIEW_THINK:     return "Reasoning";
    case VIEW_TOOL:      return "Tool";
    case VIEW_NOTE:      return "Note";
    case VIEW_ASK:       return "Permission";
    }
    return "";
}

static u32 toolcolor(Palette *p, i32 status)
{
    switch (status) {
    case VTOOL_OK:         return p->ok;
    case VTOOL_ERROR:
    case VTOOL_TIMEOUT:
    case VTOOL_UNKNOWN:    return p->bad;
    case VTOOL_DENIED:
    case VTOOL_ASKING:     return p->amber;
    case VTOOL_RUNNING:
    case VTOOL_STREAMING:
    case VTOOL_BACKGROUND: return p->active;
    }
    return p->dim;
}

static u32 livecolor(Palette *p, i32 state)
{
    switch (state) {
    case LIVE_IDLE:
    case LIVE_CLOSED:  return p->dim;
    case LIVE_ASK:
    case LIVE_QUEUED:
    case LIVE_BACKOFF: return p->amber;
    case LIVE_ERROR:   return p->bad;
    }
    return p->active;
}

static u32 jobcolor(Palette *p, ViewJob *j)
{
    switch (j->status) {
    case VJOB_RUNNING: return p->active;
    case VJOB_EXITED:  return j->hasexit && !j->exit ? p->ok : p->bad;
    case VJOB_KILLED:
    case VJOB_CLOSED:  return p->amber;
    }
    return p->bad;
}

// What a session is doing, in a word or two: "working" for a turn
// between states, "subagents" while its own wait for them.
static Str livelabel(View *v, i32 state)
{
    if (v && v->busy && state==LIVE_IDLE) {
        return "working";
    }
    return state==LIVE_TASK ? Str("subagents") : livenames[state];
}

// A whole number with thousands separated: 12,345.
static void printgrouped(Buf *b, i64 n)
{
    if (n < 0) {
        putbyte(b, '-');
        n = -n;
    }
    u8 digits[24];
    i32 len = 0;
    do {
        digits[len++] = (u8)('0' + n%10);
        n /= 10;
    } while (n);
    for (i32 i = len-1; i >= 0; i--) {
        putbyte(b, digits[i]);
        if (i && i%3==0) putbyte(b, ',');
    }
}

// Seconds as "45 s" or "4 min 5 s".
static Str secstext(Arena *a, i64 secs)
{
    Buf b(a, 32);
    if (secs >= 60) {
        print(&b, secs/60);
        print(&b, " min");
        if (secs % 60) {
            putbyte(&b, ' ');
            print(&b, secs%60);
            print(&b, " s");
        }
    } else {
        print(&b, secs);
        print(&b, " s");
    }
    return finish(&b);
}

// A state's badge: a dot in its color.
static void drawdot(ImDrawList *dl, ImVec2 c, f32 r, u32 col)
{
    dl->AddCircleFilled(c, r, col, 12);
}

enum : i32 {
    CH_BG,    // code and header backgrounds
    CH_SEL,   // selection highlight
    CH_TEXT,  // text, markers, rules
    CH_COUNT,
};

// A message's find hits, in order, and which of them is current, -1
// if none.
struct Marks {
    FindHit *h;
    iz       n;
    iz       cur;
};

// The part of a run a message's hits cover, highlighted behind its text:
// the current hit stronger.
static void drawmarks(Gui *g, ImDrawList *dl, Marks *mk, Piece *pieces, Run *r,
                      f32 ox, f32 ly, f32 lh)
{
    i32 rb = runbeg(pieces, r), re = runend(pieces, r);
    if (rb == re) {
        return;
    }
    iz lo = 0, hi = mk->n;
    while (lo < hi) {
        iz mid = lo + (hi - lo)/2;
        if (mk->h[mid].off+mk->h[mid].len <= rb) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    for (iz i = lo; i<mk->n && mk->h[i].off<re; i++) {
        FindHit *h  = &mk->h[i];
        f32      x0 = runx(&g->measure, pieces, r, h->off>rb ? h->off : rb);
        f32      x1 = runx(&g->measure, pieces, r, h->off+h->len);
        f32      a  = i==mk->cur ? 0.75f : 0.3f;
        dl->AddRectFilled(v2(ox+x0, ly), v2(ox+(x1>x0+1 ? x1 : x0+1), ly+lh),
                          fade(g->pal->amber, a));
    }
}

// Draw one placed block of a message whose content starts at origin.
static void drawblock(Gui *g, ImDrawList *dl, ImDrawListSplitter *sp,
                      Doc *d, i32 msg, Placed *p, ImVec2 origin, Range sel,
                      Marks *mk, f32 cliptop, f32 clipbot, f32 alpha)
{
    BlockLayout *bl     = p->bl;
    DocBlock    *b      = &d->blocks[p->block];
    Piece       *pieces = d->pieces.data + b->piece0;
    f32          ox     = origin.x;
    f32          oy     = origin.y + p->y;
    Fonts       *f      = &g->fonts;
    Palette     *pal    = g->pal;
    b32          code   = b->type == DOC_CODE;

    for (iz i = 0; i < bl->decos.len; i++) {
        Deco *dc = &bl->decos[i];
        ImVec2 a = v2(ox+dc->x, oy+dc->y);
        ImVec2 z = v2(ox+dc->x+dc->w, oy+dc->y+dc->h);
        switch (dc->kind) {
        case DECO_CODEBG:
            sp->SetCurrentChannel(dl, CH_BG);
            dl->AddRectFilled(a, z, fade(pal->codebg, alpha), 4*g->scale);
            break;
        case DECO_HEADERBG:
            sp->SetCurrentChannel(dl, CH_BG);
            dl->AddRectFilled(a, z, fade(pal->headerbg, alpha));
            break;
        case DECO_QUOTE:
            sp->SetCurrentChannel(dl, CH_TEXT);
            dl->AddRectFilled(a, z, fade(pal->quote, alpha));
            break;
        case DECO_RULE:
        case DECO_GRID:
            sp->SetCurrentChannel(dl, CH_TEXT);
            dl->AddRectFilled(a, v2(z.x>a.x+1 ? z.x : a.x+1, z.y>a.y+1 ? z.y : a.y+1),
                              fade(pal->rule, alpha));
            break;
        case DECO_MARKER: {
            sp->SetCurrentChannel(dl, CH_TEXT);
            ImFontBaked *bk = stylebaked(f, 0);
            ImVec2 at = v2(ox+dc->x, oy+dc->y - bk->Ascent);
            dl->AddText(stylefont(f, 0), stylesize(f, 0), at,
                        fade(pal->marker, alpha), dc->text.cdata,
                        dc->text.cdata+dc->text.len);
        } break;
        }
    }

    b32 selected = !poseq(sel.beg, sel.end) && sel.beg.msg<=msg && msg<=sel.end.msg;
    i32 s0 = sel.beg.msg<msg ? 0 : sel.beg.off;
    i32 s1 = sel.end.msg>msg ? 0x7fffffff : sel.end.off;
    for (iz li = 0; li < bl->lines.len; li++) {
        Line *l  = &bl->lines[li];
        f32   ly = oy + l->y;
        if (ly+l->h<cliptop || ly>clipbot) {
            continue;
        }
        for (i32 k = 0; k < l->nrun; k++) {
            Run   *r  = &bl->runs[l->run0+k];
            Piece *pc = &pieces[r->piece];
            f32    x0 = 0, x1 = 0;  // the run's selected part, if any
            if (mk->n) {
                sp->SetCurrentChannel(dl, CH_SEL);
                drawmarks(g, dl, mk, pieces, r, ox, ly, l->h);
            }
            if (selected) {
                i32 rb = runbeg(pieces, r), re = runend(pieces, r);
                b32 synth = rb == re;
                if ((!synth && rb<s1 && re>s0) || (synth && rb>=s0 && rb<s1)) {
                    x0 = runx(&g->measure, pieces, r, s0);
                    x1 = runx(&g->measure, pieces, r, s1);
                    if (synth) {
                        x0 = r->x;
                        x1 = r->x + (r->w>0 ? r->w : 4*g->scale);
                    }
                    sp->SetCurrentChannel(dl, CH_SEL);
                    dl->AddRectFilled(v2(ox+x0, ly), v2(ox+x1, ly+l->h), pal->sel);
                }
            }
            if (r->flags & RUN_NODRAW) {
                continue;
            }
            ImFontBaked *bk   = stylebaked(f, pc->style);
            ImVec2       at   = v2(ox + r->x, ly + l->base - bk->Ascent);
            char const  *beg  = pc->text.cdata + r->b0;
            char const  *end  = pc->text.cdata + r->b1;
            u32          col  = fade(piececolor(pal, pc, code), alpha);
            ImFont      *font = stylefont(f, pc->style);
            f32          size = stylesize(f, pc->style);
            sp->SetCurrentChannel(dl, CH_TEXT);
            if (x1 > x0) {
                // The selected part in a color of its own, which reads on
                // the selection, as the run's own color may not: the run
                // drawn in pieces, each clipped to its part
                f32    top = ly - l->h, bot = ly + 2*l->h;
                ImVec4 lo  = ImVec4(ox+r->x-l->h, top, ox+x0, bot);
                ImVec4 mid = ImVec4(ox+x0, top, ox+x1, bot);
                ImVec4 hi  = ImVec4(ox+x1, top, ox+r->x+r->w+l->h, bot);
                dl->AddText(font, size, at, col, beg, end, 0, &lo);
                dl->AddText(font, size, at, fade(pal->seltext, alpha), beg, end, 0, &mid);
                dl->AddText(font, size, at, col, beg, end, 0, &hi);
            } else {
                dl->AddText(font, size, at, col, beg, end);
            }
            if (pc->style & (STYLE_STRIKE|STYLE_LINK)) {
                f32 y = pc->style&STYLE_STRIKE ? ly + l->base - bk->Ascent*0.35f
                                               : ly + l->base + 1;
                dl->AddLine(v2(ox+r->x, y), v2(ox+r->x+r->w, y), col, 1);
            }
        }
    }
}

// Copy the selection to the clipboard.
static void copyout(Gui *g, i32 how)
{
    Arena a = g->frame;
    Str   s = copysel(g->view, &a, how);
    if (s.len) {
        ImGui::SetClipboardText(cstr0(&a, s));
    }
}

static void copymessage(Gui *g, i32 msg)
{
    Arena a = g->frame;
    ImGui::SetClipboardText(cstr0(&a, g->view->msgs[msg].text));
}

// Text drawn left to right along a line, clipped at its right end.
struct Pen {
    ImDrawList *dl;
    Fonts      *f;
    f32         x, y;
    f32         right;
    f32         size;
};

static void pen(Pen *p, Str s, i32 style, u32 col)
{
    if (!s.len || p->x>=p->right) {
        return;
    }
    ImFont *font = stylefont(p->f, style);
    ImVec4  clip = ImVec4(p->x, p->y, p->right, p->y + p->size*2);
    p->dl->AddText(font, p->size, v2(p->x, p->y), col, s.cdata, s.cdata+s.len, 0, &clip);
    p->x += font->CalcTextSizeA(p->size, 1e9f, 0, s.cdata, s.cdata+s.len).x;
}

// Milliseconds as "850 ms", "1.2 s" or "3 min 5 s".
static Str duration(Arena *a, i64 ms)
{
    Buf b(a, 32);
    if (ms < 1000) {
        print(&b, ms);
        print(&b, " ms");
    } else if (ms < 60000) {
        printfloat(&b, (f64)ms/1000, 1);
        print(&b, " s");
    } else {
        print(&b, ms/60000);
        print(&b, " min ");
        print(&b, ms/1000%60);
        print(&b, " s");
    }
    return finish(&b);
}

static Str bytesize(Arena *a, i64 n)
{
    Buf b(a, 32);
    if (n < 1024) {
        print(&b, n);
        print(&b, " B");
    } else if (n < (i64)1<<20) {
        printfloat(&b, (f64)n/1024, 1);
        print(&b, " KiB");
    } else {
        printfloat(&b, (f64)n/(1<<20), 1);
        print(&b, " MiB");
    }
    return finish(&b);
}

// A count as "523", "1.2k", "48k" or "1.3M".
static void printcount(Buf *b, i64 n)
{
    if (n < 1000) {
        print(b, n);
    } else if (n < 10000) {
        printfloat(b, (f64)n/1000, 1);
        putbyte(b, 'k');
    } else if (n < 1000000) {
        print(b, n/1000);
        putbyte(b, 'k');
    } else {
        printfloat(b, (f64)n/1000000, 1);
        putbyte(b, 'M');
    }
}

// Whole seconds left until an epoch-ms time, rounded up, and a deadline
// at the next change of that number, only while it is drawn.
static i64 countdown(Gui *g, i64 at, i64 nowms)
{
    i64 left = at - nowms;
    if (left <= 0) {
        return 0;
    }
    i64 secs = (left + 999) / 1000;
    i64 next = left - (secs - 1)*1000;  // ms until secs drops
    schedat(&g->sched, g->sched.now + next*1000 + 1000, DUE_CLOCK);
    return secs;
}

// An age in milliseconds, as printcfgage prints it, and a deadline at
// the next change of the number printed, only while it is drawn.
static void printage(Gui *g, Buf *b, i64 ms)
{
    ms = ms<0 ? 0 : ms;
    i64 unit = ms<60*1000 ? 1000 : ms<3600*1000 ? 60*1000 :
               ms<86400*1000 ? 3600*1000 : 86400*1000;
    printcfgage(b, ms);
    schedat(&g->sched, g->sched.now + (unit - ms%unit)*1000 + 1000, DUE_CLOCK);
}

static GuiSession *taskchild(Gui *, GuiSession *, ViewTool *);

// A message's header line: who, or what a tool did, or an ask's state.
static void drawheader(Gui *g, Pen *p, View *v, ViewMsg *m, i64 nowms)
{
    Arena     a   = g->frame;
    ViewTool *t   = m->tool;
    b32       cl  = m->fold || m->role==VIEW_TOOL || m->role==VIEW_THINK;
    f32       sz  = p->size;
    Palette  *pal = g->pal;
    if (cl) {
        // A triangle for whether the body shows
        f32    fs = ImGui::GetFontSize();
        ImVec2 at = v2(p->x, p->y + (sz - 0.7f*fs)/2);
        ImGui::RenderArrow(p->dl, at, pal->dim, m->collapsed ? ImGuiDir_Right : ImGuiDir_Down, 0.7f);
        p->x += fs;
    }
    u32 dim = fade(pal->dim, m->faded ? 0.5f : 1);
    switch (m->role) {
    case VIEW_TOOL: {
        pen(p, t->name.len ? t->name : Str("call"), STYLE_BOLD, dim);
        pen(p, "  ", 0, dim);
        if (t->status == VTOOL_STREAMING) {
            Buf b(&a, 64);
            print(&b, "writing arguments, ");
            print(&b, bytesize(&a, t->args.len));
            pen(p, finish(&b), 0, pal->active);
            break;
        }
        // The outcome first, so that a long summary cannot hide it
        pen(p, vtoolnames[t->status], STYLE_BOLD, toolcolor(pal, t->status));
        if (t->hasexit) {
            Buf b(&a, 32);
            print(&b, "  exit ");
            print(&b, (i64)t->exit);
            pen(p, finish(&b), 0, t->exit ? pal->bad : dim);
        }
        if (t->status==VTOOL_RUNNING && t->outbytes) {
            pen(p, "  ", 0, dim);
            pen(p, bytesize(&a, t->outbytes), 0, dim);
        } else if (t->status>=VTOOL_OK && (t->ms || t->status!=VTOOL_UNKNOWN)) {
            pen(p, "  ", 0, dim);
            pen(p, duration(&a, t->ms), 0, dim);
        }
        // What became of it since: its job, its change undone, its
        // output no longer shown to the agent
        if (t->job) {
            Buf b(&a, 32);
            print(&b, "  job ");
            print(&b, t->job);
            pen(p, finish(&b), STYLE_BOLD, pal->active);
        }
        ViewSnap *sn = rowsnap(v, t);
        if (sn && sn->undone) {
            pen(p, "  undone", STYLE_BOLD, pal->amber);
        }
        if (t->elided) {
            pen(p, "  elided", STYLE_BOLD, dim);
        }
        if (t->name=="task" && g->sess) {
            GuiSession *k = taskchild(g, g->sess, t);
            if (k && k->view && k->view->asks) {
                pen(p, "  needs an answer", STYLE_BOLD, pal->amber);
            }
        }
        pen(p, "  ", 0, dim);
        pen(p, t->summary, STYLE_CODE, pal->summary);
    } break;
    case VIEW_COMPACT: {
        ViewCard *c = m->card;
        Buf b(&a, 128);
        pen(p, "Compacted", STYLE_BOLD, dim);
        print(&b, "  ");
        printgrouped(&b, c->before);
        print(&b, " to ");
        printgrouped(&b, c->after);
        print(&b, " tokens");
        pen(p, finish(&b), STYLE_BOLD, pal->active);
        b.len = 0;
        if (c->kind == "summary") {
            print(&b, "  summary of ");
            print(&b, (i64)c->count);
            print(&b, c->count==1 ? Str(" message") : Str(" messages"));
            if (c->model.len) {
                print(&b, " by ");
                print(&b, c->model);
            }
            if (c->ms) {
                print(&b, " in ");
                print(&b, duration(&a, c->ms));
            }
        } else {
            print(&b, "  ");
            print(&b, (i64)c->count);
            print(&b, c->count==1 ? Str(" old output elided") : Str(" old outputs elided"));
        }
        if (c->why == "overflow") {
            print(&b, ", as the server found it too long");
        } else if (c->why == "manual") {
            print(&b, ", as asked");
        }
        pen(p, finish(&b), 0, dim);
    } break;
    case VIEW_REWIND: {
        ViewCard *c = m->card;
        Buf b(&a, 128);
        if (c->to.len) {
            i32 k = findcall(v, c->to);
            pen(p, "Undone from ", STYLE_BOLD, dim);
            if (k >= 0) {
                ViewTool *u = v->msgs[k].tool;
                print(&b, u->name);
                putbyte(&b, ' ');
                print(&b, u->summary);
            } else {
                print(&b, c->to);
            }
            pen(p, finish(&b), STYLE_CODE, pal->summary);
        } else {
            print(&b, "Rewound to the start of turn ");
            print(&b, (i64)c->turn);
            pen(p, finish(&b), STYLE_BOLD, dim);
        }
        b.len = 0;
        print(&b, ":  ");
        print(&b, (i64)c->count);
        print(&b, c->count==1 ? Str(" file") : Str(" files"));
        pen(p, finish(&b), 0, dim);
        if (c->conflicts) {
            b.len = 0;
            print(&b, ", ");
            print(&b, (i64)c->conflicts);
            print(&b, c->conflicts==1 ? Str(" conflict") : Str(" conflicts"));
            pen(p, finish(&b), STYLE_BOLD, pal->amber);
        }
        if (c->force) {
            pen(p, ", forced", 0, dim);
        }
    } break;
    case VIEW_THINK: {
        // Streaming, its size and a clock; once done, how long it took,
        // if seen. The clock counts the turn clock's seconds since it
        // began, so that both tick on the strip's one deadline a second.
        b32 live = v->busy && v->stream>=0 && &v->msgs[v->stream]==m &&
                   v->streamkind==VIEW_THINK && !m->faded;
        i64 secs = -1;
        if (live && m->ts0) {
            i64 t0 = v->turnts && v->turnts<=m->ts0 ? v->turnts : m->ts0;
            i64 el = nowms - t0;
            el   = el<0 ? 0 : el;
            secs = el/1000 - (m->ts0 - t0)/1000;
            schedat(&g->sched, g->sched.now + (1000 - el%1000)*1000 + 1000, DUE_CLOCK);
        } else if (m->ts0 && m->ts1-m->ts0>=1000) {
            secs = (m->ts1 - m->ts0) / 1000;
        }
        pen(p, live ? Str("Thinking\xe2\x80\xa6") : Str("Reasoning"), STYLE_BOLD,
            live ? pal->active : dim);
        if (m->collapsed || live) {
            Buf b(&a, 64);
            print(&b, "  ");
            printcount(&b, m->chars);
            print(&b, m->chars==1 ? Str(" character") : Str(" characters"));
            if (secs >= 0) {
                print(&b, ", ");
                print(&b, secstext(&a, secs));
            }
            pen(p, finish(&b), 0, dim);
        }
        g->thinkchars = m->chars;
        g->thinksecs  = live ? secs : -1;
    } break;
    case VIEW_ASK: {
        Buf b(&a, 256);
        print(&b, t->tool);
        print(&b, " ");
        print(&b, t->path);
        pen(p, "Allow ", STYLE_BOLD, pal->amber);
        pen(p, finish(&b), STYLE_CODE, pal->summary);
        pen(p, "?  ", STYLE_BOLD, pal->amber);
        if (t->answer == VASK_PENDING) {
            if (v->readonly) {
                pen(p, "waiting in another process", 0, dim);
                break;
            }
            i64 secs = countdown(g, t->deadline, nowms);
            Buf c(&a, 64);
            if (secs) {
                print(&c, "denied in ");
                print(&c, secs);
                print(&c, " s unless allowed");
            } else {
                print(&c, "denying");
            }
            pen(p, finish(&c), 0, pal->amber);
        } else {
            b32 ok = t->answer == VASK_ALLOWED;
            Buf c(&a, 256);
            print(&c, ok ? Str("allowed") : Str("denied"));
            print(&c, t->by=="user"    ? Str(" by you") :
                      t->by=="timeout" ? Str(": nobody answered in time") :
                      t->by=="steer"   ? Str(": you sent a message instead") :
                      t->by=="cancel"  ? Str(": the turn was cancelled") :
                      t->by=="policy"  ? Str(": nobody could answer") :
                      t->by=="interrupted" ? Str(": the harness stopped while asking") :
                      t->by=="stopped" ? Str(": the engine stopped") : Str(""));
            pen(p, finish(&c), STYLE_BOLD, ok ? pal->ok : pal->bad);
        }
    } break;
    default:
        pen(p, m->label.len ? m->label : rolename(m->role), STYLE_BOLD, dim);
    }
}

// A task's subagent: named by its report, or found by its parent and
// call while it works.
static GuiSession *taskchild(Gui *g, GuiSession *parent, ViewTool *t)
{
    if (t->child.len) {
        GuiSession *c = findsess(g, parent->host, t->child);
        if (c) return c;
    }
    for (GuiSession *s = g->sessions; s; s = s->next) {
        View *k = s->view;
        if (k && t->call.len && s->host==parent->host && k->parent==parent->sid &&
                k->pcall==t->call) {
            return s;
        }
    }
    return 0;
}

// Whether a session's host takes commands: the engine while it runs and
// this process does not host, or another host while connected.
static b32 hostlive(Gui *g, GuiSession *s)
{
    if (s->host->kind == GH_ENGINE) {
        return !g->enginegone && !g->host && g->enginekind!=ENGINE_FAKE;
    }
    return s->host->r && !s->host->closing;
}

// Whether the files of a session can be rewound now: shown here, its
// own, and between turns, as the engine requires.
static b32 canrewind(Gui *g, GuiSession *s)
{
    return s && s==g->sess && s->view && !s->readonly && !s->view->busy &&
           hostlive(g, s) && !g->quitting;
}

enum : i32 {
    ACT_NONE,
    ACT_UNDO,     // put back a write's or edit's file, and later ones
    ACT_REWIND,   // put back the files to the start of a turn
    ACT_OPEN,     // show a task's subagent
    ACT_OUTPUT,   // show a call's whole output (its spill file)
};

struct MsgAction {
    i32         kind;
    char const *label;
    f32         w;
    b32         amber;  // it waits for the user
    GuiSession *child;
};

// The action at the right of a message's header, if any.
static MsgAction msgaction(Gui *g, GuiSession *s, View *v, ViewMsg *m)
{
    MsgAction r = {};
    ViewTool *t = m->tool;
    if (m->role==VIEW_TOOL && t->name=="task" && s) {
        r.child = taskchild(g, s, t);
        if (r.child) {
            r.amber = r.child->view && r.child->view->asks;
            r.kind  = ACT_OPEN;
            r.label = r.amber ? "Answer the subagent" : "Open subagent";
        }
    } else if (m->role == VIEW_TOOL) {
        ViewSnap *sn = rowsnap(v, t);
        if (sn && !sn->undone && canrewind(g, s)) {
            r.kind  = ACT_UNDO;
            r.label = "Undo";
        } else if (s && t->spill.len && t->status>=VTOOL_OK && hostlive(g, s)) {
            r.kind  = ACT_OUTPUT;
            r.label = "Output";
        }
    } else if (m->startsturn && canrewind(g, s) && turnrewinds(v, m->startsturn)) {
        r.kind  = ACT_REWIND;
        r.label = "Rewind to here";
    }
    if (r.kind) {
        r.w = ImGui::CalcTextSize(r.label).x + 2*ImGui::GetStyle().FramePadding.x;
    }
    return r;
}

// Ask before rewinding: the confirmation lists the files (drawrewind).
static void askrewind(Gui *g, GuiSession *s, Str call, i32 turn, b32 force)
{
    g->rewsess    = s;
    g->rewcall    = call.len ? clone(&g->perm, call) : Str{};
    g->rewturn    = turn;
    g->rewforce   = force;
    g->openrewind = 1;
}

static void drawaction(Gui *g, GuiSession *s, ViewMsg *m, MsgAction *act)
{
    ViewTool *t = m->tool;
    g->kidlinkshown |= act->amber;
    b32 go = ImGui::SmallButton(act->label);
    switch (act->kind) {
    case ACT_OPEN:
        itemrect(g, RECT_TASKLINK);
        ImGui::SetItemTooltip("Show the subagent's own transcript");
        if (go) guiselect(g, act->child);
        break;
    case ACT_UNDO:
        itemrect(g, RECT_UNDO);
        ImGui::SetItemTooltip("Put back the file as it was before this call, and "
                              "every file changed after it");
        if (go) askrewind(g, s, t->call, 0, 0);
        break;
    case ACT_REWIND:
        itemrect(g, RECT_REWINDTURN);
        ImGui::SetItemTooltip("Put back every file write and edit changed from "
                              "this turn on");
        if (go) askrewind(g, s, {}, m->startsturn, 0);
        break;
    case ACT_OUTPUT: {
        // Where it is, which the agent's text names too
        itemrect(g, RECT_OUTPUT);
        Arena a = g->frame;
        Str   d = s->view ? s->view->dir : Str{};
        Str   p = d.len ? join(&a, d, t->spill) : t->spill;
        ImGui::SetItemTooltip("Show the whole output, from its host:\n%s (%s)",
                              cstr0(&a, p), cstr0(&a, bytesize(&a, t->bytes)));
        if (go) showfile(g, s, t->spill);
    } break;
    }
}

// A screenshot's thumbnail below its tool row, fetched as it first
// shows, which opens the image whole in the Screenshot window.
static void drawthumb(Gui *g, GuiSession *s, ImDrawList *dl, ViewTool *t,
                      ImVec2 p0, Thumb th, f32 top, f32 bot)
{
    Arena    a  = g->frame;
    f32      sc = g->scale;
    ImVec2   p1 = v2(p0.x+th.w, p0.y+th.h);
    b32      in = p1.y>=top && p0.y<=bot;
    ImgSlot *sl = in ? imgslot(g, s, t->image) : 0;
    if (sl && sl->state==IMG_READY) {
        dl->AddImage(sl->tex->td.GetTexRef(), p0, p1);
        g->thumbsdrawn++;
    } else {
        dl->AddRectFilled(p0, p1, g->pal->codebg);
        Str msg = !in ? Str("") :
                  !sl ? Str("Too many screenshots in view to show this one") :
                  sl->state==IMG_FAILED ? concat(&a, "Cannot show the screenshot: ",
                                                 imgstr(&sl->err)) :
                  Str("Loading the screenshot...");
        u32 col = sl && sl->state==IMG_FAILED ? g->pal->bad : g->pal->dim;
        ImVec4 clip = ImVec4(p0.x, p0.y, p1.x, p1.y);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), v2(p0.x+6*sc, p0.y+4*sc),
                    col, msg.cdata, msg.cdata+msg.len, th.w-12*sc, &clip);
    }
    dl->AddRect(p0, p1, g->pal->rule);

    ImGui::SetCursorScreenPos(p0);
    if (ImGui::InvisibleButton("##thumb", v2(th.w, th.h))) {
        imgopen(g, s, t->image, t->imgw, t->imgh);
    }
    itemrect(g, RECT_THUMB);
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImGui::SetItemTooltip("%dx%d pixels: open it whole", t->imgw, t->imgh);
}

// The latest rewind's button, when it left files alone.
static void drawforce(Gui *g, GuiSession *s, ViewMsg *m)
{
    ViewCard *c = m->card;
    ImGui::BeginDisabled(!canrewind(g, s));
    b32 go = ImGui::Button("Rewind anyway...");
    itemrect(g, RECT_FORCE);
    ImGui::SetItemTooltip("Put these files back too, overwriting what changed them since");
    ImGui::EndDisabled();
    if (go) {
        askrewind(g, s, c->to, c->turn, 1);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("overwrites the changes made since");
}

// An open ask's buttons, below its text.
static void drawaskbuttons(Gui *g, GuiSession *s, ViewMsg *m, b32 latest)
{
    ViewTool *t = m->tool;
    Arena     a = g->frame;
    if (ImGui::Button("Allow")) {
        guianswer(g, s, t->req, 1, {});
    }
    if (latest) itemrect(g, RECT_ALLOW);
    ImGui::SetItemTooltip("Let the %s go ahead", t->tool.len ? cstr0(&a, t->tool) : "tool");
    ImGui::SameLine();
    if (ImGui::Button("Deny")) {
        guianswer(g, s, t->req, 0, {});
    }
    if (latest) itemrect(g, RECT_DENY);
    ImGui::SetItemTooltip("The agent reads why and tries something else");
    ImGui::SameLine();
    if (ImGui::Button("Deny with note...")) {
        g->denyreq  = t->req;
        g->denysess = s;
        g->denynote[0] = 0;
        g->opendeny = 1;
    }
    ImGui::SetItemTooltip("Deny, telling the agent what to do instead");
}

// At most n bytes of s, cut on a UTF-8 boundary, with an ellipsis when
// cut.
static Str shorten(Arena *a, Str s, iz n)
{
    if (s.len <= n) {
        return s;
    }
    for (; n>0 && (s[n]&0xc0)==0x80; n--) {}
    return concat(a, clone(a, takehead(s, n)), "\xe2\x80\xa6");
}

static Str sesstitle(GuiSession *);

// Where the last item ends, in window coordinates with a spacing after
// it, for an item right-aligned on its line only if it fits there.
static f32 rightof()
{
    f32 x = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    return x + ImGui::GetStyle().ItemSpacing.x;
}

static void inforow(char const *key, Str value)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", key);
    ImGui::TableNextColumn();
    text(value.len ? value : Str("(none)"));
}

// The session's details: what it is, where it works, which model and
// role, and the instruction files the agent was given.
static void drawinfo(Gui *g, GuiSession *s, View *v)
{
    Arena a  = g->frame;
    f32   sc = g->scale;
    ImGui::PushTextWrapPos(560*sc);
    if (ImGui::BeginTable("##info", 2, ImGuiTableFlags_SizingFixedFit)) {
        inforow("Session", s->sid);
        inforow("Title", sesstitle(s));
        inforow("Directory", v->cwd);
        inforow("Model", v->model);
        inforow("Endpoint", v->endpoint);
        if (v->role.len) {
            inforow("Role", v->role);
        }
        if (v->tools.len) {
            inforow("Tools", v->tools);
        }
        if (v->parent.len) {
            GuiSession *p = findsess(g, s->host, v->parent);
            inforow("Parent", p ? sesstitle(p) : v->parent);
        }
        if (s->host->kind != GH_ENGINE) {
            inforow("Host", hostlabel(g, &a, s->host));
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Instruction files");
    if (!v->instr.len) {
        ImGui::TextDisabled("None: no AGENTS.md or CLAUDE.md applies here.");
    }
    i32 agents = 0;
    for (iz i = 0; i < v->instr.len; i++) {
        ViewInstr *in = &v->instr[i];
        Buf b(&a, 256);
        print(&b, bytesize(&a, in->bytes));
        print(&b, in->why=="nested"  ? Str(", for a directory the agent worked in") :
                  in->why=="changed" ? Str(", changed since the session began") :
                  Str(", from the start"));
        if (in->importer.len) {
            print(&b, ", imported by ");
            print(&b, showpath(v, in->importer));
        }
        ImGui::Bullet();
        text(showpath(v, in->path));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", cstr0(&a, finish(&b)));
        agents += endswith(in->path, "AGENTS.md");
    }
    for (iz i = 0; i < v->skipped.len; i++) {
        ImGui::TextDisabled("Not loaded: %s", cstr0(&a, showpath(v, v->skipped[i])));
    }
    if (v->instrtext.len) {
        if (ImGui::SmallButton("Copy what the agent was given")) {
            ImGui::SetClipboardText(cstr0(&a, v->instrtext));
        }
    }
    ImGui::PopTextWrapPos();
    g->infoshown = (i32)v->instr.len;
    g->infoagents = agents;
}

// The context gauge: a thin bar of the tokens the context holds, on the
// scale of the model's window, with a tick where compaction begins, amber
// from 90% of the way there. Nothing without the numbers.
static f32 gaugewidth(Gui *g, View *v)
{
    b32 any = v->tokens>0 && (v->ctx>0 || v->compactat>0);
    return any ? 64*g->scale : 0;
}

static void drawgauge(Gui *g, View *v, f32 w)
{
    Arena       a     = g->frame;
    f32         sc    = g->scale;
    Palette    *pal   = g->pal;
    f32         h     = ImGui::GetFrameHeight();
    i64         scale = v->ctx>0 ? v->ctx : v->compactat;
    b32         near  = v->compactat>0 && v->tokens*10 >= v->compactat*9;
    ImVec2      p0    = ImGui::GetCursorScreenPos();
    ImDrawList *dl    = ImGui::GetWindowDrawList();
    f32         bh    = 5*sc;
    f32         y0    = p0.y + (h - bh)/2;
    f32         frac  = (f32)v->tokens / (f32)scale;
    frac = frac>1 ? 1 : frac;
    dl->AddRectFilled(v2(p0.x, y0), v2(p0.x+w, y0+bh), pal->codebg, bh/2);
    dl->AddRectFilled(v2(p0.x, y0), v2(p0.x+w*frac, y0+bh),
                      near ? pal->amber : pal->active, bh/2);
    if (v->compactat>0 && v->compactat<scale) {
        f32 x = p0.x + w*(f32)v->compactat/(f32)scale;
        dl->AddLine(v2(x, y0-2*sc), v2(x, y0+bh+2*sc), pal->dim, 1.5f*sc);
    }
    ImGui::Dummy(v2(w, h));
    itemrect(g, RECT_GAUGE);
    g->gaugeshown = 1 + near;
    if (ImGui::BeginItemTooltip()) {
        Buf b(&a, 256);
        print(&b, "Context: ");
        printgrouped(&b, v->tokens);
        print(&b, " tokens");
        if (v->compactat > 0) {
            print(&b, "\nCompacts at ");
            printgrouped(&b, v->compactat);
            print(&b, " (");
            print(&b, v->tokens*100/v->compactat);
            print(&b, "% of the way)");
        }
        if (v->ctx > 0) {
            print(&b, "\nWindow: ");
            printgrouped(&b, v->ctx);
        }
        print(&b, "\nCached in the last request: ");
        printgrouped(&b, v->cached);
        text(finish(&b));
        ImGui::EndTooltip();
    }
}

// A toggle button, highlighted while on.
static b32 togglebutton(char const *label, b32 on)
{
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    b32 r = ImGui::SmallButton(label);
    if (on) {
        ImGui::PopStyleColor();
    }
    return r;
}

// A shortcut's name: Cmd on macOS, Ctrl elsewhere.
static char const *shortcutname(Gui *g, i32 which)
{
    switch (which) {
    case 0: return g->mac ? "Cmd+Shift+T" : "Ctrl+Shift+T";
    case 1: return g->mac ? "Cmd+Shift+R" : "Ctrl+Shift+R";
    }
    return g->mac ? "Cmd+F" : "Ctrl+F";
}

// A line above the transcript: the session, or a subagent's parent to
// go back to, its context gauge, the toggles that open all its tool
// output and reasoning, and the session's details in a popup.
static void drawsessbar(Gui *g)
{
    GuiSession *s = g->sess;
    View       *v = g->view;
    if (!s || !s->view) {
        return;
    }
    Arena       a     = g->frame;
    ImGuiStyle &st    = ImGui::GetStyle();
    f32         gap   = st.ItemSpacing.x;
    f32         gw    = gaugewidth(g, v);
    char const *tl    = "Tool output";
    char const *rl    = "Reasoning";
    f32         infow = ImGui::CalcTextSize("Info").x + 2*st.FramePadding.x;
    f32         tw    = ImGui::CalcTextSize(tl).x + 2*st.FramePadding.x;
    f32         rw    = ImGui::CalcTextSize(rl).x + 2*st.FramePadding.x;
    f32         rest  = (gw>0 ? gw + gap : 0) + tw + gap + rw + gap + infow;
    f32         right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - rest;
    if (v->parent.len) {
        GuiSession *p = findsess(g, s->host, v->parent);
        ImGui::TextDisabled(v->role.len ? "Subagent (%s) of" : "Subagent%s of",
                            cstr0(&a, v->role));
        ImGui::SameLine();
        Str title = shorten(&a, p ? sesstitle(p) : v->parent, 60);
        if (ImGui::TextLink(cstr0(&a, title))) {
            guiselect(g, p ? p : addsess(g, s->host, v->parent));
        }
        itemrect(g, RECT_PARENTLINK);
        ImGui::SetItemTooltip("Back to the session that started this subagent");
    } else {
        Buf b(&a, 256);
        print(&b, shorten(&a, sesstitle(s), 60));
        if (v->model.len) {
            print(&b, "  \xc2\xb7  ");  // a middle dot
            print(&b, v->model);
        }
        if (v->role.len) {
            print(&b, "  \xc2\xb7  role ");
            print(&b, v->role);
        }
        ImGui::TextDisabled("%s", cstr0(&a, finish(&b)));
    }
    if (right > rightof()) {
        ImGui::SameLine(right);
    } else if (right > ImGui::GetCursorPosX()) {
        ImGui::SetCursorPosX(right);  // too narrow: a line of their own
    }
    g->gaugeshown = 0;
    if (gw > 0) {
        drawgauge(g, v, gw);
        ImGui::SameLine();
    }
    if (togglebutton(tl, v->showtools)) {
        setsessiontools(g, s, !v->showtools);
    }
    itemrect(g, RECT_TOOLSBTN);
    ImGui::SetItemTooltip("Open every tool row's output in this session, or fold them "
                          "again as the settings say (%s)", shortcutname(g, 0));
    ImGui::SameLine();
    if (togglebutton(rl, v->showthink)) {
        setsessionthink(g, s, !v->showthink);
    }
    itemrect(g, RECT_THINKBTN);
    ImGui::SetItemTooltip("Show all reasoning in this session, even hidden, or fold it "
                          "again as the settings say (%s)", shortcutname(g, 1));
    ImGui::SameLine();
    if (ImGui::SmallButton("Info")) {
        ImGui::OpenPopup("##sessinfo");
    }
    itemrect(g, RECT_INFOBTN);
    ImGui::SetItemTooltip("The session's model, role and instruction files");
    if (ImGui::BeginPopup("##sessinfo")) {
        drawinfo(g, s, v);
        ImGui::EndPopup();
    }
}

// Before a rewind: the files it would put back, newest first, and how.
static void drawrewind(Gui *g)
{
    f32 sc = g->scale;
    if (g->openrewind) {
        ImGui::OpenPopup("Rewind files?");
        g->openrewind = 0;
    }
    ImGui::SetNextWindowSize(v2(560*sc, 0));
    if (!ImGui::BeginPopupModal("Rewind files?", 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    Arena       a  = g->frame;
    GuiSession *s  = g->rewsess;
    View       *v  = s && s->view ? s->view : g->view;
    Slice<ViewSnap> set = rewindset(v, &a, g->rewcall, g->rewturn);
    ImGui::PushTextWrapPos(0);
    if (g->rewcall.len) {
        ImGui::TextUnformatted("Undo this change, and every later change by write and edit:");
    } else {
        ImGui::Text("Put back the files changed by write and edit from turn %d on:", g->rewturn);
    }
    for (iz i = 0; i < set.len; i++) {
        ImGui::Bullet();
        text(showpath(v, set[i].path));
        ImGui::SameLine();
        ImGui::TextDisabled(set[i].existed ? "put back as it was" : "removed, as it did not exist then");
    }
    if (!set.len) {
        ImGui::TextDisabled("Nothing is left to rewind there.");
    }
    if (g->rewforce) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->amber);
        ImGui::TextUnformatted("Files changed since by something else are overwritten too.");
        ImGui::PopStyleColor();
    }
    ImGui::TextDisabled("Changes made by shell commands are not undone. The agent "
                        "is told what was put back.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::BeginDisabled(!set.len || !canrewind(g, s));
    b32 go = ImGui::Button(g->rewforce ? "Rewind anyway" : "Rewind", v2(140*sc, 0));
    itemrect(g, RECT_REWINDOK);
    ImGui::EndDisabled();
    if (go) {
        guirewind(g, s, g->rewcall, g->rewturn, g->rewforce);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    b32 cancel = ImGui::Button("Cancel", v2(120*sc, 0));
    itemrect(g, RECT_REWINDCANCEL);
    if (cancel) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// A message's find hits, while the find bar is open on its view.
static Marks msgmarks(Gui *g, View *v, i32 msg)
{
    ViewSearch *s = g->search;
    Marks       r = {0, 0, -1};
    if (!g->findopen || s->v!=v || !s->hits.len) {
        return r;
    }
    iz lo = searchfrom(s, msg, 0);
    iz hi = searchfrom(s, msg+1, 0);
    r.h   = s->hits.data + lo;
    r.n   = hi - lo;
    r.cur = s->cur>=lo && s->cur<hi ? s->cur-lo : -1;
    return r;
}

// A code block's Copy button, at its top right while the mouse is over
// the block, which it overlaps as the headers' toggles overlap the text:
// the code byte for byte, as selecting the block whole copies it. It says
// "Copied" for a moment after, until a deadline.
static void drawcodecopy(Gui *g, View *v, i32 msg, Placed *pl, ImVec2 origin, f32 top)
{
    BlockLayout *bl = pl->bl;
    Deco        *bg = 0;
    for (iz i = 0; i<bl->decos.len && !bg; i++) {
        bg = bl->decos[i].kind==DECO_CODEBG ? &bl->decos[i] : 0;
    }
    if (!bg) {
        return;
    }
    f32     sc = g->scale;
    ImVec2  a  = v2(origin.x+bg->x, origin.y+pl->y+bg->y);
    ImVec2  z  = v2(a.x+bg->w, a.y+bg->h);
    ImVec2  mp = ImGui::GetIO().MousePos;
    b32     aimed = g->drivecodemsg<0 || (msg==g->drivecodemsg && pl->block==g->drivecodeblk);
    if (aimed) {
        f32 y0 = a.y>top ? a.y : top;
        g->rects[RECT_CODEBLOCK] = {a.x, y0, z.x, z.y};
    }
    b32 over = mp.x>=a.x && mp.x<z.x && mp.y>=a.y && mp.y<z.y && mp.y>=top;
    if (!over || g->dragging ||
            !ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        return;
    }
    i64 until  = g->copiedat + COPIED_US;
    b32 copied = g->copiedat && g->sched.now<until && g->copiedview==v &&
                 g->copiedmsg==msg && g->copiedblk==pl->block;
    if (copied) {
        schedat(&g->sched, until + 1000, DUE_APP);
    }
    // As wide as its longer label, so that it stays under the mouse, and
    // one ID for both, so that what the mouse is over stays the same and
    // no hover delays start again with the change
    char const *label = copied ? "Copied###copy" : "Copy###copy";
    ImGuiStyle &st    = ImGui::GetStyle();
    f32         bw    = ImGui::CalcTextSize("Copied", 0, 1).x + 2*st.FramePadding.x;
    f32         by    = (a.y>top ? a.y : top) + 4*sc;
    ImGui::SetCursorScreenPos(v2(z.x - bw - 4*sc, by));
    ImGui::PushID(pl->block);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, v2(st.FramePadding.x, 0));
    b32 go = ImGui::Button(label, v2(bw, 0));
    ImGui::PopStyleVar();
    ImGui::PopID();
    if (aimed) {
        itemrect(g, RECT_CODECOPY);
    }
    g->copiedshown |= copied;
    if (go) {
        Arena t = g->frame;
        ImGui::SetClipboardText(cstr0(&t, copyblock(v, &t, msg, pl->block)));
        g->copiedview = v;
        g->copiedmsg  = msg;
        g->copiedblk  = pl->block;
        g->copiedat   = g->sched.now;
    }
}

// The find bar, open over the transcript: the text to find, the hits
// counted, and the buttons to go between them. A new text or session
// searches again, a budget of bytes a frame, and the first hit at or
// below the top of the screen becomes the current one.
static void drawfind(Gui *g, View *v, f64 scrolly)
{
    ViewSearch *s  = g->search;
    f32         sc = g->scale;
    Arena       a  = g->frame;
    if (!g->findopen) {
        return;
    }
    if (g->findfocus) {
        ImGui::SetKeyboardFocusHere();
        g->findfocus = 0;
    }
    ImGui::SetNextItemWidth(260*sc);
    b32 enter = ImGui::InputTextWithHint("##find", "Find in the transcript", g->findbuf,
                                         FIELD_MAX, ImGuiInputTextFlags_EnterReturnsTrue|
                                                    ImGuiInputTextFlags_AutoSelectAll);
    itemrect(g, RECT_FINDFIELD);
    b32 again = ImGui::IsItemEdited() || s->v!=v;
    b32 esc   = ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    g->findactive = ImGui::IsItemActive();
    if (enter) {
        g->findfocus = 1;  // Enter lets go of the field: take it back
    }

    if (again) {
        Str q = {(u8 *)g->findbuf, (iz)__builtin_strlen(g->findbuf)};
        searchreset(s, v, q);
        g->findfrom = viewfind(v, scrolly);
        g->findwant = 1;
    }
    b32 done = searchstep(s, FIND_BUDGET);
    if (!done) {
        scheddirty(&g->sched);  // more next frame
    }
    if (g->findwant && s->cur<0) {
        // The first hit from the top of the screen down, else from the
        // start once all are found
        iz i = searchfrom(s, g->findfrom<0 ? 0 : g->findfrom, 0);
        if (i < s->hits.len) {
            s->cur = (i32)i;
        } else if (done && s->hits.len) {
            s->cur = 0;
        }
        if (s->cur >= 0) {
            searchreveal(s, &s->hits[s->cur]);
            g->findjump = 1;
        }
    }
    g->findwant &= !done && s->cur<0;

    ImGui::SameLine();
    b32 prev = ImGui::ArrowButton("##findprev", ImGuiDir_Up);
    ImGui::SetItemTooltip("The previous match (Shift+Enter)");
    ImGui::SameLine();
    b32 next = ImGui::ArrowButton("##findnext", ImGuiDir_Down);
    ImGui::SetItemTooltip("The next match (Enter)");
    ImGui::SameLine();
    Buf b(&a, 64);
    if (s->q.len && !s->hits.len && done) {
        print(&b, "No matches");
    } else if (s->q.len) {
        print(&b, (i64)(s->cur>=0 ? s->cur+1 : 0));
        print(&b, " of ");
        print(&b, (i64)s->hits.len);
        print(&b, done ? Str("") : Str("+"));
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", cstr0(&a, finish(&b)));
    ImGui::SameLine();
    b32 close = ImGui::SmallButton("Close");
    ImGui::SetItemTooltip("Close the find bar (Esc)");
    if (esc || close) {
        gui_find_close(g);
    } else if (enter || next || prev) {
        gui_find_next(g, enter ? (ImGui::GetIO().KeyShift ? -1 : 1) : prev ? -1 : 1);
    }
}

// The transcript of the session shown: virtualized messages with
// selection across them, tool rows and ask cards.
static void drawtranscript(Gui *g)
{
    GuiSession *gs = g->sess ? g->sess : &g->nosession;
    View       *v  = g->view;
    f32         sc = g->scale;
    ImGuiStyle &st = ImGui::GetStyle();

    drawsessbar(g);
    if (gs->readonly) {
        g->bannershown = 1;
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->amber);
        ImGui::TextWrapped("Read-only: another Prospero process has this "
                           "session open. Its changes show after it lets go "
                           "and the session is opened again.");
        ImGui::PopStyleColor();
        if (ImGui::Button("Open again")) {
            guiopen(g, gs);
        }
        ImGui::SetItemTooltip("Take the session over, if the other process has let go");
    }
    if (g->engineerr.len) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
        Arena a = g->frame;
        ImGui::TextWrapped("The engine is unavailable: %s", cstr0(&a, g->engineerr));
        ImGui::PopStyleColor();
    }

    // Going between find hits is the find bar's and the transcript's
    // alone (menukeys has the rest)
    if (g->findopen && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_G)) {
        gui_find_next(g, 1);
    }
    if (g->findopen && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_G)) {
        gui_find_next(g, -1);
    }
    drawfind(g, v, gs->win ? gs->win->Scroll.y : 0);

    // Layout comes first, before the child window begins, so that its
    // content size and scroll take effect this very frame: a scroll set
    // inside it would land a frame late and cost a second present for
    // every streamed update.
    ImVec2      avail = ImGui::GetContentRegionAvail();
    f32         pad   = 12*sc;
    f32         winh  = avail.y;
    f32         width = avail.x - st.ScrollbarSize - 2*pad - g->widthadj;
    width = (f32)(i32)(width<50*sc ? 50*sc : width);
    FontMetrics fm    = fontmetrics(&g->fonts, 0);
    f32         charw = fontadvance(&g->fonts, 0, 'n');
    f32         head  = pad/4 + fm.lineh;  // the header line
    f32         extra = head + pad*3/4;
    f32         foot  = ImGui::GetFrameHeight() + pad/2;
    if (v->extra!=extra || v->foot!=foot) {
        v->topsdirty = 1;
    }
    v->extra = extra;
    v->foot  = foot;
    v->head  = head;
    v->gap   = 6*sc;
    f32 thumbw = (f32)IMG_THUMBW*sc<width ? (f32)IMG_THUMBW*sc : width;
    f32 thumbh = (f32)IMG_THUMBH*sc;
    if (v->thumbw!=thumbw || v->thumbh!=thumbh || v->thumbpad!=pad/2) {
        v->topsdirty = 1;
    }
    v->thumbw   = thumbw;
    v->thumbh   = thumbh;
    v->thumbpad = pad/2;

    // Where the view is: last frame's scroll, or where the wheel just
    // sent it.
    ImGuiWindow *last    = gs->win;
    f32          scrolly = 0;
    b32          bottom  = gs->stick;
    if (last) {
        scrolly = last->Scroll.y;
        if (last->ScrollTarget.y < FLT_MAX) {
            scrolly = last->ScrollTarget.y;
            bottom  = 0;
        }
        scrolly = scrolly<0 ? 0 : scrolly>last->ScrollMax.y ? last->ScrollMax.y : scrolly;
    }
    f32 scroll0 = scrolly;  // what the window has, unless set below

    // A new width (this session's own, since each scrolls in its own
    // coordinates): what was at the top edge stays there. Its place is
    // found with the old width's heights and layouts, before the new
    // width discards them, and put back with the new.
    if (width != v->lastw) {
        Anchor an = {-1, -1, 0};
        if (v->lastw>0 && last && !bottom) {
            viewtops(v, v->lastw, charw, fm.lineh);
            an = viewanchor(v, &g->measure, v->lastw, sc, head, scrolly);
        }
        v->topsdirty = 1;
        v->lastw     = width;
        if (an.msg >= 0) {
            viewtops(v, width, charw, fm.lineh);
            msglayout(v, an.msg, &g->measure, width, sc);
            viewtops(v, width, charw, fm.lineh);
            scrolly = (f32)anchory(v, an, &g->measure, width, sc, head);
        }
    }
    viewtops(v, width, charw, fm.lineh);

    // Going to a find hit: its message and the screen above it laid out
    // first, so that it lands a third of the way down
    ViewSearch *srch = g->search;
    if (g->findjump && g->findopen && srch->v==v && srch->cur>=0) {
        FindHit *h = &srch->hits[srch->cur];
        msglayout(v, h->msg, &g->measure, width, sc);
        viewtops(v, width, charw, fm.lineh);
        i32 j0 = viewfind(v, v->msgs[h->msg].top - winh);
        for (i32 i = j0; i>=0 && i < h->msg; i++) {
            msglayout(v, i, &g->measure, width, sc);
        }
        viewtops(v, width, charw, fm.lineh);
        ViewMsg   *m  = &v->msgs[h->msg];
        MsgLayout *ml = msglayout(v, h->msg, &g->measure, width, sc);
        f64        y  = m->top + head + msgy(msgdoc(v, h->msg), ml, h->off);
        scrolly  = (f32)(y - winh/3);
        scrolly  = scrolly<0 ? 0 : scrolly;
        bottom   = 0;
        gs->stick = 0;
        g->findjump = 0;
    }

    // Anchor on the first visible message, so that height corrections
    // above it do not move what is on screen.
    i32 anchor   = viewfind(v, scrolly);
    f64 anchordy = anchor>=0 ? scrolly - v->msgs[anchor].top : 0;

    // Lay out what is visible, plus a screen of margin each way.
    i32 i0 = viewfind(v, scrolly - winh);
    i32 i1 = viewfind(v, scrolly + 2*winh);
    i64 t0 = os_ticks(g->os);
    for (i32 i = i0; i>=0 && i <= i1; i++) {
        msglayout(v, i, &g->measure, width, sc);
    }
    g->layoutus += os_ticks(g->os) - t0;
    f64 total = viewtops(v, width, charw, fm.lineh);

    f64 want = scrolly;
    if (bottom) {
        want = total + pad - winh;  // the scroll range's end, so it sticks
    } else if (anchor >= 0) {
        want = v->msgs[anchor].top + anchordy;
    }
    if (g->scrollto >= 0) {
        want = g->scrollto;  // a benchmark or a drag past the edge
        g->scrollto = -1;
    }
    want = want>total+pad-winh ? total+pad-winh : want;
    want = want<0 ? 0 : want;

    ImGui::SetNextWindowContentSize(v2(0, (f32)total + pad));
    if ((f32)want != scroll0) {
        ImGui::SetNextWindowScroll(v2(-1, (f32)want));
    }
    pushstr(gs->sid);
    ImGui::BeginChild("##transcript", v2(0, 0), 0,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    gs->win = ImGui::GetCurrentWindow();
    ImVec2 wpos = ImGui::GetWindowPos();
    scrolly = ImGui::GetScrollY();
    f32 top = wpos.y;
    f32 bot = wpos.y + winh;

    // Mouse: an invisible button over the visible area, which the
    // headers' toggles and the asks' buttons, made later, overlap.
    ImGui::SetCursorScreenPos(wpos);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##text", v2(ImGui::GetWindowWidth() - st.ScrollbarSize, winh),
                           ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight);
    itemrect(g, RECT_TRANSCRIPT);
    ImGuiIO &io = ImGui::GetIO();
    b32 hovered = ImGui::IsItemHovered();
    b32 active  = ImGui::IsItemActive();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    }

    // The position under the mouse, clamped into the visible messages.
    Pos at = {};
    b32 hit = 0;
    if ((hovered || active) && v->msgs.len) {
        f32 my = io.MousePos.y;
        my = my<top ? top : my>bot-1 ? bot-1 : my;
        f64 cy = scrolly + (my - wpos.y);
        i32 i  = viewfind(v, cy);
        if (i >= 0) {
            ViewMsg   *m  = &v->msgs[i];
            MsgLayout *ml = msglayout(v, i, &g->measure, width, sc);
            f32 ly = (f32)(cy - m->top - head);
            at.msg = i;
            at.off = ly<0 || m->collapsed ? 0 :
                     msghit(&g->measure, msgdoc(v, i), ml, io.MousePos.x - wpos.x - pad, ly);
            if (ly > m->h) at.off = (i32)m->text.len;
            hit = 1;
        }
    }

    if (hit && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        g->menumsg = at.msg;  // the popup takes the hover from here on
    }
    if (hit && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        i32 clicks = ImGui::GetMouseClickedCount(ImGuiMouseButton_Left);
        if (io.KeyShift && v->sel.active) {
            selextend(v, at);
        } else {
            selbegin(v, at, clicks>=3 ? SEL_BLOCK : clicks==2 ? SEL_WORD : SEL_CHAR);
        }
        g->dragging = 1;
    }
    if (g->dragging) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            g->dragging = 0;
        } else if (hit) {
            selextend(v, at);
            // Past an edge: scroll toward the mouse, faster the further.
            f32 my = io.MousePos.y;
            f32 dy = my<top ? my-top : my>bot ? my-bot : 0;
            if (dy != 0) {
                f32 step = dy * 0.5f;
                step = step<-60*sc ? -60*sc : step>60*sc ? 60*sc : step;
                g->scrollto = scrolly + step;
                if (g->scrollto < 0) g->scrollto = 0;
            }
        }
    }

    if (ImGui::BeginPopupContextItem("##textmenu")) {
        b32 any = !selempty(&v->sel);
        b32 copy = ImGui::MenuItem("Copy", g->mac ? "Cmd+C" : "Ctrl+C", false, any);
        itemrect(g, RECT_COPYITEM);
        if (copy) {
            copyout(g, COPY_TEXT);
        }
        if (ImGui::MenuItem("Copy as Markdown", 0, false, any)) {
            copyout(g, COPY_MARKDOWN);
        }
        if (ImGui::MenuItem("Copy Message", 0, false, g->menumsg<v->msgs.len)) {
            copymessage(g, g->menumsg);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", g->mac ? "Cmd+A" : "Ctrl+A")) {
            selall(v);
        }
        ImGui::EndPopup();
    }

    // Drawing
    ImDrawList         *dl    = ImGui::GetWindowDrawList();
    ImDrawListSplitter *sp    = &g->splitter;
    Range               sel   = selrange(&v->sel);
    i64                 nowms = os_time(g->os);
    i32                 d0    = viewfind(v, scrolly);
    i32                 d1    = viewfind(v, scrolly + winh);
    i32                 lastask = -1;
    for (iz i = v->msgs.len-1; i >= 0; i--) {
        if (v->msgs[i].role==VIEW_ASK && v->msgs[i].tool->answer==VASK_PENDING) {
            lastask = (i32)i;
            break;
        }
    }
    i32 latest[] = {RECT_TASKLINK, RECT_UNDO, RECT_REWINDTURN, RECT_FORCE, RECT_COMPACTHDR,
                    RECT_THINKHDR};
    for (iz i = 0; i < countof(latest); i++) {
        g->rects[latest[i]] = {};  // the latest drawn, or none
    }
    g->thinkrows  = 0;
    g->thinkchars = -1;
    g->thinksecs  = -1;
    g->rects[RECT_CODEBLOCK] = {};
    g->rects[RECT_CODECOPY]  = {};
    g->copiedshown = 0;
    sp->Split(dl, CH_COUNT);
    for (i32 i = d0; i>=0 && i <= d1; i++) {
        if (msghidden(v, &v->msgs[i])) {
            continue;  // reasoning, with thinking = hide
        }
        ViewMsg   *m     = &v->msgs[i];
        MsgLayout *ml    = msglayout(v, i, &g->measure, width, sc);
        f32        y     = wpos.y + (f32)(m->top - scrolly);
        f32        x     = wpos.x + pad;
        f32        alpha = m->faded ? 0.45f : 1;
        f32        mfoot = msgfoot(v, m);
        ImVec2     box0  = v2(wpos.x+pad/2, y);
        ImVec2     box1  = v2(wpos.x+pad*1.5f+width, y+v->extra+m->h+mfoot);

        // Backgrounds: the user's messages, tool rows, ask cards
        sp->SetCurrentChannel(dl, CH_BG);
        if (m->role == VIEW_USER) {
            dl->AddRectFilled(box0, box1, g->pal->userbg, 6*sc);
        } else if (m->role == VIEW_TOOL) {
            dl->AddRectFilled(box0, box1, fade(g->pal->toolbg, alpha), 4*sc);
            dl->AddRectFilled(box0, v2(box0.x+3*sc, box1.y), fade(toolcolor(g->pal, m->tool->status), alpha*0.8f));
        } else if (m->role == VIEW_ASK) {
            b32 open = m->tool->answer == VASK_PENDING;
            dl->AddRectFilled(box0, box1, g->pal->askbg, 6*sc);
            dl->AddRect(box0, box1, open ? g->pal->amber : fade(g->pal->amber, 0.4f), 6*sc, 1.5f*sc);
        } else if (m->role==VIEW_COMPACT || m->role==VIEW_REWIND) {
            u32 bar = m->role==VIEW_COMPACT ? g->pal->active :
                      m->card->conflicts    ? g->pal->amber : g->pal->ok;
            dl->AddRectFilled(box0, box1, g->pal->toolbg, 4*sc);
            dl->AddRectFilled(box0, v2(box0.x+3*sc, box1.y), fade(bar, 0.8f));
        }

        sp->SetCurrentChannel(dl, CH_TEXT);
        MsgAction act = msgaction(g, g->sess, v, m);
        f32       actw = act.kind ? act.w + 8*sc : 0;
        Pen p = {dl, &g->fonts, x, y + pad/4, x + width - actw, g->fonts.size*0.85f};
        drawheader(g, &p, v, m, nowms);
        ImVec2 origin = v2(x, y + head);
        Doc   *d      = m->collapsed ? 0 : msgdoc(v, i);
        if (d) {
            Marks mk = msgmarks(g, v, i);
            for (iz k = 0; k < ml->blocks.len; k++) {
                Placed *pl = &ml->blocks[k];
                if (origin.y+pl->y > bot || origin.y+pl->y+pl->bl->h < top) {
                    continue;
                }
                drawblock(g, dl, sp, d, i, pl, origin, sel, &mk, top, bot, alpha);
            }
        }

        ImGui::PushID(i);
        for (iz k = 0; d && k < ml->blocks.len; k++) {
            Placed *pl = &ml->blocks[k];
            if (d->blocks[pl->block].type!=DOC_CODE || origin.y+pl->y > bot ||
                    origin.y+pl->y+pl->bl->h < top) {
                continue;
            }
            drawcodecopy(g, v, i, pl, origin, top);
        }
        if (m->fold || m->role==VIEW_TOOL || m->role==VIEW_THINK) {
            // The header toggles the body, but for its action
            ImGui::SetCursorScreenPos(v2(x, y));
            if (ImGui::InvisibleButton("##toggle", v2(width - actw, head))) {
                togglemsg(v, i);
            }
            if (m->role == VIEW_COMPACT) {
                itemrect(g, RECT_COMPACTHDR);
            } else if (m->role == VIEW_THINK) {
                itemrect(g, RECT_THINKHDR);
                g->thinkrows++;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
        }
        if (act.kind) {
            ImGui::SetCursorScreenPos(v2(x + width - act.w, y + pad/4));
            drawaction(g, gs, m, &act);
        }
        Thumb th = msgthumb(v, m);
        if (th.h > 0) {
            sp->SetCurrentChannel(dl, CH_TEXT);
            drawthumb(g, gs, dl, m->tool, v2(x, y + head + m->h + pad/4), th, top, bot);
        } else if (mfoot>0 && m->role==VIEW_ASK) {
            ImGui::SetCursorScreenPos(v2(x, y + head + m->h + pad/4));
            drawaskbuttons(g, gs, m, i==lastask);
        } else if (mfoot > 0) {
            ImGui::SetCursorScreenPos(v2(x, y + head + m->h + pad/4));
            drawforce(g, gs, m);
        }
        ImGui::PopID();
    }
    sp->Merge(dl);

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A)) {
            selall(v);
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C)) {
            copyout(g, COPY_TEXT);
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_C)) {
            copyout(g, COPY_MARKDOWN);
        }
    }

    gs->stick = scrolly >= ImGui::GetScrollMaxY()-2 || total<=winh;
    ImGui::EndChild();
    ImGui::PopID();

    if (g->opendeny) {
        ImGui::OpenPopup("##denynote");
        g->opendeny = 0;
    }
    if (ImGui::BeginPopup("##denynote")) {
        ImGui::TextUnformatted("Deny, and tell the agent:");
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(360*sc);
        b32 go = ImGui::InputText("##note", g->denynote, FIELD_MAX,
                                  ImGuiInputTextFlags_EnterReturnsTrue);
        go |= ImGui::Button("Deny");
        if (go) {
            Str note = {(u8 *)g->denynote, (iz)__builtin_strlen(g->denynote)};
            guianswer(g, g->denysess, g->denyreq, 0, trim(note));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    drawrewind(g);
}


// Liveness strip and prompt

// Esc cancels the shown session's turn, from inside the windows about
// the session while one has focus: there it goes ahead of the prompt
// box, which would otherwise take it to undo the typing, while a field
// elsewhere (Settings) keeps Esc to abandon its edit, and so does the
// find bar's, where it closes the bar.
static void escapecancel(Gui *g)
{
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !g->findactive &&
            ImGui::Shortcut(ImGuiKey_Escape, ImGuiInputFlags_RouteFocused|ImGuiInputFlags_RouteOverActive)) {
        guicancel(g, g->sess);
    }
}

// What the session is doing, how fast, and how long since the model sent
// anything: one line, whose clocks ask for frames only while shown.
static void drawstrip(Gui *g)
{
    View *v  = g->view;
    f32   sc = g->scale;
    if (!g->sess) {
        ImGui::TextDisabled("No session: start one with New session.");
        return;
    }
    Arena a     = g->frame;
    i64   nowms = os_time(g->os);
    i64   now   = g->sched.now;
    b32   req   = v->state==LIVE_DISCOVER || v->state==LIVE_WAITING ||
                  v->state==LIVE_STREAMING || v->state==LIVE_ERROR ||
                  v->state==LIVE_COMPACTING;
    ImGui::PushStyleColor(ImGuiCol_Text, livecolor(g->pal, v->state));
    ImGui::TextUnformatted(cstr0(&a, livelabel(v, v->state)));
    ImGui::PopStyleColor();

    if (v->busy && g->sess->readonly) {
        // Its records go to the other process: nothing here would see
        // the turn end, so no clock counts on forever
        ImGui::SameLine();
        ImGui::TextDisabled("in another process, as of its log when opened");
    } else if (v->busy && v->turnts) {
        // The turn's clock, once a second
        g->roclock |= g->sess->readonly;
        i64 el = nowms - v->turnts;
        el = el<0 ? 0 : el;
        schedat(&g->sched, now + (1000 - el%1000)*1000 + 1000, DUE_CLOCK);
        Buf b(&a, 32);
        print(&b, el/1000);
        print(&b, " s");
        ImGui::SameLine();
        ImGui::TextUnformatted(cstr0(&a, finish(&b)));
    }
    if (v->busy && v->thinking==THINK_HIDE && !v->showthink && v->stream>=0 &&
            v->streamkind==VIEW_THINK) {
        // Why the transcript shows nothing while the model thinks
        g->thinkhidshown = 1;
        ImGui::SameLine();
        ImGui::TextDisabled("thinking (reasoning hidden)");
    }
    if (v->state == LIVE_QUEUED) {
        // Its place in the endpoint's line, which live records update
        g->queuedshown = 1;
        Buf b(&a, 96);
        print(&b, "waiting for the endpoint");
        if (v->queuepos) {
            print(&b, ": ");
            print(&b, (i64)v->queuepos);
            print(&b, " of ");
            print(&b, (i64)(v->queuelen>v->queuepos ? v->queuelen : v->queuepos));
            print(&b, " in line");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", cstr0(&a, finish(&b)));
    } else if (v->state == LIVE_TASK) {
        g->taskshown = v->tasks;
        Buf b(&a, 64);
        print(&b, (i64)v->tasks);
        print(&b, v->tasks==1 ? Str(" subagent at work") : Str(" subagents at work"));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", cstr0(&a, finish(&b)));
    } else if (v->state == LIVE_COMPACTING) {
        g->compactingshown = 1;
        ImGui::SameLine();
        ImGui::TextDisabled("summarizing the conversation to make room");
    }
    if (req && v->tps > 0) {
        Buf b(&a, 32);
        printfloat(&b, v->tps, 1);
        print(&b, " tok/s");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", cstr0(&a, finish(&b)));
    }
    if (req && v->ptotal>0 && v->pdone<v->ptotal) {
        ImGui::SameLine();
        f32 frac = (f32)v->pdone / (f32)v->ptotal;
        Buf b(&a, 48);
        print(&b, "reading the prompt ");
        print(&b, (i64)(frac*100));
        putbyte(&b, '%');
        ImGui::ProgressBar(frac, v2(160*sc, 0), cstr0(&a, finish(&b)));
    }
    if (req && v->lastdata) {
        // Amber after AMBER_MS without data, red near the watchdog
        i64 idle = nowms - v->lastdata;
        b32 red  = v->watchat && v->watchms && nowms >= v->watchat - v->watchms/4;
        if (idle >= AMBER_MS || red) {
            g->ambershown = 1;
            schedat(&g->sched, now + (1000 - idle%1000)*1000 + 1000, DUE_CLOCK);
            Buf b(&a, 64);
            print(&b, "no data for ");
            print(&b, idle/1000);
            print(&b, " s");
            if (red && v->watchat>nowms) {
                print(&b, ", giving up in ");
                print(&b, (v->watchat - nowms + 999)/1000);
                print(&b, " s");
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, red ? g->pal->bad : g->pal->amber);
            ImGui::TextUnformatted(cstr0(&a, finish(&b)));
            ImGui::PopStyleColor();
        } else {
            schedat(&g->sched, now + (AMBER_MS - idle)*1000 + 1000, DUE_CLOCK);
        }
        if (v->watchat && v->watchms && !red) {
            i64 redat = v->watchat - v->watchms/4;
            schedat(&g->sched, now + (redat - nowms)*1000 + 1000, DUE_CLOCK);
        }
    }
    if (v->state==LIVE_BACKOFF && v->retryat) {
        // The next attempt's model, if not the session's: a fallback's
        g->retryshown = 1;
        i64 secs = countdown(g, v->retryat, nowms);
        Str left = secstext(&a, secs);
        Buf b(&a, 128 + v->retrymodel.len);
        print(&b, v->retryslow ? Str("slow retry in ") : Str("retrying in "));
        print(&b, left);
        b32 other = v->retrymodel.len && v->retrymodel!=v->model;
        if (other) {
            print(&b, v->fallback ? Str(" on fallback ") : Str(" with "));
            print(&b, v->retrymodel);
            g->fallbackshown |= v->fallback > 0;
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->amber);
        ImGui::TextUnformatted(cstr0(&a, finish(&b)));
        ImGui::PopStyleColor();
    }
    if (v->asks) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->amber);
        ImGui::TextUnformatted("waiting for your answer above");
        ImGui::PopStyleColor();
    }
}

static void drawprompt(Gui *g)
{
    f32  sc   = g->scale;
    View *v   = g->view;
    b32  can  = g->sess && g->sess->view && !g->sess->readonly && !g->quitting &&
                (hostlive(g, g->sess) || g->enginekind==ENGINE_FAKE);
    drawstrip(g);
    escapecancel(g);

    ImGuiInputTextFlags flags = ImGuiInputTextFlags_WordWrap |
                                ImGuiInputTextFlags_EnterReturnsTrue |
                                ImGuiInputTextFlags_CtrlEnterForNewLine;
    f32 bw = 80*sc;
    ImVec2 size = v2(ImGui::GetContentRegionAvail().x - bw - ImGui::GetStyle().ItemSpacing.x,
                     ImGui::GetContentRegionAvail().y);
    b32 send = ImGui::InputTextMultiline("##prompt", g->prompt, PROMPT_MAX, size, flags);
    itemrect(g, RECT_PROMPT);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::BeginDisabled(!can);
    send |= ImGui::Button(v->busy ? "Steer" : "Send", v2(bw, 0));
    ImGui::SetItemTooltip(v->busy ? "Send to the running turn: the agent reads it at its next step (Enter)"
                                  : "Send the prompt (Enter; Shift+Enter for a new line)");
    if (v->busy) {
        if (ImGui::Button("Stop", v2(bw, 0))) {
            guicancel(g, g->sess);
        }
        ImGui::SetItemTooltip("Cancel the turn, killing any command it runs (Esc)");
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();
    if (send && can) {
        Str p = {(u8 *)g->prompt, (iz)__builtin_strlen(g->prompt)};
        p = trim(p);
        if (p.len) {
            guiprompt(g, p);
            g->prompt[0] = 0;
        }
        ImGui::SetKeyboardFocusHere(-1);
    }
}


// Sessions

static Str sesstitle(GuiSession *s)
{
    Str t = s->view && s->view->title.len ? s->view->title : s->title;
    return t.len ? t : Str("(untitled)");
}

static b32 sidafter(GuiSession **a, GuiSession **b)
{
    return compare((*a)->sid, (*b)->sid) > 0;
}

static Str sessparent(GuiSession *s)
{
    return s->view && s->view->parent.len ? s->view->parent : s->parent;
}

struct SessTree {
    Map<Slice<GuiSession *>> *kids;  // by parent sid, oldest first
    Slice<GuiSession *>       out;
    Arena                    *a;
};

static void sessplace(SessTree *t, GuiSession *s, i32 depth)
{
    if (s->depth >= 0) {
        return;
    }
    s->depth = depth;
    t->out   = push(t->a, t->out, s);
    Slice<GuiSession *> *k = upsert(&t->kids, s->sid, (Arena *)0);
    for (iz i = 0; k && i<k->len && depth<8; i++) {
        sessplace(t, (*k)[i], depth+1);
    }
}

static Slice<GuiSession *> hostsessions(Gui *g, Arena *a, GuiHost *h);

// The sessions made or opened here and listed, host by host, each
// host's newest first, but each subagent's under its parent's, oldest
// first, with its depth there.
static Slice<GuiSession *> sesslist(Gui *g, Arena *a)
{
    Slice<GuiSession *> r = {};
    for (GuiHost *h = g->hosts; h; h = h->next) {
        Slice<GuiSession *> l = hostsessions(g, a, h);
        for (iz i = 0; i < l.len; i++) {
            r = push(a, r, l[i]);
        }
    }
    return r;
}

// The sessions of one host, in the sidebar's order.
static Slice<GuiSession *> hostsessions(Gui *g, Arena *a, GuiHost *h)
{
    Slice<GuiSession *> all = {};
    for (GuiSession *s = g->sessions; s; s = s->next) {
        if (s->host==h && (s->view || s->listed)) {
            all = push(a, all, s);
            s->depth = -1;  // not placed yet
        }
    }
    if (!all.len) {
        return all;
    }
    sort(all.data, all.len, sidafter, *a);

    SessTree t = {};
    t.a = a;
    Map<GuiSession *> *bysid = 0;
    for (iz i = 0; i < all.len; i++) {
        *upsert(&bysid, all[i]->sid, a) = all[i];
    }
    for (iz i = all.len-1; i >= 0; i--) {
        Str p = sessparent(all[i]);
        if (p.len && upsert(&bysid, p, (Arena *)0)) {
            Slice<GuiSession *> *k = upsert(&t.kids, p, a);
            *k = push(a, *k, all[i]);
        }
    }
    for (iz i = 0; i < all.len; i++) {
        Str p = sessparent(all[i]);
        if (!p.len || !upsert(&bysid, p, (Arena *)0)) {
            sessplace(&t, all[i], 0);
        }
    }
    for (iz i = 0; i < all.len; i++) {
        sessplace(&t, all[i], 0);  // in a cycle of parents
    }

    // A parent shows that a subagent below it needs an answer
    for (iz i = 0; i < all.len; i++) {
        all[i]->kidask = 0;
    }
    for (iz i = 0; i < all.len; i++) {
        GuiSession *s = all[i];
        if (!s->view || !s->view->asks) continue;
        for (i32 n = 0; n < 8; n++) {
            Str          p  = sessparent(s);
            GuiSession **up = p.len ? upsert(&bysid, p, (Arena *)0) : 0;
            if (!up || *up==all[i]) break;
            s = *up;
            s->kidask = 1;
        }
    }
    return t.out;
}

static u32 hostcolor(Gui *g, GuiHost *h)
{
    if (h->kind == GH_ENGINE) {
        return g->enginegone ? g->pal->bad : g->pal->ok;
    }
    switch (h->state) {
    case RS_UP:         return g->pal->ok;
    case RS_CONNECTING:
    case RS_DOWN:
    case RS_DEPLOYING:  return g->pal->amber;
    case RS_FAILED:     return g->pal->bad;
    }
    return g->pal->dim;
}

// A host's line above its sessions in the sidebar: a dot in the color of
// its connection, its name, and how it is, a countdown while down.
static void drawhosthead(Gui *g, Arena a, GuiHost *h, i64 nowms)
{
    f32         sc    = g->scale;
    ImDrawList *dl    = ImGui::GetWindowDrawList();
    f32         lineh = ImGui::GetTextLineHeight();
    ImVec2      p     = ImGui::GetCursorScreenPos();
    f32         w     = ImGui::GetContentRegionAvail().x;
    i64         left  = h->state==RS_DOWN ? countdown(g, h->retryat, nowms) : 0;
    g->downshown |= h->state==RS_DOWN && left>0;
    drawdot(dl, v2(p.x + 4*sc, p.y + lineh/2), 3.5f*sc, hostcolor(g, h));
    Pen t = {dl, &g->fonts, p.x + 13*sc, p.y, p.x + w, g->fonts.size};
    pen(&t, hostlabel(g, &a, h), STYLE_BOLD, g->pal->text);
    pen(&t, "  ", 0, g->pal->dim);
    pen(&t, hoststatus(g, &a, h, left), 0, hostcolor(g, h)==g->pal->ok ? g->pal->dim
                                                                     : hostcolor(g, h));
    ImGui::Dummy(v2(w, lineh + 2*sc));
    if (h->msg.len && h->state!=RS_UP) {
        ImGui::SetItemTooltip("%s", cstr0(&a, h->msg));
    }
}

static void drawsessions(Gui *g)
{
    f32   sc = g->scale;
    Arena a  = g->frame;
    i64   nowms = os_time(g->os);
    if (g->enginekind != ENGINE_FAKE) {
        if (ImGui::SmallButton("Refresh")) {
            guilistall(g);
        }
        ImGui::SetItemTooltip("List the sessions again");
    }
    // Heads for each host when there is more than one to show
    b32 heads = 0;
    for (GuiHost *h = g->hosts; h; h = h->next) {
        heads |= h->kind!=GH_ENGINE;
    }
    Slice<GuiSession *> list = sesslist(g, &a);
    b32         other = 0;  // the checklist's first session not shown
    ImDrawList *dl    = ImGui::GetWindowDrawList();
    f32         lineh = ImGui::GetTextLineHeight();
    f32         small = g->fonts.size * 0.85f;
    GuiHost    *head  = 0;
    for (iz i = 0; i < list.len; i++) {
        GuiSession *s     = list[i];
        View       *v     = s->view;
        i32         state = v ? v->state : s->state;
        if (heads && s->host!=head) {
            head = s->host;
            if (i) ImGui::Spacing();
            drawhosthead(g, a, head, nowms);
        }
        pushstr(s->sid);
        ImVec2 p = ImGui::GetCursorScreenPos();
        f32    w = ImGui::GetContentRegionAvail().x;
        if (ImGui::Selectable("##s", s==g->sess, 0, v2(0, 2*lineh + 4*sc))) {
            guiselect(g, s);
        }
        ImVec2 r0 = ImGui::GetItemRectMin();
        ImVec2 r1 = ImGui::GetItemRectMax();
        s->row = {r0.x, r0.y, r1.x, r1.y};
        if (!other && s!=g->sess) {
            itemrect(g, RECT_OTHERSESS);
            other = 1;
        }
        Str cwd = v && v->cwd.len ? v->cwd : s->cwd;
        ImGui::SetItemTooltip("%s\n%s", cstr0(&a, cwd), cstr0(&a, s->sid));
        f32 x = p.x + 4*sc + (f32)s->depth*14*sc;  // a subagent under its parent

        // A dot in the color of its state, amber when it, or a subagent
        // below it, waits for the user
        b32 ro    = s->readonly || (!v && s->locked);
        b32 asks  = v && v->asks;
        u32 color = ro || asks || s->kidask ? g->pal->amber : livecolor(g->pal, state);
        drawdot(dl, v2(x + 4*sc, p.y + 2*sc + lineh/2), 3.5f*sc, color);
        x += 13*sc;

        Pen t = {dl, &g->fonts, x, p.y + 2*sc, p.x + w, g->fonts.size};
        pen(&t, sesstitle(s), 0, g->pal->text);
        Pen b = {dl, &g->fonts, x, p.y + 2*sc + lineh, p.x + w, small};
        if (s->depth && v && v->role.len) {
            pen(&b, v->role, 0, g->pal->dim);
            pen(&b, "  ", 0, g->pal->dim);
        }
        if (s->host->kind == GH_LOCAL) {
            pen(&b, "background", STYLE_BOLD, g->pal->active);
            pen(&b, "  ", 0, g->pal->dim);
            g->bgbadge += s == g->sess;
        }
        if (s->host->kind!=GH_ENGINE && !s->host->up) {
            // Its link comes back by itself, or not at all
            b32 back = s->host->r && !s->host->closing;
            pen(&b, back ? Str("reconnecting") : Str("disconnected"), STYLE_BOLD,
                back ? g->pal->amber : g->pal->dim);
        } else if (ro) {
            pen(&b, "read-only", STYLE_BOLD, g->pal->amber);
        } else if (asks) {
            pen(&b, "needs an answer", STYLE_BOLD, g->pal->amber);
        } else if (s->kidask) {
            pen(&b, "a subagent needs an answer", STYLE_BOLD, g->pal->amber);
            g->kidaskshown |= s == g->sess;
        } else {
            pen(&b, livelabel(v, state), STYLE_BOLD, livecolor(g->pal, state));
            if (v && state==LIVE_QUEUED && v->queuepos) {
                Buf q(&a, 32);
                print(&q, " ");
                print(&q, (i64)v->queuepos);
                print(&q, " of ");
                print(&q, (i64)(v->queuelen>v->queuepos ? v->queuelen : v->queuepos));
                pen(&b, finish(&q), 0, g->pal->dim);
            }
        }
        i64 jobs = v ? v->jobsrun : s->jobs;
        if (jobs) {
            Buf q(&a, 32);
            print(&q, "  ");
            print(&q, jobs);
            print(&q, jobs==1 ? Str(" job") : Str(" jobs"));
            pen(&b, finish(&q), STYLE_BOLD, g->pal->active);
        }
        pen(&b, "  ", 0, g->pal->dim);
        pen(&b, cwd, 0, g->pal->dim);
        ImGui::PopID();
    }
    // Remote hosts with nothing to show yet, but how they are
    for (GuiHost *h = g->hosts; heads && h; h = h->next) {
        b32 any = 0;
        for (iz i = 0; i < list.len; i++) {
            any |= list[i]->host == h;
        }
        if (!any && h->kind==GH_REMOTE && h->state!=RS_CLOSED) {
            ImGui::Spacing();
            drawhosthead(g, a, h, nowms);
        }
    }
    if (!list.len) {
        ImGui::TextDisabled("No sessions yet.");
    }
}

// Todo list and background jobs, of the session shown

// The agent's todo list, as it last wrote it: a mark for each item's
// state, then the item, wrapped.
static void drawtodo(Gui *g)
{
    View *v = g->view;
    g->todoshown = 0;
    if (!g->sess || !v->todos.len) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("No todo list. The agent keeps one for longer tasks.");
        ImGui::PopTextWrapPos();
        return;
    }
    Palette    *pal  = g->pal;
    f32         sc   = g->scale;
    ImDrawList *dl   = ImGui::GetWindowDrawList();
    f32         lh   = ImGui::GetTextLineHeight();
    f32         r    = lh*0.28f;
    i32         done = 0;
    for (iz i = 0; i < v->todos.len; i++) {
        done += v->todos[i].status == VTODO_DONE;
    }
    ImGui::TextDisabled("%d of %d done", done, (i32)v->todos.len);
    ImVec2 top = ImGui::GetCursorScreenPos();
    ImGui::PushTextWrapPos(0);
    for (iz i = 0; i < v->todos.len; i++) {
        ViewTodo *t   = &v->todos[i];
        ImVec2    p   = ImGui::GetCursorScreenPos();
        ImVec2    c   = v2(p.x + lh*0.5f, p.y + lh*0.5f);
        u32       col = pal->paneltext;
        switch (t->status) {
        case VTODO_PENDING:
            dl->AddCircle(c, r, pal->dim, 12, 1.5f*sc);
            break;
        case VTODO_ACTIVE:
            dl->AddCircle(c, r, pal->active, 12, 1.5f*sc);
            dl->AddCircleFilled(c, r*0.55f, pal->active, 12);
            col = pal->active;
            break;
        case VTODO_DONE: {
            ImVec2 pts[] = {v2(c.x-r, c.y), v2(c.x-r*0.25f, c.y+r*0.75f), v2(c.x+r, c.y-r*0.8f)};
            dl->AddPolyline(pts, countof(pts), pal->ok, 2*sc);
            col = pal->paneldim;
        } break;
        }
        ImGui::Dummy(v2(lh*1.2f, lh));
        ImGui::SameLine(0, 0);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        text(t->content);
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    ImVec2 end = ImGui::GetCursorScreenPos();
    g->rects[RECT_TODO] = {top.x, top.y, top.x+ImGui::GetContentRegionAvail().x, end.y};
    g->todoshown = (i32)v->todos.len;
}

// The last n lines of s.
static Str lastlines(Str s, i32 n)
{
    for (; s.len && (s[s.len-1]=='\n' || s[s.len-1]=='\r'); s.len--) {}
    iz i = s.len;
    for (i32 k = 0; i > 0; i--) {
        if (s[i-1]=='\n' && ++k==n) break;
    }
    return cuthead(s, i);
}

static void guijobkill(Gui *, GuiSession *, i64);

// The session's background jobs, newest first: state, number, how long,
// a Kill button while it runs, its command, and its output's end. A
// running job's clock asks for a frame a second, only while shown.
static void drawjobs(Gui *g)
{
    View *v = g->view;
    g->jobsshown = 0;
    if (!g->sess || !v->jobs.len) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("No background jobs. Commands the agent runs in the "
                            "background, or that outlast their timeout, show here.");
        ImGui::PopTextWrapPos();
        return;
    }
    Arena       a     = g->frame;
    i64         nowms = os_time(g->os);
    ImGuiStyle &st    = ImGui::GetStyle();
    f32         killw = ImGui::CalcTextSize("Stopping...").x + 2*st.FramePadding.x;
    b32         can   = !g->sess->readonly && hostlive(g, g->sess) && !g->quitting;
    b32         first = 1;
    for (iz i = v->jobs.len-1; i >= 0; i--) {
        ViewJob *j = &v->jobs[i];
        ImGui::PushID((i32)i);
        Buf b(&a, 64);
        switch (j->status) {
        case VJOB_EXITED:
            print(&b, "exited ");
            print(&b, (i64)j->exit);
            break;
        case VJOB_KILLED:
            print(&b, j->signal ? Str("killed, signal ") : Str("killed"));
            if (j->signal) print(&b, (i64)j->signal);
            break;
        default:
            print(&b, vjobnames[j->status]);
        }
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, jobcolor(g->pal, j));
        text(finish(&b));
        ImGui::PopStyleColor();
        b.len = 0;
        print(&b, "job ");
        print(&b, j->n);
        print(&b, "  ");
        if (j->status == VJOB_RUNNING) {
            i64 el = nowms - j->start;
            el = el<0 ? 0 : el;
            schedat(&g->sched, g->sched.now + (1000 - el%1000)*1000 + 1000, DUE_CLOCK);
            print(&b, secstext(&a, el/1000));
        } else if (j->ms) {
            print(&b, duration(&a, j->ms));
        }
        if (j->bytes) {
            print(&b, "  ");
            print(&b, bytesize(&a, j->bytes));
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", cstr0(&a, finish(&b)));
        if (j->status==VJOB_RUNNING && can) {
            f32 right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - killw;
            ImGui::SameLine(right > rightof() ? right : 0);
            ImGui::BeginDisabled(j->killing);
            b32 kill = ImGui::Button(j->killing ? "Stopping..." : "Kill", v2(killw, 0));
            ImGui::EndDisabled();
            if (first) {
                itemrect(g, RECT_JOBKILL);
                first = 0;
            }
            ImGui::SetItemTooltip("Stop the job (and what it started); the agent is told");
            if (kill) {
                j->killing = 1;
                guijobkill(g, g->sess, j->n);
            }
        }

        ImGui::PushFont(g->fonts.mono, 0);
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->summary);
        text(j->summary);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && j->spill.len) {
            Str full = v->dir.len ? join(&a, v->dir, j->spill) : j->spill;
            ImGui::SetItemTooltip("%s\nFull output: %s", cstr0(&a, j->command), cstr0(&a, full));
        }
        Str tail = lastlines({j->tail, j->taillen}, 4);
        if (tail.len) {
            ImGui::PushStyleColor(ImGuiCol_Text, g->pal->paneldim);
            text(tail);
            ImGui::PopStyleColor();
        }
        ImGui::PopFont();
        ImGui::Separator();
        ImGui::PopID();
        g->jobsshown++;
    }
}

// The new session dialog: a working directory, typed or from those of
// earlier sessions, and a model, by default the configured one.
static void drawnew(Gui *g)
{
    f32 sc = g->scale;
    if (g->opennew) {
        ImGui::OpenPopup("New session");
        g->opennew = 0;
    }
    ImGui::SetNextWindowSize(v2(560*sc, 0));
    if (!ImGui::BeginPopupModal("New session", 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    Arena a = g->frame;

    // Where it runs, when there is a choice: its directory is there
    GuiHost *host  = g->newhost && hostready(g, g->newhost) ? g->newhost : g->main;
    i32      ready = 0;
    for (GuiHost *h = g->hosts; h; h = h->next) {
        ready += hostready(g, h);
    }
    if (ready > 1) {
        ImGui::TextUnformatted("Host");
        ImGui::SetNextItemWidth(-1);
        ImVec2 at = ImGui::GetCursorScreenPos();
        if (ImGui::BeginCombo("##host", cstr0(&a, hostlabel(g, &a, host)))) {
            i32 n = 0;
            for (GuiHost *h = g->hosts; h; h = h->next) {
                if (!hostready(g, h)) continue;
                ImGui::PushID(h);
                if (ImGui::Selectable(cstr0(&a, hostlabel(g, &a, h)), h==host)) {
                    g->newhost = h;
                }
                if (n < 4) {
                    itemrect(g, RECT_HOST0 + n);
                    g->hostitems[n] = h;
                }
                n++;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        f32 w = ImGui::GetContentRegionAvail().x;
        g->rects[RECT_HOSTCOMBO] = {at.x, at.y, at.x+w, at.y+ImGui::GetFrameHeight()};
        ImGui::Spacing();
    }

    ImGui::TextUnformatted("Working directory");
    if (host->kind == GH_REMOTE) {
        ImGui::SameLine();
        ImGui::TextDisabled("on %s", cstr0(&a, host->name));
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputText("##cwd", g->newcwd, FIELD_MAX, ImGuiInputTextFlags_AutoSelectAll);
    itemrect(g, RECT_NEWCWD);

    // Recent directories there, newest first
    Slice<GuiSession *> list   = sesslist(g, &a);
    Slice<Str>          recent = {};
    for (iz i = 0; i<list.len && recent.len<8; i++) {
        Str cwd = list[i]->view && list[i]->view->cwd.len ? list[i]->view->cwd : list[i]->cwd;
        b32 dup = !cwd.len || list[i]->host!=host;
        for (iz k = 0; !dup && k < recent.len; k++) {
            dup = recent[k] == cwd;
        }
        if (!dup) {
            recent = push(&a, recent, cwd);
        }
    }
    if (recent.len) {
        ImGui::TextDisabled("Recent");
        for (iz i = 0; i < recent.len; i++) {
            ImGui::PushID((i32)i);
            if (ImGui::Selectable(cstr0(&a, recent[i]))) {
                iz n = recent[i].len<FIELD_MAX ? recent[i].len : FIELD_MAX-1;
                copybytes(g->newcwd, recent[i].data, n);
                g->newcwd[n] = 0;
            }
            ImGui::PopID();
        }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Model");
    ImGui::SetNextItemWidth(-1);
    Str dflt = {};
    for (iz i = 0; i < g->cfgrows.len; i++) {
        CfgRow *r = &g->cfgrows[i];
        if (!r->kind.len && r->key=="model") {
            dflt = r->value;
        }
    }
    char const *hint = host->kind==GH_REMOTE ? "as configured there" :
                       dflt.len ? cstr0(&a, concat(&a, "configured: ", dflt)) : "the model's name";
    ImGui::InputTextWithHint("##model", hint, g->newmodel, FIELD_MAX);
    if (g->newerr.len) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
        ImGui::TextWrapped("%s", cstr0(&a, g->newerr));
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    b32 create = ImGui::Button("Create", v2(120*sc, 0));
    itemrect(g, RECT_NEWCREATE);
    if (create) {
        Str cwd   = trim({(u8 *)g->newcwd, (iz)__builtin_strlen(g->newcwd)});
        Str model = trim({(u8 *)g->newmodel, (iz)__builtin_strlen(g->newmodel)});
        guinew(g, host, cwd, model);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", v2(120*sc, 0))) {
        g->newerr = {};
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Closing the window with turns running asks first: stop them and quit,
// or keep them running in the background, this process a host of them
// with no window. Sessions at work on other hosts run on either way.
static void drawclose(Gui *g)
{
    f32 sc = g->scale;
    if (g->closereq) {
        g->closereq = 0;
        if (!g->quitting && !running(g) && !elsewhere(g)) {
            startquit(g);
        } else if (!g->quitting) {
            g->hosterr = {};
            g->whynobg = clone(&g->perm, cannothost(g));
            ImGui::OpenPopup("Quit Prospero?");
        }
    }
    if (!ImGui::BeginPopupModal("Quit Prospero?", 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    Arena a = g->frame;
    if (g->host && !g->shedreq) {
        // Open as the window went to the background: nothing to ask now
        ImGui::CloseCurrentPopup();
    } else if (g->quitting) {
        ImGui::TextUnformatted("Stopping the running sessions...");
    } else if (g->shedreq) {
        ImGui::TextUnformatted("Going to the background...");
    } else {
        i32 n = running(g);
        i32 m = elsewhere(g);
        Buf b(&a, 128);
        print(&b, "Stop ");
        print(&b, (i64)n);
        print(&b, n==1 ? Str(" session and quit") : Str(" sessions and quit"));
        Str label = finish(&b);
        if (n) {
            ImGui::TextUnformatted("Turns are still running. Quitting stops them, and kills "
                                   "their commands; in the background they go on, with no "
                                   "window.");
        } else {
            ImGui::TextUnformatted("Nothing is running here any more.");
        }
        if (m) {
            Buf e(&a, 128);
            print(&e, (i64)m);
            print(&e, m==1 ? Str(" session at work on another host keeps running")
                           : Str(" sessions at work on other hosts keep running"));
            print(&e, ", remote or in the background, whatever happens here.");
            ImGui::TextUnformatted(cstr0(&a, finish(&e)));
            g->elseshown = 1;
        }
        b32 stop = ImGui::Button(cstr0(&a, n ? label : Str("Quit")));
        itemrect(g, RECT_CLOSESTOP);
        if (stop) {
            startquit(g);
        }
        if (n) {
            ImGui::SameLine();
            ImGui::BeginDisabled(g->whynobg.len != 0);
            b32 bg = ImGui::Button("Keep running in background");
            ImGui::EndDisabled();
            itemrect(g, RECT_CLOSEBG);
            if (g->whynobg.len) {
                ImGui::SetItemTooltip("Not here: %s", cstr0(&a, g->whynobg));
            } else {
                ImGui::SetItemTooltip("Close the window, and keep this process running its "
                                      "sessions, unattended, with a tray icon to bring it back");
            }
            if (bg) {
                shedlater(g, 1);
            }
        }
        ImGui::SameLine();
        b32 cancel = ImGui::Button("Cancel", v2(120*sc, 0));
        itemrect(g, RECT_CLOSECANCEL);
        if (cancel) {
            ImGui::CloseCurrentPopup();
        }
        if (g->hosterr.len) {
            ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
            ImGui::TextWrapped("%s", cstr0(&a, g->hosterr));
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndPopup();
}

// The Output window: a file of a session's directory, as its host gave
// it, in parts, the next on demand.
static void drawoutput(Gui *g)
{
    Arena    a = g->frame;
    OutFile *o = &g->out;
    if (!o->s) {
        ImGui::TextDisabled("Nothing to show: a tool row's Output shows its output here.");
        return;
    }
    Buf b(&a, 256);
    print(&b, o->path);
    print(&b, " of ");
    print(&b, sesstitle(o->s));
    if (o->s->host->kind != GH_ENGINE) {
        print(&b, ", on ");
        print(&b, hostlabel(g, &a, o->s->host));
    }
    if (o->size) {
        print(&b, ": ");
        print(&b, bytesize(&a, o->size));
    }
    ImGui::TextDisabled("%s", cstr0(&a, finish(&b)));
    if (o->more) {
        ImGui::SameLine();
        ImGui::BeginDisabled(o->id != 0);
        if (ImGui::SmallButton("More")) {
            guifile(g);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("The next part of it");
    }
    if (o->err.len) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
        ImGui::TextWrapped("%s", cstr0(&a, o->err));
        ImGui::PopStyleColor();
    } else if (o->id && !o->text.len) {
        ImGui::TextDisabled("Loading...");
    }
    ImGui::BeginChild("##out", v2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PushFont(g->fonts.mono, 0);
    Str t = finish(&o->text);
    if (t.len) {
        ImGui::TextUnformatted((char *)t.data, (char *)t.data + t.len);
        g->outshown = 1;
    }
    ImGui::PopFont();
    ImGui::EndChild();
}

// The Screenshot window: a screenshot whole, at its own size in the
// screen's pixels, or fit to the window.
static void drawshot(Gui *g)
{
    Arena    a = g->frame;
    ImgView *v = &g->img.view;
    if (!v->s) {
        return;
    }
    i32 w = v->tex ? v->tex->td.Width  : v->w;
    i32 h = v->tex ? v->tex->td.Height : v->h;
    Buf b(&a, 512);
    print(&b, imgstr(&v->path));
    print(&b, " of ");
    print(&b, sesstitle(v->s));
    if (v->s->host->kind != GH_ENGINE) {
        print(&b, ", on ");
        print(&b, hostlabel(g, &a, v->s->host));
    }
    print(&b, ": ");
    print(&b, (i64)w);
    putbyte(&b, 'x');
    print(&b, (i64)h);
    print(&b, " pixels");
    ImGui::TextDisabled("%s", cstr0(&a, finish(&b)));
    ImGui::SameLine();
    checkbox("Fit", &v->fit);
    ImGui::SetItemTooltip("Scale it down to fit the window");
    ImGui::SameLine();
    b32 close = ImGui::SmallButton("Close");
    itemrect(g, RECT_SHOTCLOSE);
    if (v->state == IMG_FAILED) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
        ImGui::TextWrapped("Cannot show the screenshot: %s", cstr0(&a, imgstr(&v->err)));
        ImGui::PopStyleColor();
    } else if (v->state != IMG_READY) {
        ImGui::TextDisabled("Loading...");
    }
    ImGui::BeginChild("##shot", v2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (v->state == IMG_READY) {
        f32    fb   = ImGui::GetIO().DisplayFramebufferScale.x;
        ImVec2 size = v2((f32)w/(fb>0 ? fb : 1), (f32)h/(fb>0 ? fb : 1));
        if (v->fit) {
            ImVec2 room = ImGui::GetContentRegionAvail();
            f32    s    = room.x/size.x < room.y/size.y ? room.x/size.x : room.y/size.y;
            s    = s<1 ? s : 1;
            s    = s>0 ? s : 1;
            size = v2(size.x*s, size.y*s);
        }
        ImGui::Image(v->tex->td.GetTexRef(), size);
        g->shotshown = 1;
    }
    ImGui::EndChild();
    if (close) {
        imgclose(g);
    }
}

// Fitting a narrow window, which then has nothing to scroll to sideways

// Text cut to width with an ellipsis, then whole in its tooltip, ahead
// of tip, which is the tooltip otherwise. Color 0 is the text's own.
// It takes no more width than it is given. Returns 1 if cut.
static b32 textfit(Arena a, Str s, f32 width, u32 color, Str tip = {})
{
    char const *beg = s.cdata;
    char const *end = s.cdata + s.len;
    b32         cut = ImGui::CalcTextSize(beg, end).x > width;
    if (color) {
        ImGui::PushStyleColor(ImGuiCol_Text, color);
    }
    if (!cut) {
        ImGui::TextUnformatted(beg, end);
    } else {
        width = width>1 ? width : 1;
        ImVec2 p = ImGui::GetCursorScreenPos();
        f32    h = ImGui::GetTextLineHeight();
        p.y += ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset;
        ImGui::Dummy(v2(width, h));
        ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), p, v2(p.x+width, p.y+h),
                                  p.x+width, beg, end, 0);
    }
    if (color) {
        ImGui::PopStyleColor();
    }
    if (cut || tip.len) {
        Buf b(&a, s.len + tip.len + 2);
        if (cut) {
            print(&b, s);
            print(&b, tip.len ? Str("\n") : Str(""));
        }
        print(&b, tip);
        ImGui::SetItemTooltip("%s", cstr0(&a, finish(&b)));
    }
    return cut;
}

// Whether an item w wide fits after the last one on its line, then
// SameLine if it does.
static b32 samelineif(f32 w)
{
    f32 right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    f32 x     = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x;
    if (x+w > right) {
        return 0;
    }
    ImGui::SameLine();
    return 1;
}


// Hosts

// A configured remote host's line: a dot in the color of its connection,
// its name and how it is, cut to leave room for Connect or Disconnect,
// and why it failed. The button's rectangle goes to RECT_* rect, if not
// negative.
static void drawremote(Gui *g, Str name, i32 rect)
{
    Arena       a       = g->frame;
    f32         sc      = g->scale;
    ImGuiStyle &st      = ImGui::GetStyle();
    i64         nowms   = os_time(g->os);
    GuiHost    *h       = findhost(g, GH_REMOTE, name);
    b32         live    = h && h->r && !h->closing;
    b32         closing = h && h->r && h->closing;
    i64         left    = h && h->state==RS_DOWN ? countdown(g, h->retryat, nowms) : 0;
    pushstr(name);
    ImVec2 p  = ImGui::GetCursorScreenPos();
    f32    lh = ImGui::GetTextLineHeight();
    drawdot(ImGui::GetWindowDrawList(), v2(p.x + 4*sc, p.y + lh/2 + st.FramePadding.y),
            3.5f*sc, h ? hostcolor(g, h) : g->pal->dim);
    ImGui::Dummy(v2(10*sc, 0));
    ImGui::SameLine();
    Buf b(&a, 128);
    print(&b, h ? hostlabel(g, &a, h) : name);
    print(&b, ": ");
    print(&b, h ? hoststatus(g, &a, h, left) : Str("not connected"));
    char const *label = live ? "Disconnect" : "Connect";
    f32         bw    = ImGui::CalcTextSize(label).x + 2*st.FramePadding.x;
    ImGui::AlignTextToFramePadding();
    textfit(a, finish(&b), ImGui::GetContentRegionAvail().x - bw - st.ItemSpacing.x, 0);
    ImGui::SameLine();
    ImGui::BeginDisabled(closing && h->reconnect);
    b32 click = ImGui::SmallButton(label);
    ImGui::EndDisabled();
    if (rect >= 0) {
        itemrect(g, rect);
    }
    ImGui::SetItemTooltip(live ? "Leave it: its sessions run on there" :
                          closing ? "Connect again once it has let go" :
                                    "Connect over ssh, starting a host there if none runs");
    if (click && live) {
        hostclose(g, h);
    } else if (click) {
        remoteconnect(g, a, name);
    }
    if (h && h->msg.len && h->state!=RS_UP) {
        ImGui::PushStyleColor(ImGuiCol_Text, h->state==RS_FAILED ? g->pal->bad : g->pal->amber);
        ImGui::TextWrapped("%s", cstr0(&a, h->msg));
        ImGui::PopStyleColor();
        g->failshown |= h->state == RS_FAILED;
    }
    ImGui::PopID();
}

// This process's engine, the background hosts on this machine, and the
// remote ones of the configuration: how each is, connecting and leaving
// them, and a new remote added to the configuration.
static void drawhosts(Gui *g)
{
    Arena a     = g->frame;
    f32   sc    = g->scale;
    i64   nowms = os_time(g->os);
    if (g->enginekind == ENGINE_FAKE) {
        ImGui::TextDisabled("No engine: the fake agent has no hosts.");
        return;
    }
    if (ImGui::SmallButton("Look again")) {
        findhosts(g, a);
        guiconfigget(g);
    }
    ImGui::SetItemTooltip("Find background hosts again, and read the remotes from the configuration");

    // This process
    ImGui::SeparatorText("This computer");
    Buf tb(&a, 256);
    if (g->host) {
        print(&tb, "This process runs its sessions in the background, as process ");
        print(&tb, (i64)os_pid(g->os));
        print(&tb, ", at ");
        print(&tb, g->host->endpoint);
    } else {
        print(&tb, "The engine runs ");
        print(&tb, g->enginekind==ENGINE_INPROC ? Str("in this process: ")
                                                : Str("as a process of its own: "));
        print(&tb, hoststatus(g, &a, g->hosts, 0));
    }
    ImGui::TextWrapped("%s", cstr0(&a, finish(&tb)));
    for (GuiHost *h = g->hosts; h; h = h->next) {
        if (h->kind!=GH_LOCAL || h->own) continue;
        ImGui::PushID(h);
        ImGui::Bullet();
        Buf b(&a, 128);
        print(&b, hostlabel(g, &a, h));
        print(&b, ": ");
        print(&b, hoststatus(g, &a, h, 0));
        if (h->started) {
            print(&b, ", started ");
            printage(g, &b, nowms - h->started);
            print(&b, " ago");
        }
        ImGuiStyle &st = ImGui::GetStyle();
        f32         bw = ImGui::CalcTextSize("Stop").x + 2*st.FramePadding.x;
        textfit(a, finish(&b), ImGui::GetContentRegionAvail().x - bw - st.ItemSpacing.x, 0);
        ImGui::SameLine();
        ImGui::BeginDisabled(!h->up);
        if (ImGui::SmallButton("Stop")) {
            Arena c = newarena(g->cmdr);
            Buf   cb(&c, 128);
            EvOut w = cmdopen(g, &cb, CMD_QUIT, 0, h);
            putkey(&w.json, "mode");
            putstr(&w.json, "stop");
            cmdsend(g, &w, c);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Stop its sessions' work, and the host");
        ImGui::PopID();
    }

    // Remotes, as configured
    ImGui::SeparatorText("Remote hosts");
    Slice<Str> names = {};
    for (iz i = 0; i < g->cfgrows.len; i++) {
        CfgRow *r = &g->cfgrows[i];
        b32 seen = r->kind != "remote";
        for (iz n = 0; !seen && n < names.len; n++) {
            seen = names[n] == r->name;
        }
        if (!seen) {
            names = push(&a, names, r->name);
        }
    }
    for (GuiHost *h = g->hosts; h; h = h->next) {
        b32 seen = h->kind != GH_REMOTE;
        for (iz n = 0; !seen && n < names.len; n++) {
            seen = names[n] == h->name;
        }
        if (!seen) {
            names = push(&a, names, h->name);  // gone from the file since
        }
    }
    if (!names.len) {
        ImGui::TextDisabled("None yet: add one below.");
    }
    for (iz n = 0; n < names.len; n++) {
        // The first two's buttons, for the checklist
        drawremote(g, names[n], n<2 ? RECT_CONNECT+(i32)n : -1);
    }

    // A new one, which the configuration keeps: its fields side by side
    // when they fit, else one above another
    ImGui::SeparatorText("Add a remote host");
    b32 row = ImGui::GetContentRegionAvail().x >= 440*sc;
    ImGui::SetNextItemWidth(row ? 120*sc : -1);
    ImGui::InputTextWithHint("##remname", "name", g->remname, FIELD_MAX);
    itemrect(g, RECT_REMNAME);
    if (row) {
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(row ? 200*sc : -1);
    ImGui::InputTextWithHint("##remssh", "ssh destination", g->remssh, FIELD_MAX);
    itemrect(g, RECT_REMSSH);
    ImGui::SetItemTooltip("user@host, or a host from ~/.ssh/config");
    if (row) {
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##rempath", "prospero there (optional)", g->rempath, FIELD_MAX);
    b32 add = ImGui::Button("Add remote");
    itemrect(g, RECT_REMADD);
    if (add) {
        Str name = trim({(u8 *)g->remname, (iz)__builtin_strlen(g->remname)});
        Str ssh  = trim({(u8 *)g->remssh, (iz)__builtin_strlen(g->remssh)});
        Str path = trim({(u8 *)g->rempath, (iz)__builtin_strlen(g->rempath)});
        if (!name.len || !ssh.len) {
            g->remstatus = "A remote needs a name and an ssh destination";
            g->remok     = 0;
        } else if (ssh[0] == '-') {
            g->remstatus = "An ssh destination cannot begin with -";
            g->remok     = 0;
        } else {
            guiconfigset(g, "remote", name, "ssh", ssh);
            if (path.len) {
                guiconfigset(g, "remote", name, "path", path);
            }
            g->remstatus = clone(&g->perm, concat(&a, concat(&a, "Added [remote ", name), "]"));
            g->remok     = 1;
            g->remname[0] = g->remssh[0] = g->rempath[0] = 0;
        }
    }
    if (g->remstatus.len) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, g->remok ? g->pal->ok : g->pal->bad);
        ImGui::TextWrapped("%s", cstr0(&a, g->remstatus));
        ImGui::PopStyleColor();
    }
    g->hostshscroll = ImGui::GetScrollMaxX();  // for the checklist
    g->hostsframe   = g->total.frames;
}


// Settings
//
// Two panes when the window is wide enough: a list of pages on the left
// and the page on the right; narrower, the list is a combo above the
// page. The pages are General's (the top level and each kind of section
// without names), each named section under its kind with an Add at the
// end, and Diagnostics. A page is a table of its settings: the key, the
// value to edit, and where it is set, which goes under the key when the
// page is narrow. Nothing is drawn wider than the room it has, text cut
// with an ellipsis and whole in a tooltip instead, so the window never
// has anything to scroll to sideways.

enum : i32 {
    CFG_TWOPANE = 480,  // width at scale 1 for the list beside the page
    CFG_MINVAL  = 160,  // the value column's least, at scale 1, for a
                        // column of where each is set beside it
};

// "from prospero.ini:12" from a path and line, or "default", which is
// "default, empty at prospero.ini:12" when set empty, and "like large,
// from prospero.ini:12" for what a model inherits. A URL include is
// named in full, with the age of its copy as of now (Unix epoch ms).
static Str provenance(Gui *g, Arena *a, CfgRow *r, i64 now)
{
    if (!r->file.len) {
        return "default";
    }
    Str name = r->file;
    for (iz i = iniurl(name) ? -1 : name.len-1; i >= 0; i--) {
        if (name[i]=='/' || name[i]=='\\') {
            name = cuthead(name, i+1);
            break;
        }
    }
    Buf b(a, 64);
    if (r->like.len) {
        print(&b, "like ");
        print(&b, r->like);
        print(&b, r->isdflt ? Str(", empty at ") : Str(", from "));
    } else {
        print(&b, r->isdflt ? Str("default, empty at ") : Str("from "));
    }
    print(&b, name);
    putbyte(&b, ':');
    print(&b, r->line);
    if (r->fetched) {
        print(&b, ", fetched ");
        printage(g, &b, now - r->fetched);
        print(&b, " ago");
    }
    return finish(&b);
}

// A setting's row: its key, its value to edit, where it is set, which
// with wide is a column of its own, else a line under the key. Enums
// are picked from their choices, paths edited one a line, and the rest
// typed. The checklist's target row records its field's rectangle
// (RECT_*, or -1 for none).
static void drawcfgrow(Gui *g, CfgRow *r, i32 target, b32 wide)
{
    Arena a = g->frame;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    Buf tb(&a, 128);
    print(&tb, r->desc);
    print(&tb, "\n(");
    print(&tb, r->type);
    print(&tb, ", default ");
    print(&tb, r->dflt.len ? r->dflt : Str("none"));
    putbyte(&tb, ')');
    f32 cw   = ImGui::GetContentRegionAvail().x;
    Str from = r->valid ? provenance(g, &a, r, os_time(g->os)) : Str("invalid: default used");
    u32 fc   = r->valid ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : g->pal->bad;
    ImGui::AlignTextToFramePadding();
    textfit(a, r->key, cw, 0, finish(&tb));
    if (!wide) {
        textfit(a, from, cw, fc);
    }
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1);
    ImGui::PushID(r);
    if (r->choices.len) {
        // A pick goes out at once; the theme and how reasoning shows
        // apply even before the file has them
        b32    theme = r->kind=="ui" && r->key=="theme";
        ImVec2 at    = ImGui::GetCursorScreenPos();
        Str    pick  = {};
        if (ImGui::BeginCombo("##v", cstr0(&a, r->value))) {
            for (iz i = 0; i < r->choices.len; i++) {
                Str c = r->choices[i];
                if (ImGui::Selectable(cstr0(&a, c), c==r->value)) {
                    pick = c;
                }
                if (theme && i<THEME_COUNT) {
                    itemrect(g, RECT_THEME0+(i32)i);
                }
            }
            ImGui::EndCombo();
        }
        if (target >= 0) {
            f32 w = ImGui::GetContentRegionAvail().x;
            g->rects[target] = {at.x, at.y, at.x+w, at.y+ImGui::GetFrameHeight()};
        }
        if (pick.len && pick!=r->value && !(r->sent && pick==r->sentval)) {
            if (theme) {
                settheme(g, themeparse(pick));
            } else if (r->kind=="ui" && r->key=="thinking") {
                setthinking(g, thinkparse(pick));
            } else if (r->kind=="ui" && r->key=="tools") {
                settools(g, toolsparse(pick));
            }
            r->sent    = 1;
            r->sentval = clone(&g->cfgmem, pick);
            guiconfigset(g, r->kind, r->name, r->key, pick);
        }
    } else if (r->type == "paths") {
        // One a line, as many lines as there are and one more, saved
        // joined with "; " once the field is left
        i32 n = 2;
        for (char *c = r->buf; *c; c++) {
            n += *c == '\n';
        }
        n = n<8 ? n : 8;
        f32 h = (f32)n*ImGui::GetTextLineHeight() + 2*ImGui::GetStyle().FramePadding.y;
        ImGui::InputTextMultiline("##v", r->buf, (uz)r->cap, v2(-1, h));
        if (target >= 0) {
            itemrect(g, target);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            Str v   = rejoin(&a, {(u8 *)r->buf, (iz)__builtin_strlen(r->buf)}, '\n', "; ");
            Str was = r->isdflt ? Str{} : rejoin(&a, r->value, ';', "; ");
            if (v!=was && !(r->sent && v==r->sentval)) {
                r->sent    = 1;
                r->sentval = clone(&g->cfgmem, v);
                guiconfigset(g, r->kind, r->name, r->key, v.len ? v : Str{});
            }
        }
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("One directory a line, absolute or in ~; saved as "
                            "you leave the field");
        ImGui::PopTextWrapPos();
    } else {
        Str hint  = r->isdflt || r->like.len ? r->value : Str{};
        b32 enter = ImGui::InputTextWithHint("##v", hint.len ? cstr0(&a, hint) : "", r->buf, (uz)r->cap,
                                             ImGuiInputTextFlags_EnterReturnsTrue|ImGuiInputTextFlags_AutoSelectAll);
        if (target >= 0) {
            itemrect(g, target);
        }
        if (enter || ImGui::IsItemDeactivatedAfterEdit()) {
            // Enter both returns and deactivates: send each edit once
            Str v   = trim({(u8 *)r->buf, (iz)__builtin_strlen(r->buf)});
            Str was = r->isdflt || r->like.len ? Str{} : r->value;
            if (v!=was && !(r->sent && v==r->sentval)) {
                r->sent    = 1;
                r->sentval = clone(&g->cfgmem, v);
                guiconfigset(g, r->kind, r->name, r->key, v.len ? v : Str{});
            }
        }
    }
    if (target>=0 && target==g->reveal) {
        ImGui::SetScrollHereY(0.5f);  // for the checklist, which aims here
        g->reveal = -1;
    }
    ImGui::PopID();
    if (wide) {
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        textfit(a, from, ImGui::GetContentRegionAvail().x, fc);
    }
}

// Whether the latest config.get has a section.
static b32 cfghas(Gui *g, Str kind, Str name)
{
    for (iz i = 0; i < g->cfgrows.len; i++) {
        if (g->cfgrows[i].kind==kind && g->cfgrows[i].name==name) {
            return 1;
        }
    }
    return 0;
}

// Show a section's page, ("", "") for the top level's.
static void cfgselect(Gui *g, Str kind, Str name)
{
    g->cfgdiag = 0;
    if (kind==g->cfgpkind && name==g->cfgpname) {
        return;
    } else if (kind.len+name.len > FIELD_MAX) {
        kind = name = {};
    }
    copybytes(g->cfgpbuf, kind.data, kind.len);
    copybytes(g->cfgpbuf+kind.len, name.data, name.len);
    g->cfgpkind = {g->cfgpbuf, kind.len};
    g->cfgpname = {g->cfgpbuf+kind.len, name.len};
}

// A kind of section as the page list names it: the top level is
// Defaults, [ui] Interface, and others their own, capitalized, and with
// plural for a group of named ones.
static Str cfgkindlabel(Arena *a, Str kind, b32 plural)
{
    if (!kind.len) {
        return "Defaults";
    } else if (kind=="ui" && !plural) {
        return "Interface";
    }
    Buf b(a, kind.len + 2);
    putbyte(&b, kind[0]>='a' && kind[0]<='z' ? (u8)(kind[0] - 'a' + 'A') : kind[0]);
    print(&b, cuthead(kind, 1));
    print(&b, plural ? Str("s") : Str(""));
    return finish(&b);
}

// What a page is called for the checklist and in the combo: "endpoint
// local" for a named section, else its kind, "" for the top level.
static Str cfgpagekey(Arena *a, Str kind, Str name)
{
    return name.len ? concat(a, concat(a, kind, " "), name) : kind;
}

// One page in the list: selected on a click. In the combo its label is
// whole, the popup as wide as it needs; in the list it is cut to fit,
// whole in its tooltip then. Its rectangle goes to rect, if not null.
static b32 cfgitem(Gui *g, b32 combo, Str label, b32 selected, Str tip,
                   u32 color, Rect *rect)
{
    Arena a     = g->frame;
    b32   click = 0;
    if (color) {
        ImGui::PushStyleColor(ImGuiCol_Text, color);
    }
    if (combo) {
        click = ImGui::Selectable(cstr0(&a, label), selected);
        if (tip.len) {
            ImGui::SetItemTooltip("%s", cstr0(&a, tip));
        }
    } else {
        // The label drawn over an unlabeled row, which is no wider than
        // the list
        ImVec2      p   = ImGui::GetCursorScreenPos();
        f32         w   = ImGui::GetContentRegionAvail().x;
        char const *beg = label.cdata;
        char const *end = label.cdata + label.len;
        b32         cut = ImGui::CalcTextSize(beg, end).x > w;
        click = ImGui::Selectable("##page", selected);
        f32 h = ImGui::GetTextLineHeight();
        ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), p, v2(p.x+w, p.y+h), p.x+w,
                                  beg, end, 0);
        if (cut || tip.len) {
            Str t = cut && tip.len ? concat(&a, concat(&a, label, "\n"), tip) :
                    cut ? label : tip;
            ImGui::SetItemTooltip("%s", cstr0(&a, t));
        }
    }
    if (color) {
        ImGui::PopStyleColor();
    }
    if (rect) {
        ImVec2 mn = ImGui::GetItemRectMin();
        ImVec2 mx = ImGui::GetItemRectMax();
        *rect = {mn.x, mn.y, mx.x, mx.y};
    }
    return click;
}

// After an item of the list: scroll to it if it is the page selected
// other than from the list (cfgshowsel), or if the checklist is about to
// click it (key as cfgpagekey, or "add KIND"). Not in a list's first
// frame, whose places are not settled.
static void cfgreveal(Gui *g, Str key, b32 sel)
{
    b32 aim = g->showpage.data && key==g->showpage;
    if (!(sel && g->cfgshowsel) && !aim) {
        return;
    }
    scheddirty(&g->sched);  // scrolled in the next frame, or then begun
    if (!ImGui::IsWindowAppearing()) {
        ImGui::ScrollToItem();
        g->cfgshowsel &= !sel;
        g->showpage    = aim ? Str{} : g->showpage;
    }
}

static void cfgheading(Gui *g, b32 combo, Str label)
{
    Arena a = g->frame;
    ImGui::Spacing();
    textfit(a, label, combo ? 1e6f : ImGui::GetContentRegionAvail().x,
            ImGui::GetColorU32(ImGuiCol_TextDisabled));
}

// The list of pages, in the list beside the page or in the combo above
// it: General's, each named kind's sections and its Add, Diagnostics.
static void drawcfglist(Gui *g, b32 combo)
{
    Arena a     = g->frame;
    f32   sc    = g->scale;
    Json *kinds = jsonget(g->cfg, "kinds");
    cfgheading(g, combo, "General");
    ImGui::Indent(8*sc);
    for (Json *k = kinds->child; k; k = k->next) {
        Str kind = jsonstr(jsonget(k, "kind"));
        if (jsonint(jsonget(k, "named"), 0)) {
            continue;
        }
        // The first row's, for the checklist's aim
        CfgRow *first = 0;
        for (iz i = 0; !first && i < g->cfgrows.len; i++) {
            first = g->cfgrows[i].kind==kind ? &g->cfgrows[i] : 0;
        }
        b32 sel = !g->cfgdiag && g->cfgpkind==kind && !g->cfgpname.len;
        Str tip = jsonstr(jsonget(k, "desc"));
        pushstr(kind);
        if (cfgitem(g, combo, cfgkindlabel(&a, kind, 0), sel, tip, 0, first ? &first->page : 0)) {
            cfgselect(g, kind, "");
        }
        cfgreveal(g, kind, sel);
        ImGui::PopID();
    }
    ImGui::Unindent(8*sc);

    i32 group = 0;  // named kinds so far
    for (Json *k = kinds->child; k; k = k->next) {
        Str kind = jsonstr(jsonget(k, "kind"));
        if (!jsonint(jsonget(k, "named"), 0)) {
            continue;
        }
        cfgheading(g, combo, cfgkindlabel(&a, kind, 1));
        ImGui::Indent(8*sc);
        pushstr(kind);
        for (iz i = 0; i < g->cfgrows.len; i++) {
            // A section's rows come together: the first of each
            CfgRow *r    = &g->cfgrows[i];
            CfgRow *prev = i ? &g->cfgrows[i-1] : 0;
            if (r->kind!=kind || (prev && prev->kind==kind && prev->name==r->name)) {
                continue;
            }
            b32 sel = !g->cfgdiag && g->cfgpkind==kind && g->cfgpname==r->name;
            pushstr(r->name);
            if (cfgitem(g, combo, r->name, sel, {}, 0, &r->page)) {
                cfgselect(g, kind, r->name);
            }
            cfgreveal(g, cfgpagekey(&a, kind, r->name), sel);
            ImGui::PopID();
        }
        Buf ab(&a, 64);
        print(&ab, "Add ");
        print(&ab, kind);
        print(&ab, "...");
        Rect at  = {};
        b32  add = cfgitem(g, combo, finish(&ab), 0, jsonstr(jsonget(k, "desc")),
                           ImGui::GetColorU32(ImGuiCol_TextDisabled), &at);
        if (group < 4) {
            g->rects[RECT_ADDOPEN+group] = at;
        }
        cfgreveal(g, concat(&a, "add ", kind), 0);
        if (add) {
            g->addkind = clone(&g->perm, kind);
            g->openadd = 1;
        }
        ImGui::PopID();
        ImGui::Unindent(8*sc);
        group++;
    }

    // Diagnostics, colored as the worst of them
    Json *diags = jsonget(g->cfg, "diags");
    u32   color = 0;
    for (Json *d = diags->child; d; d = d->next) {
        b32 bad = jsonstr(jsonget(d, "severity")) == "error";
        color = bad || color==g->pal->bad ? g->pal->bad : g->pal->amber;
    }
    ImGui::Spacing();
    Buf db(&a, 32);
    print(&db, "Diagnostics");
    if (jsonlen(diags)) {
        print(&db, " (");
        print(&db, jsonlen(diags));
        putbyte(&db, ')');
    }
    Rect at = {};
    if (cfgitem(g, combo, finish(&db), g->cfgdiag, "Problems found in the files, "
                "with where each is", color, &at)) {
        g->cfgdiag = 1;
    }
    g->rects[RECT_DIAGPAGE] = at;
}

// Where a section is defined: its first header, or with own its first
// in the user's own file; null if none.
static CfgHead *cfghead(Gui *g, Str kind, Str name, b32 own)
{
    CfgHead *r = 0;
    for (iz i = 0; i < g->cfgheads.len; i++) {
        CfgHead *h = &g->cfgheads[i];
        if (h->kind==kind && h->name==name && (!r || (own && h->own && !r->own))) {
            r = h;
        }
    }
    return r && (!own || r->own) ? r : 0;
}

// A section as a file has it: "[kind name]".
static Str cfgtitle(Arena *a, Str kind, Str name)
{
    Buf b(a, kind.len + name.len + 4);
    putbyte(&b, '[');
    print(&b, kind);
    if (name.len) {
        putbyte(&b, ' ');
        print(&b, name);
    }
    putbyte(&b, ']');
    return finish(&b);
}

// The settings of one section, in a table: three columns when the
// value has room beside the others, else two, with where each is set
// under its key.
static void drawcfgtable(Gui *g, Str kind, Str name)
{
    // As if without a scrollbar, so that pages long and short agree
    f32          sc    = g->scale;
    ImGuiWindow *w     = ImGui::GetCurrentWindow();
    f32          avail = ImGui::GetContentRegionAvail().x;
    f32          room  = avail + (w->ScrollbarY ? ImGui::GetStyle().ScrollbarSize : 0);
    f32          keyw  = ImGui::CalcTextSize("first_byte_timeout").x;
    f32          fromw = ImGui::CalcTextSize("from prospero.ini:999").x;
    b32          wide  = room >= keyw + fromw + (f32)CFG_MINVAL*sc;
    if (!wide) {
        // Where each is set goes under its key: room for the usual
        keyw = keyw>fromw ? keyw : fromw;
        keyw = keyw<avail*0.45f ? keyw : avail*0.45f;
    }
    if (!ImGui::BeginTable("##t", wide ? 3 : 2, ImGuiTableFlags_RowBg)) {
        return;
    }
    ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, keyw);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    if (wide) {
        ImGui::TableSetupColumn("from", ImGuiTableColumnFlags_WidthFixed, fromw);
    }
    for (iz i = 0; i < g->cfgrows.len; i++) {
        CfgRow *r = &g->cfgrows[i];
        if (r->kind!=kind || r->name!=name) {
            continue;
        }
        i32 target = -1;
        if (kind=="endpoint" && r->key=="idle_timeout") {
            target = RECT_SETTING;
        } else if (kind=="ui" && r->key=="theme") {
            target = RECT_THEMECOMBO;
        } else if (kind=="guard" && r->key=="roots") {
            target = RECT_ROOTS;
        } else if (r->value.len >= FIELD_MAX) {
            target = RECT_LONGFIELD;  // more than the slack
        }
        drawcfgrow(g, r, target, wide);
    }
    ImGui::EndTable();
}

// A model's chat template argument: a name and a value, side by side
// when they fit, else one above the other.
static void drawcfgtemplate(Gui *g, Str model)
{
    Arena       a  = g->frame;
    f32         sc = g->scale;
    ImGuiStyle &st = ImGui::GetStyle();
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Set a chat template argument, sent in chat_template_kwargs:");
    ImGui::PopTextWrapPos();
    f32 avail = ImGui::GetContentRegionAvail().x;
    f32 bw    = ImGui::CalcTextSize("Set").x + 2*st.FramePadding.x;
    b32 row   = avail >= 280*sc + bw;
    f32 fw    = row ? (avail - bw - 2*st.ItemSpacing.x)/2 : -1;
    ImGui::SetNextItemWidth(fw);
    ImGui::InputTextWithHint("##tplname", "template.NAME", g->tplname, FIELD_MAX);
    if (row) {
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(fw);
    ImGui::InputTextWithHint("##tplval", "value", g->tplval, FIELD_MAX);
    if (row) {
        ImGui::SameLine();
    }
    b32 set = ImGui::Button("Set");
    ImGui::SetItemTooltip("Sent in chat_template_kwargs: true and false as booleans, "
                          "numbers as numbers, the rest, or anything in quotes, as a "
                          "string; empty removes what the model inherits");
    if (set) {
        Str name = trim({(u8 *)g->tplname, (iz)__builtin_strlen(g->tplname)});
        Str val  = trim({(u8 *)g->tplval, (iz)__builtin_strlen(g->tplval)});
        Str key  = startswith(name, "template.") ? name : concat(&a, "template.", name);
        if (!name.len) {
            g->cfgstatus = "A template argument needs a name";
            g->cfgok     = 0;
        } else {
            guiconfigset(g, "model", model, key, val.len ? val : Str(""));
            g->tplname[0] = g->tplval[0] = 0;
        }
    }
}

static void drawcfgdiags(Gui *g)
{
    Arena a     = g->frame;
    Json *diags = jsonget(g->cfg, "diags");
    ImGui::TextUnformatted("Diagnostics");
    ImGui::PushTextWrapPos(0);
    if (!jsonlen(diags)) {
        ImGui::TextDisabled("None: every line of every file was understood.");
    }
    for (Json *d = diags->child; d; d = d->next) {
        Str sev = jsonstr(jsonget(d, "severity"));
        Buf b(&a, 256);
        print(&b, jsonstr(jsonget(d, "file")));
        putbyte(&b, ':');
        print(&b, jsonint(jsonget(d, "line"), 0));
        putbyte(&b, ':');
        print(&b, jsonint(jsonget(d, "col"), 0));
        print(&b, ": ");
        print(&b, sev);
        print(&b, ": ");
        print(&b, jsonstr(jsonget(d, "message")));
        ImGui::PushStyleColor(ImGuiCol_Text, sev=="error" ? g->pal->bad : g->pal->amber);
        ImGui::TextUnformatted(cstr0(&a, finish(&b)));
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
}

// The page shown: its section's name and what it is for, Delete for a
// named one of the user's own file, Connect for a remote, its settings,
// and a model's template argument.
static void drawcfgpage(Gui *g)
{
    Arena       a  = g->frame;
    ImGuiStyle &st = ImGui::GetStyle();
    if (g->cfgdiag) {
        drawcfgdiags(g);
        return;
    }
    Str   kind  = g->cfgpkind;
    Str   name  = g->cfgpname;
    Json *k     = 0;
    for (Json *e = jsonget(g->cfg, "kinds")->child; e; e = e->next) {
        k = jsonstr(jsonget(e, "kind"))==kind ? e : k;
    }
    b32 named = jsonint(jsonget(k, "named"), 0) != 0;
    Str title = kind.len ? cfgtitle(&a, kind, name) : Str("Before any section");

    if (!named) {
        textfit(a, title, ImGui::GetContentRegionAvail().x, 0);
    } else {
        // Delete on the title's line when both fit, else under it
        CfgHead *own   = cfghead(g, kind, name, 1);
        CfgHead *any   = cfghead(g, kind, name, 0);
        b32      other = 0;  // an include defines it too
        for (iz i = 0; i < g->cfgheads.len; i++) {
            CfgHead *h = &g->cfgheads[i];
            other |= h->kind==kind && h->name==name && !h->own;
        }
        Str      label = "Delete...";
        f32      bw    = ImGui::CalcTextSize(label.cdata).x + 2*st.FramePadding.x;
        f32      avail = ImGui::GetContentRegionAvail().x;
        f32      tw    = ImGui::CalcTextSize(title.cdata, title.cdata+title.len).x;
        ImGui::AlignTextToFramePadding();
        if (tw + st.ItemSpacing.x + bw <= avail) {
            text(title);
            ImGui::SameLine(0, avail - tw - bw);
        } else {
            ImGui::PushTextWrapPos(0);
            text(title);
            ImGui::PopTextWrapPos();
        }
        Buf why(&a, 256);
        if (any && !own) {
            print(&why, "Defined in ");
            print(&why, any->file);
            putbyte(&why, ':');
            print(&why, any->line);
            print(&why, ", an included file: settings edit only your own "
                        "file, so delete it there");
        } else if (!any) {
            print(&why, "Not in any file yet");
        }
        ImGui::BeginDisabled(why.len > 0);
        b32 del = ImGui::Button(label.cdata);
        ImGui::EndDisabled();
        itemrect(g, RECT_DELETE);
        if (why.len) {
            ImGui::SetItemTooltip("%s", cstr0(&a, finish(&why)));
        } else {
            Buf tb(&a, 256);
            print(&tb, "Delete ");
            print(&tb, title);
            print(&tb, " from ");
            print(&tb, basename(own->file));
            print(&tb, ", with the comment just above it");
            if (other) {
                print(&tb, "; an included file defines it too, which stays");
            }
            ImGui::SetItemTooltip("%s", cstr0(&a, finish(&tb)));
        }
        if (del) {
            g->opendel = 1;
        }
    }
    Str desc = jsonstr(jsonget(k, "desc"));
    if (desc.len) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", cstr0(&a, desc));
        ImGui::PopTextWrapPos();
    }
    if (kind == "remote") {
        drawremote(g, name, RECT_PAGECONNECT);
    }
    ImGui::Spacing();
    if (named && !cfghas(g, kind, name)) {
        ImGui::TextDisabled("Loading...");
        return;
    }
    drawcfgtable(g, kind, name);
    if (kind == "model") {
        drawcfgtemplate(g, name);
    }
}

// The kind of section's first key, which a new one is written with: a
// remote's ssh is an alias of its name.
static void cfgseed(Str kind, Str name, Str *key, Str *val)
{
    *key = kind=="endpoint" ? Str("url") :
           kind=="model"    ? Str("reasoning") :
           kind=="remote"   ? Str("ssh") : Str("tools");
    *val = kind=="endpoint" ? Str("http://localhost/") :
           kind=="model"    ? Str("reasoning_content") :
           kind=="remote"   ? name : Str("read write edit shell");
}

// A new named section: its name, then its page.
static void drawcfgadd(Gui *g)
{
    Arena a  = g->frame;
    f32   sc = g->scale;
    if (g->openadd) {
        ImGui::OpenPopup("New section");
        g->openadd    = 0;
        g->addname[0] = 0;
        g->adderr     = {};
    }
    ImGui::SetNextWindowSize(v2(380*sc, 0));
    if (!ImGui::BeginPopupModal("New section", 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    Str kind = g->addkind;
    Str key  = {};
    Str val  = {};
    cfgseed(kind, "NAME", &key, &val);
    ImGui::PushTextWrapPos(0);
    ImGui::Text("A new [%s NAME] section, which starts as %s = %s:",
                cstr0(&a, kind), cstr0(&a, key), cstr0(&a, val));
    ImGui::PopTextWrapPos();
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1);
    b32 enter = ImGui::InputTextWithHint("##addname", "NAME", g->addname, FIELD_MAX,
                                         ImGuiInputTextFlags_EnterReturnsTrue);
    itemrect(g, RECT_ADDNAME);
    if (g->adderr.len) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
        ImGui::TextWrapped("%s", cstr0(&a, g->adderr));
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    b32 add = ImGui::Button("Add", v2(120*sc, 0));
    itemrect(g, RECT_ADDBTN);
    ImGui::SameLine();
    if (ImGui::Button("Cancel", v2(120*sc, 0))) {
        ImGui::CloseCurrentPopup();
    }
    if (add || enter) {
        Str name = trim({(u8 *)g->addname, (iz)__builtin_strlen(g->addname)});
        if (!name.len) {
            g->adderr = "A section needs a name";
        } else if (!iniword(name, 0)) {
            g->adderr = "A name is one word, without [ or ]";
        } else if (cfghas(g, kind, name)) {
            // Its first key would be overwritten: leave it be, and show it
            Buf eb(&a, 128);
            print(&eb, cfgtitle(&a, kind, name));
            print(&eb, " already exists");
            g->cfgstatus = clone(&g->perm, finish(&eb));
            g->cfgok     = 0;
            cfgselect(g, kind, name);
            g->cfgshowsel = 1;
            ImGui::CloseCurrentPopup();
        } else {
            cfgseed(kind, name, &key, &val);
            guiconfigset(g, kind, name, key, val);
            cfgselect(g, kind, name);
            g->cfgshowsel = 1;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
}

// Before deleting the page's section: what goes, from where.
static void drawcfgdel(Gui *g)
{
    Arena a  = g->frame;
    f32   sc = g->scale;
    if (g->opendel) {
        ImGui::OpenPopup("Delete section?");
        g->opendel = 0;
    }
    ImGui::SetNextWindowSize(v2(420*sc, 0));
    if (!ImGui::BeginPopupModal("Delete section?", 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    Str      kind  = g->cfgpkind;
    Str      name  = g->cfgpname;
    CfgHead *own   = cfghead(g, kind, name, 1);
    b32      other = 0;
    for (iz i = 0; i < g->cfgheads.len; i++) {
        CfgHead *h = &g->cfgheads[i];
        other |= h->kind==kind && h->name==name && !h->own;
    }
    ImGui::PushTextWrapPos(0);
    ImGui::Text("Delete %s from %s?", cstr0(&a, cfgtitle(&a, kind, name)),
                own ? cstr0(&a, basename(own->file)) : "your file");
    ImGui::TextDisabled("Its settings go, and the comment just above its header. "
                        "Sessions using it keep the settings they have.");
    if (other) {
        ImGui::TextDisabled("An included file defines it too, which stays.");
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::BeginDisabled(!own || g->cfgdelid);
    b32 go = ImGui::Button("Delete", v2(120*sc, 0));
    itemrect(g, RECT_DELOK);
    ImGui::EndDisabled();
    if (go) {
        guiconfigdelete(g, kind, name);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    b32 cancel = ImGui::Button("Cancel", v2(120*sc, 0));
    itemrect(g, RECT_DELCANCEL);
    if (cancel) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

static void drawsettings(Gui *g)
{
    Arena a  = g->frame;
    f32   sc = g->scale;
    if (g->enginekind == ENGINE_FAKE) {
        ImGui::TextDisabled("No engine: the fake agent has no settings.");
        return;
    }
    if (ImGui::IsWindowAppearing() || (!g->cfg && !g->cfgget)) {
        guiconfigget(g);  // the file may have changed by hand
    }
    if (!g->cfg) {
        ImGui::TextDisabled("Loading...");
        return;
    }
    Json *j    = g->cfg;
    Str   path = jsonstr(jsonget(j, "path"));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Edits go to %s", path.len ? cstr0(&a, path) : "(no file)");
    ImGui::PopTextWrapPos();
    if (ImGui::SmallButton("Reload")) {
        guiconfigget(g);
    }
    i32 urls = (i32)jsonlen(jsonget(j, "includes"));
    if (urls) {
        // Fetching each URL include again, which the reply waits for
        char const *label = g->cfgrefresh ? "Fetching..." : "Refresh includes";
        samelineif(ImGui::CalcTextSize(label).x + 2*ImGui::GetStyle().FramePadding.x);
        ImGui::BeginDisabled(g->cfgrefresh != 0);
        b32 refresh = ImGui::SmallButton(label);
        itemrect(g, RECT_REFRESHINC);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Fetch the %d URL include%s again, rather than wait for the "
                              "next scheduled fetch", urls, urls==1 ? "" : "s");
        if (refresh) {
            g->cfgget = 0;  // this one is the answer that counts
            guiconfigget(g, 1);
        }
    }
    i32 ndiags = (i32)jsonlen(jsonget(j, "diags"));
    if (ndiags && !g->cfgdiag) {
        // Whatever the page, a way to the problems in the files
        Buf db(&a, 32);
        print(&db, ndiags);
        print(&db, ndiags==1 ? Str(" diagnostic") : Str(" diagnostics"));
        Str label = finish(&db);
        samelineif(ImGui::CalcTextSize(label.cdata, label.cdata+label.len).x +
                   2*ImGui::GetStyle().FramePadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->amber);
        if (ImGui::SmallButton(cstr0(&a, label))) {
            g->cfgdiag = 1;
        }
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Show the problems found in the files, with where each is");
    }
    if (g->cfgstatus.len) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->cfgok ? g->pal->ok : g->pal->bad);
        ImGui::TextWrapped("%s", cstr0(&a, g->cfgstatus));
        ImGui::PopStyleColor();
    }

    // A named section's page gone, deleted or never added, gives way to
    // the first page once nothing is in flight that would bring it
    if (!g->cfgdiag && g->cfgpname.len && !cfghas(g, g->cfgpkind, g->cfgpname) &&
            !g->cfgset && !g->cfgget) {
        cfgselect(g, "", "");
        g->cfgshowsel = 1;
    }

    f32 hs = 0;  // room to scroll sideways, for the checklist
    f32 w  = ImGui::GetContentRegionAvail().x;
    g->cfgtwopane = w >= (f32)CFG_TWOPANE*sc;
    g->cfgcombo   = 0;
    if (g->cfgtwopane) {
        f32 lw = w*0.3f;
        lw = lw<130*sc ? 130*sc : lw>220*sc ? 220*sc : lw;
        if (ImGui::BeginChild("##pages", v2(lw, 0), ImGuiChildFlags_Borders)) {
            drawcfglist(g, 0);
            hs = ImGui::GetScrollMaxX();
        }
        ImGui::EndChild();
        ImGui::SameLine();
    } else {
        Str    key  = cfgpagekey(&a, g->cfgpkind, g->cfgpname);
        Str    show = g->cfgdiag ? Str("Diagnostics") :
                      g->cfgpname.len ? key : cfgkindlabel(&a, g->cfgpkind, 0);
        ImVec2 at   = ImGui::GetCursorScreenPos();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##pages", cstr0(&a, show), ImGuiComboFlags_HeightLarge)) {
            g->cfgcombo = 1;
            drawcfglist(g, 1);
            ImGui::EndCombo();
        }
        g->rects[RECT_PAGECOMBO] = {at.x, at.y, at.x+w, at.y+ImGui::GetFrameHeight()};
    }
    if (ImGui::BeginChild("##page", v2(0, 0))) {
        drawcfgpage(g);
        f32 m = ImGui::GetScrollMaxX();
        hs = hs>m ? hs : m;
    }
    ImGui::EndChild();
    drawcfgadd(g);
    drawcfgdel(g);
    f32 m = ImGui::GetScrollMaxX();
    g->cfghscroll = hs>m ? hs : m;
    g->cfgframe   = g->total.frames;
}

// Metrics overlay and log

static void drawmetrics(Gui *g)
{
    MetricsSnap *m = &g->snap;
    Arena        a = g->frame;
    Buf          b(&a, 256);

    static Str names[] = {"input", "mail", "deadline", "probe", "force", "pace", "other"};
    print(&b, "wakes/s ");
    for (i32 i = 0; i < WAKE_COUNT; i++) {
        print(&b, " ");
        print(&b, names[i]);
        putbyte(&b, ' ');
        print(&b, m->rate.wakes[i]);
    }
    text(finish(&b));

    b.len = 0;
    print(&b, "frames/s ");
    print(&b, m->rate.frames);
    print(&b, "  presents/s ");
    print(&b, m->rate.presents);
    text(finish(&b));

    b.len = 0;
    print(&b, "frame ");
    print(&b, m->frameus);
    print(&b, " us  max ");
    print(&b, m->maxframeus);
    print(&b, " us  wake max ");
    print(&b, m->maxwakeus);
    print(&b, " us  layout ");
    print(&b, m->layoutus);
    print(&b, " us");
    text(finish(&b));

    b.len = 0;
    print(&b, "totals: frames ");
    print(&b, m->frames);
    print(&b, "  presents ");
    print(&b, m->presents);
    print(&b, "  wakes ");
    print(&b, m->wakes);
    text(finish(&b));

    b.len = 0;
    print(&b, "next deadline: ");
    if (m->deadline == I64MAX) {
        print(&b, "none");
    } else {
        printfloat(&b, (f64)m->deadline/1000, 0);
        print(&b, " ms (");
        print(&b, duenames[m->duewhy]);
        putbyte(&b, ')');
    }
    text(finish(&b));

    b.len = 0;
    print(&b, "owed ");
    print(&b, (i64)m->owed);
    print(&b, "  changing ");
    print(&b, (i64)m->changing);
    print(&b, "  busy");
    for (i32 i = 0; i < BUSY_COUNT; i++) {
        if (m->busy & (1u<<i)) {
            putbyte(&b, ' ');
            print(&b, busynames[i]);
        }
    }
    text(finish(&b));

    b.len = 0;
    print(&b, "stale ");
    print(&b, m->stale);
    print(&b, "  runaway ");
    print(&b, m->runaway);
    if (m->stale) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->stale);
    }
    text(finish(&b));
    if (m->stale) {
        ImGui::PopStyleColor();
    }

    b.len = 0;
    print(&b, "messages ");
    print(&b, (i64)g->view->msgs.len);
    print(&b, "  deltas ");
    print(&b, g->view->deltas);
    print(&b, "  overflows ");
    print(&b, g->overflows);
    print(&b, "  resyncs ");
    print(&b, g->resyncs);
    if (g->view->overflow) {
        print(&b, "  OVERFLOW");
    }
    text(finish(&b));

    if (checkbox("Staleness probe", &g->sched.probing)) {
        g->sched.shadowat = 0;
    }
    ImGui::SetItemTooltip("Render a hidden frame 250 ms after going idle, and report a missed wake if it differs");
    ImGui::SameLine();
    checkbox("Aggressive", &g->sched.aggressive);
    ImGui::SetItemTooltip("Probe every second while idle");
}

static void drawlog(Gui *g)
{
    ImGui::BeginChild("##log");
    for (iz i = 0; i < g->logs.len; i++) {
        text(g->logs[i]);
    }
    ImGui::EndChild();
}

// The fonts' licenses, which their files ask to be shown.
static void drawfontcredits()
{
    ImGui::BulletText("Roboto: Copyright 2011 Google (Apache 2.0)");
    ImGui::BulletText("JetBrains Mono: Copyright 2020 JetBrains (OFL 1.1)");
    ImGui::BulletText("Noto Sans Symbols 2, Noto Sans Math: Copyright 2022 "
                      "The Noto Project Authors, Google (OFL 1.1)");
}

static void drawhelp(Gui *g)
{
    ImGui::TextWrapped(
        "Each session is a conversation with the agent in one working "
        "directory. Enter sends, Shift+Enter starts a new line, and "
        "sending while the agent works steers it. Esc or Stop cancels "
        "the turn. Tool rows and reasoning open and close with a click on "
        "their header. When a tool asks to leave the workspace, answer "
        "on its card before the countdown ends, or it is denied.");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Drag to select across messages; double-click selects a word, "
        "triple-click a block, Shift-click extends. Right-click for copy "
        "options. A code block's Copy button, at its corner while the "
        "mouse is over it, copies its code exactly.");
    ImGui::Spacing();
    char const *mod = g->mac ? "Cmd" : "Ctrl";
    ImGui::TextWrapped(
        "%s+F finds text in the transcript, folded tool output and "
        "reasoning included: Enter or %s+G goes to the next match, "
        "Shift+Enter or %s+Shift+G to the previous, and a folded row "
        "opens to show one. Esc closes the find bar.", mod, mod, mod);
    ImGui::Spacing();
    ImGui::TextWrapped(
        "The Tool output and Reasoning buttons above the transcript, or "
        "%s+Shift+T and %s+Shift+R, open all of a session's tool output or "
        "reasoning, hidden reasoning too, whatever [ui] tools and [ui] "
        "thinking say; again, and the rows fold as before. The bar beside "
        "them shows how full the context is, amber near where it is "
        "compacted.", mod, mod);
    ImGui::Spacing();
    ImGui::TextWrapped(
        "The View menu shows and hides these panes, and zooms the text: "
        "%s+= and %s+-, %s+0 to reset, or %s and the mouse wheel. "
        "[ui] font_size in the settings sets its size for good.",
        g->mac ? "Cmd" : "Ctrl", g->mac ? "Cmd" : "Ctrl",
        g->mac ? "Cmd" : "Ctrl", g->mac ? "Cmd" : "Ctrl");
    ImGui::Spacing();
    drawfontcredits();
}


// Menu bar, toolbar, dock space, and windows

static void drawfaketools(Gui *g)
{
    f32 sc = g->scale;
    b32 prompt = ImGui::Button(g->view->busy ? "Stop" : "Prompt");
    itemrect(g, RECT_PROMPTBTN);
    if (prompt) {
        if (g->view->busy) {
            fakecmd(g, FAKE_STOP, "");
        } else {
            fakecmd(g, FAKE_PROMPT, "Show me something with code and a table.");
        }
    }
    ImGui::SetItemTooltip(g->view->busy ? "Cancel the running turn"
                                        : "Send a canned prompt");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140*sc);
    if (ImGui::SliderInt("tok/s", &g->tps, 1, 2000, "%d tok/s", ImGuiSliderFlags_Logarithmic)) {
        fakeint(g, FAKE_TPS, g->tps);
    }
    ImGui::SetItemTooltip("Streaming rate of the fake agent");
    ImGui::SameLine();
    if (checkbox("Bursts", &g->bursts)) {
        fakeint(g, FAKE_BURSTS, g->bursts);
    }
    ImGui::SetItemTooltip("Deliver tokens in bursts of 25");
    ImGui::SameLine();
    if (checkbox("Pauses", &g->pauses)) {
        fakeint(g, FAKE_PAUSES, g->pauses);
    }
    ImGui::SetItemTooltip("Stall for 5 s partway through each response");
    ImGui::SameLine();
    if (ImGui::Button("Huge code")) {
        fakecmd(g, FAKE_HUGE, "");
    }
    ImGui::SetItemTooltip("Stream one response that is a single huge code block");
    ImGui::SameLine();
    b32 history = ImGui::Button("10k history");
    itemrect(g, RECT_HISTORY);
    if (history) {
        fakeint(g, FAKE_HISTORY, 10000);
    }
    ImGui::SetItemTooltip("Load 10,000 messages (about 5 MB) of history");
}

// A shortcut as the menus show it: Cmd on macOS, else Ctrl.
static char const *cmdkey(Gui *g, Arena *a, Str key)
{
    return cstr0(a, concat(a, g->mac ? Str("Cmd+") : Str("Ctrl+"), key));
}

// Whether a new session can start now.
static b32 cannew(Gui *g)
{
    return g->enginekind!=ENGINE_FAKE && g->engineok && !g->quitting;
}

static void newsession(Gui *g)
{
    g->opennew = 1;
    g->newerr  = {};
}

// The menus' shortcuts, wherever the focus is (a global route, which a
// focused field's own use of a key goes ahead of): a new session, zoom
// by key or by Ctrl+wheel (Cmd on macOS), find and the session's
// folding toggles, and quitting on Linux, where
// no system shortcut does (macOS has Cmd+Q, Windows Alt+F4).
static void menukeys(Gui *g)
{
    ImGuiInputFlags glob = ImGuiInputFlags_RouteGlobal;
    ImGuiKeyChord   ctrl = ImGuiMod_Ctrl;
    if (cannew(g) && ImGui::Shortcut(ctrl|ImGuiKey_N, glob)) {
        newsession(g);
    }
    if (ImGui::Shortcut(ctrl|ImGuiKey_Equal, glob) ||
            ImGui::Shortcut(ctrl|ImGuiMod_Shift|ImGuiKey_Equal, glob) ||
            ImGui::Shortcut(ctrl|ImGuiKey_KeypadAdd, glob)) {
        zoomby(g, 1);
    }
    if (ImGui::Shortcut(ctrl|ImGuiKey_Minus, glob) ||
            ImGui::Shortcut(ctrl|ImGuiKey_KeypadSubtract, glob)) {
        zoomby(g, -1);
    }
    if (ImGui::Shortcut(ctrl|ImGuiKey_0, glob) ||
            ImGui::Shortcut(ctrl|ImGuiKey_Keypad0, glob)) {
        setzoom(g, 1);
    }
    // Find, and the session's toggles of its tool output and reasoning
    if (g->sess && ImGui::Shortcut(ctrl|ImGuiKey_F, glob)) {
        gui_find_open(g);
    }
    if (ImGui::Shortcut(ctrl|ImGuiMod_Shift|ImGuiKey_T, glob)) {
        setsessiontools(g, 0, !sessiontools(g));
    }
    if (ImGui::Shortcut(ctrl|ImGuiMod_Shift|ImGuiKey_R, glob)) {
        setsessionthink(g, 0, !sessionthink(g));
    }
    #if !defined(_WIN32) && !defined(__APPLE__)
    if (ImGui::Shortcut(ctrl|ImGuiKey_Q, glob)) {
        gui_close(g);
    }
    #endif

    // The wheel with Ctrl held scrolls nothing (ImGui leaves it alone):
    // each notch a zoom step, a trackpad's fractions adding up to one
    ImGuiIO &io = ImGui::GetIO();
    if (!io.KeyCtrl) {
        g->zoomwheel = 0;
    } else if (io.MouseWheel!=0 && ImGui::IsMousePosValid()) {
        g->zoomwheel += io.MouseWheel;
        i32 steps = (i32)g->zoomwheel;
        if (steps) {
            g->zoomwheel -= (f32)steps;
            zoomby(g, steps);
        }
    }
}

static void drawmenubar(Gui *g)
{
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    Arena a = g->frame;

    b32 menu = ImGui::BeginMenu("Session");
    itemrect(g, RECT_SESSMENU);
    if (menu) {
        b32 add = ImGui::MenuItem("New session...", cmdkey(g, &a, "N"), false, cannew(g));
        itemrect(g, RECT_NEWBTN);
        ImGui::SetItemTooltip("Start a session in a working directory");
        if (add) {
            newsession(g);
        }
        // The engine's session operations, to come
        ImGui::BeginDisabled();
        ImGui::Separator();
        ImGui::MenuItem("Fork");
        ImGui::MenuItem("Rename...");
        ImGui::MenuItem("Compact");
        ImGui::MenuItem("Switch model...");
        ImGui::MenuItem("Export...");
        ImGui::Separator();
        ImGui::MenuItem("Archive");
        ImGui::MenuItem("Delete...");
        ImGui::Separator();
        ImGui::MenuItem("Close");
        ImGui::EndDisabled();
        ImGui::Separator();
        #if defined(__APPLE__)
        char const *quit = "Cmd+Q";
        #elif defined(_WIN32)
        char const *quit = "Alt+F4";
        #else
        char const *quit = "Ctrl+Q";
        #endif
        if (ImGui::MenuItem("Quit", quit)) {
            gui_close(g);
        }
        ImGui::EndMenu();
    }

    // The transcript's selection, as its own context menu has it
    if (ImGui::BeginMenu("Edit")) {
        View *v   = g->view;
        b32   any = !selempty(&v->sel);
        if (ImGui::MenuItem("Copy", cmdkey(g, &a, "C"), false, any)) {
            copyout(g, COPY_TEXT);
        }
        if (ImGui::MenuItem("Copy as Markdown", cmdkey(g, &a, "Shift+C"), false, any)) {
            copyout(g, COPY_MARKDOWN);
        }
        if (ImGui::MenuItem("Select all", cmdkey(g, &a, "A"), false, v->msgs.len>0)) {
            selall(v);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Find...", cmdkey(g, &a, "F"), false, g->sess!=0)) {
            gui_find_open(g);
        }
        ImGui::EndMenu();
    }

    menu = ImGui::BeginMenu("View");
    itemrect(g, RECT_MENU);
    if (menu) {
        for (i32 i = 0; i < PANE_COUNT; i++) {
            if (i == PANE_HELP) {
                ImGui::Separator();
            }
            bool on = g->panes[i];
            if (ImGui::MenuItem(panenames[i], 0, &on)) {
                setpane(g, i, on);
            }
            itemrect(g, RECT_PANE0+i);
        }
        ImGui::Separator();
        f32 lo = zoomsteps[0], hi = zoomsteps[countof(zoomsteps)-1];
        if (ImGui::MenuItem("Zoom in", cmdkey(g, &a, "="), false, g->zoom<hi)) {
            zoomby(g, 1);
        }
        if (ImGui::MenuItem("Zoom out", cmdkey(g, &a, "-"), false, g->zoom>lo)) {
            zoomby(g, -1);
        }
        Buf b(&a, 32);
        print(&b, "Actual size");
        if (g->zoom != 1) {
            print(&b, " (now ");
            print(&b, (i64)(g->zoom*100 + 0.5f));
            print(&b, "%)");
        }
        b32 reset = ImGui::MenuItem(cstr0(&a, finish(&b)), cmdkey(g, &a, "0"), false, g->zoom!=1);
        itemrect(g, RECT_ZOOMRESET);
        if (reset) {
            setzoom(g, 1);
        }
        ImGui::Separator();
        // Folding for the session shown, its toggles as the session
        // bar has them
        b32 has = g->sess && g->sess->view;
        if (ImGui::MenuItem("Expand tool output", shortcutname(g, 0), sessiontools(g), has)) {
            setsessiontools(g, 0, !sessiontools(g));
        }
        if (ImGui::MenuItem("Expand thinking", shortcutname(g, 1), sessionthink(g), has)) {
            setsessionthink(g, 0, !sessionthink(g));
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Theme")) {
            static char const *labels[] = {"System", "Dark", "Light"};
            static_assert(countof(labels) == THEME_COUNT);
            for (i32 i = 0; i < THEME_COUNT; i++) {
                if (ImGui::MenuItem(labels[i], 0, g->themeset==i) && g->themeset!=i) {
                    // As the settings window picks it: at once, then kept
                    settheme(g, i);
                    if (g->enginekind!=ENGINE_FAKE && engineup(g)) {
                        guiconfigset(g, "ui", "", "theme", themenames[i]);
                    }
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Reset layout")) {
            for (i32 i = 0; i < PANE_COUNT; i++) {
                setpane(g, i, i != PANE_METRICS);
            }
            g->panefocus    = 0;
            g->resetpending = 1;
        }
        ImGui::SeparatorText("Debug");
        bool demo = g->showdemo;
        if (ImGui::MenuItem("ImGui demo window", 0, &demo)) {
            g->showdemo = demo;
        }
        if (g->enginekind == ENGINE_FAKE) {
            b32 modal = ImGui::MenuItem("Modal dialog...");
            itemrect(g, RECT_MODALITEM);
            if (modal) {
                g->openmodal = 1;
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Help")) {
            setpane(g, PANE_HELP, 1);
        }
        if (ImGui::MenuItem("About Prospero")) {
            g->openabout = 1;
        }
        ImGui::EndMenu();
    }

    // The latest error at the right, and the empty stretch before it,
    // where the checklist moves the mouse out of the way
    Str msg = g->engineerr.len ? g->engineerr : g->lasterr;
    f32 x0  = ImGui::GetCursorScreenPos().x;
    f32 x1  = ImGui::GetWindowPos().x + ImGui::GetWindowWidth();
    f32 pad = ImGui::GetStyle().ItemSpacing.x;
    if (msg.len) {
        char const *s = cstr0(&a, msg);
        f32 w = ImGui::CalcTextSize(s).x;
        f32 x = x1 - w - pad;
        x = x<x0 ? x0 : x;
        ImGui::SetCursorScreenPos(v2(x, ImGui::GetCursorScreenPos().y));
        x1 = x;
        if (g->engineerr.len) {
            ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
            ImGui::TextUnformatted(s);
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("%s", s);
        }
    }
    ImVec2 p = ImGui::GetWindowPos();
    g->rects[RECT_TOOLBAR] = {x0, p.y, x1, p.y + ImGui::GetWindowHeight()};
    ImGui::EndMainMenuBar();
}

// Dialogs, which any window may ask for
static void drawdialogs(Gui *g)
{
    f32 sc = g->scale;
    if (g->openmodal) {
        ImGui::OpenPopup("Modal");
        g->openmodal = 0;
    }
    if (ImGui::BeginPopupModal("Modal", 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("A modal dialog, which dims the background.");
        b32 ok = ImGui::Button("OK", v2(120*sc, 0));
        itemrect(g, RECT_MODALOK);
        if (ok) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (g->openabout) {
        ImGui::OpenPopup("About Prospero");
        g->openabout = 0;
    }
    if (ImGui::BeginPopupModal("About Prospero", 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Prospero %s", PROSPERO_VERSION);
        ImGui::TextUnformatted("A coding harness: the agent's turns, tools and sessions.");
        ImGui::TextUnformatted("Free and unencumbered software released into the public domain.");
        ImGui::Spacing();
        ImGui::SeparatorText("Third-party software");
        ImGui::BulletText("Dear ImGui: Copyright 2014 Omar Cornut (MIT)");
        ImGui::BulletText("FreeType: Copyright 1996 The FreeType Project (FTL)");
        ImGui::BulletText("MD4C: Copyright 2016 Martin Mitas (MIT)");
        #ifndef _WIN32
        ImGui::BulletText("SDL: Copyright 1997 Sam Lantinga (zlib)");
        ImGui::BulletText("cpp-httplib: Copyright 2017 Yuji Hirose (MIT)");
        ImGui::BulletText("Mbed TLS: Copyright The Mbed TLS Contributors (Apache 2.0)");
        #endif
        drawfontcredits();
        ImGui::Spacing();
        if (ImGui::Button("Close", v2(120*sc, 0)) || ImGui::Shortcut(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    drawnew(g);
    drawclose(g);
}

// The dock node a window is in, or was in when the layout was saved;
// 0 if neither.
static ImGuiID saveddock(char const *name)
{
    ImGuiWindow *w = ImGui::FindWindowByName(name);
    if (w) {
        return w->DockId;
    }
    ImGuiWindowSettings *s = ImGui::FindWindowSettingsByID(ImHashStr(name));
    return s ? s->DockId : 0;
}

// Begin a pane shown: its tab's close button hides it, until the View
// menu shows it again. Returns whether its contents are visible. End
// it with ImGui::End.
static b32 beginpane(Gui *g, i32 pane)
{
    if (g->panefocus == pane+1) {
        g->panefocus = 0;
        ImGui::SetNextWindowFocus();
    }
    bool open = 1;
    b32  vis  = ImGui::Begin(panenames[pane], &open);
    if (!open) {
        setpane(g, pane, 0);
    }
    return vis;
}

static void dockwindows(Gui *g)
{
    menukeys(g);
    drawmenubar(g);
    ImGuiViewportP *vp   = (ImGuiViewportP *)ImGui::GetMainViewport();
    ImRect          work = vp->GetBuildWorkRect();

    // The fake agent's controls, for measurements, under the menu bar
    f32 toolh = 0;
    if (g->enginekind == ENGINE_FAKE) {
        ImGui::SetNextWindowPos(work.Min);
        ImGui::SetNextWindowSize(v2(work.GetWidth(), 0));
        ImGui::Begin("##Toolbar", 0,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoSavedSettings);
        drawfaketools(g);
        toolh = ImGui::GetWindowHeight();
        ImGui::End();
    }

    ImVec2 pos  = v2(work.Min.x, work.Min.y + toolh);
    ImVec2 size = v2(work.GetWidth(), work.GetHeight() - toolh);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##DockHost", 0,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground);
    ImGuiID id = ImGui::GetID("DockSpace");
    g->dockspace = id;

    // A reset waits a frame, so windows just shown again are submitted
    // before the builder places them.
    if (g->resetpending) {
        g->resetpending = 0;
        g->resetlayout  = 1;
    } else if (g->resetlayout || (g->firstlayout && !ImGui::DockBuilderGetNode(id))) {
        g->firstlayout = 0;
        g->resetlayout = 0;
        ImGui::DockBuilderRemoveNode(id);
        ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(id, size);
        // The first split is between the right column and the rest, of
        // which the sessions take the left
        ImGuiID left = 0, rest = 0, right = 0, center = 0;
        ImGuiID top = 0, bottom = 0, rtop = 0, rbot = 0;
        ImGuiID ltop = 0, lbot = 0;
        ImGui::DockBuilderSplitNode(id, ImGuiDir_Right, 0.28f, &right, &rest);
        ImGui::DockBuilderSplitNode(rest, ImGuiDir_Left, 0.25f, &left, &center);
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.22f, &bottom, &top);
        ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.40f, &rbot, &rtop);
        ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.35f, &lbot, &ltop);
        ImGui::DockBuilderDockWindow("Sessions", ltop);
        ImGui::DockBuilderDockWindow("Todo", lbot);
        ImGui::DockBuilderDockWindow("Help", top);
        ImGui::DockBuilderDockWindow("Transcript", top);
        ImGui::DockBuilderDockWindow("Prompt", bottom);
        ImGui::DockBuilderDockWindow("Metrics", rtop);
        ImGui::DockBuilderDockWindow("Hosts", rtop);
        ImGui::DockBuilderDockWindow("Settings", rtop);
        ImGui::DockBuilderDockWindow("Jobs", rbot);
        ImGui::DockBuilderDockWindow("Log", rbot);
        ImGui::DockBuilderFinish(id);
        g->jobsfocus = 2;
    }
    ImGui::DockSpace(id, v2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);
    drawdialogs(g);
    ImGui::End();

    b32 *show = g->panes;
    if (show[PANE_SESSIONS]) {
        if (beginpane(g, PANE_SESSIONS)) {
            escapecancel(g);
            drawsessions(g);
        }
        ImGui::End();
    }
    // Windows that a layout saved before them lacks join a neighbor
    ImGuiID near = saveddock("Sessions");
    if (show[PANE_TODO]) {
        if (near) {
            ImGui::SetNextWindowDockID(near, ImGuiCond_FirstUseEver);
        }
        if (beginpane(g, PANE_TODO)) {
            drawtodo(g);
        }
        ImGui::End();
    }
    // Submitted first, so that it is the tab selected in its dock node.
    if (show[PANE_TRANSCRIPT]) {
        if (beginpane(g, PANE_TRANSCRIPT)) {
            escapecancel(g);
            drawtranscript(g);
        }
        ImGui::End();
    }
    if (show[PANE_HELP]) {
        if (beginpane(g, PANE_HELP)) {
            drawhelp(g);
        }
        ImGui::End();
    }
    if (show[PANE_PROMPT]) {
        if (beginpane(g, PANE_PROMPT)) {
            drawprompt(g);
        }
        ImGui::End();
    }
    g->metricsshown = 0;
    if (show[PANE_METRICS]) {
        g->metricsshown = beginpane(g, PANE_METRICS);
        if (g->metricsshown) {
            drawmetrics(g);
        }
        ImGui::End();
    }
    if (show[PANE_SETTINGS]) {
        if (beginpane(g, PANE_SETTINGS)) {
            drawsettings(g);
        }
        ImGui::End();
    }
    // Windows that a layout saved before them lacks join a neighbor
    near = saveddock("Settings");
    if (show[PANE_HOSTS]) {
        if (near) {
            ImGui::SetNextWindowDockID(near, ImGuiCond_FirstUseEver);
        }
        if (beginpane(g, PANE_HOSTS)) {
            drawhosts(g);
        }
        ImGui::End();
    }
    if (g->outopen) {
        bool open = 1;
        if (g->outfocus) {
            g->outfocus = 0;
            ImGui::SetNextWindowFocus();
        }
        ImGui::SetNextWindowSize(v2(720*g->scale, 480*g->scale), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Output", &open)) {
            drawoutput(g);
        }
        ImGui::End();
        g->outopen = open;
    }
    if (g->img.view.open) {
        bool open = 1;
        if (g->img.view.focus) {
            g->img.view.focus = 0;
            ImGui::SetNextWindowFocus();
        }
        ImGui::SetNextWindowSize(v2(900*g->scale, 600*g->scale), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Screenshot", &open)) {
            drawshot(g);
        }
        ImGui::End();
        if (!open) {
            imgclose(g);
        }
    }
    // The tab selected in its node, rather than the log: a node selects
    // the window focused last, which a new window is as it appears, so
    // Jobs is focused once, the frame after its windows first appear
    near = saveddock("Log");
    g->jobsvisible = 0;
    if (show[PANE_JOBS]) {
        if (near) {
            ImGui::SetNextWindowDockID(near, ImGuiCond_FirstUseEver);
        }
        if (g->jobsfocus && !--g->jobsfocus) {
            ImGui::SetNextWindowFocus();
        }
        g->jobsvisible = beginpane(g, PANE_JOBS);
        if (g->jobsvisible) {
            drawjobs(g);
        }
        ImGui::End();
    }
    if (show[PANE_LOG]) {
        if (beginpane(g, PANE_LOG)) {
            drawlog(g);
        }
        ImGui::End();
    }
    if (g->showdemo) {
        bool open = 1;
        ImGui::ShowDemoWindow(&open);
        g->showdemo = open;
    }
}
