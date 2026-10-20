// ImGui user interface: the transcript widget, the prompt, the dock
// layout, the metrics overlay, and the scheduler's view of ImGui
//
// All ImDrawList code lives here, since its signatures shift between
// ImGui releases. uiscan and uideadlines read ImGui's internals after
// each frame to tell the scheduler what it is waiting on, and drawhash
// fingerprints the frame's draw data.
//
// Nothing here may make a frame differ from the last without declaring
// why: a value drawn from the clock needs a deadline for when it next
// changes, or the staleness probe reports it.
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

    // Hover delays restart whenever the mouse moves.
    if (io.MouseDelta.x!=0 || io.MouseDelta.y!=0) {
        g->lastmove = now;
    }
    if (g->lastmove) {
        i64 slack = 20000;
        f32 delays[] = {
            st.HoverStationaryDelay,
            st.HoverStationaryDelay + st.HoverDelayShort,
            st.HoverStationaryDelay + st.HoverDelayNormal,
        };
        for (iz i = 0; i < countof(delays); i++) {
            i64 t = g->lastmove + (i64)(delays[i]*1e6f) + slack;
            if (t > now) {
                schedat(s, t, i ? DUE_TOOLTIP : DUE_HOVER);
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


// Transcript

static u32 rgba(i32 r, i32 g, i32 b, i32 a)
{
    return IM_COL32(r, g, b, a);
}

static u32 hlcolor(i32 cls)
{
    switch (cls) {
    case HL_COMMENT:  return rgba(118, 160, 115, 255);
    case HL_KEYWORD:  return rgba(198, 140, 228, 255);
    case HL_TYPE:     return rgba(86, 182, 194, 255);
    case HL_STRING:   return rgba(214, 157, 133, 255);
    case HL_NUMBER:   return rgba(181, 206, 168, 255);
    case HL_PREPROC:  return rgba(197, 134, 192, 255);
    case HL_VAR:      return rgba(156, 220, 254, 255);
    case HL_FUNC:     return rgba(220, 220, 170, 255);
    case HL_INSERTED: return rgba(120, 200, 120, 255);
    case HL_DELETED:  return rgba(230, 110, 110, 255);
    case HL_HEADING:  return rgba(150, 170, 230, 255);
    }
    return rgba(212, 212, 212, 255);
}

static u32 piececolor(Piece *p)
{
    if (p->hl) {
        return hlcolor(p->hl);
    } else if (p->style & STYLE_LINK) {
        return rgba(100, 160, 240, 255);
    } else if (p->style & STYLE_DIM) {
        return rgba(150, 150, 150, 255);
    } else if (p->style & STYLE_CODE) {
        return rgba(225, 190, 140, 255);
    }
    return rgba(222, 222, 222, 255);
}

static Str rolename(i32 role)
{
    switch (role) {
    case VIEW_USER:      return "You";
    case VIEW_ASSISTANT: return "Agent";
    case VIEW_THINK:     return "Reasoning";
    case VIEW_TOOL:      return "Tool";
    case VIEW_NOTE:      return "Note";
    }
    return "";
}

enum : i32 {
    CH_BG,    // code and header backgrounds
    CH_SEL,   // selection highlight
    CH_TEXT,  // text, markers, rules
    CH_COUNT,
};

// Draw one placed block of a message whose content starts at origin.
static void drawblock(Gui *g, ImDrawList *dl, ImDrawListSplitter *sp,
                      Doc *d, i32 msg, Placed *p, ImVec2 origin, Range sel,
                      f32 cliptop, f32 clipbot)
{
    BlockLayout *bl     = p->bl;
    DocBlock       *b      = &d->blocks[p->block];
    Piece       *pieces = d->pieces.data + b->piece0;
    f32          ox     = origin.x;
    f32          oy     = origin.y + p->y;
    Fonts       *f      = &g->fonts;

    for (iz i = 0; i < bl->decos.len; i++) {
        Deco *dc = &bl->decos[i];
        ImVec2 a = v2(ox+dc->x, oy+dc->y);
        ImVec2 z = v2(ox+dc->x+dc->w, oy+dc->y+dc->h);
        switch (dc->kind) {
        case DECO_CODEBG:
            sp->SetCurrentChannel(dl, CH_BG);
            dl->AddRectFilled(a, z, rgba(30, 30, 34, 255), 4*g->scale);
            break;
        case DECO_HEADERBG:
            sp->SetCurrentChannel(dl, CH_BG);
            dl->AddRectFilled(a, z, rgba(45, 45, 52, 255));
            break;
        case DECO_QUOTE:
            sp->SetCurrentChannel(dl, CH_TEXT);
            dl->AddRectFilled(a, z, rgba(90, 90, 100, 255));
            break;
        case DECO_RULE:
        case DECO_GRID:
            sp->SetCurrentChannel(dl, CH_TEXT);
            dl->AddRectFilled(a, v2(z.x>a.x+1 ? z.x : a.x+1, z.y>a.y+1 ? z.y : a.y+1),
                              rgba(80, 80, 88, 255));
            break;
        case DECO_MARKER: {
            sp->SetCurrentChannel(dl, CH_TEXT);
            ImFontBaked *bk = stylebaked(f, 0);
            ImVec2 at = v2(ox+dc->x, oy+dc->y - bk->Ascent);
            dl->AddText(stylefont(f, 0), stylesize(f, 0), at,
                        rgba(160, 160, 170, 255), dc->text.cdata,
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
            if (selected) {
                i32 rb = runbeg(pieces, r), re = runend(pieces, r);
                b32 synth = rb == re;
                if ((!synth && rb<s1 && re>s0) || (synth && rb>=s0 && rb<s1)) {
                    f32 x0 = runx(&g->measure, pieces, r, s0);
                    f32 x1 = runx(&g->measure, pieces, r, s1);
                    if (synth) {
                        x0 = r->x;
                        x1 = r->x + (r->w>0 ? r->w : 4*g->scale);
                    }
                    sp->SetCurrentChannel(dl, CH_SEL);
                    dl->AddRectFilled(v2(ox+x0, ly), v2(ox+x1, ly+l->h),
                                      rgba(38, 79, 120, 255));
                }
            }
            if (r->flags & RUN_NODRAW) {
                continue;
            }
            ImFontBaked *bk  = stylebaked(f, pc->style);
            ImVec2       at  = v2(ox + r->x, ly + l->base - bk->Ascent);
            char const  *beg = pc->text.cdata + r->b0;
            char const  *end = pc->text.cdata + r->b1;
            u32          col = piececolor(pc);
            sp->SetCurrentChannel(dl, CH_TEXT);
            dl->AddText(stylefont(f, pc->style), stylesize(f, pc->style), at,
                        col, beg, end);
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

// The transcript: virtualized messages with selection across them.
static void drawtranscript(Gui *g)
{
    View       *v  = g->view;
    f32         sc = g->scale;
    ImGuiStyle &st = ImGui::GetStyle();

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
    f32         head  = pad/4 + fm.lineh;  // the role line
    v->extra = head + pad*3/4;
    v->gap   = 6*sc;

    if (width != g->lastwidth) {
        v->topsdirty = 1;
        g->lastwidth = width;
    }

    // Where the view is: last frame's scroll, or where the wheel just
    // sent it.
    ImGuiWindow *last    = g->transcriptwin;
    f32          scrolly = 0;
    b32          bottom  = g->stick;
    if (last) {
        scrolly = last->Scroll.y;
        if (last->ScrollTarget.y < FLT_MAX) {
            scrolly = last->ScrollTarget.y;
            bottom  = 0;
        }
        scrolly = scrolly<0 ? 0 : scrolly>last->ScrollMax.y ? last->ScrollMax.y : scrolly;
    }
    viewtops(v, width, charw, fm.lineh);

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
        want = total - winh;
    } else if (anchor >= 0) {
        want = v->msgs[anchor].top + anchordy;
    }
    if (g->scrollto >= 0) {
        want = g->scrollto;  // a benchmark or a drag past the edge
        g->scrollto = -1;
    }
    want = want>total-winh ? total-winh : want;
    want = want<0 ? 0 : want;

    ImGui::SetNextWindowContentSize(v2(0, (f32)total + pad));
    if ((f32)want != scrolly) {
        ImGui::SetNextWindowScroll(v2(-1, (f32)want));
    }
    ImGui::BeginChild("##transcript", v2(0, 0), 0,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    g->transcriptwin = ImGui::GetCurrentWindow();
    ImVec2 wpos = ImGui::GetWindowPos();
    scrolly = ImGui::GetScrollY();

    // Drawing
    ImDrawList        *dl = ImGui::GetWindowDrawList();
    ImDrawListSplitter *sp = &g->splitter;
    sp->Split(dl, CH_COUNT);
    Range sel  = selrange(&v->sel);
    f32   top  = wpos.y;
    f32   bot  = wpos.y + winh;
    i32   d0   = viewfind(v, scrolly);
    i32   d1   = viewfind(v, scrolly + winh);
    for (i32 i = d0; i>=0 && i <= d1; i++) {
        ViewMsg       *m  = &v->msgs[i];
        MsgLayout *ml = msglayout(v, i, &g->measure, width, sc);
        Doc       *d  = msgdoc(v, i);
        f32 y = wpos.y + (f32)(m->top - scrolly);
        f32 x = wpos.x + pad;

        // Role line, and a background for the user's messages.
        if (m->role == VIEW_USER) {
            sp->SetCurrentChannel(dl, CH_BG);
            dl->AddRectFilled(v2(wpos.x+pad/2, y), v2(wpos.x+pad*1.5f+width, y+v->extra+m->h),
                              rgba(40, 44, 52, 255), 6*sc);
        }
        sp->SetCurrentChannel(dl, CH_TEXT);
        Str name = rolename(m->role);
        dl->AddText(stylefont(&g->fonts, STYLE_BOLD), g->fonts.size*0.85f,
                    v2(x, y + pad/4), rgba(140, 150, 170, 255),
                    name.cdata, name.cdata+name.len);
        ImVec2 origin = v2(x, y + head);
        for (iz k = 0; k < ml->blocks.len; k++) {
            Placed *p = &ml->blocks[k];
            if (origin.y+p->y > bot || origin.y+p->y+p->bl->h < top) {
                continue;
            }
            drawblock(g, dl, sp, d, i, p, origin, sel, top, bot);
        }
    }
    sp->Merge(dl);

    // Mouse: an invisible button over the visible area.
    ImGui::SetCursorScreenPos(wpos);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##text", v2(ImGui::GetWindowWidth() - ImGui::GetStyle().ScrollbarSize, winh),
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
            ViewMsg       *m  = &v->msgs[i];
            MsgLayout *ml = msglayout(v, i, &g->measure, width, sc);
            f32 ly = (f32)(cy - m->top - head);
            at.msg = i;
            at.off = ly<0 ? 0 : msghit(&g->measure, msgdoc(v, i), ml,
                                       io.MousePos.x - wpos.x - pad, ly);
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

    g->stick = scrolly >= ImGui::GetScrollMaxY()-2 || total<=winh;
    ImGui::EndChild();
}


// Prompt

static void drawprompt(Gui *g)
{
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_WordWrap |
                                ImGuiInputTextFlags_EnterReturnsTrue |
                                ImGuiInputTextFlags_CtrlEnterForNewLine;
    f32 bw = 80*g->scale;
    ImVec2 size = v2(ImGui::GetContentRegionAvail().x - bw - ImGui::GetStyle().ItemSpacing.x,
                     ImGui::GetContentRegionAvail().y);
    b32 send = ImGui::InputTextMultiline("##prompt", g->prompt, PROMPT_MAX, size, flags);
    itemrect(g, RECT_PROMPT);
    ImGui::SameLine();
    send |= ImGui::Button("Send", v2(bw, 0));
    ImGui::SetItemTooltip("Send the prompt to the fake agent (Enter; Ctrl+Enter for a new line)");
    if (send) {
        Str p = {(u8 *)g->prompt, 0};
        while (p.data[p.len]) p.len++;
        p = trim(p);
        if (p.len) {
            fakecmd(g, FAKE_PROMPT, p);
            g->prompt[0] = 0;
        }
        ImGui::SetKeyboardFocusHere(-1);
    }
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
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.3f, 1));
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

static void drawhelp(Gui *)
{
    ImGui::TextWrapped(
        "GUI spike (M1). The transcript is fed by a fake agent thread "
        "through the real mailbox and wake path. Drag to select across "
        "messages; double-click selects a word, triple-click a block, "
        "Shift-click extends. Right-click for copy options.");
    ImGui::Spacing();
    ImGui::BulletText("Roboto: Copyright 2011 Google (Apache 2.0)");
    ImGui::BulletText("JetBrains Mono: Copyright 2020 JetBrains (OFL 1.1)");
}


// Toolbar, dock space, and windows

static void drawtoolbar(Gui *g)
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
    ImGui::SameLine();
    b32 menu = ImGui::Button("Menu");
    itemrect(g, RECT_MENU);
    if (menu) {
        ImGui::OpenPopup("##menu");
    }
    ImGui::SetItemTooltip("A popup, for testing");
    if (ImGui::BeginPopup("##menu")) {
        bool demo = g->showdemo;
        if (ImGui::MenuItem("Show demo window", 0, &demo)) {
            g->showdemo = demo;
        }
        if (ImGui::MenuItem("Reset layout")) {
            g->resetpending = 1;
        }
        b32 modal = ImGui::MenuItem("Modal dialog...");
        itemrect(g, RECT_MODALITEM);
        if (modal) {
            g->openmodal = 1;
        }
        ImGui::EndPopup();
    }
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

    // The elapsed clock of a running turn ticks once a second, and asks
    // for exactly those frames.
    if (g->view->busy) {
        i64 now = g->sched.now;
        i64 el  = now - g->turnstart;
        ImGui::SameLine();
        Arena a = g->frame;
        Buf   b(&a, 32);
        print(&b, el/1000000);
        print(&b, " s");
        text(finish(&b));
        schedat(&g->sched, g->turnstart + (el/1000000 + 1)*1000000, DUE_CLOCK);
    }
}

static void dockwindows(Gui *g)
{
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(v2(vp->WorkSize.x, 0));
    ImGui::Begin("##Toolbar", 0,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                 ImGuiWindowFlags_NoSavedSettings);
    drawtoolbar(g);
    f32 toolh = ImGui::GetWindowHeight();
    ImVec2 tp = ImGui::GetWindowPos();
    g->rects[RECT_TOOLBAR] = {tp.x, tp.y, tp.x+ImGui::GetWindowWidth(), tp.y+toolh};
    ImGui::End();

    ImVec2 pos  = v2(vp->WorkPos.x, vp->WorkPos.y + toolh);
    ImVec2 size = v2(vp->WorkSize.x, vp->WorkSize.y - toolh);
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
        ImGuiID left = 0, right = 0, top = 0, bottom = 0, rtop = 0, rbot = 0;
        ImGui::DockBuilderSplitNode(id, ImGuiDir_Right, 0.30f, &right, &left);
        ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.18f, &bottom, &top);
        ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, &rbot, &rtop);
        ImGui::DockBuilderDockWindow("Help", top);
        ImGui::DockBuilderDockWindow("Transcript", top);
        ImGui::DockBuilderDockWindow("Prompt", bottom);
        ImGui::DockBuilderDockWindow("Metrics", rtop);
        ImGui::DockBuilderDockWindow("Log", rbot);
        ImGui::DockBuilderFinish(id);
    }
    ImGui::DockSpace(id, v2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();

    // Submitted first, so that it is the tab selected in its dock node.
    if (ImGui::Begin("Transcript")) {
        drawtranscript(g);
    }
    ImGui::End();
    if (ImGui::Begin("Help")) {
        drawhelp(g);
    }
    ImGui::End();
    if (ImGui::Begin("Prompt")) {
        drawprompt(g);
    }
    ImGui::End();
    g->metricsshown = ImGui::Begin("Metrics");
    if (g->metricsshown) {
        drawmetrics(g);
    }
    ImGui::End();
    if (ImGui::Begin("Log")) {
        drawlog(g);
    }
    ImGui::End();
    if (g->showdemo) {
        bool open = 1;
        ImGui::ShowDemoWindow(&open);
        g->showdemo = open;
    }
}
