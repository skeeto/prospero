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

// The '@' picker's adapter (mention.cpp): the compose box asks it for
// candidates, and the popup below draws them. It is included here, with
// the code that uses it, rather than from app.cpp with the rest.
#include "mention.cpp"

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

// Tooltips, as ImGui's SetItemTooltip and SetTooltip, but wrapped at
// TIP_EMS: unwrapped, a long one spans the whole window. Tooltips built
// by hand wrap at tipwrap too.
enum { TIP_EMS = 32 };

static f32 tipwrap()
{
    return (f32)TIP_EMS * ImGui::GetFontSize();
}

static void tipv(char const *fmt, __builtin_va_list ap)
{
    ImGui::PushTextWrapPos(tipwrap());
    ImGui::TextV(fmt, ap);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// A tooltip's text is also the item's help for assistive tools, shown
// or not (a11y_imgui.cpp)
static void axtip(Str);
static void axtipv(char const *fmt, __builtin_va_list ap);
static void axkey(Gui *, u32);

// The last item clicked, or pressed by other means: the keyboard's Space
// on it, or an assistive tool (ImGui's activation)
static b32 uiclicked()
{
    ImGuiID id = ImGui::GetItemID();
    return ImGui::IsItemClicked(ImGuiMouseButton_Left) || (id && GImGui->NavActivateId==id);
}

static void itemtip(char const *fmt, ...) IM_FMTARGS(1);
static void itemtip(char const *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    axtipv(fmt, ap);
    __builtin_va_end(ap);
    if (ImGui::BeginItemTooltip()) {
        __builtin_va_start(ap, fmt);
        tipv(fmt, ap);
        __builtin_va_end(ap);
    }
}

static void tipnow(char const *fmt, ...) IM_FMTARGS(1);
static void tipnow(char const *fmt, ...)
{
    if (ImGui::BeginTooltip()) {
        __builtin_va_list ap;
        __builtin_va_start(ap, fmt);
        tipv(fmt, ap);
        __builtin_va_end(ap);
    }
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

// A key pressed for the current window, a modal, itself: not as it
// appears (the key that opened it), nor while a popup of its own shows
// or did last frame, which the key was for (a combo's list, which Esc
// closes before the modal draws). Whoever owns the key: a field's
// claim on Esc while it was active must not keep the modal open.
static b32 modalkey(ImGuiKey key)
{
    ImGuiContext &c   = *GImGui;
    ImGuiWindow  *cur = c.CurrentWindow;
    if (ImGui::IsWindowAppearing() || !ImGui::IsKeyPressed(key, 0)) {
        return 0;
    }
    for (i32 i = 0; i < c.Windows.Size; i++) {
        ImGuiWindow *w = c.Windows[i];
        if (w!=cur && (w->Flags & ImGuiWindowFlags_Popup) && w->ParentWindow==cur &&
                w->LastFrameActive>=c.FrameCount-1) {
            return 0;
        }
    }
    return 1;
}

// Enter for a modal's default button, called first in its body: in a
// single-line field, or with no item under the keyboard's nav cursor,
// which Enter activates instead (Cancel, a checkbox).
static b32 modalenter()
{
    ImGuiContext &c = *GImGui;
    if (!modalkey(ImGuiKey_Enter) && !modalkey(ImGuiKey_KeypadEnter)) {
        return 0;
    }
    b32 typing = c.ActiveId && c.InputTextState.ID==c.ActiveId &&
                 !(c.InputTextState.Flags & ImGuiInputTextFlags_Multiline);
    b32 navon  = c.NavCursorVisible && c.NavId && c.NavWindow==c.CurrentWindow;
    return typing || !navon;
}


// Fonts and measurement

// In whole pixels: ImGui bakes and measures a font at its size rounded,
// but draws text scaled to the size asked, so a fractional size (a zoom
// step, Windows' 125%) drew each run narrower than its measured place,
// and the shortfall gathered where runs end, as wide gaps at soft breaks.
static f32 stylesize(Fonts *f, i32 style)
{
    f32 size = f->size;
    switch ((style & STYLE_HMASK) >> STYLE_HSHIFT) {
    case 1: size *= 1.60f; break;
    case 2: size *= 1.35f; break;
    case 3: size *= 1.17f; break;
    case 4: size *= 1.05f; break;
    }
    return (f32)(i32)(size + 0.5f);
}

// The font for a style, at the display's density. ImGui tells a font
// its density only while it is pushed, and these are drawn and measured
// without that (ImDrawList::AddText, GetFontBaked): one never pushed
// would stay at 1, its glyphs rasterized at half size on a Retina
// display and scaled up, blurred.
static ImFont *stylefont(Fonts *f, i32 style)
{
    b32     bold   = style&(STYLE_BOLD|STYLE_HEADER|STYLE_HMASK);
    b32     italic = style&STYLE_ITALIC;
    ImFont *font   = 0;
    if (style & STYLE_CODE) {
        font = bold ? f->monobold : f->mono;
    } else {
        font = bold&&italic ? f->bolditalic : bold ? f->bold :
               italic ? f->italic : f->prose;
    }
    f32 density = ImGui::GetIO().DisplayFramebufferScale.x;
    font->CurrentRasterizerDensity = density>0 ? density : 1;
    return font;
}

// The small text of headers and rows, 0.85 of the font size, rounded to
// whole pixels on the display. FreeType hints a fractional size badly:
// at 12.75 px (15 at 1x), a glyph's bottom row went missing.
static f32 smallsize(Fonts *f)
{
    f32 density = ImGui::GetIO().DisplayFramebufferScale.x;
    density = density>0 ? density : 1;
    return roundf(f->size*0.85f*density) / density;
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

// ImGui's font as its widgets draw text now, for midcut on their labels.
static f32 widgetadvance(void *, i32, i32 cp)
{
    return ImGui::GetFontBaked()->GetCharAdvance((ImWchar)cp);
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

// A focus request waits for the next frame: SetKeyboardFocusHere resolves
// on the next NewFrame, which activates the field, and ImGui drops the
// characters that come in the frame a field is activated by nav.
static b32 uifocusowed()
{
    ImGuiContext &c = *GImGui;
    return c.NavMoveSubmitted || c.NavMoveForwardToNextFrame || c.NavNextActivateId;
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

// Let go of every key ImGui holds but modifiers, as busy counts them:
// their releases were lost (schedend's keyup). Returns how many.
static i32 uikeysup()
{
    ImGuiIO &io = ImGui::GetIO();
    i32      n  = 0;
    for (i32 k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
        if (k>=ImGuiKey_MouseLeft && k<=ImGuiKey_MouseWheelY) continue;
        if (!modifierkey(k) && ImGui::IsKeyDown((ImGuiKey)k)) {
            io.AddKeyEvent((ImGuiKey)k, false);
            n++;
        }
    }
    return n;
}

// Ctrl+Shift+V (Cmd on macOS) pastes as Ctrl+V does, as in many
// programs: every box takes only plain text anyway. ImGui matches
// chords exactly, so Shift is let go around a press of V in the queued
// input, before the frame reads it. The V's release follows at once,
// on the next frame (trickled), and the real one finds V already up.
// The terminal, holding the keyboard since last frame, pastes with
// Ctrl+Shift+V as itself, its Ctrl+V being ^V.
static void uishiftpaste(Gui *g)
{
    ImGuiContext &c     = *GImGui;
    ImGuiIO      &io    = c.IO;
    if (c.ActiveId && c.ActiveId==g->termid) {
        return;
    }
    b32           ctrl  = io.KeyCtrl;
    b32           shift = io.KeyShift;
    b32           other = io.KeyAlt || io.KeySuper;
    ImVector<ImGuiInputEvent> &q = c.InputEventsQueue;
    for (i32 i = 0; i < q.Size; i++) {
        ImGuiInputEvent e = q[i];
        if (e.Type != ImGuiInputEventType_Key) continue;
        switch ((i32)e.Key.Key) {
        case ImGuiMod_Ctrl:  ctrl  = e.Key.Down; break;
        case ImGuiMod_Shift: shift = e.Key.Down; break;
        case ImGuiMod_Alt:
        case ImGuiMod_Super: other = e.Key.Down; break;
        }
        if (e.Key.Key!=ImGuiKey_V || !e.Key.Down || !ctrl || !shift || other) {
            continue;
        }
        ImGuiInputEvent up = e;
        up.Key.Key  = ImGuiMod_Shift;
        up.Key.Down = 0;
        ImGuiInputEvent release = e;
        release.Key.Down = 0;
        ImGuiInputEvent down = up;
        down.Key.Down = 1;
        q.insert(q.Data+i, up);
        q.insert(q.Data+i+2, release);
        q.insert(q.Data+i+3, down);
        i += 3;
    }
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
// palette's panels, text, fields, rows, buttons, bars, tabs, borders,
// scrollbars and accent. What is left of the stock style is translucent
// (dimming, table rows' stripes), so it suits any theme on its base.
static void themestyle(ImGuiStyle *st, Palette *p)
{
    if (p->dark) {
        ImGui::StyleColorsDark(st);
    } else {
        ImGui::StyleColorsLight(st);
    }
    ImVec4 *c = st->Colors;
    auto    v = [](u32 x, f32 alpha = 1) {
        ImVec4 r = ImGui::ColorConvertU32ToFloat4(x);
        r.w = alpha;
        return r;
    };
    c[ImGuiCol_FrameBgHovered]   = v(p->framehover);
    c[ImGuiCol_FrameBgActive]    = v(p->frameactive);
    c[ImGuiCol_CheckboxSelectedBg] = ImLerp(v(p->frame), v(p->framehover), 0.65f);
    c[ImGuiCol_TitleBg]          = v(p->tabbar);
    c[ImGuiCol_TitleBgActive]    = v(p->tabbaron);
    c[ImGuiCol_MenuBarBg]        = v(p->menubar);
    // The dimmed tabs, of panes without focus, as the stock styles have them
    c[ImGuiCol_Tab]              = v(p->tab);
    c[ImGuiCol_TabSelected]      = v(p->tabsel);
    c[ImGuiCol_TabDimmed]        = ImLerp(v(p->tab),    v(p->tabbar), 0.80f);
    c[ImGuiCol_TabDimmedSelected]= ImLerp(v(p->tabsel), v(p->tabbar), 0.40f);
    c[ImGuiCol_TabSelectedOverline] = v(p->accent);
    c[ImGuiCol_CheckMark]        = v(p->accent);
    c[ImGuiCol_SliderGrab]       = v(p->accent);
    c[ImGuiCol_SliderGrabActive] = v(p->accent);
    c[ImGuiCol_NavCursor]        = v(p->accent);
    c[ImGuiCol_DockingPreview]   = v(p->accent, 0.70f);
    c[ImGuiCol_SeparatorHovered] = v(p->accent, 0.78f);
    c[ImGuiCol_SeparatorActive]  = v(p->accent);
    c[ImGuiCol_ResizeGrip]       = v(p->accent, 0.20f);
    c[ImGuiCol_ResizeGripHovered]= v(p->accent, 0.67f);
    c[ImGuiCol_ResizeGripActive] = v(p->accent, 0.95f);
    c[ImGuiCol_Border]           = v(p->border);
    c[ImGuiCol_Separator]        = v(p->border);
    c[ImGuiCol_TreeLines]        = v(p->border);
    c[ImGuiCol_ScrollbarGrab]    = v(p->scroll);
    c[ImGuiCol_ScrollbarGrabHovered] = v(p->scrollhover);
    c[ImGuiCol_ScrollbarGrabActive]  = v(p->scrollpress);
    c[ImGuiCol_TextSelectedBg]   = v(p->sel);
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
// The palette is a copy (palnow), since a configured theme's colors may
// change in place.
static void applytheme(Gui *g)
{
    Palette p = g->themehas ? g->themecustom : *themepalette(g->themeset, g->sysdark);
    if (g->pal && !__builtin_memcmp(&p, g->pal, sizeof(p))) {
        return;
    }
    g->palnow = p;
    g->pal    = &g->palnow;
    themestyle(&g->basestyle, g->pal);  // where a new DPI scale starts
    ImGuiStyle &st = ImGui::GetStyle();
    for (i32 i = 0; i < ImGuiCol_COUNT; i++) {
        st.Colors[i] = g->basestyle.Colors[i];
    }
    g->titleok = os_guitheme(g->os, g->pal->dark);
    g->woke |= WOKE_FORCE;
    schedforce(&g->sched);
}

// A setting of a configured section as the configuration has it now, or
// null if unset or not valid.
static Str cfgrowval(Gui *g, Str kind, Str name, Str key)
{
    for (iz i = 0; i < g->cfgrows.len; i++) {
        CfgRow *r = &g->cfgrows[i];
        if (r->kind==kind && r->name==name && r->key==key) {
            return r->valid ? r->value : Str{};
        }
    }
    return {};
}

// Whether the configuration has a [theme NAME] of that name.
static b32 themelisted(Gui *g, Str name)
{
    for (iz i = 0; name.len && i < g->cfgheads.len; i++) {
        if (g->cfgheads[i].kind=="theme" && g->cfgheads[i].name==name) {
            return 1;
        }
    }
    return 0;
}

// Whether the configuration's header i is the first of a [theme NAME]
// that may be picked: named, not as a built-in theme, and not again
// after a header of the same theme in an earlier file.
static b32 themehead(Gui *g, iz i)
{
    CfgHead *h = &g->cfgheads[i];
    if (h->kind!="theme" || !h->name.len || themeparse(h->name)>=0) {
        return 0;
    }
    for (iz k = 0; k < i; k++) {
        if (g->cfgheads[k].kind=="theme" && g->cfgheads[k].name==h->name) {
            return 0;
        }
    }
    return 1;
}

// The configured theme by name: a built-in one, a [theme NAME] of the
// configuration's, with its colors as they are now, or else the
// system's.
static void settheme(Gui *g, Str name)
{
    i32 theme   = themeparse(name);
    g->themeset = theme<0 ? THEME_SYSTEM : theme;
    g->themehas = 0;
    if (theme<0 && themelisted(g, name)) {
        if (!(g->themename == name)) {
            g->themename = clone(&g->perm, name);  // rarely changed
        }
        g->themecustom = themebuild(cfgrowval(g, "theme", name, "base"),
                                    [&](Str key) { return cfgrowval(g, "theme", name, key); });
        g->themehas    = 1;
    }
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

// A reasoning message's count in the unit [ui] count picks.
static i64 thinkcount(Gui *g, ViewMsg *m)
{
    return g->countset==COUNT_CHARS ? m->chars : m->words;
}

// Count folded reasoning as [ui] count says: COUNT_*, or -1 for an
// unknown name, which means words. Each message keeps both counts, so
// it shows at once with nothing recounted.
static void setcount(Gui *g, i32 unit)
{
    unit = unit<0 ? COUNT_WORDS : unit;
    if (g->countset != unit) {
        g->countset = unit;
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
// View menu work: every shell row folded, every tool row open, or all
// reasoning shown, whatever
// the settings say, until turned off again, when the settings fold them
// as before but for rows the user opened or closed. The configuration is
// left alone. A null session is the one shown.
static void setsessionshell(Gui *g, GuiSession *s, b32 on)
{
    s = s ? s : g->sess;
    if (s && s->view && viewfoldshell(s->view, on)) {
        scheddirty(&g->sched);
    }
}

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
static b32 sessionshell(Gui *g)
{
    return g->sess && g->sess->view && g->sess->view->foldshell;
}

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
    g->findheld   = 0;
    scheddirty(&g->sched);
}

// The next match (dir 1) or the previous (-1), wrapping around, shown.
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
    case LIVE_RELAY:
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
    case VJOB_CLOSED:
    case VJOB_FORKED:  return p->amber;
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
    return state==LIVE_TASK     ? Str("subagents") :
           state==LIVE_MCP      ? Str("starting MCP servers") :
           state==LIVE_WORKTREE ? Str("making the worktree") :
           state==LIVE_RELAY    ? Str("paused: no relay") :
           state==LIVE_AUTH     ? Str("signing in again") : livenames[state];
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

// Seconds as "45 s", "4 min 5 s" or "1 h 2 min" (printsecs).
static Str secstext(Arena *a, i64 secs)
{
    Buf b(a, 32);
    printsecs(&b, secs);
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
// Its inline code that names a file found from where the text is (from,
// null for nowhere) is drawn as a link.
static void namedrect(Gui *, Str, ImVec2, ImVec2);
static b32  codelinkdrawn(Gui *, Arena, LinkFrom *, Str);

static void drawblock(Gui *g, ImDrawList *dl, ImDrawListSplitter *sp,
                      Doc *d, i32 msg, Placed *p, ImVec2 origin, Range sel,
                      Marks *mk, f32 cliptop, f32 clipbot, f32 alpha, LinkFrom *from)
{
    BlockLayout *bl     = p->bl;
    DocBlock    *b      = &d->blocks[p->block];
    Piece       *pieces = d->pieces.data + b->piece0;
    f32          ox     = origin.x;
    f32          oy     = origin.y + p->y;
    Fonts       *f      = &g->fonts;
    Palette     *pal    = g->pal;
    b32          code   = b->type==DOC_CODE || b->type==DOC_LINE;

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
    i32 candat   = -1;  // the last candidate piece looked up, and whether a link
    b32 candlink = 0;
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
            if (pc->flags&PIECE_CAND && candat!=r->piece) {
                DocCand *c = from ? doccand(d, pc) : 0;
                candat   = r->piece;
                candlink = c && codelinkdrawn(g, g->frame, from, c->text);
            }
            b32          cand = pc->flags&PIECE_CAND && candlink;
            ImFontBaked *bk   = stylebaked(f, pc->style);
            ImVec2       at   = v2(ox + r->x, ly + l->base - bk->Ascent);
            char const  *beg  = pc->text.cdata + r->b0;
            char const  *end  = pc->text.cdata + r->b1;
            u32          col  = fade(cand ? pal->link : piececolor(pal, pc, code), alpha);
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
            if (pc->style&(STYLE_STRIKE|STYLE_LINK) || cand) {
                f32 y = pc->style&STYLE_STRIKE ? ly + l->base - bk->Ascent*0.35f
                                               : ly + l->base + 1;
                dl->AddLine(v2(ox+r->x, y), v2(ox+r->x+r->w, y), col, 1);
            }
            if (g->drive && pc->link>=0) {
                // Where the checklist clicks a link: "link " and where to
                Arena a = g->frame;
                namedrect(g, concat(&a, "link ", d->links[pc->link]),
                          v2(ox+r->x, ly), v2(ox+r->x+r->w, ly+l->h));
            } else if (g->drive && cand) {
                // Inline code, found a file: "codelink " and its text
                Arena a = g->frame;
                namedrect(g, concat(&a, "codelink ", pc->text),
                          v2(ox+r->x, ly), v2(ox+r->x+r->w, ly+l->h));
            }
        }
    }
}

// Copy a view's selection to the clipboard.
static void copyout(Gui *g, View *v, i32 how)
{
    Arena a = g->frame;
    Str   s = copysel(v, &a, how);
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
    // Clipped at the right only: ImGui draws from x rounded down, and a
    // glyph (bold most) may reach left of its origin, so a left edge at
    // x cut a column off a run's first glyph
    ImFont *font = stylefont(p->f, style);
    ImVec4  clip = ImVec4(p->x - p->size, p->y - p->size, p->right, p->y + p->size*2);
    p->dl->AddText(font, p->size, v2(p->x, p->y), col, s.cdata, s.cdata+s.len, 0, &clip);
    p->x += font->CalcTextSizeA(p->size, 1e9f, 0, s.cdata, s.cdata+s.len).x;
}

// A button whose label is runs of text in styles, drawn as pen draws
// them: a name in it in the code font, say. Returns whether pressed.
static b32 penbutton(Gui *g, char const *id, Str *runs, i32 *styles, i32 n)
{
    ImGuiStyle &st   = ImGui::GetStyle();
    f32         size = ImGui::GetFontSize();
    f32         w    = 0;
    for (i32 i = 0; i < n; i++) {
        Str s = runs[i];
        if (!s.len) {
            continue;  // may be null, which ImGui would strlen
        }
        w += stylefont(&g->fonts, styles[i])->CalcTextSizeA(size, 1e9f, 0, s.cdata,
                                                             s.cdata+s.len).x;
    }
    ImVec2 at   = ImGui::GetCursorScreenPos();
    b32    hit  = ImGui::InvisibleButton(id, v2(w + 2*st.FramePadding.x, ImGui::GetFrameHeight()));
    ImRect bb   = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
    i32    col  = ImGui::IsItemActive()  ? ImGuiCol_ButtonActive :
                  ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered : ImGuiCol_Button;
    ImGui::RenderNavCursor(bb, ImGui::GetItemID());
    ImGui::RenderFrame(bb.Min, bb.Max, ImGui::GetColorU32(col), 1, st.FrameRounding);
    Pen p = {ImGui::GetWindowDrawList(), &g->fonts, at.x + st.FramePadding.x,
             at.y + st.FramePadding.y, bb.Max.x, size};
    for (i32 i = 0; i < n; i++) {
        pen(&p, runs[i], styles[i], ImGui::GetColorU32(ImGuiCol_Text));
    }
    return hit;
}

// Milliseconds as "850 ms", "1.2 s", "3 min 5 s" or "1 h 2 min".
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
        printsecs(&b, ms/1000);
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
static Str kidhost(Arena *, ViewTool *);
static b32 kidnamed(Gui *, Arena, GuiSession *, Str);
static Str kidlink(Gui *, Arena *, ViewTool *, i64);

// A message's header line: who, or what a tool did, or an ask's state.
// What a mark of a change to the session says after its label: a model
// as the pickers name it, ENDPOINT/MODEL.
static Str markwhat(Gui *g, Arena *a, ViewMsg *m)
{
    GuiSession *s = g->sess;
    if (m->label=="Model" && s && hostcan(s->host, "models")) {
        return modelshown(a, s->host, m->from, m->text);
    }
    return m->text;
}

// Returns where a tool row's summary went along the line, x0 to x1, to
// be a link to its file; empty for none.
static ImVec2 drawheader(Gui *g, Pen *p, View *v, ViewMsg *m, i64 nowms)
{
    Arena     a   = g->frame;
    ViewTool *t   = m->tool;
    Palette  *pal = g->pal;
    ImVec2    sum = {};
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
        if (t->name=="task" && g->sess && t->host.len) {
            pen(p, "  on ", 0, dim);
            pen(p, t->host, STYLE_BOLD, dim);
            if (g->sess->host->kind == GH_REMOTE) {
                pen(p, concat(&a, ", as ", g->sess->host->name), 0, dim);
                pen(p, " names it", 0, dim);
            } else if (Str n = t->status==VTOOL_RUNNING ? kidhost(&a, t) : Str{};
                       n.len && kidnamed(g, a, g->sess, n)) {
                Str st = kidlink(g, &a, t, nowms);
                if (st.len) {
                    pen(p, concat(&a, "  ", st), STYLE_BOLD, pal->amber);
                }
            }
        }
        if (t->name=="task" && g->sess) {
            GuiSession *k = taskchild(g, g->sess, t);
            if (k && k->view && k->view->asks) {
                pen(p, "  needs an answer", STYLE_BOLD, pal->amber);
            } else if (t->status==VTOOL_BACKGROUND && k && k->view &&
                       (k->view->busy || k->view->jobsrun)) {
                pen(p, "  subagent still at work", STYLE_BOLD, pal->active);
            }
        }
        pen(p, "  ", 0, dim);
        sum.x = p->x<p->right ? p->x : p->right;
        pen(p, t->summary, STYLE_CODE, pal->summary);
        sum.y = p->x<p->right ? p->x : p->right;
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
            if (c->focus.len) {
                print(&b, ", focused on \xe2\x80\x9c");
                print(&b, shorten(&a, c->focus, 60));
                print(&b, "\xe2\x80\x9d");
            }
        }
        pen(p, finish(&b), 0, dim);
    } break;
    case VIEW_REWIND: {
        ViewCard *c = m->card;
        Buf b(&a, 128);
        if (c->kind == "cut") {
            pen(p, "Conversation rewound", STYLE_BOLD, dim);
            print(&b, ":  ");
            print(&b, (i64)c->count);
            print(&b, c->count==1 ? Str(" turn taken out") : Str(" turns taken out"));
            pen(p, finish(&b), 0, dim);
            if (c->conflicts) {
                b.len = 0;
                print(&b, ", ");
                print(&b, (i64)c->conflicts);
                print(&b, c->conflicts==1 ? Str(" file conflict") : Str(" file conflicts"));
                pen(p, finish(&b), STYLE_BOLD, pal->amber);
            }
            break;
        } else if (c->to.len) {
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
        i64 n    = thinkcount(g, m);
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
            printcount(&b, n);
            if (g->countset == COUNT_CHARS) {
                print(&b, n==1 ? Str(" character") : Str(" characters"));
            } else {
                print(&b, n==1 ? Str(" word") : Str(" words"));
            }
            if (secs >= 0) {
                print(&b, ", ");
                print(&b, secstext(&a, secs));
            }
            pen(p, finish(&b), 0, dim);
        }
        g->thinkcount = n;
        g->thinksecs  = live ? secs : -1;
    } break;
    case VIEW_ASK: {
        b32 plan = t->kind=="plan" || t->kind=="plan.enter";
        b32 ques = t->kind == "question";
        if (ques) {
            pen(p, t->qs.len==1 ? Str("A question for you  ") : Str("Questions for you  "),
                STYLE_BOLD, pal->amber);
        } else if (t->kind == "plan") {
            pen(p, "Approve the plan?  ", STYLE_BOLD, pal->amber);
        } else if (plan) {
            pen(p, "Plan first, changing nothing until you approve?  ",
                STYLE_BOLD, pal->amber);
        } else if (t->kind == "mcp.project") {
            // The servers, in its text, are the project's to run
            pen(p, "Start the MCP servers of ", STYLE_BOLD, pal->amber);
            pen(p, t->path, STYLE_CODE, pal->summary);
            pen(p, "?  ", STYLE_BOLD, pal->amber);
        } else if (t->kind == "approve") {
            // The urls, from web includes; what would go there in its text
            pen(p, "Send credentials to ", STYLE_BOLD, pal->amber);
            pen(p, t->path, STYLE_CODE, pal->summary);
            pen(p, "?  ", STYLE_BOLD, pal->amber);
        } else {
            Buf b(&a, 256);
            print(&b, t->tool);
            if (t->path.len) {
                print(&b, " ");
                print(&b, t->path);
            }
            pen(p, "Allow ", STYLE_BOLD, pal->amber);
            pen(p, finish(&b), STYLE_CODE, pal->summary);
            pen(p, "?  ", STYLE_BOLD, pal->amber);
        }
        if (t->answer == VASK_PENDING) {
            if (v->readonly) {
                pen(p, "waiting in another process", 0, dim);
                break;
            }
            if (!t->deadline) {
                pen(p, "waiting for you", 0, pal->amber);  // no deadline
                break;
            }
            i64 secs = countdown(g, t->deadline, nowms);
            b32 q    = t->kind == "question";
            Buf c(&a, 64);
            if (secs) {
                print(&c, q ? Str("skipped in ") : Str("denied in "));
                print(&c, secs);
                print(&c, q ? Str(" s unless answered") : Str(" s unless allowed"));
            } else {
                print(&c, q ? Str("skipping") : Str("denying"));
            }
            pen(p, finish(&c), 0, pal->amber);
        } else {
            b32 ok = t->answer == VASK_ALLOWED;
            Buf c(&a, 256);
            print(&c, ques && t->by=="user"
                                      ? (!ok    ? Str("skipped") :
                                         t->dflt ? Str("answered as recommended")
                                                 : Str("answered")) :
                      ques            ? Str("not answered") :
                      t->kind=="plan" ? (ok ? Str("approved") : Str("kept planning")) :
                      plan            ? (ok ? Str("agreed") : Str("declined")) :
                      ok              ? Str("allowed") : Str("denied"));
            print(&c, t->trusted       ? Str(" (trusted) by you") :
                      t->always        ? Str(" (always) by you") :
                      t->by=="user"    ? Str(" by you") :
                      t->by=="timeout" ? Str(": nobody answered in time") :
                      t->by=="steer"   ? Str(": you sent a message instead") :
                      t->by=="cancel"  ? Str(": the turn was cancelled") :
                      t->by=="policy"  ? Str(": nobody could answer") :
                      t->by=="interrupted" ? Str(": the harness stopped while asking") :
                      t->by=="shared"  ? Str(" for every session") :
                      t->by=="stopped" ? Str(": the engine stopped") : Str(""));
            pen(p, finish(&c), STYLE_BOLD, ok ? pal->ok : pal->bad);
        }
    } break;
    case VIEW_MARK: {
        // A divider: a rule, what changed, to what, and the rule on
        f32 mid  = p->y + p->size*0.6f;
        f32 sc   = g->scale;
        u32 rule = pal->rule;
        p->dl->AddLine(v2(p->x, mid), v2(p->x + 14*sc, mid), rule, sc);
        p->x += 20*sc;
        pen(p, m->label, STYLE_BOLD, dim);
        pen(p, m->label=="Model" ? Str("  ") : Str(": "), 0, dim);
        pen(p, markwhat(g, &a, m), m->label=="Model" ? STYLE_CODE : 0, dim);
        if (p->x + 6*sc < p->right) {
            p->dl->AddLine(v2(p->x + 6*sc, mid), v2(p->right, mid), rule, sc);
        }
    } break;
    case VIEW_ASSISTANT:
        pen(p, m->label.len ? m->label : rolename(m->role), STYLE_BOLD, dim);
        if (m->worked >= 1000) {
            // The last of a turn: how long the turn worked (viewwork)
            pen(p, concat(&a, "  worked ", secstext(&a, (m->worked + 500)/1000)), 0, dim);
            g->workedshown = 1;
        }
        break;
    default:
        pen(p, m->label.len ? m->label : rolename(m->role), STYLE_BOLD, dim);
    }
    return sum;
}

// A task's subagent: named by its report, or found by its parent and
// call while it works.
static GuiSession *taskchild(Gui *g, GuiSession *parent, ViewTool *t)
{
    if (t->host.len) {
        // On another host, named by task.host: only one this window
        // reaches by that name, as the parent's engine shares its
        // configuration (kidnamed). In a container, once it is gone, its
        // copy here (ctrkeep), a subagent of its parent.
        Arena       a   = g->frame;
        b32         own = parent->host->kind != GH_REMOTE;
        GuiHost    *h   = own ? findhost(g, GH_REMOTE, kidhost(&a, t)) : 0;
        GuiSession *k   = h && t->child.len ? findsess(g, h, t->child) : 0;
        b32         up  = h && h->r && !h->closing;
        if (!t->ctr.len || !own || (k && up)) {
            return k;
        }
        GuiSession *kept = t->child.len ? findsess(g, parent->host, t->child) : 0;
        return kept && kept->listed ? kept : k;
    }
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

// Subagents on other hosts (hostlink.cpp)
//
// The parent's engine starts one through its own link to that host, by
// a name from its configuration, which is this window's when the parent
// runs here: in the engine, or a background host of this machine. The
// window then reaches the subagent through its own link, by the same
// name, to show it and answer its asks; the engine relays nothing.

static Slice<Str> remotenames(Gui *, Arena *);

// The name this window reaches a task's host by: ctr:NAME for a
// container made for it, else the name the parent's engine gave it.
static Str kidhost(Arena *a, ViewTool *t)
{
    return t->ctr.len ? concat(a, "ctr:", t->ctr) : t->host;
}

// Whether this window reaches a task's host by its name (kidhost): a
// container while it is there.
static b32 kidnamed(Gui *g, Arena a, GuiSession *parent, Str host)
{
    if (!host.len || parent->host->kind==GH_REMOTE) {
        return 0;  // another host's names are its own
    } else if (startswith(host, "ctr:")) {
        return ctrtask(g, a, host) != 0;
    }
    Slice<Str> names = remotenames(g, &a);
    for (iz i = 0; i < names.len; i++) {
        if (names[i] == host) {
            return 1;
        }
    }
    return 0;
}

// Connect to a task's host, and once it is up, open its subagent there,
// attended, as a local subagent of a session open here is: shown too if
// the user asked (want). Unless the user asks, not a host the user
// disconnected, nor one that failed.
static void kidreach(Gui *g, Arena a, ViewTool *t, b32 user)
{
    Str      name = kidhost(&a, t);
    GuiHost *h    = t->ctr.len ? ctrtask(g, a, name) : findhost(g, GH_REMOTE, name);
    if (t->ctr.len && !h) {
        return;  // gone
    } else if (!user && h && (h->userclosed || h->state==RS_FAILED)) {
        return;
    } else if (!h || !h->r || h->closing || h->state==RS_FAILED) {
        remoteconnect(g, a, name);
        return;
    } else if (!h->up) {
        return;  // drainmail sweeps again once it is
    }
    GuiSession *k = findsess(g, h, t->child);
    k = k ? k : addsess(g, h, t->child);
    if (!k->view) {
        guiopen(g, k);
    }
    t->opened = 1;
    if (t->want) {
        t->want = 0;
        setcur(g, k);
    }
}

// Open subagent, for one on another host: reached first if need be.
static void kidopen(Gui *g, ViewTool *t)
{
    t->want = 1;
    kidreach(g, g->frame, t, 1);
}

// Each drainmail: the subagents on other hosts of the parents open here
// whose calls run, each reached once (opened). One in the background
// only while its host, already up, lists it at work.
static void kidsweep(Gui *g)
{
    for (GuiSession *s = g->sessions; s; s = s->next) {
        View *v = s->view;
        if (!v || !v->hostcalls || s->readonly || s->host->kind==GH_REMOTE) {
            continue;
        }
        for (iz i = 0; i < v->msgs.len; i++) {
            ViewTool *t    = v->msgs[i].tool;
            b32       work = t && (t->want || t->status==VTOOL_RUNNING ||
                               t->status==VTOOL_BACKGROUND);
            Arena     a    = g->frame;
            Str       name = work ? kidhost(&a, t) : Str{};  // before a goes by value
            if (v->msgs[i].role!=VIEW_TOOL || !t->host.len || !t->child.len || !work ||
                    (t->opened && !t->want) || !kidnamed(g, a, s, name)) {
                continue;
            } else if (t->want || t->status==VTOOL_RUNNING) {
                kidreach(g, g->frame, t, t->want);
            } else if (t->status == VTOOL_BACKGROUND) {
                GuiHost    *h = findhost(g, GH_REMOTE, name);
                b32         up = h && h->r && !h->closing && h->up;
                GuiSession *k = up ? findsess(g, h, t->child) : 0;
                if (k && k->listed && k->state!=LIVE_IDLE && k->state!=LIVE_CLOSED) {
                    kidreach(g, g->frame, t, 0);
                }
            }
        }
    }
}

// How the window's link to a running task's host is, if not up.
static Str kidlink(Gui *g, Arena *a, ViewTool *t, i64 nowms)
{
    GuiHost *h = findhost(g, GH_REMOTE, kidhost(a, t));
    if (h && h->userclosed && (!h->r || h->closing)) {
        return "disconnected: its asks are denied";
    } else if (!h || h->state==RS_CONNECTING || h->state==RS_CLOSED) {
        return "connecting";
    }
    switch (h->state) {
    case RS_DEPLOYING:
        return "deploying an engine there";
    case RS_DOWN: {
        i64 left = countdown(g, h->retryat, nowms);
        Buf b(a, 64);
        print(&b, "reconnecting");
        if (left > 0) {
            print(&b, " in ");
            printsecs(&b, left);
        }
        return finish(&b);
    }
    case RS_FAILED:
        return h->deploys ? Str("no engine of this version there (Deploy engine, in its menu)")
                          : concat(a, "cannot connect: ", h->msg);
    }
    return {};
}

// Whether the user moved subagent v to the background (its call's row in
// parent p, if shown here, says so).
static b32 kidinbg(GuiSession *p, View *v)
{
    View *pv = p ? p->view : 0;
    for (iz i = pv ? pv->msgs.len-1 : -1; i >= 0; i--) {
        ViewTool *t = pv->msgs[i].tool;
        if (pv->msgs[i].role==VIEW_TOOL && t->call==v->pcall) {
            return t->status == VTOOL_BACKGROUND;
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
    ACT_BACKGROUND, // move a running command or subagent to the background
    ACT_STOP,     // stop a running command alone, the agent going on
    ACT_HOLD,     // keep a waiting ask waiting, its deadline dropped
};

// An icon button's width: the same for every icon, which all share
// one advance.
static f32 iconw()
{
    return ImGui::CalcTextSize(ICON_ADD).x + 2*ImGui::GetStyle().FramePadding.x;
}

// A button that shows an icon, as tall as a combo or a text field beside
// it, so that the icon has room above and below; highlighted while on.
static b32 iconbutton(char const *label, b32 on = 0)
{
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    b32 r = ImGui::Button(label, v2(iconw(), 0));
    if (on) {
        ImGui::PopStyleColor();
    }
    return r;
}

// An icon that toggles: flat with a dim icon while off, filled (btnon)
// with a bright one while on, and still filled under the mouse, so its
// state shows in either theme, hovered or not. Off, the mouse brings a
// row's faint fill, not a button's, which would look on.
static b32 togglebutton(Gui *g, char const *label, b32 on)
{
    Palette *p = g->pal;
    ImGui::PushStyleColor(ImGuiCol_Text, on ? p->paneltext : p->paneldim);
    ImGui::PushStyleColor(ImGuiCol_Button, on ? p->btnon : 0);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? p->btnonhover : p->rowhover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, on ? p->btnonhover : p->rowpress);
    b32 r = ImGui::Button(label, v2(iconw(), 0));
    ImGui::PopStyleColor(4);
    return r;
}

struct MsgAction {
    i32         kind;
    char const *label;
    f32         w;
    b32         amber;  // it waits for the user
    GuiSession *child;
    f32         bgw;    // a running task's Run in background too, to the
                        // left of its Open subagent, or a command's to
                        // the left of its Stop: its width, else 0
    b32         icon;   // label is an icon (Undo, Output, Rewind)
    b32         cut;    // Rewind to here takes the conversation back too
    Str         why;    // why it is disabled, empty if it is not
};

static char const bglabel[] = "Run in background";
static char const *cmdkey(Gui *, Arena *, Str);
static void itemnamed(Gui *, Str);
static b32  hostlive(Gui *, GuiSession *);

// Whether a row's command can be stopped alone (call.stop): a shell call
// running in the foreground, as its host can.
static b32 canstop(Gui *g, GuiSession *s, ViewTool *t)
{
    return s && s->view && !s->readonly && t->name=="shell" &&
           t->status==VTOOL_RUNNING && !t->job && !t->stopasked &&
           hostcap(s->host, "call.stop") && hostlive(g, s);
}

static Str sesswhy(Gui *, GuiSession *, Str, b32, b32);
static Str sessparent(GuiSession *);
static void whytip(Str);

// Why the conversation cannot be rewound to a prompt now (cut), or
// empty: as the engine has it, only while the session and everything
// under it is idle, and never to before the latest compaction.
static Str cutwhy(Gui *g, GuiSession *s, View *v, ViewMsg *m)
{
    Str why = sesswhy(g, s, "cut", 1, 1);
    if (why.len) {
        return why;
    } else if (v->tasks) {
        return "Wait for its subagents to finish";
    } else if (v->jobsrun) {
        return "Wait for its background jobs to end, or stop them";
    } else if (g->cutid || g->quitting) {
        return "Wait for the rewind under way";
    } else if (m->seq>0 && m->seq<v->compactseq) {
        return "This prompt is before the latest compaction, which summarized "
               "it: the conversation can go back only to after that";
    }
    return {};
}

// The action at the right of a message's header, if any.
static MsgAction msgaction(Gui *g, GuiSession *s, View *v, ViewMsg *m)
{
    MsgAction r = {};
    ViewTool *t = m->tool;
    if (m->role==VIEW_TOOL && t->name=="task" && s) {
        r.child = taskchild(g, s, t);
        Arena a = g->frame;
        Str   n = kidhost(&a, t);  // before a goes by value
        if (r.child || (t->child.len && kidnamed(g, a, s, n))) {
            r.amber = r.child && r.child->view && r.child->view->asks;
            r.kind  = ACT_OPEN;
            r.label = r.amber ? "Answer the subagent" : "Open subagent";
        }
    } else if (s && ((m->role==VIEW_TOOL && t->name=="send_message" && t->child.len) ||
                     (m->role==VIEW_NOTE && m->from.len))) {
        // A message's other session, if this window knows it
        GuiHost *h = m->fromhost.len ? findhost(g, GH_REMOTE, m->fromhost) : s->host;
        b32      ok = !m->fromhost.len || (h && kidnamed(g, g->frame, s, m->fromhost));
        r.child = ok && h ? findsess(g, h, m->from.len ? m->from : t->child) : 0;
        if (r.child) {
            r.kind  = ACT_OPEN;
            r.label = m->fromkid ? "Open subagent" :
                      m->from.len ? "Open sender" : "Open session";
        }
    } else if (m->role == VIEW_TOOL) {
        ViewSnap *sn = rowsnap(v, t);
        if (sn && !sn->undone && canrewind(g, s)) {
            r.kind  = ACT_UNDO;
            r.label = ICON_DISCARD "###undo";
            r.icon  = 1;
        } else if (s && t->spill.len && t->status>=VTOOL_OK && hostlive(g, s)) {
            r.kind  = ACT_OUTPUT;
            r.label = ICON_EXTERNAL "###output";
            r.icon  = 1;
        }
    } else if (m->startsturn && m->role==VIEW_USER && s && s==g->sess &&
               !sessparent(s).len && hostcap(s->host, "cut")) {
        // The conversation too, from any prompt that began a turn; before
        // a compaction, the files alone, as before, where there are any
        r.kind  = ACT_REWIND;
        r.label = ICON_HISTORY "###rewind";
        r.icon  = 1;
        r.cut   = 1;
        r.why   = cutwhy(g, s, v, m);
        if (m->seq>0 && m->seq<v->compactseq && !sesswhy(g, s, "cut", 1, 1).len &&
                canrewind(g, s) && turnrewinds(v, m->startsturn)) {
            r.cut = 0;
            r.why = {};
        }
    } else if (m->startsturn && canrewind(g, s) && turnrewinds(v, m->startsturn)) {
        r.kind  = ACT_REWIND;
        r.label = ICON_HISTORY "###rewind";
        r.icon  = 1;
    } else if (m->role==VIEW_ASK && s && t->answer==VASK_PENDING && t->deadline &&
               !v->readonly && hostcap(s->host, "ask.hold") && hostlive(g, s)) {
        // Beside its countdown: no deadline for this one
        r.kind  = ACT_HOLD;
        r.label = "Keep waiting###hold";
    }
    f32 pad = 2*ImGui::GetStyle().FramePadding.x;
    if (r.kind) {
        r.w = r.icon ? iconw() : ImGui::CalcTextSize(r.label, 0, 1).x + pad;
    }
    if (m->role==VIEW_TOOL && s==g->sess && bgrow(t) && canbackground(s)) {
        f32 w = ImGui::CalcTextSize(bglabel).x + pad;
        if (r.kind) {
            r.bgw = w;
        } else {
            r.kind  = ACT_BACKGROUND;
            r.label = bglabel;
            r.w     = w;
        }
    }
    if (m->role==VIEW_TOOL && s==g->sess && canstop(g, s, t) &&
            (!r.kind || r.kind==ACT_BACKGROUND)) {
        // At the right, Run in background left of it
        r.bgw   = r.kind ? r.w : 0;
        r.kind  = ACT_STOP;
        r.label = ICON_STOP "###stopcall";
        r.icon  = 1;
        r.w     = iconw();
    }
    return r;
}

// The room a message's actions take at the right of its header.
static f32 actwidth(MsgAction *act)
{
    f32 gap = ImGui::GetStyle().ItemSpacing.x;
    return act->kind ? act->w + (act->bgw>0 ? act->bgw + gap : 0) : 0;
}

// Ask before rewinding: the confirmation lists the files (drawrewind),
// and with conv, says what happens to the conversation, which goes back
// to before turn's prompt, the files with it if checked.
static void askrewind(Gui *g, GuiSession *s, Str call, i32 turn, b32 force, b32 conv = 0)
{
    g->rewsess    = s;
    g->rewcall    = call.len ? clone(&g->perm, call) : Str{};
    g->rewturn    = turn;
    g->rewforce   = force;
    g->rewconv    = conv;
    g->rewfiles   = 1;
    g->openrewind = 1;
}

static void drawaction(Gui *g, GuiSession *s, ViewMsg *m, MsgAction *act)
{
    ViewTool *t = m->tool;
    g->kidlinkshown |= act->amber;
    // A header line tall, as a small button, but as wide as act says
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        v2(ImGui::GetStyle().FramePadding.x, 0));
    ImGui::BeginDisabled(act->why.len > 0);
    b32 go = ImGui::Button(act->label, v2(act->w, 0));
    ImGui::EndDisabled();
    ImGui::PopStyleVar();
    switch (act->kind) {
    case ACT_OPEN:
        if (t && t->name=="task") {
            itemrect(g, RECT_TASKLINK);
            itemtip(t->host.len ? "Show the subagent's own transcript, "
                                                "on its host"
                                              : "Show the subagent's own transcript");
        } else {
            itemrect(g, RECT_MSGLINK);
            itemtip("Show that session's transcript");
        }
        if (go && t && t->name=="task" && t->host.len &&
                !(act->child && act->child->host==s->host)) {
            kidopen(g, t);  // not one kept here from its container
        } else if (go) {
            guiselect(g, act->child);
        }
        break;
    case ACT_HOLD:
        itemrect(g, RECT_HOLD);
        itemtip(t->kind=="question"
                ? "Keep waiting: no countdown for these questions, which wait "
                  "until you answer them or skip them (while this window has "
                  "the session open)"
                : "Keep waiting: no countdown for this ask, which waits until "
                  "you answer it (while this window has the session open)");
        if (go) guihold(g, s, t->req);
        break;
    case ACT_UNDO:
        itemrect(g, RECT_UNDO);
        itemtip("Undo: put back the file as it was before this call, "
                              "and every file changed after it");
        if (go) askrewind(g, s, t->call, 0, 0);
        break;
    case ACT_REWIND:
        itemrect(g, RECT_REWINDTURN);
        if (act->why.len) {
            Arena a = g->frame;
            whytip(concat(&a, "Rewind to here: not now. ", act->why));
        } else if (act->cut) {
            itemtip("Rewind to here: take the conversation back to just "
                                  "before this prompt, which goes back to the prompt "
                                  "box to edit, and if you choose, put back the files "
                                  "write and edit changed from here on");
        } else {
            itemtip("Rewind to here: put back every file write and edit "
                                  "changed from this turn on (the conversation cannot "
                                  "go back past the latest compaction)");
        }
        if (go) askrewind(g, s, {}, m->startsturn, 0, act->cut);
        break;
    case ACT_OUTPUT: {
        // Where it is, which the agent's text names too
        itemrect(g, RECT_OUTPUT);
        Arena a = g->frame;
        Str   d = s->view ? s->view->dir : Str{};
        Str   p = d.len ? join(&a, d, t->spill) : t->spill;
        itemtip("Output: show all of it, from its host:\n%s (%s)",
                              cstr0(&a, p), cstr0(&a, bytesize(&a, t->bytes)));
        if (go) showfile(g, s, t->spill);
    } break;
    case ACT_BACKGROUND: {
        // Its shortcut moves everything the turn waits for at once
        itemnamed(g, bglabel);
        g->bgshown++;
        Arena a = g->frame;
        if (t->name == "task") {
            itemtip("Let the subagent go on by itself, and the agent with "
                                  "it: the report comes to the agent once the subagent "
                                  "is done (%s)", cmdkey(g, &a, "B"));
        } else {
            itemtip("Let the command go on as a background job, and the "
                                  "agent with it: it is told when the job ends (%s)",
                                  cmdkey(g, &a, "B"));
        }
        if (go) guibackground(g, s, t->call);
    } break;
    case ACT_STOP:
        itemnamed(g, "stop command");
        g->stopshown++;
        itemtip("Stop: kill the command, and let the agent go on with "
                              "its output so far");
        if (go) guistopcall(g, s, t);
        break;
    }
}

static void itemnamed(Gui *, Str);
static void whytip(Str);

// A screenshot's thumbnail below its tool row, or an image's below the
// user's message it was attached to, fetched as it first shows, which
// opens the image whole in the Screenshot window.
static void drawthumb(Gui *g, GuiSession *s, ImDrawList *dl, Str path, i32 w, i32 h,
                      b32 user, ImVec2 p0, Thumb th, f32 top, f32 bot)
{
    Arena    a    = g->frame;
    f32      sc   = g->scale;
    ImVec2   p1   = v2(p0.x+th.w, p0.y+th.h);
    b32      in   = p1.y>=top && p0.y<=bot;
    ImgSlot *sl   = in ? imgslot(g, s, path) : 0;
    if (sl && sl->state==IMG_READY) {
        dl->AddImage(sl->tex->td.GetTexRef(), p0, p1);
        g->thumbsdrawn++;
        g->userthumbs += user;
    } else {
        dl->AddRectFilled(p0, p1, g->pal->codebg);
        Str msg = !in ? Str("") :
                  !sl ? Str("Too many images in view to show this one") :
                  sl->state==IMG_FAILED ? concat(&a, "Cannot show the image: ",
                                                 imgstr(&sl->err)) :
                  Str("Loading the image...");
        u32 col = sl && sl->state==IMG_FAILED ? g->pal->bad : g->pal->dim;
        ImVec4 clip = ImVec4(p0.x, p0.y, p1.x, p1.y);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), v2(p0.x+6*sc, p0.y+4*sc),
                    col, msg.cdata, msg.cdata+msg.len, th.w-12*sc, &clip);
    }
    dl->AddRect(p0, p1, g->pal->rule);

    ImGui::SetCursorScreenPos(p0);
    if (ImGui::InvisibleButton("##thumb", v2(th.w, th.h))) {
        imgopen(g, s, path, w, h);
    }
    if (user) {
        itemnamed(g, "user image");
    } else {
        itemrect(g, RECT_THUMB);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    itemtip("%dx%d pixels: open it whole", w, h);
}

// A rewound conversation's Undo, until a turn begins after it.
static void drawcutundo(Gui *g, GuiSession *s, ViewMsg *m)
{
    View *v   = s ? s->view : 0;
    Str   why = !v ? Str("Open the session") : sesswhy(g, s, "cut", 1, 1);
    why = why.len ? why : v->tasks   ? Str("Wait for its subagents to finish") :
                          v->jobsrun ? Str("Wait for its background jobs to end") :
                          g->cutid   ? Str("Wait for the rewind under way") : why;
    ImGui::BeginDisabled(why.len > 0);
    b32 go = ImGui::Button("Undo rewind");
    itemrect(g, RECT_CUTUNDO);
    ImGui::EndDisabled();
    if (why.len) {
        whytip(why);
    } else {
        itemtip("Bring back the turns taken out, and the files as they "
                              "were before the rewind: until the next message is sent");
    }
    if (go) {
        guiuncut(g, s);
    }
    ImGui::SameLine();
    if (m->card->conflicts) {
        // The files left alone, as the files' own rewind has it
        ImGui::BeginDisabled(!canrewind(g, s));
        b32 force = ImGui::Button("Put back anyway...");
        itemrect(g, RECT_FORCE);
        itemtip("Put back the files left alone too, overwriting what "
                              "changed them since (not undone by Undo rewind)");
        ImGui::EndDisabled();
        if (force) {
            askrewind(g, s, {}, m->card->turn, 1);
        }
    } else {
        ImGui::TextDisabled("until the next message");
    }
}

// The latest rewind's button, when it left files alone.
static void drawforce(Gui *g, GuiSession *s, ViewMsg *m)
{
    ViewCard *c = m->card;
    ImGui::BeginDisabled(!canrewind(g, s));
    b32 go = ImGui::Button("Rewind anyway...");
    itemrect(g, RECT_FORCE);
    itemtip("Put these files back too, overwriting what changed them since");
    ImGui::EndDisabled();
    if (go) {
        askrewind(g, s, c->to, c->turn, 1);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("overwrites the changes made since");
}

// Whether the plan of the session is out in the editor, for the ask req
// (Approve held), or for none, which holds any.
static b32 planediting(Gui *g, GuiSession *s, Str req)
{
    return g->edsess==s && g->edplan && (!g->edreq.len || g->edreq==req);
}

// Why a session's plan file cannot go out to the editor, or empty: only
// a plan on this computer does.
static Str planeditwhy(Gui *g, GuiSession *s, Str path, Arena *a)
{
    if (s->host->kind == GH_REMOTE) {
        return concat(a, "The plan is on ", concat(a, s->host->name,
                      ": only a plan on this computer opens in the editor"));
    }
    return !path.len ? Str("No plan file") : editorwhy(g, s, a);
}

static Str planedittip = "Revise the plan file by hand in your editor before you approve "
                         "it: the agent carries out the plan as the file has it then";

// The plan file in the editor (planedit), for the plan's ask req, else
// none: its button, or while it is out, which editor and a Cancel.
static void planeditbutton(Gui *g, GuiSession *s, Str path, Str req, char const *label,
                           Str named, Str tip)
{
    Arena a = g->frame;
    if (g->edsess==s && g->edplan) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("editing in %s...", cstr0(&a, g->edname));
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel##planedit")) {
            editcancel(g);
        }
        itemnamed(g, "cancel plan editor");
        itemtip("Stop the editor: the plan is as it left the file");
        return;
    }
    Str why = planeditwhy(g, s, path, &a);
    ImGui::BeginDisabled(why.len != 0);
    if (ImGui::Button(label)) {
        planedit(g, a, s, path, req);
    }
    ImGui::EndDisabled();
    itemnamed(g, named);
    whytip(why.len ? why : tip);
}

// The form for a session's waiting questions, started over for each new
// ask with its recommended options picked.
static QForm *qformof(Gui *g, GuiSession *s, ViewTool *t)
{
    QForm *f = s->qform;
    if (!f) {
        f = s->qform = alloc<QForm>(&g->perm);
        for (i32 i = 0; i < 4; i++) {
            f->own[i] = alloc<char>(&g->perm, FIELD_MAX);
        }
        f->note = alloc<char>(&g->perm, FIELD_MAX);
    }
    if (f->req != t->req) {
        f->req = clone(&g->perm, t->req);
        for (i32 i = 0; i < 4; i++) {
            ViewQuestion *q = i<t->qs.len ? &t->qs[i] : 0;
            for (i32 k = 0; k < 4; k++) {
                f->pick[i][k] = q && k<q->opts.len && q->opts[k].rec;
            }
            f->other[i]  = 0;
            f->own[i][0] = 0;
        }
        f->note[0] = 0;
        f->sent    = 0;
    }
    return f;
}

// Before a question form's field id is drawn: Esc leaves it, keeping
// what was typed, where ImGui's own would undo the edit, and is not the
// turn's cancel (escapecancel) for this frame either. Returns whether
// Enter was pressed in it.
static b32 askfield(Gui *g, char const *id)
{
    if (ImGui::GetActiveID() != ImGui::GetID(id)) {
        return 0;
    }
    g->askactive = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::ClearActiveID();
    }
    return ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
           ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
}

static ImVec2 textsize(ImFont *f, f32 size, Str s, f32 wrap)
{
    return f->CalcTextSizeA(size, 1e9f, wrap, s.cdata, s.cdata+s.len);
}

// The agent's questions (ask_user) as a form below their card: for each,
// its header in a chip and the question, then its options, each a row
// to click with a radio or a check, the label, a Recommended badge and
// the description, then a field for words of the user's own; below them
// a note, Submit and Skip. Without draw it only measures: the height
// comes from the text and the width alone, never from what is picked or
// typed, so that the transcript reserves it (msgfoot) before the form
// is drawn. Returns the height.
static f32 questionform(Gui *g, GuiSession *s, ViewMsg *m, f32 width, b32 draw,
                        b32 latest)
{
    ViewTool   *t      = m->tool;
    Palette    *pal    = g->pal;
    Arena       a      = g->frame;
    f32         sc     = g->scale;
    f32         size   = g->fonts.size;
    f32         small  = smallsize(&g->fonts);
    ImFont     *bold   = stylefont(&g->fonts, STYLE_BOLD);
    ImFont     *prose  = stylefont(&g->fonts, 0);
    f32         frame  = ImGui::GetFrameHeight();
    f32         indent = 26*sc;
    f32         gap    = 6*sc;
    Str         rec    = "Recommended";
    ImVec2      bs     = textsize(prose, small, rec, 0);
    f32         badgew = bs.x + 10*sc;
    ImVec2      o      = draw ? ImGui::GetCursorScreenPos() : v2(0, 0);
    ImDrawList *dl     = draw ? ImGui::GetWindowDrawList() : 0;
    QForm      *f      = draw ? qformof(g, s, t) : 0;
    b32         off    = s->view->readonly || (f && f->sent);
    Str         need   = {};
    b32         entered = 0;
    b32         touched = 0;  // the user works on the answers
    f32         y      = 0;

    for (iz i = 0; i < t->qs.len; i++) {
        ViewQuestion *q = &t->qs[i];
        if (i) {
            y += 2*gap;
        }

        // The header's chip, and the question beside it
        ImVec2 hs    = textsize(bold, small, q->header, 0);
        f32    chipw = hs.x + 12*sc;
        f32    chiph = hs.y + 4*sc;
        f32    qwrap = width - chipw - 8*sc;
        ImVec2 qs    = textsize(bold, size, q->text, qwrap);
        if (draw) {
            dl->AddRectFilled(v2(o.x, o.y+y), v2(o.x+chipw, o.y+y+chiph),
                              fade(pal->amber, 0.18f), 4*sc);
            dl->AddText(bold, small, v2(o.x+6*sc, o.y+y+2*sc), pal->amber,
                        q->header.cdata, q->header.cdata+q->header.len);
            dl->AddText(bold, size, v2(o.x+chipw+8*sc, o.y+y), pal->text,
                        q->text.cdata, q->text.cdata+q->text.len, qwrap);
        }
        y += (chiph>qs.y ? chiph : qs.y) + gap;

        // Its options
        b32 answered = 0;
        for (iz k = 0; k < q->opts.len; k++) {
            ViewOption *opt   = &q->opts[k];
            f32         lwrap = width - indent - (opt->rec ? badgew+6*sc : 0);
            ImVec2      ls    = textsize(bold, size, opt->label, lwrap);
            ImVec2      ds    = opt->desc.len ? textsize(prose, small, opt->desc, width-indent)
                                              : v2(0, 0);
            f32         body  = ls.y + (opt->desc.len ? 2*sc + ds.y : 0);
            f32         rowh  = (frame>body ? frame : body) + 4*sc;
            if (draw) {
                b32 on = f->pick[i][k];
                answered |= on;
                ImGui::PushID((i32)(i*8 + k));
                ImGui::SetCursorScreenPos(v2(o.x, o.y+y));
                ImGui::BeginDisabled(off);
                if (ImGui::InvisibleButton("##opt", v2(width, rowh))) {
                    touched = 1;
                    if (q->multi) {
                        f->pick[i][k] = !on;
                    } else {
                        for (iz j = 0; j < 4; j++) {
                            f->pick[i][j] = j == k;
                        }
                        f->other[i] = 0;
                    }
                }
                ImGui::EndDisabled();
                Buf nb(&a, 64);
                print(&nb, "question ");
                print(&nb, (i64)i+1);
                print(&nb, " option ");
                print(&nb, (i64)k+1);
                itemnamed(g, finish(&nb));
                if (!off && ImGui::IsItemHovered()) {
                    dl->AddRectFilled(v2(o.x-4*sc, o.y+y), v2(o.x+width, o.y+y+rowh),
                                      fade(pal->amber, 0.10f), 4*sc);
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                }
                // The radio or the check, beside the label's first line
                u32    col = off ? pal->dim : pal->link;
                ImVec2 c   = v2(o.x + 11*sc, o.y + y + 2*sc + size*0.6f);
                if (q->multi) {
                    ImVec2 c0 = v2(c.x-6*sc, c.y-6*sc), c1 = v2(c.x+6*sc, c.y+6*sc);
                    if (f->pick[i][k]) {
                        dl->AddRectFilled(c0, c1, col, 2*sc);
                        ImVec2 tick[3] = {v2(c.x-3.5f*sc, c.y), v2(c.x-1*sc, c.y+2.5f*sc),
                                          v2(c.x+3.5f*sc, c.y-3*sc)};
                        dl->AddPolyline(tick, 3, pal->panel, 1.8f*sc);
                    } else {
                        dl->AddRect(c0, c1, col, 2*sc, 1.5f*sc);
                    }
                } else {
                    dl->AddCircle(c, 6*sc, col, 0, 1.5f*sc);
                    if (f->pick[i][k]) {
                        dl->AddCircleFilled(c, 3.5f*sc, col);
                    }
                }
                dl->AddText(bold, size, v2(o.x+indent, o.y+y+2*sc),
                            off ? pal->dim : pal->text,
                            opt->label.cdata, opt->label.cdata+opt->label.len, lwrap);
                if (opt->rec) {
                    f32 lx = textsize(bold, size, opt->label, 0).x;
                    lx = lx<lwrap ? lx : lwrap;
                    ImVec2 b0 = v2(o.x+indent+lx+6*sc, o.y+y+2*sc + (size-bs.y)/2 - 1*sc);
                    ImVec2 b1 = v2(b0.x+badgew, b0.y+bs.y+2*sc);
                    dl->AddRectFilled(b0, b1, fade(pal->link, 0.16f), 3*sc);
                    dl->AddText(prose, small, v2(b0.x+5*sc, b0.y+1*sc), pal->link,
                                rec.cdata, rec.cdata+rec.len);
                }
                if (opt->desc.len) {
                    dl->AddText(prose, small, v2(o.x+indent, o.y+y+2*sc+ls.y+2*sc), pal->dim,
                                opt->desc.cdata, opt->desc.cdata+opt->desc.len, width-indent);
                }
                ImGui::PopID();
            }
            y += rowh;
        }

        // Words of the user's own, which typing chooses
        if (draw) {
            ImGui::PushID((i32)i);
            u32    col = off ? pal->dim : pal->link;
            ImVec2 c   = v2(o.x + 11*sc, o.y + y + 2*sc + frame/2);
            ImGui::SetCursorScreenPos(v2(o.x, o.y+y+2*sc));
            ImGui::BeginDisabled(off);
            if (ImGui::InvisibleButton("##otherpick", v2(indent, frame))) {
                touched = 1;
                f->other[i] = !f->other[i];
                for (iz j = 0; j<4 && f->other[i] && !q->multi; j++) {
                    f->pick[i][j] = 0;
                }
            }
            if (q->multi) {
                ImVec2 c0 = v2(c.x-6*sc, c.y-6*sc), c1 = v2(c.x+6*sc, c.y+6*sc);
                if (f->other[i]) {
                    dl->AddRectFilled(c0, c1, col, 2*sc);
                    ImVec2 tick[3] = {v2(c.x-3.5f*sc, c.y), v2(c.x-1*sc, c.y+2.5f*sc),
                                      v2(c.x+3.5f*sc, c.y-3*sc)};
                    dl->AddPolyline(tick, 3, pal->panel, 1.8f*sc);
                } else {
                    dl->AddRect(c0, c1, col, 2*sc, 1.5f*sc);
                }
            } else {
                dl->AddCircle(c, 6*sc, col, 0, 1.5f*sc);
                if (f->other[i]) {
                    dl->AddCircleFilled(c, 3.5f*sc, col);
                }
            }
            ImGui::SameLine(0, 0);
            ImGui::SetNextItemWidth(width - indent);
            entered |= askfield(g, "##own");
            b32 typed = ImGui::InputTextWithHint("##own", "Other: answer in your own words",
                                                 f->own[i], FIELD_MAX);
            touched |= typed;
            if (typed && f->own[i][0]) {
                f->other[i] = 1;
                for (iz j = 0; j<4 && !q->multi; j++) {
                    f->pick[i][j] = 0;
                }
            }
            g->askactive |= ImGui::IsItemActive();
            ImGui::EndDisabled();
            Buf nb(&a, 64);
            print(&nb, "question ");
            print(&nb, (i64)i+1);
            print(&nb, " other");
            itemnamed(g, finish(&nb));
            ImGui::PopID();
            Str own = trim(Str{(u8 *)f->own[i], (iz)__builtin_strlen(f->own[i])});
            answered |= f->other[i] && own.len;
            if (!answered && !need.len) {
                need = concat(&a, "Answer every question first: ", q->header);
            }
        }
        y += frame + 4*sc;
    }

    // A note for the agent, then Submit and Skip
    y += gap;
    if (draw) {
        ImGui::SetCursorScreenPos(v2(o.x, o.y+y));
        ImGui::BeginDisabled(off);
        ImGui::SetNextItemWidth(width);
        entered |= askfield(g, "##qnote");
        touched |= ImGui::InputTextWithHint("##qnote", "Note for the agent (optional)",
                                            f->note, FIELD_MAX);
        g->askactive |= ImGui::IsItemActive();
        itemnamed(g, "question note");
        ImGui::EndDisabled();
    }
    y += frame + gap;
    if (draw) {
        // Enter in a field submits, once every question has an answer
        b32 enter = entered && !need.len && !off;
        ImGui::SetCursorScreenPos(v2(o.x, o.y+y));
        ImGui::BeginDisabled(off || need.len);
        if (ImGui::Button(t->qs.len==1 ? "Submit answer" : "Submit answers") || enter) {
            guianswerq(g, s, t, f, 0);
        }
        ImGui::EndDisabled();
        if (latest) itemrect(g, RECT_ALLOW);
        whytip(f->sent ? Str("Sent: waiting for the agent") :
               need.len ? need : Str("Send your answers to the agent"));
        ImGui::SameLine();
        ImGui::BeginDisabled(off);
        if (ImGui::Button("Skip")) {
            guianswerq(g, s, t, f, 1);
        }
        ImGui::EndDisabled();
        if (latest) itemrect(g, RECT_DENY);
        itemtip("Answer none of them: the agent goes with its "
                              "recommendations, or with your note instead");
    }
    y += frame;
    if (draw) {
        g->qformdrawn = y;
        if (touched && !off) {
            guitouchq(g, s, t, f);
        }
    }
    return y;
}

// An open ask's buttons, below its text. Always, where the ask offers
// it and the host's engine can take it, stands apart from Allow, past a
// rule, so that neither is pressed for the other.
static void drawaskbuttons(Gui *g, GuiSession *s, ViewMsg *m, b32 latest)
{
    ViewTool *t = m->tool;
    Arena     a = g->frame;
    if (t->kind=="plan" || t->kind=="plan.enter") {
        // Plan mode's asks: the plan to approve, or to plan first. Not
        // while the plan is out in the editor, nor once it is empty.
        b32 enter   = t->kind == "plan.enter";
        b32 editing = !enter && planediting(g, s, t->req);
        Str nogo    = editing ? Str("The plan is open in the editor: close it first, "
                                    "and the plan as it saved it is the one approved") :
                      !enter && !trim(m->text).len
                              ? Str("The plan file is empty, or too long to show: write "
                                    "the plan there, or keep planning") : Str{};
        ImGui::BeginDisabled(nogo.len != 0);
        if (ImGui::Button(enter ? "Plan first" : "Approve plan")) {
            guianswer(g, s, t->req, "allow", {});
        }
        ImGui::EndDisabled();
        if (latest) itemrect(g, RECT_ALLOW);
        whytip(nogo.len ? nogo :
               enter    ? Str("Plan mode: the agent explores and writes a plan, "
                              "changing nothing else until you approve it")
                        : Str("End plan mode: the agent carries out this plan"));
        ImGui::SameLine();
        if (ImGui::Button(enter ? "Go ahead without" : "Keep planning\xe2\x80\xa6")) {
            if (enter) {
                guianswer(g, s, t->req, "deny", {});
            } else {
                g->denyreq   = t->req;
                g->denysess  = s;
                g->denytimed = t->deadline != 0;
                g->denynote[0] = 0;
                g->opendeny  = 2;
            }
        }
        if (latest) itemrect(g, RECT_DENY);
        itemtip(enter ? "No plan mode: the agent works as before"
                                    : "Plan mode stays on: tell the agent what to change "
                                      "in the plan");
        if (enter) {
            return;
        }

        // Edit plan: the plan file in the editor, on this machine
        ImGui::SameLine();
        planeditbutton(g, s, t->path, t->req, "Edit plan", "edit plan", planedittip);
        return;
    }
    if (ImGui::Button("Allow")) {
        guianswer(g, s, t->req, "allow", {});
    }
    if (latest) itemrect(g, RECT_ALLOW);
    b32 proj = t->scope == "mcp.project";
    if (proj) {
        itemtip("Start these servers for this session and its subagents, "
                              "whose agents may then ask to use their tools");
    } else if (t->scope == "approve") {
        itemtip("Send these to these urls, from now on, for every "
                              "session, until a web include changes a url");
    } else {
        itemtip("Let the %s go ahead", t->tool.len ? cstr0(&a, t->tool) : "tool");
    }

    b32 desk   = t->scope == "desktop";
    b32 mcp    = t->scope == "mcp";
    b32 web    = t->scope == "web";
    b32 ctr    = t->scope == "container";
    b32 always = t->grant.len &&
                 hostcap(s->host, desk ? Str("answer.always.desktop") :
                                  mcp || proj ? Str("mcp") :
                                  web ? Str("web") : Str("answer.always"));
    if (always) {
        f32 sp = ImGui::GetStyle().ItemSpacing.x;
        f32 h  = ImGui::GetFrameHeight();
        ImGui::SameLine(0, sp);
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(p, v2(p.x, p.y+h),
                                            ImGui::GetColorU32(ImGuiCol_Separator));
        ImGui::Dummy(v2(1, h));
        ImGui::SameLine(0, sp);

        // The class or directory in the code font, as the header has it
        Str runs[3]   = {};
        i32 styles[3] = {0, STYLE_CODE, 0};
        if (desk) {
            runs[0] = "Allow ";
            runs[1] = t->grant;
            runs[2] = " for this session";
        } else if (proj) {
            runs[0] = "Always for this project";
        } else if (ctr) {
            runs[0] = "Always allow ";
            runs[1] = t->grant.len<=32 ? t->grant :  // an image
                      concat(&a, utf8head(t->grant, 31), "\xe2\x80\xa6");
            runs[2] = "";
        } else if (web && t->grant=="search") {
            runs[0] = "Always allow web searches";
        } else if (web) {
            runs[0] = "Always allow ";
            runs[1] = t->grant.len<=32 ? t->grant :  // a host
                      concat(&a, utf8head(t->grant, 31), "\xe2\x80\xa6");
            runs[2] = "";
        } else if (mcp) {
            // The server's name, or an older engine's tool's own, after
            // mcp__ or mcp__SERVER__, cut if long
            Str tool = t->grant;
            iz  at   = find(cuthead(tool, 5), "__");
            tool = !startswith(tool, "mcp__") ? tool :
                   at>=0                      ? cuthead(tool, 5+at+2) : cuthead(tool, 5);
            if (tool.len > 24) {
                tool = concat(&a, utf8head(tool, 23), "\xe2\x80\xa6");
            }
            runs[0] = "Always allow ";
            runs[1] = tool;
            runs[2] = "";
        } else {
            // Its last name, cut to its end if long, so that the
            // buttons fit; the tooltip has the whole path
            Str dir = t->grant;
            iz  i   = dir.len;
            for (; i>1 && dir[i-1]!='/' && dir[i-1]!='\\'; i--) {}
            dir = i>1 && i<dir.len ? cuthead(dir, i) : dir;
            if (dir.len > 20) {
                i = dir.len - 19;
                for (; i<dir.len && (dir[i]&0xc0)==0x80; i++) {}
                dir = concat(&a, "\xe2\x80\xa6", cuthead(dir, i));
            }
            runs[0] = "Always allow ";
            runs[1] = dir;
            runs[2] = t->access=="write" ? Str(" (write)") : Str(" (read)");
        }
        if (penbutton(g, "##always", runs, styles, countof(runs))) {
            guianswer(g, s, t->req, "always", {});
        }
        if (latest) itemrect(g, RECT_ALWAYS);
        if (desk) {
            itemtip("Allow this action, and every %s action from now on in "
                                  "this session and its subagents, without asking",
                                  cstr0(&a, t->grant));
        } else if (proj) {
            itemtip("Start these servers now, and in later sessions here "
                                  "without asking, until the file changes them");
        } else if (ctr) {
            itemtip("Allow this, and subagents in containers of %s from "
                                  "now on in this session and its subagents, without "
                                  "asking, when they have no network", cstr0(&a, t->grant));
        } else if (web && t->grant=="search") {
            itemtip("Allow this, and every web search from now on in this "
                                  "session and its subagents, without asking: their "
                                  "queries go to the search service unseen");
        } else if (web) {
            itemtip("Allow this, and reading every page of %s from now on "
                                  "in this session and its subagents, without asking",
                                  cstr0(&a, t->grant));
        } else if (mcp && find(cuthead(t->grant, 5), "__")<0) {
            itemtip("Allow this, and every call to a tool of %s from now on "
                                  "in this session and its subagents, without asking",
                                  cstr0(&a, cuthead(t->grant, 5)));
        } else if (mcp) {
            itemtip("Allow this, and every call to %s from now on in this "
                                  "session and its subagents, without asking",
                                  cstr0(&a, t->grant));
        } else {
            itemtip("Allow this, and every %s in %s from now on in this "
                                  "session and its subagents, without asking",
                                  t->access=="write" ? "read and write" : "read",
                                  cstr0(&a, t->grant));
        }
    }

    // A project's server, trusted for good
    if (mcp && t->trust.len && hostcap(s->host, "answer.trust")) {
        ImGui::SameLine();
        Str runs[3]   = {"Trust ", t->trust, " in this project"};
        i32 styles[3] = {0, STYLE_CODE, 0};
        if (penbutton(g, "##trust", runs, styles, countof(runs))) {
            guianswer(g, s, t->req, "trust", {});
        }
        itemtip("Allow this, and every call to a tool of %s from now on, "
                              "in later sessions too, without asking, until the "
                              "project's file changes the server", cstr0(&a, t->trust));
    }

    ImGui::SameLine();
    if (ImGui::Button("Deny")) {
        guianswer(g, s, t->req, "deny", {});
    }
    if (latest) itemrect(g, RECT_DENY);
    itemtip("The agent reads why and tries something else");
    ImGui::SameLine();
    if (ImGui::Button("Deny with note...")) {
        g->denyreq   = t->req;
        g->denysess  = s;
        g->denytimed = t->deadline != 0;
        g->denynote[0] = 0;
        g->opendeny  = 1;
    }
    itemtip("Deny, telling the agent what to do instead");
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

// The folder a session works in, which Open working folder shows: else
// null, and why in *why. A container's is here where a folder here is
// mounted at /work.
static Str sesswork(Gui *g, Arena *a, GuiSession *s, Str *why)
{
    Str  cwd = s->view && s->view->cwd.len ? s->view->cwd : s->cwd;
    b32  run = 0;
    Ctr *c   = s->host->kind==GH_REMOTE ? hostctr(g, s->host, &run) : 0;
    if (c && cwd.len) {
        cwd  = ctrhostpath(a, c, cwd, HOST_WINDOWS);
        *why = cwd.len ? Str{} : Str("Its files are only in its container");
        return cwd;
    }
    *why = s->host->kind==GH_REMOTE ? Str("Its files are on another computer") :
           !cwd.len                 ? Str("Its folder is not known yet") : Str{};
    return why->len ? Str{} : cwd;
}

static void workopen(Gui *g, Arena a, Str cwd)
{
    if (!os_guireveal(g->os, a, cwd)) {
        g->lasterr = clone(&g->perm, concat(&a, "cannot show ", cwd));
    }
}

static void inforow(char const *key, Str value)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", key);
    ImGui::TableNextColumn();
    text(value.len ? value : Str("(none)"));
}

static b32  workrows(Arena, View *);

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
        inforow("Directory", v->nofolder ? Str("No folder") : v->cwd);
        Str model = v->model;
        if (model.len && hostcan(s->host, "models")) {
            model = modelshown(&a, s->host, v->endpoint, model);  // ENDPOINT/MODEL
        }
        inforow("Model", model);
        inforow("Endpoint", v->endpoint);
        if (v->shell.len) {
            inforow("Shell", v->shellpath.len && v->shellpath!=v->shell
                             ? concat(&a, concat(&a, v->shell, ", "), v->shellpath) : v->shell);
        }
        if (v->role.len) {
            inforow("Role", v->role);
        }
        if (v->tools.len) {
            inforow("Tools", v->tools);
        }
        if (v->parent.len) {
            GuiSession *p = sessup(g, s);
            inforow("Parent", p ? sesstitle(p) : v->remote ? concat(&a, v->parent, " (on another host)")
                                                           : v->parent);
        }
        if (s->host->kind != GH_ENGINE) {
            inforow("Host", hostlabel(g, &a, s->host));
        }
        g->infowork = workrows(a, v);
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
        if (iconbutton(ICON_COPY "##instrcopy")) {
            ImGui::SetClipboardText(cstr0(&a, v->instrtext));
        }
        itemnamed(g, "instructions copy");
        itemtip("Copy what the agent was given: these files' text, as its "
                              "system prompt has it");
    }

    // Skills, as its engine last listed them: /name invokes one
    ImGui::Separator();
    ImGui::TextUnformatted("Skills");
    if (!hostcap(s->host, "skills")) {
        ImGui::TextDisabled("None: this host's engine is too old for skills.");
    } else if (!s->skills.len) {
        ImGui::TextDisabled("None: no SKILL.md in the project's skill directories, nor in yours.");
    }
    for (iz i = 0; i < s->skills.len; i++) {
        SkillItem *k = &s->skills[i];
        ImGui::Bullet();
        text(concat(&a, "/", k->name));
        if (k->desc.len) {
            itemtip("%s", cstr0(&a, k->desc));
        }
        ImGui::SameLine();
        Str where = k->builtin ? Str("built into Prospero") : showpath(v, k->path);
        ImGui::TextDisabled("%s%s", cstr0(&a, where),
                            k->hidden ? ", for you alone: not offered to the agent" : "");
    }

    // Memory: the project's, its index as the agent knows it
    ImGui::Separator();
    ImGui::TextUnformatted("Memory");
    if (!hostcap(s->host, "memory")) {
        ImGui::TextDisabled("None: this host's engine is too old for memory.");
    } else if (!v->memdir.len) {
        ImGui::TextDisabled("None: memory is for your own sessions in a project, "
                            "while [memory] is on.");
    } else {
        text(v->memdir);
        ImGui::SameLine();
        Str why = s->host->kind==GH_REMOTE
                ? concat(&a, "The memory is on ", concat(&a, s->host->name,
                         ": only a file on this computer shows in its file manager"))
                : !v->mems.len ? Str("Nothing is saved yet") : Str{};
        ImGui::BeginDisabled(why.len != 0);
        if (iconbutton(ICON_FOLDER "##memreveal")) {
            guishow(g, join(&a, v->memdir, "MEMORY.md"));
        }
        ImGui::EndDisabled();
        itemnamed(g, "memory reveal");
        whytip(why.len ? why : Str("Reveal: show MEMORY.md, the index of the memories, in "
                                   "the file manager"));
        if (!v->mems.len) {
            ImGui::TextDisabled("Nothing is saved yet: the agent saves what later "
                                "sessions here should know, and what you ask it to "
                                "remember.");
        }
        for (iz i = 0; i < v->mems.len; i++) {
            ViewMem *m = &v->mems[i];
            ImGui::Bullet();
            text(m->name);
            if (m->desc.len) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", cstr0(&a, m->desc));
            }
        }
    }
    ImGui::PopTextWrapPos();
    g->infoshown = (i32)v->instr.len;
    g->infoagents = agents;
    g->infomems = (i32)v->mems.len;
}

// The context gauge: a thin bar of the tokens the context holds, on the
// scale of the model's window, with a tick where compaction begins, amber
// from 90% of the way there. With no window known, a bar has no scale,
// so the count alone (issue #43); nothing before there is a count.
static b32 gaugescaled(View *v)
{
    return v->tokens>0 && (v->ctx>0 || v->compactat>0);
}

static char const gaugewidest[] = "999k tokens";

static f32 gaugewidth(Gui *g, View *v)
{
    return gaugescaled(v) ? 64*g->scale :
           v->tokens>0    ? ImGui::CalcTextSize(gaugewidest).x : 0;
}

// The bar itself, bh high from (x, y), w wide: the session bar's, and
// thinner, a sidebar row's. Returns whether it is near compaction.
static b32 gaugebar(Gui *g, ImDrawList *dl, View *v, f32 x, f32 y, f32 w, f32 bh, f32 tick)
{
    Palette *pal   = g->pal;
    i64      scale = v->ctx>0 ? v->ctx : v->compactat;
    b32      near  = v->compactat>0 && v->tokens*10 >= v->compactat*9;
    f32      frac  = (f32)v->tokens / (f32)scale;
    frac = frac>1 ? 1 : frac;
    dl->AddRectFilled(v2(x, y), v2(x+w, y+bh), pal->codebg, bh/2);
    dl->AddRectFilled(v2(x, y), v2(x+w*frac, y+bh), near ? pal->amber : pal->active, bh/2);
    if (v->compactat>0 && v->compactat<scale) {
        f32 tx = x + w*(f32)v->compactat/(f32)scale;
        dl->AddLine(v2(tx, y-tick), v2(tx, y+bh+tick), pal->dim, 1.5f*g->scale);
    }
    return near;
}

// The session's work (viewwork): ms in all, and whether there is any.
static i64 worktotal(View *v)
{
    ViewWork *w = &v->work;
    return w->model + w->tools + w->compact;
}

static void printwork(Buf *b, i64 ms)
{
    printsecs(b, (ms + 500)/1000);
}

// The info pane's rows of it: the time, the tokens and the rate.
// Returns whether there were any.
static b32 workrows(Arena a, View *v)
{
    ViewWork *w = &v->work;
    if (!w->requests && worktotal(v)<1000) {
        return 0;
    }
    Buf b(&a, 256);
    printwork(&b, worktotal(v));
    print(&b, ": the model ");
    printwork(&b, w->model);
    if (w->first >= 1000) {
        print(&b, " (");
        printwork(&b, w->first);
        print(&b, " before its first tokens)");
    }
    if (w->tools >= 1000) {
        print(&b, ", tools ");
        printwork(&b, w->tools);
        if (w->tasks >= 1000) {
            print(&b, " (subagents ");
            printwork(&b, w->tasks);
            putbyte(&b, ')');
        }
    }
    if (w->compact >= 1000) {
        print(&b, ", compacting ");
        printwork(&b, w->compact);
    }
    inforow("Worked", finish(&b));

    Buf t(&a, 256);
    printgrouped(&t, w->in);
    print(&t, " in");
    if (w->in > 0) {
        print(&t, ", ");
        printgrouped(&t, w->cached);
        print(&t, " of them cached (");
        print(&t, w->cached*100/w->in);
        print(&t, "%)");
    }
    print(&t, ", ");
    printgrouped(&t, w->out);
    print(&t, " out, in ");
    printgrouped(&t, w->requests);
    print(&t, w->requests==1 ? Str(" request") : Str(" requests"));
    inforow("Tokens", finish(&t));

    if (w->genms>0 && w->genout>0) {
        Buf r(&a, 64);
        printfloat(&r, (f64)w->genout*1000/(f64)w->genms, 1);
        print(&r, " tok/s, from the first token on");
        inforow("Output", finish(&r));
    }
    return 1;
}

// The gauge's numbers, for a tooltip.
static void gaugetip(Buf *b, View *v)
{
    print(b, "Context: ");
    printgrouped(b, v->tokens);
    print(b, " tokens");
    if (v->compactat > 0) {
        print(b, "\nCompacts at ");
        printgrouped(b, v->compactat);
        print(b, " (");
        print(b, v->tokens*100/v->compactat);
        print(b, "% of the way)");
    }
    if (v->ctx > 0) {
        print(b, "\nWindow: ");
        printgrouped(b, v->ctx);
    } else if (v->compactat <= 0) {
        print(b, "\nWindow: unknown, as the server does not report it, so "
                 "compaction waits until it refuses a prompt as too long. Set "
                 "the model's context in Settings to compact ahead of that.");
    }
    print(b, "\nCached in the last request: ");
    printgrouped(b, v->cached);
}

static void drawgauge(Gui *g, View *v, f32 w)
{
    Arena       a     = g->frame;
    f32         sc    = g->scale;
    f32         h     = ImGui::GetFrameHeight();
    ImVec2      p0    = ImGui::GetCursorScreenPos();
    if (gaugescaled(v)) {
        f32 bh   = 5*sc;
        b32 near = gaugebar(g, ImGui::GetWindowDrawList(), v, p0.x, p0.y + (h - bh)/2,
                            w, bh, 2*sc);
        g->gaugeshown = 1 + near;
    } else {
        Buf b(&a, 32);
        printcount(&b, v->tokens);
        print(&b, " tokens");
        Str    label = finish(&b);
        ImVec2 ts    = ImGui::CalcTextSize((char *)label.data, (char *)label.data+label.len);
        ImGui::GetWindowDrawList()->AddText(v2(p0.x + w - ts.x, p0.y + (h - ts.y)/2),
                                            ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                            (char *)label.data,
                                            (char *)label.data+label.len);
        g->gaugeshown = 3;
    }
    ImGui::Dummy(v2(w, h));
    itemrect(g, RECT_GAUGE);
    if (ImGui::BeginItemTooltip()) {
        Buf b(&a, 256);
        gaugetip(&b, v);
        ImGui::PushTextWrapPos(tipwrap());
        text(finish(&b));
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// A toggle button, highlighted while on.
// A shortcut's name: Cmd on macOS, Ctrl elsewhere.
static char const *shortcutname(Gui *g, i32 which)
{
    switch (which) {
    case 0: return g->mac ? "Cmd+Shift+T" : "Ctrl+Shift+T";
    case 1: return g->mac ? "Cmd+Shift+R" : "Ctrl+Shift+R";
    case 2: return g->mac ? "Cmd+Shift+S" : "Ctrl+Shift+S";
    }
    return g->mac ? "Cmd+F" : "Ctrl+F";
}

static void modelitems(Gui *, Arena, GuiSession *);
static Str  sesswhy(Gui *, GuiSession *, Str cap, b32 idle, b32 rootonly);
static Str  sessparent(GuiSession *);
static Str  sessside(GuiSession *);
static b32  menuitem(Gui *, Arena, Str label, Str why, char const *key);
static void whytip(Str);
static void itemnamed(Gui *, Str);
static b32  planwaits(View *);
static Str  modewhy(Gui *, GuiSession *, Str mode);

// The session's model, a combo of those configured to switch to (model)
// while idle, and what the latest switch's reply noted. Like the shell's
// and the mode's, it goes where the caller put the cursor.
static void drawmodelpick(Gui *g, Arena a, GuiSession *s, View *v)
{
    Str   why = g->enginekind==ENGINE_FAKE ? Str("No engine") :
                sesswhy(g, s, "model", !planwaits(v), 0);
    Str   now = v->model.len ? v->model : s->model;
    if (now.len && hostcan(s->host, "models")) {
        now = modelshown(&a, s->host, v->endpoint, now);  // ENDPOINT/MODEL
    }
    // A button that looks the part, its list a popup: a combo's own
    // popup would begin inside the disabled block
    Str label = concat(&a, shorten(&a, now.len ? now : Str("(model)"), 40),
                       "  \xe2\x96\xbe##modelpick");  // a small down triangle
    if (why.len) {
        ImGui::BeginDisabled();
    }
    b32 click = ImGui::Button(cstr0(&a, label));
    if (why.len) {
        ImGui::EndDisabled();
    }
    itemnamed(g, "model picker");
    whytip(why.len ? why : Str("Switch the model for the rest of the session"));
    if (click || (g->pickmodel==s && !why.len)) {
        ImGui::OpenPopup("##modelpop");
    }
    if (g->pickmodel == s) {
        if (why.len) {
            setnote(g, s, concat(&a, "/model: ", why), 1);
        }
        g->pickmodel = 0;
    }
    if (ImGui::BeginPopup("##modelpop")) {
        g->modelsshown = 1;
        modelitems(g, a, s);
        ImGui::EndPopup();
    }
}

static b32  cfghas(Gui *, Str, Str);
static void cfgselect(Gui *, Str, Str);
static void followlink(Gui *, Str, LinkFrom * = 0);

// A list's last item, label: where its choices are set, in Settings,
// at the section of kind named, else the kind's first section, else a
// search for the kind.
static void settingsitem(Gui *g, Arena a, Str label, Str kind, Str name)
{
    ImGui::Separator();
    if (!menuitem(g, a, label, {}, 0)) {
        return;
    } else if (name.len && cfghas(g, kind, name)) {
        cfgselect(g, kind, name);
        g->cfgshowsel = 1;
        setpane(g, PANE_SETTINGS, 1);
    } else {
        followlink(g, concat(&a, "settings:", kind));
    }
}

// The shells a host lists (shells), as menu items: the current one
// checked, one not found disabled, with why. Asked for anew each time
// the list opens.
static void shellitems(Gui *g, Arena a, GuiSession *s, b32 opened)
{
    GuiHost *h   = s->host;
    View    *v   = s->view;
    Str      now = v ? v->shell : Str{};
    if (opened) {
        hostshells(g, h);
    }
    if (!h->shellsgot) {
        ImGui::TextDisabled(h->shellserr.len ? "The host's shells: %s"
                                             : "Asking the host for its shells...",
                            cstr0(&a, h->shellserr));
    }
    for (iz i = 0; i < h->shells.len; i++) {
        GuiShell *sh  = &h->shells[i];
        b32       cur = sh->name == now;
        Str       lbl = sh->name==h->shelldflt ? concat(&a, sh->name, "  (default)")
                                               : sh->name;
        if (ImGui::MenuItem(cstr0(&a, lbl), 0, cur, sh->ok && !cur)) {
            s->note = {};
            guishell(g, s, sh->name);
        }
        itemnamed(g, concat(&a, "shell ", sh->name));
        Str tip = !sh->ok      ? sh->err :
                  sh->desc.len ? concat(&a, concat(&a, sh->path, "\n"), sh->desc)
                               : sh->path;
        if (tip.len) {
            itemtip("%s", cstr0(&a, tip));
        }
    }
    if (s->host == g->main) {
        settingsitem(g, a, "Shells...", "shell", now);
    }
}

// The session's shell, a list of those its host has to switch to
// (shell) while idle, on hosts whose engine can: whether it drew.
static b32 drawshellpick(Gui *g, Arena a, GuiSession *s, View *v)
{
    if (!hostcap(s->host, "shell") || !v->shell.len) {
        if (g->pickshell == s) {
            setnote(g, s, "/shell: this host's engine is too old for it", 1);
            g->pickshell = 0;
        }
        return 0;
    }
    Str why = g->enginekind==ENGINE_FAKE ? Str("No engine") :
              sesswhy(g, s, "shell", !planwaits(v), 0);
    Str label = concat(&a, shorten(&a, v->shell, 24),
                       "  \xe2\x96\xbe##shellpick");  // a small down triangle
    if (why.len) {
        ImGui::BeginDisabled();
    }
    b32 click = ImGui::Button(cstr0(&a, label));
    if (why.len) {
        ImGui::EndDisabled();
    }
    itemnamed(g, "shell picker");
    whytip(why.len ? why : concat(&a, "The shell commands run in, ", v->shellpath));
    b32 opened = 0;
    if (click || (g->pickshell==s && !why.len)) {
        ImGui::OpenPopup("##shellpop");
        opened = 1;
    }
    if (g->pickshell == s) {
        if (why.len) {
            setnote(g, s, concat(&a, "/shell: ", why), 1);
        }
        g->pickshell = 0;
    }
    if (ImGui::BeginPopup("##shellpop")) {
        shellitems(g, a, s, opened);
        ImGui::EndPopup();
    }
    return 1;
}

// The session's mode, Build, Plan or Bypass, a list of them like the
// model's and shell's: amber in Plan, where the list also shows the
// plan, and red in Bypass, which only the user picks, trusted to know
// what it does.
static void drawmodepick(Gui *g, Arena a, GuiSession *s, View *v)
{
    static struct { Str name; Str mode; Str tip; } const modes[] = {
        {"Build",  "default", "Build: the agent works on the task, changing files as "
                              "it goes (/plan off)"},
        {"Plan",   "plan",    "Plan: the agent explores and writes a plan for you to "
                              "approve before it changes anything (/plan)"},
        {"Bypass", "bypass",  "Bypass: the agent reads, writes and acts anywhere, outside "
                              "its working directory and on the desktop, without asking "
                              "you. Only for an agent working alone somewhere it can do "
                              "no harm"},
    };
    iz  cur = v->bypass ? 2 : v->plan ? 1 : 0;
    iz  n   = hostcap(s->host, "bypass") ? 3 : 2;
    Str why = {};  // why no other mode can be picked, if none can
    b32 any = 0;
    for (iz i = 0; i < n; i++) {
        Str off = i==cur ? Str{} : modewhy(g, s, modes[i].mode);
        any |= i!=cur && !off.len;
        why  = why.len || i==cur ? why : off;
    }
    why = any ? Str{} : why;

    u32 color = cur==2 ? g->pal->bad : cur==1 ? g->pal->amber : ImGui::GetColorU32(ImGuiCol_Text);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    g->modetext = ImGui::GetColorU32(ImGuiCol_Text);  // for CHK_BYPASS
    Str label = concat(&a, modes[cur].name, "  \xe2\x96\xbe##modepick");  // a small down triangle
    // Plan's list still shows the plan while no mode can be picked
    b32 off = why.len && cur!=1;
    if (off) {
        ImGui::BeginDisabled();
    }
    b32 click = ImGui::Button(cstr0(&a, label));
    if (off) {
        ImGui::EndDisabled();
    }
    ImGui::PopStyleColor();
    itemnamed(g, "session mode");
    whytip(why.len ? why : modes[cur].tip);
    if (click) {
        ImGui::OpenPopup("##modepop");
    }
    if (ImGui::BeginPopup("##modepop")) {
        for (iz i = 0; i < n; i++) {
            b32 on   = i == cur;
            Str nope = on ? Str{} : modewhy(g, s, modes[i].mode);
            ImGui::BeginDisabled(nope.len > 0);
            if (i == 2) {
                ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
            }
            if (ImGui::Selectable(cstr0(&a, modes[i].name), on) && !on) {
                guimode(g, s, modes[i].mode);
            }
            if (i == 2) {
                ImGui::PopStyleColor();
            }
            ImGui::EndDisabled();
            itemnamed(g, concat(&a, Str("session mode "), modes[i].name));
            whytip(nope.len ? nope : modes[i].tip);
        }
        if (v->plan) {
            ImGui::Separator();
            if (ImGui::Selectable("Show the plan")) {
                setpane(g, PANE_PLAN, 1);
            }
            itemnamed(g, "show the plan");
            itemtip("The plan the agent writes, for you to approve");
        }
        ImGui::EndPopup();
    }
}

// The session's branch, on the session bar's first line after what is
// there, cut in its middle to end by end (window x): dim, or amber while
// a rebase, a merge or the like is under way, and a toggle of the
// Changes pane. Nothing outside a repository, nor where its engine
// can't tell, nor where too little of it would fit: whether it drew.
// With lead, it follows an item on the line, else it begins the line.
static b32 drawbranch(Gui *g, Arena *a, GuiSession *s, f32 end, b32 lead)
{
    GitWatch *w = &s->git;
    if (w->state!=GITW_REPO || !hostcap(s->host, "git")) {
        return 0;
    }
    Str name = w->detached ? takehead(w->oid, 7) : w->branch;
    Buf op(a, 64 + w->op.len);
    if (w->op.len) {
        print(&op, "  ");
        print(&op, w->op);
        if (w->steps) {
            putbyte(&op, ' ');
            print(&op, w->step);
            putbyte(&op, '/');
            print(&op, w->steps);
        }
    }
    Str         tail  = finish(&op);
    Str         glyph = name.len ? Str("\xee\x82\xa0 ") : Str("\xee\x82\xa0");  // a branch
    ImGuiStyle &st    = ImGui::GetStyle();
    f32         fixed = 2*st.FramePadding.x + ImGui::CalcTextSize(cstr0(a, glyph)).x +
                        ImGui::CalcTextSize(cstr0(a, tail)).x;
    f32         room  = end - (lead ? rightof() : ImGui::GetCursorPosX()) - fixed;
    f32         least = ImGui::CalcTextSize("ab\xe2\x80\xa6yz").x;
    Measure     m     = {};
    m.advance = widgetadvance;
    Str shown = midcut(a, name, room, &m, 0);
    if (room<0 || (shown.len!=name.len && (room<least || !shown.len))) {
        return 0;
    }
    g->branchcut = shown.len != name.len;
    b32 open = g->changesshown;
    u32 col  = w->op.len ? g->pal->amber : ImGui::GetColorU32(ImGuiCol_TextDisabled);
    Str label = concat(a, concat(a, glyph, shown), concat(a, tail, "###gitbranch"));
    if (lead) {
        ImGui::SameLine();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::PushStyleColor(ImGuiCol_Button, open ? ImGui::GetColorU32(ImGuiCol_ButtonActive) : 0);
    b32 click = ImGui::SmallButton(cstr0(a, label));
    ImGui::PopStyleColor(2);
    itemnamed(g, "git branch");
    if (ImGui::BeginItemTooltip()) {
        Buf b(a, 256 + w->branch.len + w->root.len);
        if (w->detached) {
            print(&b, "Detached at ");
            print(&b, w->oid);
        } else if (w->branch.len) {
            print(&b, "On ");
            print(&b, w->branch);
        } else {
            print(&b, "On a branch only git can name: its refs are in a reftable");
        }
        if (w->op.len) {
            print(&b, "\n");
            print(&b, w->op=="am" ? Str("Applying patches (git am)") :
                      w->op=="bisect" ? Str("Bisecting") : concat(a, "In a ", w->op));
            if (w->steps) {
                print(&b, ", step ");
                print(&b, w->step);
                print(&b, " of ");
                print(&b, w->steps);
            }
        }
        print(&b, "\n");
        print(&b, homeshow(a, s->host, w->root));
        ImGui::PushTextWrapPos(tipwrap());
        text(finish(&b));
        ImGui::TextDisabled(open ? "Click to hide its changes" : "Click to show its changes");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    if (click) {
        setpane(g, PANE_CHANGES, !open);
    }
    return 1;
}

// The session's mode, model and shell, as the Prompt's status line
// begins, each followed on the line. A subagent's mode is its parent's,
// unless that is not here: then a chip for Plan or Bypass, if either.
// Its shell is its parent's too.
static void drawsesspicks(Gui *g, Arena a, GuiSession *s, View *v)
{
    b32 picks = (!v->parent.len || v->remote) && hostcap(s->host, "plan");
    if (picks) {
        drawmodepick(g, a, s, v);
        ImGui::SameLine();
    } else if (v->plan) {
        // A chip that shows the plan
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->amber);
        if (ImGui::Button("Plan mode")) {
            setpane(g, PANE_PLAN, 1);
        }
        ImGui::PopStyleColor();
        itemnamed(g, "plan chip");
        itemtip("The agent changes nothing but its plan until you approve "
                "it. Click to see the plan.");
        ImGui::SameLine();
    } else if (v->bypass) {
        // A warning that nothing is asked
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
        ImGui::TextUnformatted("Bypass");
        ImGui::PopStyleColor();
        itemnamed(g, "bypass chip");
        itemtip("Bypass mode: the agent reads and writes outside its working "
                "directory, and acts on the desktop, without asking you. "
                "Change it in its session's mode list.");
        ImGui::SameLine();
    }
    drawmodelpick(g, a, s, v);
    ImGui::SameLine();
    if ((!sessparent(s).len || sessside(s).len) && drawshellpick(g, a, s, v)) {
        ImGui::SameLine();
    }
}

// Where a subagent's or side chat's up arrow goes: its parent, or a side
// chat's original (p, null if not listed here yet). It goes nowhere (far)
// for a subagent whose parent is on another host and not open here.
struct UpTo {
    Str         up;    // the session id, null for neither
    GuiSession *p;
    b32         side;
    b32         far;
};

static UpTo uptoof(Gui *g, GuiSession *s)
{
    UpTo r = {};
    if (!s || !s->view || (!sessparent(s).len && !sessside(s).len)) {
        return r;
    }
    r.side = !sessparent(s).len;
    r.up   = r.side ? sessside(s) : sessparent(s);
    r.p    = r.side ? findsess(g, s->host, r.up) : sessup(g, s);
    r.far  = !r.side && s->view->remote && !r.p;
    return r;
}

// Goes up from a subagent or side chat, by its arrow or Alt+Up.
static void goup(Gui *g, GuiSession *s)
{
    UpTo u = uptoof(g, s);
    if (u.up.len && !u.far) {
        guiselect(g, u.p ? u.p : addsess(g, s->host, u.up));
    }
}

// A line above the transcript: up to a subagent's parent, or a side
// chat's original; its branch, context gauge, the toggles that fold all
// its shell output and open its tool output and reasoning, and its
// details in a popup. Its model,
// shell and mode are the Prompt's (drawsesspicks).
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
    char const *sl    = ICON_SHELL;      // Shell output
    char const *tl    = ICON_TOOLS;      // Tool output
    char const *rl    = ICON_LIGHTBULB;  // Reasoning
    f32         rest  = (gw>0 ? gw + gap : 0) + 4*iconw() + 4*gap;
    b32         lead  = 0;  // something on the line before the branch
    f32         right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - rest;
    if (sessparent(s).len || sessside(s).len) {
        // Up to its parent, or a side chat's original, then what it is:
        // the Session pane says whose
        UpTo        u    = uptoof(g, s);
        b32         side = u.side;
        GuiSession *p    = u.p;
        ImGui::BeginDisabled(u.far);
        b32 click = iconbutton(ICON_UP "##barup");
        ImGui::EndDisabled();
        itemnamed(g, "up to parent");
        if (!u.far) {
            itemrect(g, side ? RECT_SIDELINK : RECT_PARENTLINK);
        }
        Str title = shorten(&a, p ? sesstitle(p) : u.up, 60);
        #if defined(__APPLE__)
        Str key   = " (Cmd+Up)";
        #else
        Str key   = " (Alt+Up)";
        #endif
        whytip(u.far ? Str("Its parent is a session on another host, not open here") :
               concat(&a, concat(&a, side ? Str("Up to the session this side chat began from: ")
                                          : Str("Up to the session that started this subagent: "),
                                 title), key));
        if (click) {
            goup(g, s);
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (side) {
            ImGui::TextDisabled("Side chat");
        } else {
            ImGui::TextDisabled(v->role.len ? "Subagent (%s)" : "Subagent%s",
                                cstr0(&a, shorten(&a, v->role, 40)));
            if (kidinbg(p, v) && (v->busy || v->jobsrun)) {
                ImGui::SameLine();
                ImGui::TextDisabled("in the background: it reports when done");
                itemnamed(g, "reports in the background");
            }
        }
        lead = 1;
    } else if (v->role.len) {
        // Its title is the sidebar's selected row: only a role here
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Role %s", cstr0(&a, shorten(&a, v->role, 40)));
        lead = 1;
    }
    // The branch in what room is left before them, else on the line
    b32 fits = right > (lead ? rightof() : ImGui::GetCursorPosX());
    lead |= drawbranch(g, &a, s, fits ? right - gap : right + rest, lead);
    if (fits && lead) {
        ImGui::SameLine(right);
    } else if (right > ImGui::GetCursorPosX()) {
        ImGui::SetCursorPosX(right);  // too narrow, or alone: a line of their own
    }
    g->gaugeshown = 0;
    if (gw > 0) {
        drawgauge(g, v, gw);
        ImGui::SameLine();
    }
    if (togglebutton(g, sl, v->foldshell)) {
        setsessionshell(g, s, !v->foldshell);
    }
    itemrect(g, RECT_SHELLBTN);
    itemtip("Shell output: %s\n\nFold every shell command's output in this session, "
            "or show it again as the settings say (%s)",
            v->foldshell ? "on, all of it folded" : "off, as the settings say",
            shortcutname(g, 2));
    ImGui::SameLine();
    if (togglebutton(g, tl, v->showtools)) {
        setsessiontools(g, s, !v->showtools);
    }
    itemrect(g, RECT_TOOLSBTN);
    itemtip("Tool output: %s\n\nOpen every tool row's output in this session, "
            "or fold them again as the settings say (%s)",
            v->showtools ? "on, all of it open" : "off, as the settings say",
            shortcutname(g, 0));
    ImGui::SameLine();
    if (togglebutton(g, rl, v->showthink)) {
        setsessionthink(g, s, !v->showthink);
    }
    itemrect(g, RECT_THINKBTN);
    itemtip("Reasoning: %s\n\nShow all of it in this session, even hidden, or "
            "fold it again as the settings say (%s)",
            v->showthink ? "on, all of it shown" : "off, as the settings say",
            shortcutname(g, 1));
    // Info opens a popup, not a toggle: apart from them, filled while open
    ImGui::SameLine(0, 2*gap);
    if (togglebutton(g, ICON_INFO, ImGui::IsPopupOpen("##sessinfo"))) {
        ImGui::OpenPopup("##sessinfo");
    }
    itemrect(g, RECT_INFOBTN);
    itemtip("Info: the session's model, role and instruction files");
    if (ImGui::BeginPopup("##sessinfo")) {
        drawinfo(g, s, v);
        ImGui::EndPopup();
    }
}

// Before a rewind of the conversation (cut): what it does, and the files
// it would put back too, if checked, newest first.
static void drawcut(Gui *g, GuiSession *s, View *v, Slice<ViewSnap> set)
{
    f32 sc    = g->scale;
    i32 turns = 0;
    for (iz i = 0; i < v->msgs.len; i++) {
        ViewMsg *m = &v->msgs[i];
        turns += m->startsturn>=g->rewturn && !m->cuts;
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted("The conversation goes back to just before this prompt, "
                           "which returns to the prompt box to edit and send again.");
    ImGui::Text("%d %s taken out of the conversation, though the session's log "
                "keeps %s. Undo rewind brings %s back until the next message is sent.",
                turns, turns==1 ? "turn is" : "turns are", turns==1 ? "it" : "them",
                turns==1 ? "it" : "them");
    ImGui::Spacing();
    if (set.len) {
        bool on = g->rewfiles;
        ImGui::Checkbox("Also put back the files write and edit changed from here on:", &on);
        itemrect(g, RECT_CUTFILES);
        g->rewfiles = on;
        ImGui::BeginDisabled(!on);
        for (iz i = 0; i < set.len; i++) {
            ImGui::Indent();
            ImGui::Bullet();
            text(showpath(v, set[i].path));
            ImGui::SameLine();
            ImGui::TextDisabled(set[i].existed ? "put back as it was"
                                               : "removed, as it did not exist then");
            ImGui::Unindent();
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("Changes made by shell commands are not undone. Files "
                            "changed since by something else are left alone.");
    } else {
        ImGui::TextDisabled("write and edit changed no files from here on. Changes "
                            "made by shell commands are not tracked.");
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::BeginDisabled(!canrewind(g, s) || g->cutid);
    b32 go = ImGui::Button("Rewind", v2(140*sc, 0));
    itemrect(g, RECT_REWINDOK);
    ImGui::EndDisabled();
    if (go) {
        guicut(g, s, g->rewturn, g->rewfiles && set.len);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    b32 cancel = ImGui::Button("Cancel", v2(120*sc, 0));
    itemrect(g, RECT_REWINDCANCEL);
    if (cancel) {
        ImGui::CloseCurrentPopup();
    }
}

// Before a rewind: the files it would put back, newest first, and how.
static void drawrewind(Gui *g)
{
    f32         sc    = g->scale;
    char const *title = g->rewconv ? "Rewind to here?" : "Rewind files?";
    if (g->openrewind) {
        ImGui::OpenPopup(title);
        g->openrewind = 0;
    }
    ImGui::SetNextWindowSize(v2(560*sc, 0));
    if (!ImGui::BeginPopupModal(title, 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    Arena       a  = g->frame;
    GuiSession *s  = g->rewsess;
    View       *v  = s && s->view ? s->view : g->view;
    Slice<ViewSnap> set = rewindset(v, &a, g->rewcall, g->rewturn);
    if (g->rewconv) {
        drawcut(g, s, v, set);
        ImGui::EndPopup();
        return;
    }
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
// the code byte for byte, as selecting the block whole copies it. It
// shows a check mark for a moment after, until a deadline. With aim, the
// one the checklist chose is where it can find it.
static void drawcodecopy(Gui *g, View *v, i32 msg, Placed *pl, ImVec2 origin, f32 top,
                         b32 dragging, b32 aim)
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
    b32     aimed = aim && (g->drivecodemsg<0 ||
                            (msg==g->drivecodemsg && pl->block==g->drivecodeblk));
    if (aimed) {
        f32 y0 = a.y>top ? a.y : top;
        g->rects[RECT_CODEBLOCK] = {a.x, y0, z.x, z.y};
    }
    b32 over = mp.x>=a.x && mp.x<z.x && mp.y>=a.y && mp.y<z.y && mp.y>=top;
    if (!over || dragging ||
            !ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        return;
    }
    i64 until  = g->copiedat + COPIED_US;
    b32 copied = g->copiedat && g->sched.now<until && g->copiedview==v &&
                 g->copiedmsg==msg && g->copiedblk==pl->block;
    if (copied) {
        schedat(&g->sched, until + 1000, DUE_APP);
    }
    // One ID for both icons, so that what the mouse is over stays the same
    // and no hover delays start again with the change
    char const *label = copied ? ICON_CHECK "###copy" : ICON_COPY "###copy";
    ImGuiStyle &st    = ImGui::GetStyle();
    f32         bw    = iconw();
    f32         by    = (a.y>top ? a.y : top) + 4*sc;
    ImGui::SetCursorScreenPos(v2(z.x - bw - 4*sc, by));
    ImGui::PushID(pl->block);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, v2(st.FramePadding.x, 0));
    b32 go = ImGui::Button(label, v2(bw, 0));
    ImGui::PopStyleVar();
    ImGui::PopID();
    itemtip("Copy the code");
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


// Selectable text: a view's messages drawn in a child window, which the
// transcript and documents (DocView) share. The transcript adds its
// headers, cards and buttons around what is here.

// Where a view's text lies this frame, in the child window it is drawn
// in: what hit-testing and drawing it share.
struct TextArea {
    View  *v;
    ImVec2 wpos;     // the child window's top left
    f32    winh;     // and its height
    f32    width;    // the text's wrap width
    f32    pad;      // its left margin
    f32    head;     // a message's header line, above its text
    f64    scrolly;
    b32   *dragging; // a selection is being dragged
    f64   *scrollto; // where a drag past an edge scrolls to, -1 if none
    f32    padx;     // more left margin: a file's line numbers
    f32    scrollx;  // scrolled right by, unwrapped
    b32    margin;   // head is a top margin, over the message before's foot
    LinkFrom *from;  // where its links to files are, else null: nowhere,
    b32    permsg;   // or (the transcript) each message's own folder
};

struct TextHit {
    Pos at;
    b32 hit;
    b32 body;  // in the message's text, not its header nor folded
};

// The mouse over a view's text: an invisible button over the visible
// area, which items made later may overlap, then the place under the
// mouse, clamped into the visible messages. A click selects there (a
// double click a word, a triple click a block, with Shift extending)
// and a drag extends it, scrolling past an edge toward the mouse,
// faster the further. Right clicks are the caller's, on the button,
// which stays the last item.
static TextHit textmouse(Gui *g, TextArea *ta)
{
    View       *v     = ta->v;
    f32         sc    = g->scale;
    ImGuiStyle &st    = ImGui::GetStyle();
    ImVec2      wpos  = ta->wpos;
    f32         top   = wpos.y;
    f32         bot   = wpos.y + ta->winh;
    ImGui::SetCursorScreenPos(wpos);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##text", v2(ImGui::GetWindowWidth() - st.ScrollbarSize, ta->winh),
                           ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight);
    ImGuiIO &io = ImGui::GetIO();
    b32 hovered = ImGui::IsItemHovered();
    b32 active  = ImGui::IsItemActive();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    }

    TextHit r = {};
    if ((hovered || active) && v->msgs.len) {
        f32 my = io.MousePos.y;
        my = my<top ? top : my>bot-1 ? bot-1 : my;
        f64 cy = ta->scrolly + (my - wpos.y);
        i32 i  = viewfind(v, ta->margin ? cy - ta->head : cy);
        if (i >= 0) {
            ViewMsg   *m  = &v->msgs[i];
            MsgLayout *ml = msglayout(v, i, &g->measure, ta->width, sc);
            f32 ly = (f32)(cy - m->top - ta->head);
            r.at.msg = i;
            f32 lx = io.MousePos.x - wpos.x - ta->pad - ta->padx + ta->scrollx;
            r.at.off = ly<0 || m->collapsed ? 0 : msghit(&g->measure, msgdoc(v, i), ml, lx, ly);
            if (ly > m->h) r.at.off = (i32)m->text.len;
            r.hit  = 1;
            r.body = ly>=0 && ly<=m->h && !m->collapsed;
        }
    }

    if (r.hit && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        i32 clicks = ImGui::GetMouseClickedCount(ImGuiMouseButton_Left);
        if (io.KeyShift && v->sel.active) {
            selextend(v, r.at);
        } else {
            selbegin(v, r.at, clicks>=3 ? SEL_BLOCK : clicks==2 ? SEL_WORD : SEL_CHAR);
        }
        *ta->dragging = 1;
    }
    if (*ta->dragging) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            *ta->dragging = 0;
        } else if (r.hit) {
            selextend(v, r.at);
            f32 my = io.MousePos.y;
            f32 dy = my<top ? my-top : my>bot ? my-bot : 0;
            if (dy != 0) {
                f32 step = dy * 0.5f;
                step = step<-60*sc ? -60*sc : step>60*sc ? 60*sc : step;
                *ta->scrollto = ta->scrolly + step;
                if (*ta->scrollto < 0) *ta->scrollto = 0;
            }
            // And sideways, past the text's left edge or the right
            f32 mx    = io.MousePos.x;
            f32 left  = wpos.x + ta->pad + ta->padx;
            f32 right = wpos.x + ImGui::GetWindowWidth() - st.ScrollbarSize;
            f32 dx    = mx<left ? mx-left : mx>right ? mx-right : 0;
            if (dx!=0 && ImGui::GetScrollMaxX()>0) {
                f32 step = dx * 0.5f;
                step = step<-60*sc ? -60*sc : step>60*sc ? 60*sc : step;
                ImGui::SetScrollX(ImGui::GetScrollX() + step);
            }
        }
    }
    return r;
}

// Links to files (files.cpp)
static LinkFrom filelinkfrom(GuiSession *, Str);
static Str  termcwd(GuiSession *);
static Str  filelinktip(Gui *, Arena *, Str, Dest *, LinkFrom *);
static void filelinkopen(Gui *, Arena *, Dest *, LinkFrom *);
static void filelinkmark(Gui *, Str, LinkFrom *);
static Str  codelinkat(Gui *, Arena, View *, TextHit, LinkFrom *);
static void codelinkhostend(Gui *, u64);
static void filelinkitems(Gui *);

// The text's context menu, if it opened on the last item: Copy and Copy
// as Markdown, which the caller may follow with items of its own before
// endtextmenu. With copyrect not negative, Copy is that RECT_*.
static b32 begintextmenu(Gui *g, View *v, i32 copyrect)
{
    if (!ImGui::BeginPopupContextItem("##textmenu")) {
        return 0;
    }
    filelinkitems(g);
    b32 any  = !selempty(&v->sel);
    b32 copy = ImGui::MenuItem("Copy", g->mac ? "Cmd+C" : "Ctrl+C", false, any);
    itemnamed(g, "text copy");
    if (copyrect >= 0) {
        itemrect(g, copyrect);
    }
    if (copy) {
        copyout(g, v, COPY_TEXT);
    }
    if (ImGui::MenuItem("Copy as Markdown", 0, false, any)) {
        copyout(g, v, COPY_MARKDOWN);
    }
    itemnamed(g, "text copy markdown");
    return 1;
}

static void endtextmenu(Gui *g, View *v)
{
    ImGui::Separator();
    if (ImGui::MenuItem("Select All", g->mac ? "Cmd+A" : "Ctrl+A")) {
        selall(v);
    }
    itemnamed(g, "text select all");
    ImGui::EndPopup();
}

// The selection's keys, while the window around the text has focus.
static void textkeys(Gui *g, View *v)
{
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A)) {
            selall(v);
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C)) {
            copyout(g, v, COPY_TEXT);
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_C)) {
            copyout(g, v, COPY_MARKDOWN);
        }
    }
}

// A message's text, laid out in ml, from origin: its blocks within the
// window's [top, bot], with the selection and its find hits, then its
// code blocks' Copy buttons. A chunk of a file's lines has hundreds of
// blocks, so the first on screen is found by its bottom, which grows
// block by block, and none past the window's bottom is looked at.
static void drawmsgtext(Gui *g, ImDrawList *dl, ImDrawListSplitter *sp, TextArea *ta,
                        i32 i, MsgLayout *ml, ImVec2 origin, Range sel, Marks *mk,
                        f32 alpha, b32 aim)
{
    View *v   = ta->v;
    Doc  *d   = msgdoc(v, i);
    f32   top = ta->wpos.y;
    f32   bot = ta->wpos.y + ta->winh;
    iz    k0  = 0;
    for (iz hi = ml->blocks.len; k0 < hi;) {
        iz      mid = k0 + (hi - k0)/2;
        Placed *pl  = &ml->blocks[mid];
        if (origin.y+pl->y+pl->bl->h < top) {
            k0 = mid + 1;
        } else {
            hi = mid;
        }
    }
    LinkFrom  own  = {};
    LinkFrom *from = 0;
    if (d->cands.len && !v->msgs[i].prov) {
        // Inline code that may name a file, once committed
        if (ta->permsg) {
            own  = filelinkfrom(g->sess, msgcwd(v, i));
            from = &own;
        } else {
            from = ta->from;
        }
    }
    for (iz k = k0; k < ml->blocks.len; k++) {
        Placed *pl = &ml->blocks[k];
        if (origin.y+pl->y > bot) {
            break;
        }
        drawblock(g, dl, sp, d, i, pl, origin, sel, mk, top, bot, alpha, from);
    }
    if (v->msgs[i].lines) {
        return;  // no code blocks, no Copy buttons
    }
    ImGui::PushID(i);
    for (iz k = 0; k < ml->blocks.len; k++) {
        Placed *pl = &ml->blocks[k];
        if (d->blocks[pl->block].type!=DOC_CODE || origin.y+pl->y > bot ||
                origin.y+pl->y+pl->bl->h < top) {
            continue;
        }
        drawcodecopy(g, v, i, pl, origin, top, *ta->dragging, aim);
    }
    ImGui::PopID();
}

// Links in a view's text (help.cpp's helpdest says where each goes):
// a tooltip saying where, and followed when let go of where pressed,
// without a drag between, since a press alone may begin a selection.

static void helppick(Gui *, i32);

// Where a link goes, in the window's words: a file's from where the
// link is (from, null for nowhere).
static Str linktip(Gui *g, Arena *a, Str link, LinkFrom *from)
{
    Dest d = helpdest(a, link);
    switch (d.kind) {
    case DEST_HELP: {
        if (d.page < 0) {
            return concat(a, "No Help topic is named ", cut(cuthead(link, 5), '#').head);
        }
        Str r   = concat(a, "Help \xe2\x86\x92 ", helptitle(d.page));
        Str sec = d.section.len ? helpsection(a, d.page, d.section) : Str{};
        return sec.len ? concat(a, concat(a, r, " \xe2\x80\xba "), sec) : r;
    }
    case DEST_SETTINGS: {
        if (d.whole) {
            return "Settings";
        } else if (d.diag) {
            return "Settings \xe2\x86\x92 Diagnostics";
        }
        Buf b(a, 128);
        print(&b, "Settings \xe2\x86\x92 ");
        print(&b, d.known ? cfgkindlabel(a, d.cfgkind, d.named) : d.cfgkind);
        if (d.cfgname.len) {
            print(&b, " \xe2\x86\x92 ");
            print(&b, d.cfgname);
        }
        if (d.cfgkey.len) {
            print(&b, " \xe2\x86\x92 ");
            print(&b, d.cfgkey);
        }
        return finish(&b);
    }
    case DEST_VIEW:
        return d.pane<0 ? concat(a, "Prospero has no pane named ", cuthead(link, 5))
                        : concat(a, "View \xe2\x86\x92 ", Str{(u8 *)panenames[d.pane],
                                     (iz)__builtin_strlen(panenames[d.pane])});
    case DEST_WEB:
        return link;
    case DEST_FILE:
        return filelinktip(g, a, link, &d, from);
    }
    return concat(a, link, " (Prospero does not open this link)");
}

// Go where a link goes. A settings: link waits in cfglink for the
// configuration (drawsettings), a section of Help for its page's layout
// (drawdoc). Under the checklist, a web link is only noted. A file's
// opens in Files, from where the link is (from, null for nowhere).
static void followlink(Gui *g, Str link, LinkFrom *from)
{
    Arena a = g->frame;
    Dest  d = helpdest(&a, link);
    switch (d.kind) {
    case DEST_HELP:
        if (d.page >= 0) {
            helppick(g, d.page);
            g->helpsect = d.section.len ? clone(&g->perm, d.section) : Str{};
            setpane(g, PANE_HELP, 1);
        }
        break;
    case DEST_SETTINGS:
        d.cfgkind = clone(&g->perm, d.cfgkind);
        d.cfgname = clone(&g->perm, d.cfgname);
        d.cfgkey  = clone(&g->perm, d.cfgkey);
        d.find    = clone(&g->perm, d.find);
        d.section = {};
        g->cfglink    = d;
        g->cfglinked  = 1;
        g->cfglinkget = 0;
        setpane(g, PANE_SETTINGS, 1);
        break;
    case DEST_VIEW:
        if (d.pane >= 0) {
            setpane(g, d.pane, 1);
        }
        break;
    case DEST_WEB:
        if (g->drive && !g->realreveal) {
            g->linkopened = clone(&g->perm, link);
        } else {
            os_guiopenurl(g->os, a, link);
        }
        break;
    case DEST_FILE:
        filelinkopen(g, &a, &d, from);
        break;
    }
    scheddirty(&g->sched);
}

// A link's tooltip, tip, shown now and, under the checklist, kept. The
// arena is the one tip was made in, past it.
static void linktipnow(Gui *g, Arena *a, Str tip)
{
    tipnow("%s", cstr0(a, tip));
    if (g->drive) {
        iz n = tip.len<countof(g->linktipped) ? tip.len : countof(g->linktipped);
        copybytes(g->linktipped, tip.data, n);
        g->linktippedlen = (i32)n;
    }
}

// The link under the mouse in a view's text, right after textmouse made
// its item: the hand cursor and the link's tooltip over it. Returns a
// link to follow, let go of in the message and at the link it was
// pressed on (press, a hash, and pressmsg keep which). A link to a
// file is from where from says; right-clicked, the text's menu, opened
// as the button is let go of, is for it (filelinkitems).
static Str textlink(Gui *g, View *v, TextHit hit, u64 *press, i32 *pressmsg, LinkFrom *from)
{
    Arena a    = g->frame;
    Str   link = {};
    if (hit.hit && hit.body && ImGui::IsItemHovered()) {
        link = doclinkat(msgdoc(v, hit.at.msg), hit.at.off);
        link = link.len ? link : codelinkat(g, a, v, hit, from);
    }
    if (link.len) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            linktipnow(g, &a, linktip(g, &a, link, from));
        }
    }
    // The release that opens the text's menu, even over a menu open
    // already (which the press closed): what it is over is the menu's
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)) {
        Str at = hit.hit && hit.body ? doclinkat(msgdoc(v, hit.at.msg), hit.at.off) : Str{};
        at = at.len ? at : codelinkat(g, a, v, hit, from);
        filelinkmark(g, at, from);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        b32 on = link.len && !ImGui::GetIO().KeyShift &&
                 ImGui::GetMouseClickedCount(ImGuiMouseButton_Left)==1;
        *press    = on ? hash(link)|1 : 0;
        *pressmsg = hit.at.msg;
    }
    Str r = {};
    if (*press && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        b32 same = link.len && (hash(link)|1)==*press && hit.at.msg==*pressmsg;
        r = same && !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left) ? link : Str{};
        *press = 0;
    }
    return r;
}

// A tool row's file (toolfileref), its summary drawn along the header
// from x0 to x1 (the header from top, h high; its text at y, size
// high), made a link as one in the text is, from the folder the
// session worked in then: the hand and an underline over it, its
// tooltip, and a click opens it in Files. The rest of the header still
// folds the row.
static void toolpathlink(Gui *g, View *v, i32 i, f32 x0, f32 x1, f32 top, f32 h,
                         f32 y, f32 size)
{
    Arena     a = g->frame;
    ViewTool *t = v->msgs[i].tool;
    ImGui::SetCursorScreenPos(v2(x0, top));
    b32 go = ImGui::InvisibleButton("##path", v2(x1 - x0, h));
    if (g->drive) {
        Buf b(&a, 32);
        print(&b, "toolpath ");
        print(&b, (i64)i);
        itemnamed(g, finish(&b));
        itemrect(g, RECT_TOOLPATH);
        g->toolpathmsg = i;
    }
    LinkFrom from = filelinkfrom(g->sess, msgcwd(v, i));
    Dest     d    = {};
    d.kind = DEST_FILE;
    d.file = t->file;
    if (ImGui::IsItemHovered()) {
        ImFont *font = stylefont(&g->fonts, STYLE_CODE);
        f32     base = y + font->GetFontBaked(size)->Ascent + 1;
        u32     col  = fade(g->pal->summary, v->msgs[i].faded ? 0.5f : 1);
        ImGui::GetWindowDrawList()->AddLine(v2(x0, base), v2(x1, base), col, 1);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            linktipnow(g, &a, filelinktip(g, &a, t->file.path, &d, &from));
        }
    }
    if (go) {
        filelinkopen(g, &a, &d, &from);
        scheddirty(&g->sched);
    }
}

// The text of a document's block, as shown.
static Str blocktext(Arena *a, Doc *d, DocBlock *b)
{
    Buf t(a, 128);
    for (i32 i = 0; i < b->npiece; i++) {
        print(&t, d->pieces[b->piece0+i].text);
    }
    return finish(&t);
}

// A file's lines (DocLines; files.cpp) in a child window, as drawdoc's
// documents, but laid out a screen at a time around what is shown, in
// the code font: numbered in a gutter on the left, the cited lines
// banded, scrolled to them once (centered if they fit, else from a
// third of the way down), or to where the view was, and wrapped at the
// pane's width or else at FILES_COLS columns, scrolled sideways. Until
// the pane has settled its size (its first frame, a layout applied, a
// height that changed) a jump or a restore is made again each frame,
// and not reported done, nor the place recorded.
static f32 linesy(View *v, Gui *g, f32 width, f32 pad, i32 line)
{
    i32 ci = line / FILES_CHUNK;
    if (ci >= v->msgs.len) {
        return 0;
    }
    MsgLayout *ml = msglayout(v, ci, &g->measure, width, g->scale);
    i32        k  = line % FILES_CHUNK;
    k = k<(i32)ml->blocks.len ? k : (i32)ml->blocks.len-1;
    return (f32)v->msgs[ci].top + pad + (k>=0 ? ml->blocks[k].y : 0);
}

static f32 linesh(View *v, Gui *g, f32 width, i32 line)
{
    i32 ci = line / FILES_CHUNK;
    if (ci >= v->msgs.len) {
        return 0;
    }
    MsgLayout *ml = msglayout(v, ci, &g->measure, width, g->scale);
    i32        k  = line % FILES_CHUNK;
    return k<ml->blocks.len ? ml->blocks[k].bl->h : 0;
}

// Lay out the chunks from y0 to y1, then the tops again.
static f64 lineslay(View *v, Gui *g, f32 width, f32 charw, f32 lineh, f64 y0, f64 y1)
{
    i32 i = viewfind(v, y0<0 ? 0 : y0);
    for (; i>=0 && i<v->msgs.len && v->msgs[i].top<=y1; i++) {
        msglayout(v, i, &g->measure, width, g->scale);
    }
    return viewtops(v, width, charw, lineh);
}

static Str drawlines(Gui *g, DocView *d, char const *id, DocLines *dl)
{
    View       *v     = d->v;
    f32         sc    = g->scale;
    ImGuiStyle &st    = ImGui::GetStyle();
    ImVec2      avail = ImGui::GetContentRegionAvail();
    f32         pad   = 4*sc;
    Palette    *pal   = g->pal;
    FontMetrics fm    = fontmetrics(&g->fonts, STYLE_CODE);
    f32         adv   = fontadvance(&g->fonts, STYLE_CODE, 'n');
    v->extra = 0;
    v->gap   = 0;

    i32 digits = 1;
    for (i32 n = dl->lines; n >= 10; n /= 10) {
        digits++;
    }
    f32 gutter = (f32)(digits + 2)*adv;
    f32 width  = 0;
    if (dl->wrap) {
        width = (f32)(i32)(avail.x - st.ScrollbarSize - 2*pad - gutter);
        width = width<50*sc ? 50*sc : width;
    } else {
        width = ((f32)FILES_COLS + 0.5f)*adv;  // perline exactly FILES_COLS
    }
    f32 winh    = avail.y;
    b32 pending = ImGui::IsWindowAppearing() || g->layoutpending || g->layoutnow ||
                  winh!=dl->lasth;
    dl->lasth    = winh;
    dl->jumped   = 0;
    dl->restored = 0;
    dl->recorded = 0;

    // A new width keeps the line at the top there
    f64 want  = -1;
    f32 wantx = -1;
    f64 cur   = d->win ? d->win->Scroll.y : 0;
    if (width != v->lastw) {
        Anchor an = {-1, -1, 0};
        if (v->lastw>0 && d->win) {
            an = viewanchor(v, &g->measure, v->lastw, sc, pad, cur);
        }
        v->lastw     = width;
        v->topsdirty = 1;
        viewtops(v, width, adv, fm.lineh);
        if (an.msg >= 0) {
            msglayout(v, an.msg, &g->measure, width, sc);
            viewtops(v, width, adv, fm.lineh);
            want = anchory(v, an, &g->measure, width, sc, pad);
        }
    }
    viewtops(v, width, adv, fm.lineh);

    // Unwrapped, as wide as the widest line, as far as drawn, and so a
    // horizontal scrollbar, which the height shown is less
    f32 textw = (f32)(dl->cols<FILES_COLS ? dl->cols : FILES_COLS)*adv + adv;
    textw = dl->seenw>textw ? dl->seenw : textw;
    f32 contw = dl->wrap ? 0 : gutter + 2*pad + textw;
    ImGuiWindowFlags wf = ImGuiWindowFlags_AlwaysVerticalScrollbar;
    if (!dl->wrap && contw>avail.x-st.ScrollbarSize) {
        wf |= ImGuiWindowFlags_AlwaysHorizontalScrollbar;
    }
    f32 vish = winh - (wf&ImGuiWindowFlags_AlwaysHorizontalScrollbar ? st.ScrollbarSize : 0);

    if (dl->jump0>0 && v->msgs.len) {
        i32 l0 = dl->jump0 - 1;
        i32 l1 = (dl->jump1>=dl->jump0 ? dl->jump1 : dl->jump0) - 1;
        f64 y0 = linesy(v, g, width, pad, l0);
        lineslay(v, g, width, adv, fm.lineh, y0-winh, y0);
        y0 = linesy(v, g, width, pad, l0);
        f64 y1 = linesy(v, g, width, pad, l1) + linesh(v, g, width, l1);
        y0 = linesy(v, g, width, pad, l0);  // tops as laid out
        want  = y1-y0 <= vish-2*pad ? (y0+y1)/2 - vish/2 : y0 - vish/3;
        wantx = 0;
        dl->jumped = !pending;
    } else if (dl->restore && v->msgs.len) {
        i32 line = dl->top<0 ? 0 : dl->top>=dl->lines ? dl->lines-1 : dl->top;
        f64 y    = linesy(v, g, width, pad, line);
        lineslay(v, g, width, adv, fm.lineh, y-winh, y);
        want  = linesy(v, g, width, pad, line) + dl->dy;
        wantx = dl->wrap ? 0 : dl->scrollx;
        dl->restored = !pending;
    }

    // What is shown, and a screen each way, laid out, the top kept on
    // the line it is on as heights above it settle
    f64 at = want>=0 ? want : cur;
    at = at<0 ? 0 : at;
    Anchor an = viewanchor(v, &g->measure, width, sc, pad, at);
    lineslay(v, g, width, adv, fm.lineh, at-winh, at+2*winh);
    if (an.msg >= 0) {
        f64 y = anchory(v, an, &g->measure, width, sc, pad);
        if (y != at) {
            want = y;
        }
    }
    f64 total = viewtops(v, width, adv, fm.lineh);
    f64 most  = total + 2*pad - vish;
    most = most<0 ? 0 : most;
    want = want<0 ? -1 : want>most ? most : want;
    if (pending) {
        scheddirty(&g->sched);
    }

    ImGui::SetNextWindowContentSize(v2(contw, (f32)total + 2*pad));
    if (want>=0 || wantx>=0) {
        ImGui::SetNextWindowScroll(v2(wantx, want>=0 ? (f32)want : -1));
    }
    if (d->scrollto >= 0) {
        ImGui::SetNextWindowScroll(v2(-1, (f32)d->scrollto));
        d->scrollto = -1;
    }
    ImGui::BeginChild(id, v2(0, 0), 0, wf);
    d->win = ImGui::GetCurrentWindow();
    ImVec2 wpos = ImGui::GetWindowPos();
    f32    wh   = ImGui::GetWindowHeight() - (wf&ImGuiWindowFlags_AlwaysHorizontalScrollbar
                                              ? st.ScrollbarSize : 0);
    f32    ww   = ImGui::GetWindowWidth() - st.ScrollbarSize;
    f32    sx   = dl->wrap ? 0 : ImGui::GetScrollX();
    TextArea ta = {v, wpos, wh, width, pad, pad, ImGui::GetScrollY(), &d->dragging,
                   &d->scrollto, gutter, sx, 1, 0, 0};
    TextHit hit  = textmouse(g, &ta);
    Str     link = textlink(g, v, hit, &d->press, &d->pressmsg, &d->from);
    if (begintextmenu(g, v, -1)) {
        endtextmenu(g, v);
    }

    ImDrawList         *dr   = ImGui::GetWindowDrawList();
    ImDrawListSplitter *sp   = &g->splitter;
    Range               sel  = selrange(&v->sel);
    b32                 band = dl->band0>0 && poseq(sel.beg, sel.end);
    b32                 drew = 0;
    Marks               mk   = {0, 0, -1};
    f32                 ox   = wpos.x + pad + gutter - sx;
    f32                 seen = dl->seenw;
    Arena               a    = g->frame;
    ImFont             *font = stylefont(&g->fonts, STYLE_CODE);
    f32                 size = stylesize(&g->fonts, STYLE_CODE);
    sp->Split(dr, CH_COUNT);
    dr->PushClipRect(v2(wpos.x + pad + gutter - adv/2, wpos.y), v2(wpos.x + ww, wpos.y + wh), 1);
    i32 first = viewfind(v, ta.scrolly - pad);  // the last line above, its foot in view
    for (i32 i = first; i>=0 && i<v->msgs.len; i++) {
        ViewMsg *m = &v->msgs[i];
        f32      y = wpos.y + (f32)(m->top - ta.scrolly) + pad;
        if (y > wpos.y+wh) {
            break;
        }
        MsgLayout *ml = msglayout(v, i, &g->measure, width, sc);
        drawmsgtext(g, dr, sp, &ta, i, ml, v2(ox, y), sel, &mk, 1, 0);
        for (iz k = 0; k < ml->blocks.len; k++) {
            Placed *pl = &ml->blocks[k];
            f32     ly = y + pl->y;
            if (ly > wpos.y+wh) {
                break;
            } else if (ly+pl->bl->h < wpos.y) {
                continue;
            }
            for (iz li = 0; li < pl->bl->lines.len; li++) {
                Line *l = &pl->bl->lines[li];
                if (l->nrun) {
                    Run *r = &pl->bl->runs[l->run0 + l->nrun - 1];
                    seen = r->x+r->w>seen ? r->x+r->w : seen;
                }
            }
            i32 n = m->line0 + (i32)k;
            if (band && n>=dl->band0 && n<=dl->band1) {
                sp->SetCurrentChannel(dr, CH_BG);
                dr->PushClipRect(v2(wpos.x, wpos.y), v2(wpos.x + ww, wpos.y + wh), 0);
                dr->AddRectFilled(v2(wpos.x, ly), v2(wpos.x + ww, ly + pl->bl->h),
                                  fade(pal->codetext, 0.08f));
                dr->PopClipRect();
                drew = 1;
            }
        }
    }
    dr->PopClipRect();

    // The line numbers, right-aligned in the gutter, on the band brighter
    sp->SetCurrentChannel(dr, CH_TEXT);
    for (i32 i = first; i>=0 && i<v->msgs.len; i++) {
        ViewMsg *m = &v->msgs[i];
        f32      y = wpos.y + (f32)(m->top - ta.scrolly) + pad;
        if (y > wpos.y+wh) {
            break;
        }
        MsgLayout *ml = msglayout(v, i, &g->measure, width, sc);
        for (iz k = 0; k < ml->blocks.len; k++) {
            Placed *pl = &ml->blocks[k];
            f32     ly = y + pl->y;
            if (ly > wpos.y+wh) {
                break;
            } else if (ly+pl->bl->h<wpos.y || !pl->bl->lines.len) {
                continue;
            }
            i32 n  = m->line0 + (i32)k;
            Buf b(&a, 16);
            print(&b, (i64)n);
            Str num = finish(&b);
            Line *l  = &pl->bl->lines[0];
            f32   nx = wpos.x + pad + (f32)(digits - num.len)*adv;
            ImFontBaked *bk = stylebaked(&g->fonts, STYLE_CODE);
            b32 on = band && n>=dl->band0 && n<=dl->band1;
            dr->AddText(font, size, v2(nx, ly + l->base - bk->Ascent),
                        on ? pal->codetext : pal->paneldim, num.cdata, num.cdata+num.len);
        }
    }
    sp->Merge(dr);
    dl->seenw = seen;

    // Where the view is, by its top line
    f32 sy = ImGui::GetScrollY();
    if (!pending && want<0 && v->msgs.len) {
        f64 y  = sy - pad;
        i32 ci = viewfind(v, y<0 ? 0 : y);
        if (ci >= 0) {
            ViewMsg   *m  = &v->msgs[ci];
            MsgLayout *ml = msglayout(v, ci, &g->measure, width, sc);
            f32        ry = (f32)(y - m->top);
            iz         lo = 0, hi = ml->blocks.len;
            while (hi-lo > 1) {
                iz mid = lo + (hi - lo)/2;
                if (ml->blocks[mid].y <= ry) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            dl->top      = m->line0 - 1 + (i32)lo;
            dl->dy       = sy - linesy(v, g, width, pad, dl->top);
            dl->scrollx  = sx;
            dl->recorded = 1;
        }
    }

    FileShown *fs = &g->fshown;
    fs->v       = v;
    fs->wpos    = wpos;
    fs->winh    = wh;
    fs->pad     = pad;
    fs->gutter  = gutter;
    fs->scrolly = sy;
    fs->scrollx = sx;
    fs->width   = width;
    fs->hbar    = ImGui::GetScrollMaxX() > 0;
    fs->band    = drew;
    ImGui::EndChild();
    textkeys(g, v);
    return link;
}

// A document (DocView) in a child window filling the rest of the window
// at the cursor, or with fit, one fit wide and as tall as the document,
// unscrolled: laid out at its width whole, as a document is short, its
// text selectable, with the text's context menu, and its keys while the
// window has focus, scrolled to its section heading named d->section
// once laid out. Returns the destination of a link clicked, if any,
// which is the caller's to follow. With dl, it is a file's lines,
// drawn by drawlines.
static Str drawdoc(Gui *g, DocView *d, char const *id, f32 fit = 0, DocLines *lines = 0)
{
    if (lines) {
        return drawlines(g, d, id, lines);
    }
    View       *v     = d->v;
    f32         sc    = g->scale;
    ImGuiStyle &st    = ImGui::GetStyle();
    ImVec2      avail = ImGui::GetContentRegionAvail();
    f32         pad   = 4*sc;
    f32         width = fit>0 ? fit - 2*pad : avail.x - st.ScrollbarSize - 2*pad;
    width = (f32)(i32)(width<50*sc ? 50*sc : width);
    FontMetrics fm    = fontmetrics(&g->fonts, 0);
    f32         charw = fontadvance(&g->fonts, 0, 'n');
    if (width != v->lastw) {
        v->topsdirty = 1;
        v->lastw     = width;
    }
    viewtops(v, width, charw, fm.lineh);
    for (i32 i = 0; i < v->msgs.len; i++) {
        msglayout(v, i, &g->measure, width, sc);
    }
    f64 total = viewtops(v, width, charw, fm.lineh);

    if (d->section.len && v->msgs.len && !ImGui::IsWindowAppearing()) {
        // The heading, whose letters and digits the section's are
        Arena      a   = g->frame;
        Doc       *doc = msgdoc(v, 0);
        MsgLayout *ml  = msglayout(v, 0, &g->measure, width, sc);
        for (iz k = 0; k < doc->blocks.len; k++) {
            DocBlock *b = &doc->blocks[k];
            if (b->type==DOC_HEADING && helpsame(blocktext(&a, doc, b), d->section)) {
                d->scrollto = v->msgs[0].top + msgy(doc, ml, b->off0);
                break;
            }
        }
        d->section = {};
    } else if (d->section.len) {
        scheddirty(&g->sched);  // laid out at its size in a later frame
    }

    ImGui::SetNextWindowContentSize(v2(0, (f32)total + 2*pad));
    if (d->scrollto >= 0) {
        ImGui::SetNextWindowScroll(v2(-1, (f32)d->scrollto));
        d->scrollto = -1;
    }
    if (fit > 0) {
        ImGui::BeginChild(id, v2(fit, (f32)total + 2*pad), 0,
                          ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    } else {
        ImGui::BeginChild(id, v2(0, 0));
    }
    d->win = ImGui::GetCurrentWindow();
    ImVec2   wpos = ImGui::GetWindowPos();
    TextArea ta   = {v, wpos, ImGui::GetWindowHeight(), width, pad, pad,
                     ImGui::GetScrollY(), &d->dragging, &d->scrollto, 0, 0, 0, &d->from, 0};
    TextHit hit  = textmouse(g, &ta);
    Str     link = textlink(g, v, hit, &d->press, &d->pressmsg, &d->from);
    if (begintextmenu(g, v, -1)) {
        endtextmenu(g, v);
    }

    ImDrawList         *dl  = ImGui::GetWindowDrawList();
    ImDrawListSplitter *sp  = &g->splitter;
    Range               sel = selrange(&v->sel);
    Marks               mk  = {0, 0, -1};
    sp->Split(dl, CH_COUNT);
    for (i32 i = viewfind(v, ta.scrolly - pad); i>=0 && i<v->msgs.len; i++) {
        ViewMsg *m = &v->msgs[i];
        f32      y = wpos.y + (f32)(m->top - ta.scrolly);
        if (y > wpos.y+ta.winh) {
            break;
        }
        MsgLayout *ml = msglayout(v, i, &g->measure, width, sc);
        drawmsgtext(g, dl, sp, &ta, i, ml, v2(wpos.x + pad, y + pad), sel, &mk, 1, 0);
    }
    sp->Merge(dl);
    ImGui::EndChild();
    textkeys(g, v);  // the window's, as its child's
    return link;
}

// The find bar, floating over the top right of the transcript (r0 to
// r1) as the toasts do over its foot, so that opening and closing it
// moves nothing: the text to find, the hits counted, and the buttons to
// go between them. A new text or session searches again, a budget of
// bytes a frame, and the first hit at or below the top of the screen
// becomes the current one. Drawn after the transcript, to be over it,
// so a jump it asks for lands next frame.
static void drawfind(Gui *g, View *v, f64 scrolly, ImVec2 r0, ImVec2 r1)
{
    ViewSearch *s  = g->search;
    f32         sc = g->scale;
    Arena       a  = g->frame;
    if (!g->findopen) {
        return;
    }
    // Esc while the bar has the focus but its field does not, as after
    // a click on an arrow. ImGui's own Esc (keyboard navigation) has by
    // now moved the focus out of the bar, to the window around it, so
    // it goes by last frame's.
    b32 held = g->findheld;
    g->findheld = 0;
    ImGuiStyle &st     = ImGui::GetStyle();
    f32         pad    = 6*sc;
    f32         fh     = ImGui::GetFrameHeight();
    f32         sp     = st.ItemSpacing.x;
    f32         countw = ImGui::CalcTextSize("No matches").x;
    f32         countm = ImGui::CalcTextSize("8888 of 8888+").x;
    countw = countm>countw ? countm : countw;
    f32 fieldw = 260*sc;
    f32 rest   = 2*fh + countw + iconw() + 4*sp + 2*pad;
    f32 room   = r1.x - r0.x - st.ScrollbarSize - 24*sc;
    if (fieldw+rest > room) {
        fieldw = room - rest;
    }
    if (fieldw < 60*sc) {
        fieldw = 60*sc;  // narrower than this, let it overhang the left
    }
    f32 w = fieldw + rest;
    f32 h = fh + 2*pad;

    ImGuiWindow *pw   = ImGui::GetCurrentWindow();
    ImVec2       maxp = pw->DC.CursorMaxPos;
    ImGui::SetCursorScreenPos(v2(r1.x - st.ScrollbarSize - 12*sc - w, r0.y + 8*sc));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, g->pal->frame);
    ImGui::PushStyleColor(ImGuiCol_Border, g->pal->rule);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6*sc);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, v2(pad, pad));
    ImGui::BeginChild("##findbar", v2(w, h), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                      ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetCursorPos(v2(pad, pad));

    if (g->findfocus) {
        ImGui::SetKeyboardFocusHere();
        g->findfocus = 0;
    }
    ImGui::SetNextItemWidth(fieldw);
    b32 enter = ImGui::InputTextWithHint("##find", "Find in the transcript", g->findbuf,
                                         FIELD_MAX, ImGuiInputTextFlags_EnterReturnsTrue|
                                                    ImGuiInputTextFlags_AutoSelectAll);
    itemrect(g, RECT_FINDFIELD);
    b32 again = ImGui::IsItemEdited() || s->v!=v;
    b32 esc   = ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
                (ImGui::IsItemDeactivated() || held);
    g->findactive = ImGui::IsItemActive();
    if (enter) {
        g->findfocus = 1;  // Enter lets go of the field: take it back
    }

    if (again) {
        Str q = {(u8 *)g->findbuf, (iz)__builtin_strlen(g->findbuf)};
        searchreset(s, v, q);
        g->findfrom = viewfind(v, scrolly);
    }
    b32 done = searchstep(s, a, FIND_BUDGET);
    if (!done) {
        scheddirty(&g->sched);  // more next frame
    }
    if (s->cur < 0) {
        // The first hit from the top of the screen down, else from the
        // start once all are found; with none yet, the first to turn up
        // as rows open, from the top of the screen as it is then
        iz i = searchfrom(s, g->findfrom<0 ? 0 : g->findfrom, 0);
        if (i < s->hits.len) {
            s->cur = (i32)i;
        } else if (done && s->hits.len) {
            s->cur = 0;
        }
        g->findjump |= s->cur >= 0;
        g->findfrom  = done && s->cur<0 ? viewfind(v, scrolly) : g->findfrom;
    }

    ImGui::SameLine();
    b32 prev = ImGui::ArrowButton("##findprev", ImGuiDir_Up);
    itemtip("The previous match (Shift+Enter)");
    ImGui::SameLine();
    b32 next = ImGui::ArrowButton("##findnext", ImGuiDir_Down);
    itemrect(g, RECT_FINDNEXT);
    itemtip("The next match (Enter)");
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
    // In a column as wide as its widest, so that the bar never changes
    // width as the count does
    f32 x = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", cstr0(&a, finish(&b)));
    ImGui::SameLine(x + countw + sp);
    b32 close = iconbutton(ICON_CLOSE "##findclose");
    itemnamed(g, "find close");
    itemtip("Close the find bar (Esc)");
    g->findheld = ImGui::IsWindowFocused();

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    pw->DC.CursorMaxPos = maxp;  // over the transcript, extending nothing

    if (esc || close) {
        gui_find_close(g);
    } else if (enter || next || prev) {
        gui_find_next(g, enter ? (ImGui::GetIO().KeyShift ? -1 : 1) : prev ? -1 : 1);
    }
    if (g->findjump) {
        scheddirty(&g->sched);  // the layout, before the child, goes there
    }
}

// The transcript of the session shown: virtualized messages with
// selection across them, tool rows and ask cards.
// The transcript's menu's fork at a message: a user's prompt forks the
// turns before it, the prompt left in the new session's box to edit and
// send again; anything else, the conversation to the end of its turn.
static b32 forkitem(Gui *g, ViewMsg *m)
{
    GuiSession *s      = g->sess;
    b32         prompt = 0;
    i64         turn   = m ? viewforkat(g->view, m, &prompt) : -1;
    if (!s || turn<0) {
        return 0;
    }
    Arena a   = g->frame;
    Str   why = sesswhy(g, s, "fork", 1, 1);
    ImGui::Separator();
    if (menuitem(g, a, prompt ? Str("Fork before this prompt") : Str("Fork after this turn"),
                 why, 0)) {
        guifork(g, s, turn);
    }
    if (!why.len) {
        itemtip(prompt ? "A new session of the turns before this one, "
                                       "this prompt in its box to edit and send"
                                     : "A new session of the conversation up to here");
    }
    return 1;
}

// Why a session cannot have a side chat now, or empty if it can: busy
// or not, but a root.
static Str sidewhy(Gui *g, GuiSession *s)
{
    if (!s) {
        return "Open a session first";
    } else if (sessparent(s).len) {
        return "A subagent has no side chat: start one from the session it works for";
    }
    return sesswhy(g, s, "side", 0, 1);
}

// Why a session cannot have a worktree session now, or empty if it
// can: as for a side chat, and it has a folder, a repository's.
static Str wtwhy(Gui *g, GuiSession *s)
{
    Str why = sidewhy(g, s);
    if (why.len) {
        return why;
    } else if (sessnofolder(s)) {
        return "A session of no folder has no repository to make a worktree of";
    } else if (!hostcap(s->host, "worktree")) {
        return "This host's engine is too old for worktree sessions: update the engine";
    }
    return {};
}

// Why a session cannot be forked into another folder now, or empty if
// it can: idle, a root, its engine new enough.
static Str movewhy(Gui *g, GuiSession *s)
{
    if (!s) {
        return "Open a session first";
    }
    return sesswhy(g, s, "fork.cwd", 1, 1);
}

// The dialog of a fork into another folder (drawmove), for s.
static void movedialog(Gui *g, GuiSession *s)
{
    g->mvsess = s;
    g->openmv = 1;
}

// The worktree session dialog (drawwt), for s, its draft the quote.
static void wtdialog(Gui *g, GuiSession *s, Str quote)
{
    g->wtsess  = s;
    g->wtquote = quote.len ? clone(&g->perm, quote) : Str{};
    g->openwt  = 1;
}

// The transcript's menu's side chat: a copy of the whole session, at
// work or not, the selection quoted in its box (sidequote). And the
// same in a worktree of its own.
static void sideitem(Gui *g, View *v, b32 sep)
{
    GuiSession *s   = g->sess;
    Arena       a   = g->frame;
    Str         why = sidewhy(g, s);
    if (sep) {
        ImGui::Separator();
    }
    if (menuitem(g, a, "Start a side chat", why, 0)) {
        guiside(g, s, sidequote(&a, copysel(v, &a, COPY_MARKDOWN)));
    }
    if (!why.len) {
        itemtip(selempty(&v->sel)
            ? "A new session of this conversation, to ask about it while its agent works on"
            : "A new session of this conversation, to ask about it while its agent "
              "works on, the selection quoted in its box");
    }
    Str wwhy = wtwhy(g, s);
    if (menuitem(g, a, "Start a worktree session\xe2\x80\xa6", wwhy, 0)) {
        wtdialog(g, s, sidequote(&a, copysel(v, &a, COPY_MARKDOWN)));
    }
    if (!wwhy.len) {
        itemtip("A fresh session under this one that works in a new "
                              "worktree, on a branch of its own from this folder's HEAD, "
                              "and can report its work back here");
    }
}

// Messages sent to the turn at work that wait to be taken in: a tail
// below the last message, of dim user bubbles, oldest first, each with
// a Withdraw that puts it back in the draft (guiunqueue), unless it was
// taken in meanwhile. They are no messages (msgs), so that selection,
// find, export and fork never see them.

enum { QUEUE_LINES = 3 };

struct QueueShow {
    Str lines[QUEUE_LINES];  // wrapped, the last with an ellipsis if more
    i32 n;
    f32 h;                   // the bubble's
};

static QueueShow queueshow(Arena *a, ViewQueued *q, f32 width, f32 pad)
{
    ImFont   *f    = ImGui::GetFont();
    f32       fs   = ImGui::GetFontSize();
    f32       lh   = ImGui::GetTextLineHeight();
    QueueShow r    = {};
    Str       rest = trim(q->text);
    for (; rest.len && r.n<QUEUE_LINES; rest = trim(rest)) {
        iz nl = 0;
        for (; nl<rest.len && rest[nl]!='\n'; nl++) {}
        char const *beg = rest.cdata;
        iz          n   = f->CalcWordWrapPosition(fs, beg, beg+nl, width) - beg;
        for (n = n>0 ? n : 1; n<nl && (rest[n]&0xc0)==0x80; n++) {}
        r.lines[r.n++] = trimright(takehead(rest, n));
        rest = cuthead(rest, n);
    }
    if (r.n && (rest.len || q->cut)) {
        Str         last = r.lines[r.n-1];
        f32         ew   = ImGui::CalcTextSize("...").x;
        char const *end  = f->CalcWordWrapPosition(fs, last.cdata, last.cdata+last.len, width-ew);
        r.lines[r.n-1] = concat(a, trimright(takehead(last, end - last.cdata)), "...");
    }
    r.h = pad/4 + lh + (f32)r.n*lh + (q->images ? lh : 0) + pad/2;
    return r;
}

// The tail's bubbles, as measured (shows), from y in the transcript's
// coordinates.
static void drawqueue(Gui *g, GuiSession *s, Slice<QueueShow> shows, f64 y,
                      ImVec2 wpos, f32 scrolly, f32 winh, f32 width, f32 pad)
{
    View       *v  = s->view;
    Arena       a  = g->frame;
    f32         sc = g->scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32         lh = ImGui::GetTextLineHeight();
    f32         bw = ImGui::CalcTextSize("Withdraw").x + 2*ImGui::GetStyle().FramePadding.x;
    for (iz i = 0; i < shows.len; y += shows[i].h + v->gap, i++) {
        ViewQueued *q   = &v->queue[i];
        QueueShow  *qs  = &shows[i];
        f32         top = wpos.y + (f32)(y - scrolly);
        ImVec2      b0  = v2(wpos.x+pad/4, top);
        ImVec2      b1  = v2(wpos.x+pad*1.5f+width, top+qs->h);
        if (!i) {
            g->rects[RECT_QUEUEITEM] = {b0.x, b0.y, b1.x, b1.y};
        }
        g->queueshown++;
        if (b1.y<wpos.y || b0.y>wpos.y+winh) {
            continue;  // out of sight
        }
        dl->AddRectFilled(b0, b1, fade(g->pal->userbg, 0.55f), 6*sc);
        f32 x     = wpos.x + pad;
        f32 ty    = top + pad/4;
        Str label = q->steer ? Str("Next step") : Str("Queued");
        dl->AddText(v2(x, ty), g->pal->paneldim, label.cdata, label.cdata+label.len);
        for (i32 k = 0; k < qs->n; k++) {
            Str line = qs->lines[k];
            ty += lh;
            dl->AddText(v2(x, ty), g->pal->paneldim, line.cdata, line.cdata+line.len);
        }
        if (q->images) {
            Buf b(&a, 32);
            print(&b, (i64)q->images);
            print(&b, q->images==1 ? Str(" image") : Str(" images"));
            Str n = finish(&b);
            ty += lh;
            dl->AddText(v2(x, ty), g->pal->paneldim, n.cdata, n.cdata+n.len);
        }
        ImGui::SetCursorScreenPos(v2(x + width - bw, top + pad/4));
        ImGui::PushID((i32)q->id);
        ImGui::BeginDisabled(s->unqueue != 0);
        if (ImGui::SmallButton("Withdraw")) {
            guiunqueue(g, s, q->id);
        }
        ImGui::EndDisabled();
        itemnamed(g, "withdraw");
        itemtip(q->steer ? "Take it back into the box, before the agent reads it"
                                       : "Take it back into the box, before its turn begins");
        ImGui::PopID();
    }
}

enum : i64 { TOAST_LIFE = 6*1000*1000 };  // µs, as the scheduler counts

// A toast's button: its label, the name the checklist knows it by, and
// its tooltip.
struct ToastBtn {
    Str         label;
    Str         name;
    char const *tip;
};

// What a toast came to, as drawn.
struct ToastOut {
    f32 height;   // over the foot, its gap included, else 0
    b32 hovered;
};

// A toast at the foot of r0 to r1, over what is there, which moves
// nothing: text, and buttons at its right, the first's rect as which,
// or -1, as the child window id. With fit, it is only as wide as its
// text needs, at the right. The button pressed, or -1, also when there
// is no room.
static i32 toastbtns(Gui *g, char const *id, Str text, u32 color, u32 edge,
                     Slice<ToastBtn> btns, i32 which, ImVec2 r0, ImVec2 r1,
                     b32 fit = 0, ToastOut *out = 0)
{
    Arena       a    = g->frame;
    f32         sc   = g->scale;
    ImGuiStyle &st   = ImGui::GetStyle();
    f32         pad  = 8*sc;
    f32         room = r1.x - r0.x - st.ScrollbarSize - 24*sc;
    f32         w    = 560*sc<room ? 560*sc : room;
    if (out) {
        *out = {};
    }
    if (w < 100*sc) {
        return -1;  // no room
    }
    f32 bw = 0;
    for (iz i = 0; i < btns.len; i++) {
        bw += ImGui::CalcTextSize(cstr0(&a, btns[i].label)).x + 2*st.FramePadding.x;
        bw += i ? st.ItemSpacing.x : 0;
    }
    // Narrow, as in the Sessions pane: the button goes under the text,
    // which takes the whole width, rather than squeeze it
    b32  stack = w < 360*sc;
    if (fit && !stack) {
        f32 need = ImGui::CalcTextSize(cstr0(&a, text)).x + 2*pad + bw + st.ItemSpacing.x;
        w = need<w ? need : w;
    }
    f32  tw    = stack ? w - 2*pad : w - 2*pad - bw - st.ItemSpacing.x;
    f32  th    = ImGui::CalcTextSize(cstr0(&a, text), 0, false, tw).y;
    f32  fh    = ImGui::GetFrameHeight();
    f32  h     = stack ? th + st.ItemSpacing.y + fh + 2*pad : (th>fh ? th : fh) + 2*pad;
    f32  x     = fit ? r1.x - st.ScrollbarSize - 12*sc - w : r0.x + 12*sc;
    ImGui::SetCursorScreenPos(v2(x, r1.y - h - 10*sc));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, g->pal->frame);
    ImGui::PushStyleColor(ImGuiCol_Border, edge);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6*sc);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, v2(pad, pad));
    ImGui::BeginChild(id, v2(w, h), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                      ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetCursorPos(v2(pad, pad + (fh>th && !stack ? (fh-th)/2 : 0)));
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(pad + tw);
    ImGui::TextUnformatted(cstr0(&a, text));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    itemnamed(g, "toast");
    ImGui::SetCursorPos(v2(w - pad - bw, stack ? pad + th + st.ItemSpacing.y : pad));
    i32 pressed = -1;
    for (iz i = 0; i < btns.len; i++) {
        if (i) {
            ImGui::SameLine();
        }
        if (ImGui::Button(cstr0(&a, concat(&a, btns[i].label, "##toastx")))) {
            pressed = (i32)i;
        }
        if (btns[i].name.len) {
            itemnamed(g, btns[i].name);
        }
        itemtip("%s", btns[i].tip);
        if (which>=0 && !i) {
            itemrect(g, which);
        }
    }
    if (out) {
        out->height  = h + 10*sc;
        out->hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    return pressed;
}

// One with a single button, as most are: true when it is pressed.
static b32 toastbox(Gui *g, Str text, u32 color, u32 edge, Str btn, Str name,
                    char const *tip, i32 which, ImVec2 r0, ImVec2 r1, b32 fit = 0)
{
    ToastBtn b = {btn, name, tip};
    return toastbtns(g, "##toast", text, color, edge, {&b, 1}, which, r0, r1, fit) == 0;
}

// The session's note, what the latest command from the box came to, or
// the editor at work on its draft: a toast over the foot of the
// transcript (r0 to r1). A note that is not an error fades after a few
// seconds; an error stays until the next is typed, or its x dismisses
// it.
static void drawtoast(Gui *g, GuiSession *s, ImVec2 r0, ImVec2 r1)
{
    g->toasted   = 1;
    g->editshown = 0;
    b32 editing  = g->edsess && g->edsess==s && !g->edplan;
    b32 fresh    = s->noteerr || g->sched.now-s->noteat<TOAST_LIFE;
    if (s->note.len && !s->noteerr && fresh) {
        schedat(&g->sched, s->noteat + TOAST_LIFE, DUE_CLOCK);
    }
    // Esc left the box mid-turn (drawprompt): while the turn runs, a few
    // seconds, after an error note, which waits on the user
    g->eschintshown = 0;
    b32 hint = g->eschint==s && s->view && s->view->busy &&
               g->sched.now-g->eschintat<TOAST_LIFE && !(s->note.len && s->noteerr);
    if (g->eschint==s && !hint) {
        g->eschint = 0;
    } else if (hint) {
        schedat(&g->sched, g->eschintat + TOAST_LIFE, DUE_CLOCK);
    }
    if (editing) {
        // In the frame itself: toastbtns takes its scratch from there
        Str text = concat(&g->frame, concat(&g->frame, "Editing in ", g->edname), "...");
        if (toastbox(g, text, g->pal->paneltext, g->pal->rule, "Cancel", "",
                     "Stop the editor, leaving the draft as it was",
                     RECT_EDITCANCEL, r0, r1)) {
            editcancel(g);
        }
        g->editshown = 1;
    } else if (hint) {
        if (toastbox(g, "Esc again stops the turn", g->pal->paneltext, g->pal->rule,
                     "Stop", "esc stop", "Stop: cancel the turn, killing any command it runs",
                     -1, r0, r1)) {
            g->eschint = 0;
            guicancel(g, s);
        }
        g->eschintshown = 1;
    } else if (s->note.len && fresh) {
        if (toastbox(g, s->note, s->noteerr ? g->pal->bad : g->pal->paneldim,
                     s->noteerr ? g->pal->bad : g->pal->rule,
                     "\xc3\x97", "dismiss", "Dismiss", -1, r0, r1)) {  // a multiplication sign
            s->note = {};
        }
    }
}

// A host's toast (hostfailed): a failed connect, deployment or update,
// over the foot of a pane: the one it began from (Sessions, unless it
// was Hosts), or the other where that one was not drawn last frame, as
// when the user has gone to another tab. It stays until dismissed, or
// until the host is reached or tried again. A connect that failed for
// want of an engine there offers to deploy one. The pane's window
// scrolls as a whole, so its content keeps the extent it had without the
// toast. Its height, if drawn here, for a toast above it.
static f32 drawhosttoast(Gui *g, i32 pane)
{
    GuiHost *h = 0;
    for (GuiHost *k = g->hosts; k && g->htoasthost; k = k->next) {
        h = k->uid==g->htoasthost ? k : h;
    }
    if (!h) {
        return 0;
    }
    i32 home  = h->toastfrom==PANE_HOSTS ? PANE_HOSTS : PANE_SESSIONS;
    i32 other = home==PANE_HOSTS ? PANE_SESSIONS : PANE_HOSTS;
    b32 shown = g->panedrawn[home] >= g->total.frames-1;
    if (pane != (shown ? home : other)) {
        return 0;
    }
    ImGuiWindow *w    = ImGui::GetCurrentWindow();
    ImVec2       maxp = w->DC.CursorMaxPos;
    ImVec2       r0   = ImGui::GetWindowPos();
    ImVec2       r1   = v2(r0.x + ImGui::GetWindowWidth(), r0.y + ImGui::GetWindowHeight());
    ToastBtn     btns[2] = {};
    iz           n       = 0;
    b32          deploy  = h->kind==GH_REMOTE && h->state==RS_FAILED && h->deploys &&
                           !h->deployfail && !h->r && !remoteset(g, h->name, "path").len &&
                           !deploywhy(g, h, h->name).len;
    if (deploy) {
        // In the frame itself: toastbtns takes its scratch from there
        btns[n++] = {"Deploy engine", concat(&g->frame, "toast deploy ", h->name),
                     "Copy this version's engine there, as prospero deploy does, "
                     "then connect to it"};
    }
    btns[n++] = {"\xc3\x97", "host dismiss", "Dismiss"};  // a multiplication sign
    ToastOut out = {};
    i32      b   = toastbtns(g, "##toast", g->htoasttext, g->pal->bad, g->pal->bad,
                             {btns, n}, -1, r0, r1, 0, &out);
    if (b>=0 && deploy && b==0) {
        guideploy(g, h->name);
    }
    if (b >= 0) {
        g->htoasthost = 0;
    }
    g->failshown |= h->state == RS_FAILED;
    w->DC.CursorMaxPos = maxp;
    return out.height;
}

static GuiSession *dragged();

// The toast for sessions just moved to the Trash (trashtold), over the
// foot of the Sessions pane, or of Hosts where Sessions was not drawn
// last frame, above any deploy toast there (lift high). It goes after a
// while, but not while hovered, nor while a session is dragged.
static void drawtrashtoast(Gui *g, i32 pane, f32 lift)
{
    TrashToast *t = &g->trashtoast;
    GuiHost    *h = 0;
    for (GuiHost *k = g->hosts; k && t->host; k = k->next) {
        h = k->uid==t->host ? k : h;
    }
    if (!h || g->sched.now>=t->until) {
        t->host = 0;
        return;
    }
    b32 shown = g->panedrawn[PANE_SESSIONS] >= g->total.frames-1;
    if (pane!=(shown ? PANE_SESSIONS : PANE_HOSTS) || dragged()) {
        return;
    }
    schedat(&g->sched, t->until, DUE_CLOCK);
    ImGuiWindow *w    = ImGui::GetCurrentWindow();
    ImVec2       maxp = w->DC.CursorMaxPos;
    ImVec2       r0   = ImGui::GetWindowPos();
    ImVec2       r1   = v2(r0.x + ImGui::GetWindowWidth(), r0.y + ImGui::GetWindowHeight() - lift);
    ToastBtn     btns[] = {
        {"Undo", "trash undo", "Back where they were"},
        {"Delete now", "trash delete now", "Remove them for good now, not after their days in "
                                           "the Trash"},
        {"\xc3\x97", "trash dismiss", "Dismiss: they stay in the Trash"},  // a multiplication sign
    };
    ToastOut out = {};
    i32      b   = toastbtns(g, "##trashtoast", t->text, g->pal->paneltext, g->pal->rule,
                             {btns, countof(btns)}, -1, r0, r1, 0, &out);
    if (out.hovered && t->until-g->sched.now < 2*1000*1000) {
        t->until = g->sched.now + 2*1000*1000;  // kept while the mouse is on it
    }
    for (iz i = 0; b>=0 && b<2 && i < t->sids.len; i++) {
        GuiSession *s = findsess(g, h, t->sids[i]);
        if (s && !s->gone && b==0) {
            guitrash(g, s, 0);
        } else if (s && !s->gone) {
            guidelete(g, s);
        }
    }
    if (b >= 0) {
        t->host = 0;
    }
    w->DC.CursorMaxPos = maxp;
}

#if !PROSPERO_ITCH
#ifdef __clang__
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wc23-extensions"
#endif
static u8 const greetingmd[] = {
    #embed "../assets/greeting.md"
};
#ifdef __clang__
#  pragma clang diagnostic pop
#endif

// The Community Edition's greeting, assets/greeting.md, in an empty
// transcript: drawn here alone, never an event of the session, so no
// model, export or engine sees it. The itch.io builds leave the
// transcript blank.
static void drawgreeting(Gui *g, ImVec2 wpos, f32 winh, f32 width)
{
    f32 sc   = g->scale;
    f32 wrap = width<480*sc ? width : 480*sc;
    if (!g->greeting) {
        g->greeting = newdocview(&g->perm, g->os, g->vcache);
        docset(g->greeting, {(u8 *)greetingmd, countof(greetingmd)});
    }
    ImGui::SetCursorScreenPos(v2(wpos.x + (width - wrap)/2, wpos.y + winh/3));
    Str link = drawdoc(g, g->greeting, "##greeting", wrap);
    if (link.len) {
        followlink(g, link);
    }
}
#endif

static void drawtranscript(Gui *g)
{
    GuiSession *gs = g->sess ? g->sess : &g->nosession;
    View       *v  = g->view;
    f32         sc = g->scale;
    ImGuiStyle &st = ImGui::GetStyle();

    drawsessbar(g);
    if (gs->readonly && gs->kept.len) {
        g->bannershown = 1;
        Arena a    = g->frame;
        Str   name = gs->kept=="?" ? Str{} : concat(&a, concat(&a, ", ", gs->kept), ",");
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->amber);
        ImGui::TextWrapped("Read-only: this subagent ran in a container%s "
                           "that is gone. Its transcript was kept here.",
                           cstr0(&a, name));
        ImGui::PopStyleColor();
    } else if (gs->readonly) {
        g->bannershown = 1;
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->amber);
        ImGui::TextWrapped("Read-only: another Prospero process has this "
                           "session open. Its changes show after it lets go "
                           "and the session is opened again.");
        ImGui::PopStyleColor();
        if (ImGui::Button("Open again")) {
            guiopen(g, gs);
        }
        itemtip("Take the session over, if the other process has let go");
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
    if (v->thumbw!=thumbw || v->thumbh!=thumbh || v->thumbpad!=pad/2 ||
            v->thumbrow!=width) {
        v->topsdirty = 1;
    }
    v->thumbrow = width;
    v->thumbw   = thumbw;
    v->thumbh   = thumbh;
    v->thumbpad = pad/2;
    // A waiting question's form, measured before the tops are, so that
    // no frame draws it at another height than they leave it
    g->askactive = 0;
    for (iz i = v->msgs.len-1; v->asks && i >= 0; i--) {
        ViewMsg *m = &v->msgs[i];
        if (m->role==VIEW_ASK && m->tool->answer==VASK_PENDING) {
            if (m->tool->qs.len) {
                f32 h = questionform(g, gs, m, width, 0, 0);
                v->topsdirty |= h != m->tool->formh;
                m->tool->formh = h;
            }
            break;
        }
    }

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

    // The queue's tail below the messages, in the height so that the
    // view stuck to the bottom shows it
    Slice<QueueShow> shows = {};
    f64              tail0 = total + v->gap;
    g->queueshown = 0;
    g->rects[RECT_QUEUEITEM] = {};
    g->rects[RECT_LASTMSG]   = {};
    if (g->sess && g->sess->view==v && v->queue.len && !gs->readonly &&
            hostcap(gs->host, "queue")) {
        shows.data = alloc<QueueShow>(&g->frame, v->queue.len);
        shows.len  = shows.cap = v->queue.len;
        f64 y = tail0;
        for (iz i = 0; i < shows.len; i++) {
            shows[i] = queueshow(&g->frame, &v->queue[i], width, pad);
            total    = y + shows[i].h;
            y        = total + v->gap;
        }
    }
    if (g->queueview != v) {
        g->queueview = v;
        g->queuelen  = shows.len;
        g->queuemark = v->msgs.len;
    } else if (g->queuelen != shows.len) {
        // Shrunk with no message of the user's since: withdrawn, or
        // gone missing for a while (the drive checks count these)
        b32 taken = 0;
        for (iz i = g->queuemark; i < v->msgs.len; i++) {
            taken |= v->msgs[i].role == VIEW_USER;
        }
        g->queuedrops += shows.len<g->queuelen && !taken;
        g->queuelen  = shows.len;
        g->queuemark = v->msgs.len;
    }

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
    TextArea ta  = {v, wpos, winh, width, pad, head, scrolly, &g->dragging, &g->scrollto,
                    0, 0, 0, 0, 1};
    TextHit  hit  = textmouse(g, &ta);
    LinkFrom from = filelinkfrom(g->sess, hit.hit ? msgcwd(v, hit.at.msg) : v->cwd);
    Str      link = textlink(g, v, hit, &g->linkpress, &g->linkmsg, &from);
    itemrect(g, RECT_TRANSCRIPT);
    if (link.len) {
        followlink(g, link, &from);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        g->menumsg = hit.hit ? hit.at.msg : -1;  // the popup takes the hover from here on
    }
    if (begintextmenu(g, v, RECT_COPYITEM)) {
        b32 onmsg = g->menumsg>=0 && g->menumsg<v->msgs.len;
        if (ImGui::MenuItem("Copy Message", 0, false, onmsg)) {
            copymessage(g, g->menumsg);
        }
        b32 forked = forkitem(g, onmsg ? &v->msgs[g->menumsg] : 0);
        sideitem(g, v, !forked);
        endtextmenu(g, v);
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
    i32 latest[] = {RECT_USERMSG, RECT_TASKLINK, RECT_UNDO, RECT_REWINDTURN, RECT_FORCE,
                    RECT_CUTUNDO, RECT_COMPACTHDR,
                    RECT_THINKHDR, RECT_ALWAYS, RECT_RAIL, RECT_TOOLPATH};
    for (iz i = 0; i < countof(latest); i++) {
        g->rects[latest[i]] = {};  // the latest drawn, or none
    }
    g->thinkrows  = 0;
    g->thinkcount = -1;
    g->thinksecs  = -1;
    g->rects[RECT_MSGLINK]   = {};
    g->rects[RECT_CODEBLOCK] = {};
    g->rects[RECT_CODECOPY]  = {};
    g->copiedshown = 0;
    g->clockshown  = 0;
    g->railseen    = 0;
    g->bgshown     = 0;
    g->stopshown   = 0;
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
        ImVec2     box0  = v2(wpos.x+pad/4, y);
        ImVec2     box1  = v2(wpos.x+pad*1.5f+width, y+v->extra+m->h+mfoot);

        // Backgrounds: the user's messages, tool rows, ask cards
        sp->SetCurrentChannel(dl, CH_BG);
        if (i == v->msgs.len-1) {
            g->rects[RECT_LASTMSG] = {box0.x, box0.y, box1.x, box1.y};
        }
        if (m->role == VIEW_USER) {
            g->rects[RECT_USERMSG] = {box0.x, box0.y, box1.x, box1.y};
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
        f32       actw = act.kind ? actwidth(&act) + 8*sc : 0;
        Pen    p    = {dl, &g->fonts, x, y + pad/4, x + width - actw, smallsize(&g->fonts)};
        ImVec2 path = drawheader(g, &p, v, m, nowms);
        if ((m->role==VIEW_USER || m->role==VIEW_ASSISTANT) && m->ts &&
                ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(box0, box1)) {
            // When it was sent, or finished, at the header's right,
            // over room the header line has anyway
            Arena a = g->frame;
            Buf   b(&a, 32);
            printclock(&b, m->ts, os_utcoffset(g->os, m->ts), nowms,
                       os_utcoffset(g->os, nowms));
            Str     s    = finish(&b);
            ImFont *font = stylefont(&g->fonts, 0);
            f32     w    = font->CalcTextSizeA(p.size, 1e9f, 0, s.cdata, s.cdata+s.len).x;
            if (p.right - w >= p.x + 2*g->fonts.size) {
                p.x = p.right - w;
                pen(&p, s, 0, fade(g->pal->dim, 0.8f));
                g->clockshown = 1;
            }
        }
        if (!m->collapsed) {
            Marks mk = msgmarks(g, v, i);
            drawmsgtext(g, dl, sp, &ta, i, ml, v2(x, y + head), sel, &mk, alpha, 1);
        }

        ImGui::PushID(i);
        if (m->role==VIEW_TOOL && m->tool->file.kind==FREF_PATH && m->tool->file.path.len &&
                path.y > path.x) {
            // Its file's path, before the header's toggle, which it is over
            toolpathlink(g, v, i, path.x, path.y, y, head, p.y, p.size);
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
        // The fold rail: the bar at the left of a message that folds, a
        // tool row's colored one, or a faint one for reasoning, folds or
        // opens it from anywhere along it, through the margin left of
        // its text, and brings its header back into view if above
        b32 folds = m->fold || m->role==VIEW_TOOL || m->role==VIEW_THINK;
        b32 bar   = m->role==VIEW_TOOL || m->role==VIEW_COMPACT || m->role==VIEW_REWIND;
        f32 r0    = box0.y>top ? box0.y : top;
        f32 r1    = box1.y<bot ? box1.y : bot;
        if (folds && r1-r0>=1) {
            ImGui::SetCursorScreenPos(v2(wpos.x, r0));
            b32 go  = ImGui::InvisibleButton("##rail", v2(x - wpos.x, r1 - r0));
            b32 hov = ImGui::IsItemHovered() || ImGui::IsItemActive();
            itemrect(g, RECT_RAIL);
            if (hov) {
                dl->AddRectFilled(box0, v2(box0.x + 5*sc, box1.y), g->pal->active);
            } else if (!bar) {
                dl->AddRectFilled(box0, v2(box0.x + 3*sc, box1.y), fade(g->pal->dim, 0.5f*alpha));
            }
            if (hov) {
                dl->AddRect(box0, box1, fade(g->pal->active, 0.5f), 4*sc, sc);
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                itemtip(m->collapsed ? "Unfold" : "Fold");
            }
            if (go) {
                if (box0.y < top) {
                    g->scrollto = m->top;  // its header at the top
                }
                togglemsg(v, i);
                g->railfold = i + 1;
            }
        }
        if (g->railfold==i+1 && y>=top && y+head<=bot) {
            g->railseen = 1;  // the message the rail folded, its header in view
        }
        if (act.bgw > 0) {
            MsgAction bg = {};
            bg.kind  = ACT_BACKGROUND;
            bg.label = bglabel;
            bg.w     = act.bgw;
            ImGui::SetCursorScreenPos(v2(x + width - actwidth(&act), y + pad/4));
            drawaction(g, gs, m, &bg);
        }
        Thumb th = msgthumb(v, m);
        if (th.h > 0) {
            sp->SetCurrentChannel(dl, CH_TEXT);
            drawthumb(g, gs, dl, m->tool->image, m->tool->imgw, m->tool->imgh, 0,
                      v2(x, y + head + m->h + pad/4), th, top, bot);
        } else if (mfoot>0 && m->role==VIEW_USER) {
            sp->SetCurrentChannel(dl, CH_TEXT);
            for (iz k = 0; k < m->images.len; k++) {
                ViewImage *im = &m->images[k];
                ThumbAt    t  = userthumbat(v, m, k);
                ImGui::PushID((i32)k);
                drawthumb(g, gs, dl, im->name, im->w, im->h, 1,
                          v2(x + t.x, y + head + m->h + pad/4 + t.y), {t.w, t.h}, top, bot);
                ImGui::PopID();
            }
        } else if (mfoot>0 && m->role==VIEW_ASK && m->tool->qs.len) {
            ImGui::SetCursorScreenPos(v2(x, y + head + m->h + pad/4));
            questionform(g, gs, m, width, 1, i==lastask);
        } else if (mfoot>0 && m->role==VIEW_ASK) {
            ImGui::SetCursorScreenPos(v2(x, y + head + m->h + pad/4));
            drawaskbuttons(g, gs, m, i==lastask);
        } else if (mfoot>0 && m->role==VIEW_REWIND && m->card->kind=="cut") {
            ImGui::SetCursorScreenPos(v2(x, y + head + m->h + pad/4));
            drawcutundo(g, gs, m);
        } else if (mfoot > 0) {
            ImGui::SetCursorScreenPos(v2(x, y + head + m->h + pad/4));
            drawforce(g, gs, m);
        }
        ImGui::PopID();
    }
    sp->Merge(dl);
    if (shows.len) {
        drawqueue(g, g->sess, shows, tail0, wpos, scrolly, winh, width, pad);
    }
#if !PROSPERO_ITCH
    if (!v->msgs.len && !shows.len) {
        drawgreeting(g, wpos, winh, width + 2*pad);
    }
#endif
    textkeys(g, v);

    gs->stick = scrolly >= ImGui::GetScrollMaxY()-2 || total<=winh;
    ImGui::EndChild();
    ImGui::PopID();
    ImVec2 tr0 = ImGui::GetItemRectMin();
    ImVec2 tr1 = ImGui::GetItemRectMax();
    drawfind(g, v, scrolly, tr0, tr1);
    if (g->sess == gs) {
        drawtoast(g, gs, tr0, tr1);
    }

    if (g->opendeny) {
        ImGui::OpenPopup("##denynote");
        g->denyplan = g->opendeny == 2;
        g->opendeny = 0;
        g->denytouch = 0;
    }
    if (ImGui::BeginPopup("##denynote")) {
        ImGui::TextUnformatted(g->denyplan ? "Keep planning, and tell the agent:"
                                           : "Deny, and tell the agent:");
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(360*sc);
        b32 go = ImGui::InputText("##note", g->denynote, FIELD_MAX,
                                  ImGuiInputTextFlags_EnterReturnsTrue);
        // Opening the note, then writing it, starts the ask's clock over,
        // so its deadline cannot deny the call mid-sentence
        if (g->denytimed && (ImGui::IsWindowAppearing() || ImGui::IsItemEdited())) {
            guitouch(g, g->denysess, g->denyreq, &g->denytouch, "ask.deadline");
        }
        go |= ImGui::Button(g->denyplan ? "Keep planning" : "Deny");
        if (go) {
            Str note = {(u8 *)g->denynote, (iz)__builtin_strlen(g->denynote)};
            guianswer(g, g->denysess, g->denyreq, "deny", trim(note));
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

// Esc cancels the shown session's turn, from the Transcript and the
// Prompt while one has focus, once nothing is left for it to close:
// a popup or menu, the find bar, a field of the agent's questions, the
// box's lists, and the box itself, which Esc leaves first (drawprompt),
// as a field elsewhere (Settings) keeps Esc to abandon its edit. Nor
// while ImGui's keyboard navigation shows its cursor, or is in a menu
// bar (Alt): that Esc is ImGui's, to end it. A Shortcut owns its key
// from the next frame on, ahead of ImGui's own handling, so it is not
// even asked for then.
static void escapecancel(Gui *g)
{
    ImGuiContext &c   = *GImGui;
    ImGuiID       box = g->sess ? g->sess->draftid : 0;
    b32 nav = (c.NavCursorVisible && c.NavId && c.NavId!=c.ActiveId && c.NavId!=box) ||
              c.NavLayer!=ImGuiNavLayer_Main;
    if (!nav && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !g->findactive &&
            !g->findheld && !g->askactive &&
            ImGui::Shortcut(ImGuiKey_Escape, ImGuiInputFlags_RouteFocused|ImGuiInputFlags_RouteOverActive)) {
        g->eschint = 0;
        guicancel(g, g->sess);
    }
}

// Cmd+B (Ctrl+B) moves to the background whatever the shown session's
// turn waits for, its command or its subagents, from the same windows
// as Esc and gated alike.
static void backgroundkey(Gui *g)
{
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !g->findactive &&
            !g->askactive && ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_B, ImGuiInputFlags_RouteFocused|ImGuiInputFlags_RouteOverActive)) {
        guibackground(g, g->sess, {});
    }
}

// What the session is doing, how fast, and how long since the model sent
// anything: one line, whose clocks ask for frames only while shown.
enum : i32 {
    STRIP_FULL,     // on the picks' line, in full
    STRIP_BRIEF,    // on the picks' line, brief
    STRIP_BELOW,    // on a line of their own, in full
    STRIP_BELOWBRIEF,
};

// One readout of the Prompt's status line, after the picks: its text,
// or a progress bar, each in a full form and a brief one for a cramped
// line (stripdraw).
struct StripItem {
    Str  text;    // the full form
    Str  brief;   // the brief form, null for the full form's
    b32  drop;    // left out of the brief form altogether
    u32  col;     // its text color, 0 for the disabled color
    f32  bar;     // a progress bar's fraction, else negative
    f32  barw;    // and its widths, full and brief
    f32  briefw;
    Str  tip;     // its tooltip, null for none
    b32 *shown;   // set once drawn
};

struct Strip {
    StripItem items[16];
    i32       n;
};

static StripItem *stripadd(Strip *st, Str text, Str brief = {}, u32 col = 0)
{
    if (st->n == countof(st->items)) {
        return 0;
    }
    StripItem *it = st->items + st->n++;
    *it = {};
    it->text  = text;
    it->brief = brief;
    it->col   = col;
    it->bar   = -1;
    return it;
}

// The readouts' width, in a form (brief or not), spaced as SameLine
// spaces them.
static f32 stripwidth(Strip *st, b32 brief)
{
    f32 gap = ImGui::GetStyle().ItemSpacing.x;
    f32 w   = 0;
    i32 n   = 0;
    for (i32 i = 0; i < st->n; i++) {
        StripItem *it = st->items + i;
        if (brief && it->drop) {
            continue;
        }
        Str t = brief && it->brief.data ? it->brief : it->text;
        w += n++ ? gap : 0;
        w += it->bar>=0 ? (brief ? it->briefw : it->barw)
                        : ImGui::CalcTextSize(t.cdata, t.cdata+t.len).x;
    }
    return w;
}

// The readouts after the picks, on the picks' line, or on a line of their
// own below where that line's room is short of a reference: the brief
// form of a prompt being read (stripref). The room depends only on the
// pane's width and the picks, a long model name's widening its pick, never
// on what the readouts say, so the line they take, and the compose box's
// height with it, stays put as they change. On whichever line, they are
// in full if they fit, else brief.
static f32 stripref(f32 sc)
{
    Strip st = {};
    stripadd(&st, "streaming");
    stripadd(&st, "10 min 59 s");
    StripItem *it = stripadd(&st, "prompt 99%");
    it->bar    = 1;
    it->briefw = 100*sc;
    stripadd(&st, "4 min 59 s left");
    return stripwidth(&st, 1);
}

static void stripdraw(Gui *g, Arena a, Strip *st, b32 picks)
{
    f32 room  = ImGui::GetContentRegionAvail().x;
    b32 below = picks && room<stripref(g->scale);
    // The line taken changes only with the room
    g->stripbob    |= g->stripseen && below!=g->stripbelow && room==g->striproom;
    g->stripseen    = 1;
    g->stripbelow   = below;
    g->striproom    = room;
    if (below) {
        ImGui::NewLine();
        room = ImGui::GetContentRegionAvail().x;
    }
    b32 brief = stripwidth(st, 0) > room;
    g->stripform     = below ? (brief ? STRIP_BELOWBRIEF : STRIP_BELOW)
                             : (brief ? STRIP_BRIEF : STRIP_FULL);
    g->stripcramped |= g->stripform != STRIP_FULL;
    g->striprate     = 0;
    ImGui::AlignTextToFramePadding();
    f32 right = ImGui::GetCursorScreenPos().x + room;
    i32 n = 0;
    for (i32 i = 0; i < st->n; i++) {
        StripItem *it = st->items + i;
        if (brief && it->drop) {
            continue;
        }
        if (n++) {
            ImGui::SameLine();
        }
        Str t = brief && it->brief.data ? it->brief : it->text;
        if (it->bar >= 0) {
            ImGui::ProgressBar(it->bar, v2(brief ? it->briefw : it->barw, 0),
                               cstr0(&a, t));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, it->col ? it->col :
                                  ImGui::GetColorU32(ImGuiCol_TextDisabled));
            ImGui::TextUnformatted(t.cdata, t.cdata+t.len);
            ImGui::PopStyleColor();
        }
        if (it->tip.len) {
            itemtip("%s", cstr0(&a, it->tip));
        }
        if (it->shown) {
            *it->shown = 1;
        }
        g->striprate |= it->shown == &g->prateshown;
        // Past the edge where the room was enough for the reference,
        // not in a pane too narrow for any (as briefly while docking)
        if (!g->stripover && ImGui::GetItemRectMax().x>right+1 &&
                room>=stripref(g->scale)) {
            Buf b(&a, 128 + t.len);
            print(&b, t);
            print(&b, below ? Str(" (below, ") : Str(" (beside, "));
            print(&b, brief ? Str("brief, room ") : Str("full, room "));
            print(&b, (i64)room);
            print(&b, ", ends at ");
            print(&b, (i64)(ImGui::GetItemRectMax().x - right + room));
            print(&b, ")");
            g->stripover = 1;
            g->stripwhat = clone(&g->perm, finish(&b));
        }
    }
}

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
    b32   picks = g->sess->view != 0;
    if (picks) {
        drawsesspicks(g, a, g->sess, v);
    }
    Strip st = {};
    stripadd(&st, livelabel(v, v->state), {}, livecolor(g->pal, v->state));

    if (v->busy && g->sess->readonly) {
        // Its records go to the other process: nothing here would see
        // the turn end, so no clock counts on forever
        stripadd(&st, "in another process, as of its log when opened",
                 "in another process");
    } else if (v->busy && v->turnts) {
        // The turn's clock, once a second
        g->roclock |= g->sess->readonly;
        i64 el = nowms - v->turnts;
        el = el<0 ? 0 : el;
        schedat(&g->sched, now + (1000 - el%1000)*1000 + 1000, DUE_CLOCK);
        Buf b(&a, 32);
        printsecs(&b, el/1000);
        stripadd(&st, finish(&b), {}, ImGui::GetColorU32(ImGuiCol_Text));
    }
    if (v->busy && v->thinking==THINK_HIDE && !v->showthink && v->stream>=0 &&
            v->streamkind==VIEW_THINK) {
        // Why the transcript shows nothing while the model thinks
        StripItem *it = stripadd(&st, "thinking (reasoning hidden)", "thinking");
        if (it) {
            it->shown = &g->thinkhidshown;
        }
    }
    if (v->state == LIVE_QUEUED) {
        // Its place in the endpoint's line, which live records update
        Buf b(&a, 96);
        Str brief = "waiting";
        print(&b, "waiting for the endpoint");
        if (v->queuepos) {
            i64 len = v->queuelen>v->queuepos ? v->queuelen : v->queuepos;
            print(&b, ": ");
            Buf c(&a, 48);
            print(&c, (i64)v->queuepos);
            print(&c, " of ");
            print(&c, len);
            print(&c, " in line");
            brief = finish(&c);
            print(&b, brief);
        }
        StripItem *it = stripadd(&st, finish(&b), brief);
        if (it) {
            it->shown = &g->queuedshown;
        }
    } else if (v->state == LIVE_TASK) {
        g->taskshown = v->tasks;
        Buf b(&a, 64);
        print(&b, (i64)v->tasks);
        print(&b, v->tasks==1 ? Str(" subagent") : Str(" subagents"));
        Str brief = finish(&b);
        stripadd(&st, concat(&a, brief, " at work"), brief);
    } else if (v->state == LIVE_COMPACTING) {
        StripItem *it = stripadd(&st, "summarizing the conversation to make room",
                                 "summarizing");
        if (it) {
            it->shown = &g->compactingshown;
        }
    }
    if (req && v->tps > 0) {
        Buf b(&a, 32);
        printfloat(&b, v->tps, 1);
        print(&b, " tok/s");
        stripadd(&st, finish(&b));
    }
    if (req && v->ptotal>0 && v->pdone<v->ptotal) {
        f32 frac = (f32)v->pdone / (f32)v->ptotal;
        Buf b(&a, 48);
        print(&b, (i64)(frac*100));
        putbyte(&b, '%');
        Str pct = finish(&b);
        StripItem *it = stripadd(&st, concat(&a, "reading the prompt ", pct),
                                 concat(&a, "prompt ", pct));
        if (it) {
            it->bar    = frac;
            it->barw   = 160*sc;
            it->briefw = 100*sc;
        }
        if (v->ptps > 0) {
            // Its rate, as the output's, tokens computed with the cache's
            // left out: whole tokens, as it runs to thousands; the first
            // to go from a cramped line
            Buf r(&a, 32);
            printgrouped(&r, (i64)(v->ptps + 0.5));
            print(&r, " tok/s");
            it = stripadd(&st, finish(&r));
            if (it) {
                it->drop  = 1;
                it->shown = &g->prateshown;
            }
        }
        // The engine's estimate, counted down between its records, and
        // left out near the end, where it matters little
        i64 left = v->petaat ? v->petaat - nowms : 0;
        if (left >= ETA_MIN) {
            schedat(&g->sched, now + (left%1000)*1000 + 1000, DUE_CLOCK);
            i64 secs = (left + 999)/1000;
            Buf e(&a, 48);
            printsecs(&e, secs<600 ? secs : (secs + 30)/60*60);
            print(&e, " left");
            Str brief = finish(&e);
            it = stripadd(&st, concat(&a, "about ", brief), brief);
            if (it) {
                it->shown = &g->etashown;
            }
        }
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
            printsecs(&b, idle/1000);
            if (red && v->watchat>nowms) {
                print(&b, ", giving up in ");
                printsecs(&b, (v->watchat - nowms + 999)/1000);
            }
            stripadd(&st, finish(&b), {}, red ? g->pal->bad : g->pal->amber);
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
        Str brief = finish(&b);
        b32 other = v->retrymodel.len && v->retrymodel!=v->model;
        Str full  = brief;
        if (other) {
            full = concat(&a, brief, v->fallback ? Str(" on fallback ") : Str(" with "));
            full = concat(&a, full, v->retrymodel);
            g->fallbackshown |= v->fallback > 0;
        }
        stripadd(&st, full, brief, g->pal->amber);
    }
    if (v->state == LIVE_RELAY) {
        // A thin host's session, with no computer to make its calls
        Buf b(&a, 128 + v->model.len);
        print(&b, "until a computer offering ");
        print(&b, v->model.len ? v->model : Str("its model"));
        print(&b, " connects");
        if (v->relayuntil > nowms) {
            print(&b, ", for up to ");
            printsecs(&b, (v->relayuntil - nowms + 59999)/60000*60);
        }
        StripItem *it = stripadd(&st, finish(&b), "until a computer connects");
        if (it) {
            it->tip = "Its host has no endpoint of its own: a computer "
                      "connected to it makes its model calls, with its "
                      "own endpoint and key. It resumes once one that "
                      "offers this model connects, or the turn ends "
                      "after the endpoint's slow_retry_budget.";
        }
    } else if (req && v->relaylen) {
        // The computer making its calls, and its place in that line
        Str machine = Str(v->relay, v->relaylen);
        if (!g->machine.len) {
            g->machine = relaymachine(&g->perm, cut(os_hostname(g->os, &a), '.').head);
        }
        b32 here = machine == g->machine;
        Buf b(&a, 160);
        Str via = here ? Str("via this computer") : concat(&a, "via ", machine);
        print(&b, via);
        if (v->queuepos) {
            print(&b, ", waiting for the endpoint: ");
            print(&b, (i64)v->queuepos);
            print(&b, " of ");
            print(&b, (i64)(v->queuelen>v->queuepos ? v->queuelen : v->queuepos));
            print(&b, " in line");
        }
        StripItem *it = stripadd(&st, finish(&b), via);
        if (it) {
            it->tip = concat(&a, concat(&a, "Its host has no endpoint of its own: ",
                                        here ? Str("this computer") : machine),
                             " makes its model calls, with its own endpoint and key.");
        }
    }
    if (v->asks) {
        stripadd(&st, "waiting for your answer above", "waiting for your answer",
                 g->pal->amber);
    }
    stripdraw(g, a, &st, picks);
}

// The compose box: one draft per session (app.cpp), with history, slash
// commands, a context menu and an external editor (compose.cpp holds
// the logic apart from ImGui). ImGui keeps undo for the box last
// active, in stb_textedit's state, whose layout imgui_internal.h only
// declares: its undo points say what Undo and Redo can do.
namespace ImStb {
#include "imstb_textedit.h"
}

static char const *cmdkey(Gui *, Arena *, Str);

// The context menu's picks, done by the box itself: each as the key
// chord it answers to, once the box is active again.
enum : i32 {
    CM_NONE,
    CM_UNDO,
    CM_REDO,
    CM_CUT,
    CM_COPY,
    CM_PASTE,
    CM_SELALL,
};

// The session's own prompts and steers, which history recalls: its
// user messages, but not the tasks a subagent's parent gave it.
static b32 histmsg(ViewMsg *m)
{
    return m->role==VIEW_USER && !m->prov && m->label!="Task from the parent agent";
}

static i32 histcount(View *v)
{
    i32 n = 0;
    for (iz i = 0; i < v->msgs.len; i++) {
        n += histmsg(&v->msgs[i]);
    }
    return n;
}

// History entry pos, 1 the newest: its message, else -1.
static i32 histfind(View *v, i32 pos)
{
    for (iz i = v->msgs.len-1; i>=0 && pos>0; i--) {
        if (histmsg(&v->msgs[i]) && !--pos) {
            return (i32)i;
        }
    }
    return -1;
}

// What the box's callback works with, for one frame.
struct ComposeCb {
    GuiSession *s;
    View       *v;
    i32        *slashsel;
    i32        *mentionsel;  // the '@' popup's pick, as slashsel is the slash's
    i32         slash;     // the slash popup's entries, 0 if it is closed
    i32         mention;   // and the '@' popup's rows, 0 if it is closed
    MentionEdit *edit;     // a pick's edit, queued for this frame to apply
    i32         cursor;    // the caret before this frame's keys, -1 if inactive
    b32         hadsel;
    b32         recalled;  // history replaced the text
    b32         tab;       // Tab, kept out of the text, completes the pick
    b32         escaped;   // Esc left history: the box stays active
    b32         full;      // the box had no room for the edit
    b32         applied;   // the edit landed: the text grew, but not by a paste
    b32         toend;     // put the caret at the end (promptend)
};

// Up and Down that moved nothing, on the first or last visual line,
// go to the start or end of the text; on an empty box, and once
// recalling, through the session's history (histkey). With the slash
// popup open, they pick among its commands instead, and Tab, which the
// box then takes as a character, completes (filtered out). Esc while
// recalling empties the box, as the box gives it up. A pick the '@'
// popup accepted is applied here first, its offsets checked against the
// token they were taken from. The active box owns every key, so the
// callback, inside it, is where they are seen.
static i32 composecb(ImGuiInputTextCallbackData *d)
{
    ComposeCb *c  = (ComposeCb *)d->UserData;
    ImGuiIO   &io = ImGui::GetIO();
    if (d->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
        if (d->EventChar=='\t' && (c->slash || c->mention)) {
            c->tab = 1;
            return 1;
        }
        return 0;
    }
    // Before a pick, which returns early: a box just activated starts at
    // the end of its draft, and a pick then puts the caret where it goes
    if (c->toend) {
        d->CursorPos = d->SelectionStart = d->SelectionEnd = d->BufTextLen;
        c->toend = 0;
    }
    // A pick queued last frame, applied now that the box is at the top of
    // its own frame: before the keys, so they see the text it made, and
    // never after the token has moved, which is checked below.
    MentionEdit *e = c->edit;
    if (e && e->on) {
        // The offsets are a frame old: the token must still be there
        if (e->end > d->BufTextLen || e->end - e->beg != e->was.len ||
                __builtin_memcmp(d->Buf + e->beg, e->was.data, (uz)e->was.len)) {
            e->on = 0;
            return 0;
        }
        // Never delete before this check: InsertChars clips silently
        if (d->BufTextLen - (e->end - e->beg) + e->text.len >= d->BufSize) {
            e->on = 0;
            c->full = 1;
            return 0;
        }
        d->DeleteChars((i32)e->beg, (i32)(e->end - e->beg));
        d->InsertChars((i32)e->beg, e->text.cdata, e->text.cdata + e->text.len);
        d->CursorPos = d->SelectionStart = d->SelectionEnd = (i32)e->caret;
        e->on = 0;
        c->applied = 1;  // the text grew: a pick, not a paste
        return 0;
    }
    if (c->s->hist && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        d->DeleteChars(0, d->BufTextLen);
        c->s->hist  = 0;
        c->recalled = 1;
        c->escaped  = 1;
        return 0;
    }
    b32 up = ImGui::IsKeyPressed(ImGuiKey_UpArrow);
    b32 dn = ImGui::IsKeyPressed(ImGuiKey_DownArrow);
    if ((!up && !dn) || c->cursor<0 || io.KeyCtrl || io.KeyShift || io.KeyAlt || io.KeySuper) {
        return 0;
    }
    if (c->slash) {
        i32 n = c->slash;
        *c->slashsel = (*c->slashsel + (up ? n-1 : 1)) % n;
        return 0;
    }
    // The '@' popup's rows, claimed as the slash popup's are, before
    // histkey reads the same keys as history
    if (c->mention) {
        i32 n = c->mention;
        *c->mentionsel = (*c->mentionsel + (up ? n-1 : 1)) % n;
        return 0;
    }
    b32     edge = d->CursorPos==c->cursor && !c->hadsel;
    HistKey k    = histkey(up, edge, c->s->hist, histcount(c->v), d->BufTextLen==0);
    switch (k.act) {
    case HK_START:
        d->CursorPos = d->SelectionStart = d->SelectionEnd = 0;
        break;
    case HK_END:
        d->CursorPos = d->SelectionStart = d->SelectionEnd = d->BufTextLen;
        break;
    case HK_RECALL: {
        i32 i    = k.pos ? histfind(c->v, k.pos) : -1;
        Str text = i>=0 ? c->v->msgs[i].text : Str{};
        if (k.pos && (i<0 || text.len>=d->BufSize)) {
            break;
        }
        d->DeleteChars(0, d->BufTextLen);
        d->InsertChars(0, text.cdata, text.cdata+text.len);
        d->CursorPos = d->SelectionStart = d->SelectionEnd = d->BufTextLen;
        c->s->hist    = k.pos;
        c->s->histmsg = i;
        c->recalled   = 1;
    } break;
    }
    return 0;
}

// A context menu pick, as its key chord, queued for the next frame's
// input: the platform's command key is Super to ImGui on macOS, which
// it then swaps for Ctrl, as it does the real key. Its key is held
// while the release waits in the queue, which is no lost release.
static void composechord(Gui *g, i32 act)
{
    ImGuiKey key   = ImGuiKey_None;
    b32      shift = 0;
    switch (act) {
    case CM_UNDO:   key = ImGuiKey_Z; break;
    case CM_REDO:   key = ImGuiKey_Z; shift = 1; break;
    case CM_CUT:    key = ImGuiKey_X; break;
    case CM_COPY:   key = ImGuiKey_C; break;
    case CM_PASTE:  key = ImGuiKey_V; break;
    case CM_SELALL: key = ImGuiKey_A; break;
    }
    if (key == ImGuiKey_None) {
        return;
    }
    ImGuiIO &io  = ImGui::GetIO();
    ImGuiKey mod = g->mac ? ImGuiMod_Super : ImGuiMod_Ctrl;
    schedkey(&g->sched, g->sched.now);
    io.AddKeyEvent(mod, true);
    if (shift) {
        io.AddKeyEvent(ImGuiMod_Shift, true);
    }
    io.AddKeyEvent(key, true);
    io.AddKeyEvent(key, false);
    if (shift) {
        io.AddKeyEvent(ImGuiMod_Shift, false);
    }
    io.AddKeyEvent(mod, false);
}

// Run a slash command typed in the box (parseslash). Sent commands
// clear the box, keeping the text to put back if the command fails.
static void runslash(Gui *g, GuiSession *s, Slash sl, Str text)
{
    s->note = {};
    if (sl.err.len) {
        setnote(g, s, sl.err, 1);
        return;
    }
    GuiHost *h    = s->host;
    b32      sent = 0;
    switch (sl.cmd) {
    case SLASH_COMPACT:
        if ((sent = hostcan(h, "compact"))) {
            guicompact(g, s, sl.arg);
        }
        break;
    case SLASH_MODEL:
        if (!sl.arg.len) {
            g->pickmodel = s;  // the session bar's model picker
            draftput(g, s, "");
            drafttyped(&s->dclock, g->sched.now);
            return;
        } else if ((sent = hostcan(h, "model"))) {
            guimodel(g, s, sl.arg);
        }
        break;
    case SLASH_SHELL:
        if (!sl.arg.len) {
            g->pickshell = s;  // the session bar's shell picker
            draftput(g, s, "");
            drafttyped(&s->dclock, g->sched.now);
            return;
        } else if ((sent = hostcan(h, "shell"))) {
            guishell(g, s, sl.arg);
        }
        break;
    case SLASH_RENAME:
        if ((sent = hostcan(h, "rename"))) {
            guirename(g, s, sl.arg);
        }
        break;
    case SLASH_FORK:
        if ((sent = hostcap(h, "fork"))) {
            guifork(g, s, sl.turn);
        }
        break;
    case SLASH_SIDE:
        if (sessparent(s).len) {
            setnote(g, s, concat(&g->frame, "/side: ", sidewhy(g, s)), 1);
            return;
        } else if ((sent = hostcap(h, "side"))) {
            guiside(g, s, {});
        }
        break;
    case SLASH_WORKTREE: {
        Str why = wtwhy(g, s);
        if (why.len) {
            setnote(g, s, concat(&g->frame, "/worktree: ", why), 1);
            return;
        } else if (!sl.arg.len) {
            draftput(g, s, "");
            drafttyped(&s->dclock, g->sched.now);
            wtdialog(g, s, {});
            return;
        }
        sent = 1;
        guiworktree(g, s, sl.arg, {});
    } break;
    case SLASH_EXPORT:
        // As the Session menu's Export does: Markdown's options first
        draftput(g, s, "");
        drafttyped(&s->dclock, g->sched.now);
        if (sl.raw) {
            guiexport(g, s, EXPORT_RAW, 0);
        } else {
            g->exportsess = s;
            g->openexport = 1;
        }
        return;
    case SLASH_PLAN: {
        Str why = modewhy(g, s, sl.off ? Str("default") : Str("plan"));
        if (!why.len && sl.off && s->view && !s->view->plan) {
            why = "Plan mode is not on";
        }
        if (why.len) {
            setnote(g, s, concat(&g->frame, "/plan: ", why), 1);
            return;
        }
        guimode(g, s, sl.off ? Str("default") : Str("plan"));
        sent = 1;
    } break;
    case SLASH_BACKGROUND:
        // As Cmd+B, everything the turn waits for
        if (hostcap(h, "background") && !canbackground(s)) {
            setnote(g, s, "/background: nothing running can go to the background", 1);
            return;
        } else if ((sent = hostcap(h, "background"))) {
            guibackground(g, s, {});
        }
        break;
    }
    if (!sent) {
        Arena a = g->frame;
        setnote(g, s, concat(&a, slashinfo[sl.cmd].name,
                             ": this host's engine is too old for it; update the engine"), 1);
        return;
    }
    s->cmdtext = clone(&g->perm, text);
    draftput(g, s, "");
    drafttyped(&s->dclock, g->sched.now);
}

// The slash popup, above the box while its first word is typed: each
// command and skill that word may become, with what it does, the pick
// marked. Its scratch is a, after what the caller keeps there (picks).
static void drawslash(Gui *g, Arena a, Str text, Slice<SkillItem> skills, ImVec2 at)
{
    ImGui::SetNextWindowPos(at, ImGuiCond_Always, v2(0, 1));
    ImGuiWindowFlags f = ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##slash", 0, f)) {
        Slice<i32>    picks = slashpicks(&a, text, skills);
        Slice<char *> heads = {};
        f32           w     = ImGui::CalcTextSize("/compact [focus]").x;
        for (iz i = 0; i < picks.len; i++) {
            SlashInfo c = slashpickinfo(&a, picks[i], skills);
            Str       h = c.args.len ? concat(&a, concat(&a, c.name, " "), c.args) : c.name;
            heads = push(&a, heads, cstr0(&a, h));
            f32 hw = ImGui::CalcTextSize(heads[i]).x;
            w = hw>w ? hw : w;
        }
        for (iz i = 0; i < picks.len; i++) {
            SlashInfo c = slashpickinfo(&a, picks[i], skills);
            ImGui::PushID((i32)i);
            ImGui::Selectable(heads[i], i==g->slashsel, 0, v2(w, 0));
            ImGui::PopID();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", cstr0(&a, utf8head(c.desc, 120)));
        }
        ImGui::TextDisabled("Tab or Enter completes; Up and Down pick "
                            "\xc2\xb7 @ mentions a file");
    }
    ImGui::End();
    g->slashshown = 1;
}

// A string cut to fit w pixels, its middle replaced by "..." when it does
// not: a long folder keeps its first and last characters, which are what tell
// one from another, and the caller keeps the rest of the row's columns.
static Str mentionelide(Arena *a, Str s, f32 w)
{
    if (w <= 0 || ImGui::CalcTextSize(s.cdata, s.cdata+s.len).x <= w) {
        return s;
    }
    for (iz keep = s.len; keep > 1; keep--) {
        iz  head = keep/2, tail = keep - head;
        Buf b(a, keep + 4);
        print(&b, span(s.data, s.data+head));
        print(&b, "...");
        print(&b, span(s.data+s.len-tail, s.data+s.len));
        Str t = finish(&b);
        if (ImGui::CalcTextSize(t.cdata, t.cdata+t.len).x <= w) {
            return t;
        }
    }
    return "...";
}

// The '@' picker, above the box while the caret is in an '@' word: the
// candidates the source found, the pick marked, and one line under them
// saying what they are and which keys work. It takes no input, as the slash
// popup does not, so the caret keeps blinking and typing goes on. It draws
// with no rows too, so that the adapter's one line — no files match,
// indexing, a folder it could not list — is not written to nobody.
static void drawmention(Gui *g, Arena a, MentionSrc *src, ImVec2 at, f32 maxw)
{
    ImGui::SetNextWindowPos(at, ImGuiCond_Always, v2(0, 1));
    ImGuiWindowFlags f = ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoDocking;
    i32 rows  = (i32)src->cands.len;
    i32 sel   = g->mentionsel<rows ? g->mentionsel : 0;
    i32 first = sel<MENTION_ROWS ? 0 : sel-MENTION_ROWS+1;  // the window follows it
    if (ImGui::Begin("##mention", 0, f)) {
        f32 cue   = ImGui::CalcTextSize("\xe2\x80\xba ").x;  // "> "
        f32 glyph = ImGui::CalcTextSize(ICON_FILE).x;
        f32 gap   = ImGui::GetStyle().ItemSpacing.x;
        Str tag   = "attached";  // the right column, where a file is in already
        f32 tagw  = ImGui::CalcTextSize(tag.cdata, tag.cdata+tag.len).x;
        // The name column is one width for every row, so the parents line up
        // and the eye can scan them: without it the dim text starts wherever
        // each name ends and the row reads as a sentence
        f32 namew = 0;
        for (i32 i = first; i<rows && i<first+MENTION_ROWS; i++) {
            f32 nw = ImGui::CalcTextSize(cstr0(&a, src->cands.data[i].name)).x;
            namew = nw>namew ? nw : namew;
        }
        f32 lead  = cue + glyph + gap + namew + gap;  // to the parent's column
        f32 w     = 0;
        for (i32 i = first; i<rows && i<first+MENTION_ROWS; i++) {
            MentionCand *c  = &src->cands.data[i];
            f32          rw = lead;
            if (c->dir.len) {
                rw += ImGui::CalcTextSize(cstr0(&a, c->dir)).x;
            }
            if (c->attached) {
                rw += gap + tagw;  // the right column, as it is drawn below
            }
            w = rw>w ? rw : w;
        }
        // A deep parent must not make the popup wider than the box it serves
        if (maxw > 0 && w > maxw) {
            w = maxw;
        }
        ImDrawList *dl = ImGui::GetWindowDrawList();
        f32         lh = ImGui::GetTextLineHeight();
        for (i32 i = first; i<rows && i<first+MENTION_ROWS; i++) {
            MentionCand *c    = &src->cands.data[i];
            b32          s    = i == sel;
            char        *name = cstr0(&a, c->name);
            ImGui::PushID(i);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Selectable("##row", s, 0, v2(w, lh));
            ImGui::PopID();
            // Drawn over the row, so each part keeps its own color: the
            // cue, the kind's glyph, the name, and the folder it is in
            f32 x = p.x;
            if (s) {
                dl->AddText(v2(x, p.y), g->pal->text, "\xe2\x80\xba ");
            }
            x += cue;
            // The kind, so a folder does not look like a file until picked,
            // nor a session like either
            dl->AddText(v2(x, p.y), g->pal->dim,
                        c->sid.len ? ICON_SESSION : c->isdir ? ICON_FOLDER : ICON_FILE);
            x += glyph + gap;
            dl->AddText(v2(x, p.y), g->pal->text, name);
            x = p.x + lead;  // the parent's column, the same on every row
            if (c->dir.len) {
                // What is left after the name column and, when it is drawn,
                // the tag: the parent gives way to both
                f32 room = w - lead - (c->attached ? gap + tagw : 0);
                dl->AddText(v2(x, p.y), g->pal->dim,
                            cstr0(&a, mentionelide(&a, c->dir, room)));
            }
            // The right column: a file already in the draft, at the row's
            // right. A folder has nothing to say there: its token is a
            // pointer the agent lists itself, not a file attached.
            if (c->attached) {
                dl->AddText(v2(p.x + w - tagw, p.y), g->pal->paneldim, tag.cdata,
                            tag.cdata + tag.len);
            }
        }
        // Space commits a folder as a token, since it ends the word; the
        // line says so while a folder is the pick, so Enter is not pressed
        // twice expecting something to happen
        MentionCand *pick = rows ? &src->cands.data[sel] : 0;
        Str keys = pick && pick->isdir
                 ? Str("Enter opens \xc2\xb7 Space keeps this folder \xc2\xb7 Esc close")
                 : Str("Tab or Enter insert \xc2\xb7 Up/Down pick \xc2\xb7 Esc close");
        Str line = src->status.len ? concat(&a, concat(&a, src->status, " \xc2\xb7 "), keys) : keys;
        ImGui::TextDisabled("%s", cstr0(&a, line));
        // What a check reads is what this frame drew, so it is written
        // here, inside the block: a Begin that returned false drew nothing,
        // and the state then stays as drawprompt reset it
        g->mentionshown = 1;
        g->mentionrows  = rows;
        g->mentionkind  = src->kind;
        g->mentiontrunc = src->truncated;
        // The pick's path, kept in the permanent arena so that a check can
        // name it, and whether it was drawn with its attached tag: a re-rank
        // may move the pick, so the path is what a check compares, not the
        // row. Cloned only when the pick changed, since this runs on every
        // frame the popup is up and the arena keeps all it is given; with no
        // row there is no pick to name, and the last one stands (mentionrows
        // is 0, so nothing reads it as this frame's)
        MentionCand *row = rows ? &src->cands.data[sel] : 0;
        if (row) {
            keepset(&g->mentionpath, row->path);
        }
        g->mentiontag = row && row->attached;
        // The popup's one line, as drawn, kept in the permanent arena so that
        // a check can read it, and cloned only when it changes: with no row to
        // list it is all the popup says, so it must outlive the frame that drew
        // it (the keys after it are the popup's own, not the source's)
        keepset(&g->mentionstatus, src->status);
    }
    ImGui::End();
}


// The images attached to the draft: thumbnails above the box, in rows,
// each with an x that removes it, dimmed while they are sent.
static void drawattached(Gui *g, GuiSession *s)
{
    AttBox     *x    = s->att;
    Arena       a    = g->frame;
    f32         sc   = g->scale;
    f32         box  = (f32)ATT_THUMB * sc;
    f32         gap  = 6*sc;
    f32         rad  = 8*sc;
    ImDrawList *dl   = ImGui::GetWindowDrawList();
    ImVec2      p    = ImGui::GetCursorScreenPos();
    f32         maxw = ImGui::GetContentRegionAvail().x;
    f32         cx   = 0;
    f32         cy   = 0;
    b32         busy = x->state != ATT_IDLE;
    i32         drop = -1;
    for (i32 i = 0; i < x->len; i++) {
        AttImage *m = x->imgs + i;
        if (cx>0 && cx+box>maxw) {
            cx  = 0;
            cy += box + gap;
        }
        ImVec2 c0 = v2(p.x+cx, p.y+cy);
        ImVec2 c1 = v2(c0.x+box, c0.y+box);
        cx += box + gap;
        dl->AddRectFilled(c0, c1, g->pal->codebg, 4*sc);

        // The image fit in the box: its own thumbnail, else as uploaded;
        // a text file, its icon and its name, as much as fits
        ImTextureData *td = m->tex ? &m->tex->td : 0;
        if (m->file) {
            f32 fs = ImGui::GetFontSize();
            ImU32 col = busy || m->pathonly ? g->pal->dim : ImGui::GetColorU32(ImGuiCol_Text);
            dl->AddText(ImGui::GetFont(), 1.6f*fs, v2(c0.x + (box - 1.6f*fs)/2, c0.y + 6*sc),
                        col, ICON_FILE);
            dl->PushClipRect(v2(c0.x+3*sc, c0.y), v2(c1.x-3*sc, c1.y), true);
            Str    t  = m->label;
            ImVec2 ts = ImGui::CalcTextSize(t.cdata, t.cdata+t.len);
            f32    tx = ts.x < box-6*sc ? c0.x + (box - ts.x)/2 : c0.x + 3*sc;
            dl->AddText(v2(tx, c1.y - fs - 6*sc), col, t.cdata, t.cdata+t.len);
            dl->PopClipRect();
            g->attready++;
        } else if (!td && m->name.len) {
            ImgSlot *sl = imgslot(g, s, m->name);
            td = sl && sl->state==IMG_READY ? &sl->tex->td : 0;
        }
        if (m->file) {
            // drawn above
        } else if (td) {
            f32 iw = (f32)td->Width;
            f32 ih = (f32)td->Height;
            f32 k  = box/iw < box/ih ? box/iw : box/ih;
            k = k<1 ? k : 1;
            ImVec2 q0 = v2(c0.x + (box - iw*k)/2, c0.y + (box - ih*k)/2);
            dl->AddImage(td->GetTexRef(), q0, v2(q0.x+iw*k, q0.y+ih*k), v2(0, 0), v2(1, 1),
                         busy ? IM_COL32(255, 255, 255, 110) : IM_COL32_WHITE);
            g->attready++;
        } else {
            Str t = imagename(m->mime);
            dl->AddText(v2(c0.x+6*sc, c0.y+4*sc), g->pal->dim, t.cdata, t.cdata+t.len);
        }
        dl->AddRect(c0, c1, g->pal->rule, 4*sc);

        ImGui::PushID(i);
        // Its x, in a circle at the corner, before the rest of the
        // thumbnail claims the pointer
        ImVec2 k = v2(c1.x - rad - 2*sc, c0.y + rad + 2*sc);
        ImGui::SetCursorScreenPos(v2(k.x-rad, k.y-rad));
        ImGui::BeginDisabled(busy);
        b32 go  = ImGui::InvisibleButton("##remove", v2(2*rad, 2*rad));
        b32 hov = ImGui::IsItemHovered();
        ImGui::EndDisabled();
        if (!busy) {
            itemnamed(g, "attach remove");
            itemtip("Remove it");
            f32 d = rad*0.4f;
            dl->AddCircleFilled(k, rad, hov ? g->pal->bad : g->pal->paneldim);
            dl->AddLine(v2(k.x-d, k.y-d), v2(k.x+d, k.y+d), g->pal->codebg, 1.5f*sc);
            dl->AddLine(v2(k.x-d, k.y+d), v2(k.x+d, k.y-d), g->pal->codebg, 1.5f*sc);
        }

        ImGui::SetCursorScreenPos(c0);
        ImGui::InvisibleButton("##att", v2(box, box));
        if (ImGui::IsItemHovered()) {
            Buf b(&a, 128 + m->label.len);
            print(&b, m->label.len ? m->label : Str("Pasted"));
            if (m->file && m->pathonly) {
                // Never read: no size and no lines, only the path it names
                // and why its text did not come
                print(&b, ": its text did not come ");
                if (m->why.len) {
                    print(&b, "\xe2\x80\x94 ");
                    print(&b, m->why);
                    print(&b, " \xe2\x80\x94 ");
                }
                print(&b, "so the agent reads this path itself");
            } else if (m->rpath.len) {
                // On another machine: its host reads it as the message goes
                print(&b, busy ? Str(": being attached on its host")
                               : Str(": its host attaches it to the next message"));
            } else {
                print(&b, ", ");
                if (m->file && m->pdf) {
                    print(&b, "PDF");  // its size, of the PDF or its text
                } else if (m->file) {
                    print(&b, m->lines);
                    print(&b, m->lines==1 ? Str(" line of text") : Str(" lines of text"));
                } else {
                    print(&b, (i64)m->w);
                    putbyte(&b, 'x');
                    print(&b, (i64)m->h);
                    putbyte(&b, ' ');
                    print(&b, imagename(m->mime));
                }
                if (m->data.len || m->bytes) {
                    print(&b, ", ");
                    print(&b, bytesize(&a, m->data.len ? m->data.len : m->bytes));
                }
                print(&b, busy ? Str(": being sent") : Str(": sent with the next message"));
            }
            tipnow("%s", cstr0(&a, finish(&b)));
        }
        drop = go ? i : drop;
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(v2(maxw, cy + box));
    if (busy) {
        ImGui::TextDisabled(x->state==ATT_UPLOADING ? "sending the attachments..." : "sending...");
    }
    g->attshown = x->len;
    if (drop >= 0) {
        attachremove(g, s, drop);
    }
}

// What waits for an answer: the agent's questions, another ask, or
// nothing. A steer waits behind either, read with its outcome.
enum : i32 { WAITS_NONE, WAITS_QUESTION, WAITS_ASK };
static i32 askwaits(View *v)
{
    for (iz i = v->msgs.len-1; v->asks && i >= 0; i--) {
        ViewMsg *m = &v->msgs[i];
        if (m->role==VIEW_ASK && m->tool->answer==VASK_PENDING) {
            return m->tool->kind=="question" ? WAITS_QUESTION : WAITS_ASK;
        }
    }
    return WAITS_NONE;
}

static void drawprompt(Gui *g)
{
    View       *v   = g->view;
    GuiSession *s   = g->sess ? g->sess : &g->nosession;
    b32         can = g->sess && g->sess->view && !g->sess->readonly && !g->quitting &&
                      (hostlive(g, g->sess) || g->enginekind==ENGINE_FAKE);
    b32         editing = g->edsess && g->edsess==s && !g->edplan;
    char       *buf     = draftbuf(g, s);
    Arena       a       = g->frame;
    b32         wasshown = g->mentionshown;  // last frame's: the Esc test's
    b32         wasslash = g->slashshown;    // likewise
    i32         wasrows  = g->mentionrows;   // and its rows, for the callback's
    // Views of last frame's pick and query, which nothing changes before
    // they are last read: the query after its compare, the pick in
    // drawmention, after the fill that looks for it
    Str         waspath  = keepstr(&g->mentionpath);   // its pick, to keep
    Str         wasquery = keepstr(&g->mentionquery);  // and its query
    g->slashshown   = 0;
    g->cmenushown   = 0;
    g->histshown    = 0;
    g->attshown     = 0;
    g->attready     = 0;
    g->mentionshown = 0;
    g->mentionrows  = 0;  // the picker's, as drawmention sets them
    g->mentiontag   = 0;
    g->modelsshown  = 0;
    // mentionpath and mentionquery are not reset here: they are how the
    // next fill re-finds the pick among the rows it makes (below)
    drawstrip(g);
    ImGui::Dummy(v2(0, 2*g->scale));  // its lists apart from the box below

    // A line for the editor at work, else the latest command's outcome,
    // where the transcript is not shown to have its toast (drawtoast)
    b32 toasted = g->toasted;
    g->toasted  = 0;
    if (!toasted) {
        g->editshown = 0;
    }
    if (!toasted && editing) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("editing in %s...", cstr0(&a, g->edname));
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel")) {
            editcancel(g);
            editing = 0;
        }
        itemtip("Stop the editor, leaving the draft as it was");
        itemrect(g, RECT_EDITCANCEL);
        g->editshown = 1;
    } else if (!toasted && s->note.len) {
        ImGui::PushStyleColor(ImGuiCol_Text, s->noteerr ? g->pal->bad : g->pal->paneldim);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(cstr0(&a, s->note));
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    if (g->sess && g->sess->att && g->sess->att->len) {
        drawattached(g, g->sess);
    }

    ImGui::PushID(s);
    ImGuiID id     = ImGui::GetID("##prompt");
    b32     active = ImGui::GetActiveID() == id;
    s->draftid = id;
    ImGui::PopID();

    // A pick queued for a box the editor holds read-only is dropped before
    // the box runs: its offsets are the moment it was accepted, and this
    // frame it could only land in a draft nobody may type in
    if (s->medit.on && editing) {
        s->medit.on = 0;
    }

    // The slash popup while its first word is typed: its pick, which
    // Tab completes, as does Enter on a word typed in part (below)
    // Esc closes it before it reaches the turn, as for the '@' popup
    // below, and it stays closed while its word is there
    Str        text  = draftstr(s);
    Slice<i32> picks = active && !editing ? slashpicks(&a, text, s->skills) : Slice<i32>{};
    b32        esctaken = 0;  // this frame's Esc closed something
    s->slashmute = s->slashmute && picks.len;
    if (picks.len && !s->slashmute && wasslash && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        s->slashmute = 1;
        esctaken     = 1;
        ImGui::SetKeyOwner(ImGuiKey_Escape, ImGui::GetID("##slashkeys"),
                           ImGuiInputFlags_LockThisFrame);
    }
    picks = s->slashmute ? Slice<i32>{} : picks;
    i32        slash = (i32)picks.len;
    i32        pick  = SLASH_NONE;
    if (slash) {
        g->slashsel = g->slashsel<slash ? g->slashsel : 0;
        pick = picks[g->slashsel];
    } else {
        g->slashsel = 0;
    }

    // The '@' picker: the caret's mention as the box left it last frame,
    // and the rows last frame's fill found, which are the count the
    // callback claims Tab and the arrows for. Those offsets are a keystroke
    // old, so they only say which keys the callback claims here; the popup
    // and the accept are worked out again once the box has run (below), and
    // that one fill a frame is all the walk, the ranker and the listing get.
    ImGuiInputTextState *st = ImGui::GetInputTextState(id);
    MentionAt  m0    = {};
    i32        mrows = wasrows;
    if (active && !editing) {
        m0 = mentionat(text, st ? st->GetCursorPos() : 0);
        // The mute is this word's, not this offset's: it is dropped as soon
        // as the caret is in no mention, or in another one, so an '@' the
        // user dismissed can be opened again where it stands
        if (!m0.on || m0.beg != s->mutebeg) {
            s->mutebeg = -1;
        }
        m0.on = m0.on && m0.beg != s->mutebeg;  // dismissed: not opened again
    }
    // Esc closes the '@' popup before it reaches the turn: the key is
    // taken here, ahead of escapecancel's Shortcut, which honours key
    // ownership, as ##pathkeys does in the path field
    if (!esctaken && wasshown && m0.on && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        s->mutebeg = (i32)m0.beg;
        esctaken   = 1;
        ImGui::SetKeyOwner(ImGuiKey_Escape, ImGui::GetID("##mentionkeys"),
                           ImGuiInputFlags_LockThisFrame);
    }
    // Esc while recalling empties the box (composecb). Else, with nothing
    // else to close, it leaves the box, keeping the draft, before the box
    // runs, where ImGui's own Esc would undo the edit. Mid-turn the
    // transcript says that Esc again stops it (drawtoast).
    if (!esctaken && active && !s->hist && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::SetKeyOwner(ImGuiKey_Escape, ImGui::GetID("##promptleave"),
                           ImGuiInputFlags_LockThisFrame);
        ImGui::ClearActiveID();
        active = 0;
        if (g->sess==s && v && v->busy) {
            g->eschint   = s;
            g->eschintat = g->sched.now;
        }
    }
    if (active) {
        g->eschint = 0;  // back in the box: the hint is done
    }

    // Then, with the box left, Esc cancels the turn
    if (!s->hist) {
        escapecancel(g);
    }
    backgroundkey(g);
    // Ctrl+G opens the draft in the editor: Ctrl on every platform, as
    // in terminal tools, which ImGui calls Super on macOS. The active
    // box owns the keys, so they are read as they are, with the focus
    // here (the transcript's find bar takes its own Ctrl+G).
    ImGuiIO &io   = ImGui::GetIO();
    ImGuiKey ctrl = g->mac ? ImGuiMod_Super : ImGuiMod_Ctrl;
    if (!editing && io.KeyMods==ctrl && ImGui::IsKeyPressed(ImGuiKey_G, false) &&
            ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) {
        editstart(g, a, s);
        editing = g->edsess==s && !g->edplan;
    }

    // A context menu pick, once the box is active again: as its chord
    if (g->menuact && active && !GImGui->ActiveIdIsJustActivated) {
        composechord(g, g->menuact);
        g->menuact = 0;
    } else if (g->menuact && ++g->menuwait>30) {
        g->menuact = 0;  // the box never came back
    } else if (g->menuact) {
        scheddirty(&g->sched);
    }

    ComposeCb cb = {};
    cb.s          = s;
    cb.v          = v;
    cb.slashsel   = &g->slashsel;
    cb.slash      = slash;
    cb.mentionsel = &g->mentionsel;
    cb.mention    = mrows;
    cb.edit       = &s->medit;
    cb.cursor     = -1;
    cb.toend      = g->promptend==2;  // activated in this call, as it runs
    st = ImGui::GetInputTextState(id);
    if (st && active) {
        cb.cursor = st->GetCursorPos();
        cb.hadsel = st->HasSelection();
    }

    ImGuiInputTextFlags flags = ImGuiInputTextFlags_WordWrap |
                                ImGuiInputTextFlags_EnterReturnsTrue |
                                ImGuiInputTextFlags_CtrlEnterForNewLine |
                                ImGuiInputTextFlags_CallbackAlways |
                                ImGuiInputTextFlags_CallbackCharFilter;
    if (editing) {
        flags |= ImGuiInputTextFlags_ReadOnly;
    }
    if (slash || mrows) {
        flags |= ImGuiInputTextFlags_AllowTabInput;  // to complete, not move on
    }
    // Paste: an image, or an image file, the clipboard holds is attached,
    // the key kept from the box
    if (active && !editing && g->sess && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V) &&
            attachpaste(g, s)) {
        ImGui::SetKeyOwner(ImGuiKey_V, ImGui::GetID("##attachpaste"), ImGuiInputFlags_LockThisFrame);
        g->promptfocus = 1;
    }

    f32 bw = 1.5f*ImGui::GetFrameHeight();  // its buttons' icons
    ImVec2 size = v2(ImGui::GetContentRegionAvail().x - bw - ImGui::GetStyle().ItemSpacing.x,
                     ImGui::GetContentRegionAvail().y);
    i32 selbefore = g->mentionsel;  // the pick before this frame's keys
    ImGui::PushID(s);
    b32 send = ImGui::InputTextMultiline("##prompt", buf, DRAFT_MAX+1, size, flags, composecb, &cb);
    b32 edited = ImGui::IsItemEdited();
    g->promptend = g->promptend==2 ? 0 : g->promptend;
    ImGui::PopID();
    axkey(g, AXKEY_PROMPT);  // the same to a tool in every session
    itemrect(g, RECT_PROMPT);
    ImVec2 r0 = ImGui::GetItemRectMin();
    ImVec2 r1 = ImGui::GetItemRectMax();
    if (edited && !cb.recalled) {
        // Typed: the draft, kept once typing pauses, out of history. A note
        // marked fixed is what typing must not take away, as a mention's
        // reason for attaching no text is
        s->hist = 0;
        if (!s->notefixed) {
            s->note = {};
        }
        drafttyped(&s->dclock, g->sched.now);
    }
    // Not from under a dialog: the focus waits until it closes
    if (cb.escaped || (g->promptfocus && g->sess==s && !ImGui::GetTopMostPopupModal())) {
        ImGui::ActivateItemByID(id);
        g->promptfocus = 0;
        g->promptend   = g->promptend ? 2 : 0;  // the caret moved next frame
    }
    g->promptactive = ImGui::GetActiveID() == id;
    // A pick the box did not take as it ran is dropped: the active box is
    // what applies it (composecb), so a box that ran without it means the
    // moment it was accepted for has passed — another session's box, or
    // this one after the user moved away — and it must not land later
    if (s->medit.on && !g->promptactive) {
        s->medit.on = 0;
    }
    SlashInfo c = pick ? slashpickinfo(&a, pick, s->skills) : SlashInfo{};
    if (pick && (cb.tab || (send && text!=c.name))) {
        // Completed rather than sent, the box kept active
        draftput(g, s, c.args.len ? concat(&a, c.name, " ") : c.name);
        drafttyped(&s->dclock, g->sched.now);
        send = 0;
    }
    // The '@' picker once the box has run: this frame's offsets, the rows
    // the popup shows, and the pick Tab or a bare Enter accepts. The pick
    // is queued for the callback, which applies it next frame, and never
    // sends: a half-typed path must not start a turn.
    MentionSrc msrc = {};
    MentionAt  m1   = {};
    i32        rows = 0;
    st      = ImGui::GetInputTextState(id);
    Str now = draftstr(s);
    // A bare Enter is the box's send, and it lets go of the box's active id
    // as it returns, so the send itself says the box had the keys
    b32 hadkeys = g->promptactive || send;
    if (hadkeys && !editing) {
        m1 = mentionat(now, st ? st->GetCursorPos() : 0);
        // The mute is this word's, not this offset's: the caret has left a
        // mention, or is in another one, so the '@' it dismissed is
        // forgotten. Cleared before the paste below, so that a paste still
        // mutes the word it landed in
        if (!m1.on || m1.beg != s->mutebeg) {
            s->mutebeg = -1;
        }
        // A paste lands as a burst of bytes: the mention it makes is
        // dismissed as Esc dismisses one, so a pasted @Override does not
        // open the picker and eat the next Enter. It is told by the text
        // just before the caret ending with the clipboard's, and by the
        // draft growing by that text's length in the one frame, which a
        // burst of typing does not do: growth alone would mute the '@' of
        // two keys that landed in one frame on a loaded machine. One byte
        // on the clipboard is left out, since a paste of it is a keystroke
        // to the box, and typing must open the picker. A pick's own growth
        // and a recalled message are not pastes.
        char const *clip   = ImGui::GetClipboardText();
        Str         pasted = clip ? Str{(u8 *)clip, (iz)__builtin_strlen(clip)} : Str{};
        b32         paste;
        if (pasted.len > 1) {
            // A paste puts the clipboard's text in at the caret, so the
            // draft grows by its length in one frame, however few the
            // frames; typing cannot, however many keys land in one
            paste = st && now.len - s->draftslen >= pasted.len &&
                    endswith(takehead(now, st->GetCursorPos()), pasted);
        } else {
            // Nothing on the clipboard worth matching, so growth is all
            // there is to go on: a paste of a burst arrives as one
            paste = now.len - s->draftslen > 1;
        }
        if (!cb.applied && !cb.recalled && paste && m1.on) {
            s->mutebeg = (i32)m1.beg;
        }
        m1.on = m1.on && m1.beg != s->mutebeg && !g->nomentions;
        if (m1.on) {
            mentionfill(&a, g, s, m1.query, &msrc);
            rows = (i32)msrc.cands.len;
            // The pick is pinned by a path, not by a row: a fill that brings
            // a candidate in late re-ranks the rows, and the highlight must
            // not move under the user's hands. So with the query as it was
            // last frame, the row whose path was the pick is the pick again,
            // wherever it now stands; a query that changed starts at the top.
            // A key this frame moved the pick already, and is kept.
            b32 sameq = m1.query.len==wasquery.len && m1.query==wasquery;
            if (g->mentionsel == selbefore) {
                g->mentionsel = 0;
                for (i32 i = 0; sameq && i<rows; i++) {
                    if (msrc.cands.data[i].path == waspath) {
                        g->mentionsel = i;
                        break;
                    }
                }
            }
            g->mentionsel = g->mentionsel<rows ? g->mentionsel : 0;
            if (!sameq) {
                keepset(&g->mentionquery, m1.query);
            }
        } else {
            g->mentionsel = 0;
        }
    }
    s->draftslen = (i32)now.len;
    if (cb.full) {
        setnote(g, s, "the message is full: delete some text, then pick again", 1);
    }
    MentionCand *cand = rows && g->mentionsel<rows ? &msrc.cands.data[g->mentionsel] : 0;
    if (cand && hadkeys && (cb.tab || (send && !io.KeyShift && !io.KeyCtrl))) {
        Str t   = mentiontext(&a, cand->path, cand->isdir);
        Str was = span(now.data+m1.beg, now.data+m1.end);  // to check, as it is now
        if (t.len && !mentionqueue(&s->medit, m1.beg, m1.end, was, t)) {
            setnote(g, s, "that path is too long to be a mention", 1);
        } else if (t.len) {
            ImGui::ActivateItemByID(id);
            // A file's text comes with the token, once; a folder's does
            // not, since the token is a pointer the agent lists itself
            // (mentionlate, as the next frame begins), and a session's id
            // goes with the message (mentionrefs)
            if (!cand->isdir && !cand->sid.len && cand->path.len<=(iz)MENTION_KEEP) {
                keepset(&s->mattach, cand->path);
            }
        } else {
            setnote(g, s, "that path cannot be a mention: it holds a quote or @", 1);
        }
        send = 0;  // a pick never sends, and never makes a new line
    }
    if (slash && ImGui::GetActiveID()==id) {
        drawslash(g, a, draftstr(s), s->skills, v2(r0.x, r0.y));
    }
    // The popup while a mention is on, even with no row to list: its line
    // is then all there is to say (no files match, indexing, a folder it
    // could not list), and the accept above still needs a candidate, so
    // Enter sends as it does with no pick
    if (m1.on && ImGui::GetActiveID()==id) {
        drawmention(g, a, &msrc, v2(r0.x, r0.y), 0.7f*(r1.x - r0.x));
    }

    // The context menu
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::IsMouseHoveringRect(r0, r1) &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        ImGui::OpenPopup("##composemenu");
    }
    if (ImGui::BeginPopup("##composemenu")) {
        g->cmenushown = 1;
        st = ImGui::GetInputTextState(id);
        b32 sel  = st && st->HasSelection();
        b32 undo = st && st->Stb->undostate.undo_point>0;
        b32 redo = st && st->Stb->undostate.redo_point<IMSTB_TEXTEDIT_UNDOSTATECOUNT;
        b32 clip = ImGui::GetClipboardText() && *ImGui::GetClipboardText();
        b32 any  = draftstr(s).len > 0;
        i32 act  = CM_NONE;
        act = ImGui::MenuItem("Undo", cmdkey(g, &a, "Z"), false, undo && !editing) ? CM_UNDO : act;
        itemrect(g, RECT_CMUNDO);
        act = ImGui::MenuItem("Redo", g->mac ? "Cmd+Shift+Z" : "Ctrl+Y", false, redo && !editing) ? CM_REDO : act;
        itemrect(g, RECT_CMREDO);
        ImGui::Separator();
        act = ImGui::MenuItem("Cut", cmdkey(g, &a, "X"), false, sel && !editing) ? CM_CUT : act;
        itemrect(g, RECT_CMCUT);
        act = ImGui::MenuItem("Copy", cmdkey(g, &a, "C"), false, sel) ? CM_COPY : act;
        itemrect(g, RECT_CMCOPY);
        act = ImGui::MenuItem("Paste", cmdkey(g, &a, "V"), false, clip && !editing) ? CM_PASTE : act;
        itemrect(g, RECT_CMPASTE);
        act = ImGui::MenuItem("Select all", cmdkey(g, &a, "A"), false, any) ? CM_SELALL : act;
        itemrect(g, RECT_CMSELALL);
        ImGui::Separator();
        b32 ed = !g->edproc && editorcmd(g, &a).len;
        if (ImGui::MenuItem("Open in editor", "Ctrl+G", false, ed)) {
            editstart(g, a, s);
        }
        itemrect(g, RECT_CMEDITOR);
        if (!editorcmd(g, &a).len) {
            itemtip("Set [ui] editor in the settings, or $VISUAL or $EDITOR");
        }
        g->cmenustate = (undo ? 1 : 0) | (redo ? 2 : 0) | (sel ? 4 : 0) | (clip ? 8 : 0) | (ed ? 16 : 0);
        if (act) {
            g->menuact  = act;
            g->menuwait = 0;
            ImGui::ActivateItemByID(id);
            scheddirty(&g->sched);
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::BeginDisabled(!can || editing);
    send |= ImGui::Button(v->busy ? ICON_REPLY "##send" : ICON_SEND "##send", v2(bw, 0));
    itemnamed(g, "send");
    i32 waits = v->busy ? askwaits(v) : WAITS_NONE;
    itemtip(!v->busy               ? "Send the prompt (Enter; Shift+Enter for a new line)"
            : waits==WAITS_QUESTION ? "Steer: send to the running turn, which the agent reads "
                                      "with your answers to its questions (Enter)"
            : waits==WAITS_ASK      ? "Steer: send to the running turn, which the agent reads "
                                      "once you answer what it asks (Enter)"
                                    : "Steer: send to the running turn, which the agent reads "
                                      "at its next step (Enter)");
    if (v->busy) {
        if (ImGui::Button(ICON_STOP "##stop", v2(bw, 0))) {
            guicancel(g, g->sess);
        }
        itemnamed(g, "stop");
        itemtip("Stop: cancel the turn, killing any command it runs (Esc)");
    }
    ImGui::EndDisabled();
    if (g->sess && hostcap(s->host, "images")) {
        Str why = attachwhy(s);
        why = why.len ? why : attachpending(g, s);
        ImGui::BeginDisabled(why.len>0 || editing);
        if (ImGui::Button(ICON_ATTACH "##attach", v2(bw, 0))) {
            attachpick(g, s);
        }
        ImGui::EndDisabled();
        itemnamed(g, "attach");
        whytip(why.len ? why :
               hostcap(s->host, "files")
               ? Str("Attach a text file or an image (PNG, JPEG, GIF, WebP, BMP) "
                     "to the next message; or type @ in the prompt to mention a "
                     "file (its text is attached too)")
               : Str("Attach an image (PNG, JPEG, GIF, WebP, BMP) to the next message"));
    }
    if (g->sess) {
        // Its folder and its terminal, as the Session menu opens them
        Str wwhy = {};
        Str cwd  = sesswork(g, &a, s, &wwhy);
        ImGui::BeginDisabled(wwhy.len > 0);
        if (ImGui::Button(ICON_FOLDER "##folder", v2(bw, 0))) {
            workopen(g, a, cwd);
        }
        ImGui::EndDisabled();
        itemnamed(g, "prompt folder");
        whytip(wwhy.len ? wwhy : concat(&a, "Open working folder: ", cwd));
        // The Terminal pane, on any host, else why not; only the fake
        // agent, which has no terminals, opens an external one instead
        b32 fake = g->enginekind==ENGINE_FAKE && s->host->kind==GH_ENGINE;
        Str why  = fake ? termplan(g, &a, s).why : termwhy(g, s);
        ImGui::BeginDisabled(why.len > 0);
        if (ImGui::Button(ICON_TERMINAL "##terminal", v2(bw, 0))) {
            if (fake) {
                termopen(g, a, s);
            } else {
                termgo(g);
            }
        }
        ImGui::EndDisabled();
        itemnamed(g, "prompt terminal");
        whytip(why.len ? why :
               fake ? Str("Open in external terminal: its shell in its folder, as the "
                          "fake agent has no terminal tabs")
                    : Str("Terminal: its shell in its folder, in the Terminal pane (Ctrl+`)"));
    }
    if (s->hist) {
        // Which of the session's messages the box holds: oldest is 1
        i32 m = histcount(v);
        Buf b(&a, 32);
        putbyte(&b, '(');
        print(&b, (i64)(m - s->hist + 1));
        putbyte(&b, '/');
        print(&b, (i64)m);
        putbyte(&b, ')');
        ImGui::TextDisabled("%s", cstr0(&a, finish(&b)));
        itemtip("A message sent before, recalled: Up for older, Down for "
                              "newer, Esc for the empty draft, or edit it to send again");
        g->histshown = s->hist;
    }
    ImGui::EndGroup();
    if (send && can && !editing) {
        Str   p  = trim(draftstr(s));
        Slash sl = parseslash(p);
        Slash sk = parseskill(p, s->skills);
        if (sl.cmd) {
            runslash(g, s, sl, p);
        } else if (sk.skill.len && hostcap(s->host, "skills")) {
            draftdone(&s->dclock);
            guiskill(g, s, sk.skill, sk.arg, p);
            draftput(g, s, "");
        } else if (s->att && s->att->state!=ATT_IDLE) {
            setnote(g, s, "wait for the images being sent", 1);
        } else if (s->att && s->att->len) {
            // A token deleted from the box takes its attachment with it, so
            // the message sent and the box cannot disagree
            mentionreconcile(g, s, p);
            draftdone(&s->dclock);
            attachsend(g, s, p);
            draftput(g, s, "");
        } else if (p.len) {
            draftdone(&s->dclock);  // the prompt clears the engine's draft
            guiprompt(g, p);
            draftput(g, s, "");
            s->note      = {};  // sent: no note, fixed or not
            s->notefixed = 0;
        }
        if (!g->pickmodel && !g->pickshell && !g->openexport) {
            // To type the next, unless a popup of another window's
            // opens (/model, /export), which the box would close
            ImGui::ActivateItemByID(id);
        }
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

// Whether a subagent's parent is gone: its record names one on this
// host, but the engine's list, which has every session here, gave it
// none, so it is a root, to delete or archive as one.
static b32 sessorphan(GuiSession *s)
{
    return s->listed && !s->parent.len && s->view && s->view->parent.len &&
           !s->view->remote;
}

static Str sessparent(GuiSession *s)
{
    if (sessorphan(s)) {
        return {};
    }
    return s->view && s->view->parent.len ? s->view->parent : s->parent;
}

// Whether a subagent's parent is on another host: then its mode is its
// own to change, as its host has no parent for it.
static b32 sessremote(GuiSession *s)
{
    return s->view ? s->view->remote : s->remote;
}

// A side chat's original session, else empty.
static Str sessside(GuiSession *s)
{
    return s->view && s->view->sideof.len ? s->view->sideof : s->sideof;
}

// The session another shows under in the sidebar: a subagent's parent,
// a side chat's original while in its group and archived with it, as
// the sidebar would place it anyway (sidebar.cpp). Only there: a side
// chat is a root otherwise.
static Str sessnest(Gui *g, GuiSession *s)
{
    Str p = sessparent(s);
    if (p.len) {
        return p;
    }
    Str         o  = sessside(s);
    GuiSession *up = o.len ? findsess(g, s->host, o) : 0;
    b32         ok = up && up->group==s->group && !up->archived==!s->archived &&
                     !up->trashed==!s->trashed;
    return ok ? o : Str{};
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
// host's newest made first, but each subagent's under its parent's,
// oldest first, with its depth there: the sidebar's order by date
// created.
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

static b32 actafter(GuiSession **a, GuiSession **b)
{
    return (*a)->lastact > (*b)->lastact;
}

// The sessions of one host, the most recently active first, newest made
// first on a tie: where recent folders come from, whatever the sidebar's
// order.
static Slice<GuiSession *> sessrecent(Gui *g, Arena *a, GuiHost *h)
{
    Slice<GuiSession *> r = hostsessions(g, a, h);
    sort(r.data, r.len, actafter, *a);
    return r;
}

// The sessions of one host, in the sidebar's order by date created.
static Slice<GuiSession *> hostsessions(Gui *g, Arena *a, GuiHost *h)
{
    Slice<GuiSession *> all = {};
    for (GuiSession *s = g->sessions; s; s = s->next) {
        if (s->host==h && !s->gone && (s->view || s->listed)) {
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
        Str p = sessnest(g, all[i]);
        if (p.len && upsert(&bysid, p, (Arena *)0)) {
            Slice<GuiSession *> *k = upsert(&t.kids, p, a);
            *k = push(a, *k, all[i]);
        }
    }
    for (iz i = 0; i < all.len; i++) {
        Str p = sessnest(g, all[i]);
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

static b32  cannew(Gui *);
static void newsession(Gui *);

// Remember where something went by name, for the scripted checklist
// (T_NAMED): menu items by their labels, and the sidebar's headings and
// fold arrows by what they are for.
static void namedrect(Gui *g, Str name, ImVec2 a, ImVec2 z)
{
    if (!g->drive) {
        return;
    }
    name = takehead(name, name.len<64 ? name.len : 64);
    Named *n = 0;
    for (i32 i = 0; !n && i < g->nnamed; i++) {
        Named *k = &g->named[i];
        n = Str{k->name, k->len}==name ? k : 0;
    }
    if (!n && g->nnamed<NAMED_MAX) {
        n = &g->named[g->nnamed++];
        copybytes(n->name, name.data, name.len);
        n->len = name.len;
    }
    if (n) {
        n->r = {a.x, a.y, z.x, z.y};
    }
}

// The last item, by name.
static void itemnamed(Gui *g, Str name)
{
    namedrect(g, name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

// A tooltip for the last item, disabled or not: why it is disabled.
static void whytip(Str why)
{
    if (why.len) {
        axtip(why);
    }
    if (why.len && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        tipnow("%.*s", (int)why.len, why.cdata);
    }
}

// A menu item, disabled with a reason unless why is empty, its
// rectangle named by its label for the checklist.
static b32 menuitem(Gui *g, Arena a, Str label, Str why, char const *key = 0)
{
    b32   r = ImGui::MenuItem(cstr0(&a, label), key, false, !why.len);
    itemnamed(g, label);
    whytip(why);
    return r;
}

// One with a tooltip, tip, saying what it does while it can. It takes
// the caller's arena, where its strings may be made in the same call.
static b32 menutip(Gui *g, Arena *a, Str label, Str why, Str tip, char const *key = 0)
{
    b32 r = menuitem(g, *a, label, why, key);
    if (!why.len) {
        itemtip("%s", cstr0(a, tip));
    }
    return r;
}

// A session's host's sidebar key, stable across runs (sidebar.cpp).
static Str hostkey(Arena *a, GuiHost *h, Str remote)
{
    if (!h) {
        return concat(a, "remote:", remote);
    }
    switch (h->kind) {
    case GH_ENGINE: return "this";
    case GH_LOCAL:  return h->own ? Str("own") : concat(a, "bg:", h->name);
    }
    return concat(a, "remote:", h->name);
}

// The configured remotes' names, in the configuration's order, then
// the unconfigured WSL distros'.
static Slice<Str> remotenames(Gui *g, Arena *a)
{
    Slice<Str> names = {};
    for (iz i = 0; i < g->cfgrows.len; i++) {
        CfgRow *r = &g->cfgrows[i];
        b32 seen = r->kind != "remote";
        for (iz n = 0; !seen && n < names.len; n++) {
            seen = names[n] == r->name;
        }
        if (!seen) {
            names = push(a, names, r->name);
        }
    }
    for (iz i = 0; i < g->wslauto.len; i++) {
        b32 seen = 0;
        for (iz n = 0; !seen && n < names.len; n++) {
            seen = names[n] == g->wslauto[i].name;
        }
        if (!seen) {
            names = push(a, names, g->wslauto[i].name);
        }
    }
    return names;
}

// Whether a host is a WSL distro, reached through wsl.exe.
static b32 hostwsl(Gui *g, GuiHost *h)
{
    return h && h->kind==GH_REMOTE && !remoteset(g, h->name, "ssh").len &&
           remoteset(g, h->name, "wsl").len;
}

// How Connect reaches a remote, for its tooltip.
static char const *connecttip(Gui *g, Str name)
{
    Reach l = remotelink(g, name);
    return ctrof(g, name) ? "Connect, starting the container first if it is stopped"
         : l.wsl.len  ? "Connect through wsl.exe, starting a host in the distro if none runs"
         : l.exec.len ? "Connect by its command, starting a host there if none runs"
                      : "Connect over ssh, starting a host there if none runs";
}

// Whether a remote shows in the sidebar while not connected: unless its
// list setting says only while connected.
static b32 remotelisted(Gui *g, GuiHost *h, Str name)
{
    b32 live = h && h->r && !h->closing;
    return live || remoteset(g, h ? h->name : name, "list")!="connected";
}

// Why a session operation cannot be done now, or empty if it can: the
// host's engine names it in its caps (cap), the host takes commands,
// the session is its own here, and, where idle matters, nothing runs.
// Why a session's host can't do what cap names now, else empty. A
// remote not connected says so first: it has no caps to judge until
// its engine says hello.
static Str hostwhy(Gui *g, GuiSession *s, Str cap)
{
    if (s->host->ctr && !s->host->r && (cap=="trash" || cap=="delete") &&
            sesstrashed(g, s)) {
        return {};  // a stopped container's Trash is the window's (ctrtrash)
    } else if (!hostlive(g, s) || (s->host->kind==GH_REMOTE && !s->host->up)) {
        return "Its host is not connected";
    } else if (cap.len && !hostcap(s->host, cap)) {
        return s->host->kind==GH_REMOTE
             ? Str("The engine there is too old for this: update it (Update engine)")
             : Str("This host's engine is too old for this");
    }
    return {};
}

static Str sesswhy(Gui *g, GuiSession *s, Str cap, b32 idle, b32 rootonly)
{
    Str why = hostwhy(g, s, cap);
    if (why.len) {
        return why;
    } else if (s->readonly && s->kept.len) {
        return "Its container is gone: kept to read";
    } else if (s->readonly || (!s->view && s->locked)) {
        return "Another process holds it";
    } else if (rootonly && sessparent(s).len) {
        return "A subagent goes with the session that started it";
    } else if (idle && (s->view ? s->view->busy : s->state!=LIVE_IDLE && s->state!=LIVE_CLOSED)) {
        return "Wait for its turn to end";
    }
    return {};
}

// Whether the session waits for the user to approve its plan: then
// the model may change, though the turn runs.
static b32 planwaits(View *v)
{
    for (iz i = v->msgs.len-1; v->asks && i >= 0; i--) {
        ViewMsg *m = &v->msgs[i];
        if (m->role==VIEW_ASK && m->tool->answer==VASK_PENDING) {
            return m->tool->kind=="plan" || m->tool->kind=="question";
        }
    }
    return 0;
}

// Why the session cannot change to mode ("plan", "bypass" or "default")
// now, or empty: as the engine's mode command has it, at any time, even
// mid-turn, which goes on in the new mode, but never while the agent
// asks about plan mode itself.
static Str modewhy(Gui *g, GuiSession *s, Str mode)
{
    View *v   = s->view;
    Str   cap = mode=="bypass" ? Str("bypass") : Str("plan");
    Str   why = sesswhy(g, s, cap, 0, !sessremote(s));
    if (why.len || !v) {
        return why;
    }
    for (iz i = v->msgs.len-1; v->asks && i >= 0; i--) {
        ViewMsg *m = &v->msgs[i];
        if (m->role==VIEW_ASK && m->tool->answer==VASK_PENDING) {
            Str kind = m->tool->kind;
            if (kind=="plan" || kind=="plan.enter") {
                return "Answer the agent's question about plan mode";
            }
            break;
        }
    }
    return {};
}

static void namedialog(Gui *g, i32 what, GuiSession *s, Str group, Str value)
{
    g->nameask    = what;
    g->namesess   = s;
    g->namegroup  = clone(&g->perm, group);
    iz n = value.len<FIELD_MAX ? value.len : FIELD_MAX-1;
    copybytes(g->namebuf, value.data, n);
    g->namebuf[n] = 0;
    g->opennamer  = 1;
}

// Roots to delete, confirmed first, with the group going with them.
static void askdelete(Gui *g, Slice<GuiSession *> roots, Str group)
{
    g->delroots = {};
    g->delcount = 0;
    for (iz i = 0; i < roots.len; i++) {
        g->delroots  = push(&g->perm, g->delroots, roots[i]);
        g->delcount += sesscount(g, roots[i]);
    }
    g->delgroup    = clone(&g->perm, group);
    g->delempty    = 0;
    g->opendelsess = roots.len || group.len;
}

// Every host's Trash emptied, confirmed first.
static void askempty(Gui *g)
{
    g->delroots    = {};
    g->delgroup    = {};
    g->delcount    = trashcount(g, 0);
    g->delempty    = 1;
    g->opendelsess = g->delcount > 0;
}

// Why the Trash cannot be emptied now, or empty.
static Str emptywhy(Gui *g)
{
    return trashcount(g, 0) ? Str{} : Str("The Trash is empty");
}

// Roots into the Trash, told in one toast. It takes the caller's arena.
static void trashroots(Gui *g, Arena a, Slice<GuiSession *> roots)
{
    for (iz i = 0; i < roots.len; i++) {
        guitrash(g, roots[i], 1);
    }
    trashtold(g, a, roots);
}

// Close a session's view here: its host may let it go, and it goes back
// to being listed.
static void sessclose(Gui *g, GuiSession *s)
{
    if (!s->view) {
        return;
    }
    // Its subagents on other hosts with it, but the one shown
    for (GuiSession *k = g->sessions; k; k = k->next) {
        if (k!=s && k!=g->sess && k->view && k->host->kind==GH_REMOTE &&
                sessremote(k) && sessup(g, k)==s) {
            sessclose(g, k);
        }
    }
    if (hostlive(g, s)) {
        guiclose(g, s);
    }
    imgdrop(g, s);
    View *v = s->view;
    s->view     = 0;
    s->openid   = 0;
    s->readonly = 0;
    s->win      = 0;
    os_release(g->os, v->text.region);
    os_release(g->os, v->meta.region);
    os_release(g->os, v->aux.region);
    if (g->sess == s) {
        setcur(g, 0);
    }
    if (g->exp.s==s && g->exp.state==EXP_OPENING) {
        g->exp.state = EXP_IDLE;
    }
    guilist(g, s->host);
}

// The models a session can switch to, as configured here: the default,
// then each [model NAME] section's.
static Slice<Str> cfgmodels(Gui *g, Arena *a)
{
    Slice<Str> r = {};
    for (iz i = 0; i < g->cfgrows.len; i++) {
        CfgRow *c = &g->cfgrows[i];
        Str     m = !c->kind.len && c->key=="model" ? c->value :
                    c->kind=="model" ? c->name : Str{};
        b32 dup = !m.len;
        for (iz k = 0; !dup && k < r.len; k++) {
            dup = r[k] == m;
        }
        if (!dup) {
            r = push(a, r, m);
        }
    }
    return r;
}

// Rows of a host's model picks (modelpicks), those whose label has the
// filter in it, ignoring case, in a list of their own that scrolls past
// a dozen, a failed endpoint's row disabled, the defaults' marked. Row
// cur is checked, and row mark's place kept as rect. Returns the row
// clicked, else -1.
static i32 pickrows(Gui *g, Arena a, Slice<ModelPick> picks, Str filter, i32 cur,
                    i32 mark, i32 rect)
{
    f32 w = 280*g->scale;
    i32 n = 0;
    for (iz i = 0; i < picks.len; i++) {
        Str label = picks[i].dflt ? concat(&a, picks[i].label, "  (default)") : picks[i].label;
        if (!filter.len || contains(picks[i].label, filter)) {
            f32 lw = ImGui::CalcTextSize(cstr0(&a, label)).x;
            w = lw>w ? lw : w;
            n++;
        }
    }
    if (!n) {
        ImGui::TextDisabled(filter.len ? "None has that: Enter takes it as typed"
                                       : "No models listed");
        return -1;
    }
    ImGuiStyle &st  = ImGui::GetStyle();
    f32         row = ImGui::GetTextLineHeightWithSpacing();
    f32         h   = (f32)(n<12 ? n : 12)*row + 2*st.WindowPadding.y;
    i32         hit = -1;
    f32         cw  = w + 2*st.WindowPadding.x + st.ScrollbarSize;
    f32         av  = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("##picks", v2(cw>av ? cw : av, h));  // a combo's width
    for (iz i = 0; i < picks.len; i++) {
        ModelPick *p = &picks[i];
        if (filter.len && !contains(p->label, filter)) {
            continue;
        }
        Str label = p->dflt ? concat(&a, p->label, "  (default)") : p->label;
        ImGui::PushID((i32)i);
        ImGui::BeginDisabled(p->err.len > 0);
        if (ImGui::Selectable(cstr0(&a, label), i==cur) && !p->err.len) {
            hit = (i32)i;
        }
        ImGui::EndDisabled();
        if (i == cur) {
            ImGui::SetItemDefaultFocus();
        }
        itemnamed(g, p->dflt ? Str("model (default)") : concat(&a, "model ", p->model));
        if (i == mark) {
            itemrect(g, rect);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    return hit;
}

// An endpoint of a host's listing, by name, as its URL; else as given
// (the default endpoint may be a URL).
static Str pickurl(GuiHost *h, Str endpoint)
{
    for (iz i = 0; i < h->eps.len; i++) {
        if (h->eps[i].name == endpoint) {
            return h->eps[i].url;
        }
    }
    return endpoint;
}

// The models to switch to, as the host's endpoints list them (models):
// a field to filter them, or for another model, MODEL or ENDPOINT/MODEL,
// then the list. Each goes to the engine as ENDPOINT/MODEL, which it
// splits.
static void listitems(Gui *g, Arena a, GuiSession *s)
{
    View    *v   = s->view;
    GuiHost *h   = s->host;
    Str      now = v && v->model.len ? v->model : s->model;
    Str      url = v ? v->endpoint : Str{};
    if (ImGui::IsWindowAppearing()) {
        g->picklisted  = 0;  // anew, the last answer shown meanwhile
        g->modelbuf[0] = 0;
    }
    if (!g->picklisted) {
        g->picklisted = hostlist(g, h);  // once it can be asked
    }
    ImGui::SetNextItemWidth(280*g->scale);
    Str filter = {};
    if (ImGui::InputTextWithHint("##other", "filter, or another model, Enter", g->modelbuf,
                                 FIELD_MAX, ImGuiInputTextFlags_EnterReturnsTrue)) {
        Str m = trim({(u8 *)g->modelbuf, (iz)__builtin_strlen(g->modelbuf)});
        if (m.len) {
            s->note = {};
            guimodel(g, s, m);
            g->modelbuf[0] = 0;
            ImGui::CloseCurrentPopup();
        }
    }
    filter = trim({(u8 *)g->modelbuf, (iz)__builtin_strlen(g->modelbuf)});
    if (!h->listgot) {
        ImGui::TextDisabled("Asking the endpoints for their models...");
    }
    Slice<ModelPick> picks = modelpicks(g, &a, h);
    i32 cur = -1, mark = -1;
    for (iz i = 0; i < picks.len; i++) {
        ModelPick *p  = &picks[i];
        b32        is = !p->err.len && p->model==now && (!url.len || pickurl(h, p->endpoint)==url);
        cur  = cur<0 && is && !p->dflt ? (i32)i : cur;
        mark = mark<0 && !is && !p->err.len ? (i32)i : mark;
    }
    i32 hit = pickrows(g, a, picks, filter, cur, mark, RECT_MODEL0);
    if (hit >= 0) {
        ModelPick *p = &picks[hit];
        s->note = {};
        if (pickurl(h, p->endpoint) != p->endpoint) {
            guimodel(g, s, concat(&a, concat(&a, p->endpoint, "/"), p->model));
        } else {
            guimodel(g, s, p->model, p->endpoint);  // the default's, by URL
        }
        g->modelbuf[0] = 0;
        ImGui::CloseCurrentPopup();
    }
}

// The models to switch to, as menu items, then a field for another: a
// host's other than the main one's as its own configuration has them.
// An engine that lists its endpoints' models has those instead.
static void modelpicks(Gui *g, Arena a, GuiSession *s)
{
    if (hostcan(s->host, "models")) {
        listitems(g, a, s);
        return;
    }
    Str        now    = s->view && s->view->model.len ? s->view->model : s->model;
    GuiHost   *h      = s->host;
    b32        theirs = h != g->main;  // its engine's configuration
    Slice<Str> models = theirs ? h->models : cfgmodels(g, &a);
    b32        first  = 1;
    if (theirs) {
        hostmodels(g, h);
        if (!h->modelsgot) {
            ImGui::TextDisabled("Asking the host for its models...");
        } else if (h->modelserr.len) {
            ImGui::TextDisabled("The host's models: %s", cstr0(&a, h->modelserr));
        }
    }
    for (iz i = 0; i < models.len; i++) {
        b32 cur = models[i] == now;
        if (ImGui::MenuItem(cstr0(&a, models[i]), 0, cur, !cur)) {
            s->note = {};
            guimodel(g, s, models[i]);
        }
        itemnamed(g, concat(&a, "model ", models[i]));
        if (!cur && first) {
            itemrect(g, RECT_MODEL0);  // the first to switch to
            first = 0;
        }
    }
    ImGui::SetNextItemWidth(200*g->scale);
    if (ImGui::InputTextWithHint("##other", "another model, Enter", g->modelbuf, FIELD_MAX,
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        Str m = trim({(u8 *)g->modelbuf, (iz)__builtin_strlen(g->modelbuf)});
        if (m.len) {
            s->note = {};
            guimodel(g, s, m);
            g->modelbuf[0] = 0;
            ImGui::CloseCurrentPopup();
        }
    }
}

// Those, then for a session on this computer's endpoints (Settings edits
// their configuration), a thin remote's too, its endpoint in Settings.
static void modelitems(Gui *g, Arena a, GuiSession *s)
{
    modelpicks(g, a, s);
    if (s->host==g->main || s->host->thin) {
        settingsitem(g, a, "Endpoints and models...", "endpoint",
                     s->view ? s->view->endpoint : Str{});
    }
}

// The session operations, as menu items: the sidebar's context menu
// and the Session menu have them alike.
static void sessitems(Gui *g, Arena a, GuiSession *s)
{
    GuiHost *h     = s->host;
    b32      older = 0;  // an item lacks the engine's support

    if (sesstrashed(g, s)) {
        // In the Trash: back, or gone for good
        if (menutip(g, &a, "Restore", sesswhy(g, s, "trash", 0, 1),
                    "Back where it was, with its subagents")) {
            guitrash(g, s, 0);
        }
        if (menutip(g, &a, "Delete Permanently...", sesswhy(g, s, "delete", 1, 0),
                    "Remove it and its subagents for good now, once you confirm")) {
            Slice<GuiSession *> one = {};
            one = push(&a, one, s);
            askdelete(g, one, {});
        }
        ImGui::Separator();
        if (menutip(g, &a, "Copy session ID", {}, concat(&a, "Copy ", s->sid))) {
            ImGui::SetClipboardText(cstr0(&a, s->sid));
        }
        return;
    }

    if (menutip(g, &a, "Rename...", sesswhy(g, s, "rename", 0, 0),
                "Give it a title of your own, in place of its first prompt's")) {
        namedialog(g, NAME_RENAME, s, {}, sesstitle(s));
    }
    older |= !hostcap(h, "rename");

    Str why = sesswhy(g, s, "group", 0, 1);
    older |= !hostcap(h, "group");
    b32 sub = ImGui::BeginMenu("Move to group", !why.len);
    itemnamed(g, "Move to group");
    whytip(why);
    if (sub) {
        for (iz i = 0; i < g->groups.len; i++) {
            Str n = g->groups[i];
            b32 cur = n == s->group;
            ImGui::PushID((i32)i);
            if (ImGui::MenuItem(cstr0(&a, n), 0, cur, !cur)) {
                guigroup(g, s, n);
            }
            itemnamed(g, concat(&a, "group ", n));
            ImGui::PopID();
        }
        if (g->groups.len) {
            ImGui::Separator();
        }
        if (menutip(g, &a, "New group...", {}, "Name a new group and move it there")) {
            namedialog(g, NAME_NEWGROUP, s, {}, {});
        }
        if (menutip(g, &a, "Remove from group", s->group.len ? Str{} : Str("It is in no group"),
                    "Back among the sessions in no group")) {
            guigroup(g, s, {});
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();

    if (menutip(g, &a, "Fork", sesswhy(g, s, "fork", 1, 1),
                "A new session with a copy of this conversation, in Build mode")) {
        guifork(g, s, -1);
    }
    older |= !hostcap(h, "fork");
    if (menutip(g, &a, "Fork into another folder\xe2\x80\xa6", movewhy(g, s),
                "A copy of this conversation that works in another folder, "
                "while this session stays where it is")) {
        movedialog(g, s);
    }
    older |= !hostcap(h, "fork.cwd");
    if (menutip(g, &a, "Start a side chat", sidewhy(g, s),
                "A fork to ask about this session in, even while its agent works")) {
        guiside(g, s, {});
    }
    older |= !hostcap(h, "side");
    if (menutip(g, &a, "Start a worktree session\xe2\x80\xa6", wtwhy(g, s),
                "A fresh session under this one that works in a new worktree, on a "
                "branch of its own, and can report its work back here")) {
        wtdialog(g, s, {});
    }
    older |= !hostcap(h, "worktree");
    why = sesswhy(g, s, "model", 1, 0);
    older |= !hostcap(h, "model");
    sub = ImGui::BeginMenu("Switch model", !why.len);
    itemnamed(g, "Switch model");
    whytip(why);
    if (sub) {
        modelitems(g, a, s);
        ImGui::EndMenu();
    }
    why = sesswhy(g, s, "shell", 1, 0);
    older |= !hostcap(h, "shell");
    why = why.len || (s->view && s->view->shell.len) ? why : Str("Open the session first");
    sub = ImGui::BeginMenu("Switch shell", !why.len);
    itemnamed(g, "Switch shell");
    whytip(why);
    if (sub) {
        shellitems(g, a, s, ImGui::IsWindowAppearing());
        ImGui::EndMenu();
    }
    sub = ImGui::BeginMenu("Export");
    itemnamed(g, "Export");
    if (sub) {
        if (menutip(g, &a, "Markdown...", {},
                    "The conversation as Markdown, to a file or the clipboard")) {
            g->exportsess = s;
            g->openexport = 1;
        }
        Str raw = h->kind!=GH_REMOTE || hostcap(h, "file.base64") ? Str{} :
                  Str("The engine there is too old to send the log");
        raw = raw.len || h->kind!=GH_REMOTE || hostlive(g, s) ? raw : Str("Its host is not connected");
        if (menutip(g, &a, "Raw log...", raw, "The session's log of records, as it is")) {
            guiexport(g, s, EXPORT_RAW, 0);
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();

    b32 arch = s->archived;
    if (menutip(g, &a, arch ? Str("Unarchive") : Str("Archive"), sesswhy(g, s, "archive", 0, 1),
                arch ? Str("Back among the sessions above")
                     : Str("Under Archived, at the end of the sidebar"))) {
        guiarchive(g, s, !arch);
    }
    older |= !hostcap(h, "archive");
    Slice<GuiSession *> one = {};
    one = push(&a, one, s);
    if (trashon(h)) {
        Buf tip(&a, 128);
        print(&tip, "Into the Trash with its subagents, kept ");
        print(&tip, (i64)h->trashdays);
        print(&tip, h->trashdays==1 ? Str(" day") : Str(" days"));
        print(&tip, ", then removed");
        if (menutip(g, &a, "Move to Trash", sesswhy(g, s, "trash", 1, 1), finish(&tip))) {
            trashroots(g, a, one);
        }
        if (menutip(g, &a, "Delete Permanently...", sesswhy(g, s, "delete", 1, 0),
                    "Remove it and its subagents for good now, once you confirm")) {
            askdelete(g, one, {});
        }
    } else if (menutip(g, &a, "Delete...", sesswhy(g, s, "delete", 1, 0),
                       "Remove it and its subagents for good, once you confirm")) {
        askdelete(g, one, {});
    }
    older |= !hostcap(h, "delete");
    ImGui::Separator();
    if (menutip(g, &a, "Copy session ID", {}, concat(&a, "Copy ", s->sid))) {
        ImGui::SetClipboardText(cstr0(&a, s->sid));
    }
    // Its two folders: where it works, and where its records are
    Str there = h->kind==GH_REMOTE ? Str("Its files are on another computer") : Str{};
    Str work  = {};
    Str cwd   = sesswork(g, &a, s, &work);
    if (menutip(g, &a, "Open working folder", work,
                concat(&a, "Open the folder it works in, ", cwd))) {
        workopen(g, a, cwd);
    }
    if (menutip(g, &a, "Open session records", there,
                "Open its folder in the state directory: its log, plan and "
                "command output")) {
        Str dir = sessdir(g, &a, s->sid);
        if (!os_guireveal(g->os, a, dir)) {
            g->lasterr = clone(&g->perm, concat(&a, "cannot show ", dir));
        }
    }
    Str term = termplan(g, &a, s).why;
    if (menutip(g, &a, "Open in external terminal", term,
                h->kind==GH_REMOTE ? Str("A shell in its folder there, in a terminal here")
                                   : Str("Its shell in its folder, in a terminal"))) {
        termopen(g, a, s);
    }
    if (menutip(g, &a, "New terminal tab", termaddwhy(g, s),
                "A shell in its folder, in a tab of the Terminal pane",
                "Ctrl+Shift+`")) {
        termadd(g, s);
    }
    Str close = !s->view ? Str("It is not open here") :
                s->view->busy && !s->readonly ? Str("Close it once its turn ends") : Str{};
    if (menutip(g, &a, "Close", close,
                s->readonly
                    ? Str("Let it go from memory here")
                    : Str("Stop its background jobs and subagents, deny what it asks "
                          "from now on, and let it go from memory here"))) {
        sessclose(g, s);
    }
    if (older && h->kind==GH_REMOTE) {
        ImGui::Separator();
        Str busy = updating(h) ? Str("Updating already") :
                   !h->up      ? Str("Its host is not connected") : Str{};
        if (menuitem(g, a, "Update engine", busy)) {
            guiupdate(g, h);
        }
        itemtip("Deploy this version's engine there, then start it once "
                              "nothing runs");
    }
}

// The roots of a group, on any host.
static Slice<GuiSession *> grouproots(Gui *g, Arena *a, Str group)
{
    Slice<GuiSession *> r = {};
    for (GuiSession *s = g->sessions; s; s = s->next) {
        if (!s->gone && (s->view || s->listed) && !sessparent(s).len &&
                !s->archived && !s->trashed && s->group==group) {
            r = push(a, r, s);
        }
    }
    return r;
}

// Why every root of a group cannot have an operation, or empty.
static Str groupwhy(Gui *g, Slice<GuiSession *> roots, Str cap, b32 idle)
{
    for (iz i = 0; i < roots.len; i++) {
        Str why = sesswhy(g, roots[i], cap, idle, 1);
        if (why.len) {
            return why;
        }
    }
    return {};
}

static void groupmove(Gui *g, Str group, i32 by)
{
    for (iz i = 0; i < g->groups.len; i++) {
        iz k = i + by;
        if (g->groups[i]==group && k>=0 && k<g->groups.len) {
            Str t = g->groups[k];
            g->groups[k] = g->groups[i];
            g->groups[i] = t;
            g->sidedirty = 1;
            return;
        }
    }
}

static void groupdrop(Gui *g, Str group)
{
    iz n = 0;
    for (iz i = 0; i < g->groups.len; i++) {
        if (g->groups[i] != group) {
            g->groups[n++] = g->groups[i];
        }
    }
    g->sidedirty |= n != g->groups.len;
    g->groups.len = n;
}

// A group heading's menu: its sessions on every host.
static void groupitems(Gui *g, Arena a, Str group)
{
    Slice<GuiSession *> roots = grouproots(g, &a, group);
    if (menuitem(g, a, "Rename group...", groupwhy(g, roots, "group", 0))) {
        namedialog(g, NAME_GROUP, 0, group, group);
    }
    Str why = !roots.len ? Str("It has no sessions") : groupwhy(g, roots, "archive", 0);
    if (menuitem(g, a, "Archive group", why)) {
        for (iz i = 0; i < roots.len; i++) {
            guiarchive(g, roots[i], 1);
        }
    }
    if (menuitem(g, a, "Ungroup", groupwhy(g, roots, "group", 0))) {
        for (iz i = 0; i < roots.len; i++) {
            guigroup(g, roots[i], {});
        }
        groupdrop(g, group);
    }
    itemtip("The group goes; its sessions stay");
    ImGui::Separator();
    if (menuitem(g, a, "Move up", g->groups.len && g->groups[0]!=group ? Str{} : Str("First already"))) {
        groupmove(g, group, -1);
    }
    b32 last = !g->groups.len || g->groups[g->groups.len-1]==group;
    if (menuitem(g, a, "Move down", last ? Str("Last already") : Str{})) {
        groupmove(g, group, 1);
    }
    ImGui::Separator();
    b32 trash = roots.len > 0;
    for (iz i = 0; i < roots.len; i++) {
        trash &= trashon(roots[i]->host);
    }
    if (trash) {
        // Into the Trash, host by host, told as one
        if (menuitem(g, a, "Move group to Trash", groupwhy(g, roots, "trash", 1))) {
            for (iz i = 0; i < roots.len; i++) {
                guitrash(g, roots[i], 1);
            }
            trashtold(g, a, roots);
            groupdrop(g, group);
        }
        itemtip("The group goes; its sessions go to the Trash");
    } else if (menuitem(g, a, "Delete group and sessions...", groupwhy(g, roots, "delete", 1))) {
        askdelete(g, roots, group);
    }
}

static Str importwhy(Gui *, GuiHost *);

// A host heading's menu.
static void hostitems(Gui *g, Arena a, GuiHost *h, Str remote)
{
    Str   name = h ? h->name : remote;
    b32   live = h && h->r && !h->closing;
    if (!h || h->kind==GH_REMOTE) {
        if (live) {
            if (menuitem(g, a, "Disconnect", {})) {
                hostleave(g, h);
            }
            itemtip("Leave it: its sessions run on there");
        } else {
            b32 closing = h && h->r && h->closing;
            if (menuitem(g, a, "Connect", closing && h->reconnect ? Str("Connecting once it lets go")
                                                               : Str{})) {
                remoteconnect(g, a, name);
            }
            itemtip("%s", connecttip(g, name));
        }
    }
    GuiCtr *hc = h ? ctrofhost(g, h) : 0;
    b32     cs = hc && !hostready(g, h) && ctrstartable(g, hc);  // started first
    Str why = cs || (h && hostready(g, h)) ? Str{} : Str("It is not connected");
    if (menuitem(g, a, "New session here...", why)) {
        if (cs) {
            ctrnewafter(g, hc);
        } else {
            g->newhost = h;
            newsession(g);
        }
    }
    Str iwhy = why.len ? why : importwhy(g, h);
    if (menuitem(g, a, "Import session here...", iwhy)) {
        importstart(g, h);
    }
    itemtip("A session from its raw log, exported here or elsewhere");
    if (GuiCtr *c = h ? ctrofhost(g, h) : 0) {
        ImGui::Separator();
        Str why = c->job ? Str("Busy") : !c->made ? Str("Not made") :
                  c->state=="gone" ? Str("It is gone") :
                  !live && c->state!="running" ? Str("It is stopped") : Str{};
        if (menuitem(g, a, "Stop container", why)) {
            ctrstop(g, c);
        }
        itemtip("Stop it: its sessions wait in it until it starts again "
                              "(Connect)");
        if (menuitem(g, a, "Delete container...", c->job==CJOB_REMOVE ? Str("Removing") : Str{})) {
            g->delctr     = c;
            g->opendelctr = 1;
        }
        itemtip("Remove it and its sessions for good");
    } else if (!h || h->kind==GH_REMOTE) {
        ImGui::Separator();
        Str why = deploywhy(g, h, name);
        if (h && h->up) {
            if (menuitem(g, a, "Update engine", why)) {
                guiupdate(g, h);
            }
            itemtip("Deploy this version's engine there, then start it "
                                  "once nothing runs");
        } else {
            why = why.len || !h || !h->r ? why : Str("Connecting already");
            if (menuitem(g, a, "Deploy engine", why)) {
                guideploy(g, name);
            }
            itemtip("Copy this version's engine there, as prospero deploy "
                                  "does, then connect to it");
        }
    }
}

// A fold arrow at x on the line at y, pointing right when folded.
// A sidebar row's dot, centered at c in col: a session's or a host's
// state, else dim. A row that folds has no arrow: open, its rail runs
// down from the dot (drawrails); folded, a ring goes around it.
static void folddot(Gui *g, ImDrawList *dl, ImVec2 c, u32 col, b32 folded)
{
    f32 sc = g->scale;
    drawdot(dl, c, 3.5f*sc, col);
    if (folded) {
        dl->AddCircle(c, 6.5f*sc, col, 0, 1.25f*sc);
    }
}

// Where a sidebar row's dot goes, from the row's left and its depth:
// its center's x, and the left of the text after it.
static f32 sidedotx(Gui *g, f32 left, i32 depth)
{
    return left + 8*g->scale + (f32)depth*12*g->scale;
}

// Flip a row's fold, kept with the layout, unless it stays open.
static void foldrow(Gui *g, SideRow *r)
{
    if (r->forced || !r->key.len) {
        return;
    }
    b32 *f = upsert(&g->folds, r->key, (Arena *)0);
    if (!f) {
        f = upsert(&g->folds, clone(&g->perm, r->key), &g->perm);
    }
    *f = !r->folded;
    g->sidedirty = 1;
    scheddirty(&g->sched);
}

// Dragging sessions in the sidebar: a root, with its subagents, onto a
// group's heading, Archived, a host's heading or another session, which
// puts it in that section, or below the rows, out of its group; or onto
// the strip at the foot of the pane, into the Trash. Never to another
// host: a heading or a session there is no target. No order and no
// parent changes: the sections keep the sidebar's order. One in the Trash
// dragged out is restored, into where it is dropped; the Trash heading
// is no target, so close under Archived.
static Str dragwhy(Gui *g, GuiSession *s)
{
    if (s->trashed) {
        return sesswhy(g, s, "trash", 0, 1);
    }
    Str why = sesswhy(g, s, "group", 0, 1);
    return why.len ? why : sesswhy(g, s, "archive", 0, 1);
}

// The session being dragged, else null.
static GuiSession *dragged()
{
    ImGuiPayload const *p = ImGui::GetDragDropPayload();
    return p && p->IsDataType("SESS") ? *(GuiSession **)p->Data : 0;
}

// Whether s dropped into a section (group, or archived) on host h (any
// if null) would change anything there.
static b32 dropok(GuiSession *s, GuiHost *h, Str group, b32 archived)
{
    if (s->gone || (h && h!=s->host)) {
        return 0;
    } else if (s->trashed) {
        return 1;  // restored, wherever it goes
    }
    return archived ? !s->archived : s->archived || !(s->group==group);
}

// The session's row just drawn, as the source of a drag, if it may go
// anywhere; else, dragged, why not.
static void dragsource(Gui *g, Arena a, GuiSession *s)
{
    Str why = dragwhy(g, s);
    if (why.len) {
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            tipnow("%s", cstr0(&a, why));
        }
        return;
    }
    if (!g->dragcancel && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
        if (!g->dragoffset) {
            ImVec2 m      = ImGui::GetIO().MouseClickedPos[0];
            g->dragoff    = v2(m.x - s->row.x0, m.y - s->row.y0);
            g->dragoffset = 1;
        }
        ImGui::SetDragDropPayload("SESS", &s, sizeof(s));
        ImGui::EndDragDropSource();
    }
}

// The item just drawn as a place to drop a session: into group, or
// Archived, on host h (any if null). It takes the session only where
// that changes something; label says where, for the drag's tooltip.
static void droptarget(Gui *g, Arena a, GuiHost *h, Str group, b32 archived, Str label)
{
    GuiSession *s = dragged();
    ImVec2      m = ImGui::GetIO().MousePos;
    ImVec4      t = g->trashrect;
    if (!s || (m.x>=t.x && m.x<t.z && m.y>=t.y && m.y<t.w)) {
        return;  // over the drop strip (dragstrip), drawn over this
    } else if (g->sideview==SIDEVIEW_FOLDERS && !archived) {
        return;  // no groups shown to drop into, nor out of
    } else if (!dropok(s, h, group, archived)) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                h && h!=s->host) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
            fieldput(g->droplabel, concat(&a, "not to ", h->name));
        }
        return;
    }
    if (ImGui::BeginDragDropTarget()) {
        ImGuiPayload const *p = ImGui::AcceptDragDropPayload(
            "SESS", ImGuiDragDropFlags_AcceptBeforeDelivery);
        if (p) {
            fieldput(g->droplabel, concat(&a, "\xe2\x86\x92 ", label));  // a rightwards arrow
        }
        if (p && p->IsDelivery()) {
            if (s->trashed) {
                guitrash(g, s, 0);
            }
            if (!archived && !(s->group==group)) {
                guigroup(g, s, group);
            }
            if (!s->archived != !archived) {
                guiarchive(g, s, archived);
            }
        }
        ImGui::EndDragDropTarget();
    }
}

// A heading: a group, a host, Archived or the Trash. It folds by a
// click, and has a menu by a right click; a host's heading has buttons
// for what is done there most. The Trash, opened, is listed again, for
// what was purged since.
// The container runtime's menu: a new container session, or look for
// the runtime again.
static void rtitems(Gui *g, Arena a)
{
    Str why = g->ctrrt.up ? Str{} : g->ctrrt.why.len ? g->ctrrt.why : Str("Not running");
    if (menuitem(g, a, "New container session...", why)) {
        g->newhost = 0;
        g->newctr  = 1;
        newsession(g);
    }
    if (menuitem(g, a, "Look again", g->ctrprobing ? Str("Looking") : Str{})) {
        ctrlook(g);
    }
    itemtip("Ask whether %s runs now", cstr0(&a, ctrtitle(g->ctrrt.name)));
}

static void drawheading(Gui *g, Arena a, SideRow *r, GuiHost *h, Str remote, i64 nowms,
                        b32 rt = 0)
{
    f32         sc    = g->scale;
    ImDrawList *dl    = ImGui::GetWindowDrawList();
    f32         lineh = ImGui::GetTextLineHeight();
    ImVec2      p     = ImGui::GetCursorScreenPos();
    f32         dx    = sidedotx(g, p.x, r->depth);  // its dot's middle
    f32         w     = ImGui::GetContentRegionAvail().x;
    ImGuiStyle &st    = ImGui::GetStyle();

    // Buttons at the right of a host's heading
    b32         remote1 = r->kind==SROW_HOST && !rt && (!h || h->kind==GH_REMOTE);
    b32         live    = h && h->r && !h->closing;
    char const *cbtn    = live ? ICON_DISCONNECT : ICON_PLUG;  // Disconnect, Connect
    f32         bw      = 0;
    if (r->kind==SROW_HOST || (r->kind==SROW_FOLDER && r->folder.len)) {
        bw += iconw() + st.ItemSpacing.x;
        if (remote1) {
            bw += iconw() + st.ItemSpacing.x;
        }
    }

    pushstr(r->key);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##head", v2(w>1 ? w : 1, lineh + 4*sc));
    b32 click = uiclicked();
    itemnamed(g, concat(&a, "head ", r->key));
    if (r->key.len && !r->forced && ImGui::IsItemHovered() &&
            ImGui::GetIO().MousePos.x < dx + 7*sc) {
        // On its dot, which folds it as its rail does, and lights the rail
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        g->dothot = r->key;
    }
    if (r->forced && ImGui::IsItemHovered()) {
        tipnow("Open while something under it needs an answer");
    } else if (r->kind==SROW_HOST && h && h->msg.len && h->state!=RS_UP) {
        itemtip("%s", cstr0(&a, h->msg));
    }
    if (ImGui::IsItemHovered()) {
        dl->AddRectFilled(p, v2(p.x+w, p.y+lineh+4*sc), fade(g->pal->dim, 0.12f));
    }
    if (click) {
        foldrow(g, r);
        if (r->kind==SROW_TRASH && r->folded) {
            guilistall(g);
        }
    }
    if (r->kind == SROW_DONE) {
        itemtip("Subagents whose tasks are over, kept here out of the way of those "
                "still at work");
    }
    if (r->kind==SROW_FOLDER && !r->forced) {
        itemtip("%s", r->folder.len ? cstr0(&a, homeshow(&a, h, r->folder))
                                    : "Sessions that each work in a folder of their own");
    }
    if (r->kind == SROW_TRASH) {
        itemtip("Deleted sessions, each removed for good after the "
                              "days its host keeps them ([sessions] trash_days)");
    }
    switch (r->kind) {
    case SROW_GROUP:
        droptarget(g, a, 0, r->group, 0, r->group);
        break;
    case SROW_ARCHIVED:
        droptarget(g, a, 0, {}, 1, "Archived");
        break;
    case SROW_HOST:
        if (h) {  // a remote never reached lists nothing to go there
            droptarget(g, a, h, r->group, r->archived,
                       r->archived ? Str("Archived") : r->group.len ? r->group
                                                                   : Str("out of groups"));
        }
        break;
    }
    b32 menu = r->kind!=SROW_ARCHIVED && r->kind!=SROW_DONE && r->kind!=SROW_FOLDER &&
               ImGui::BeginPopupContextItem("##headmenu");
    if (menu) {
        if (r->kind == SROW_GROUP) {
            groupitems(g, a, r->group);
        } else if (r->kind == SROW_TRASH) {
            if (menutip(g, &a, "Empty Trash...", emptywhy(g),
                        "Remove every session in the Trash for good now, once you "
                        "confirm")) {
                askempty(g);
            }
        } else if (rt) {
            rtitems(g, a);
        } else {
            hostitems(g, a, h, remote);
        }
        ImGui::EndPopup();
    }

    ImVec2 dc = v2(dx, p.y + 2*sc + lineh/2);
    Pen    t  = {dl, &g->fonts, dx + 11*sc, p.y + 2*sc, p.x + w - bw, g->fonts.size};
    Buf c(&a, 32);
    if (r->count || r->kind!=SROW_HOST) {
        print(&c, "  ");
        print(&c, (i64)r->count);
    }
    switch (r->kind) {
    case SROW_GROUP:
        folddot(g, dl, dc, g->pal->dim, r->folded);
        pen(&t, r->group, STYLE_BOLD, g->pal->text);
        pen(&t, finish(&c), 0, g->pal->dim);
        break;
    case SROW_ARCHIVED:
        folddot(g, dl, dc, g->pal->dim, r->folded);
        pen(&t, "Archived", STYLE_BOLD, g->pal->dim);
        pen(&t, finish(&c), 0, g->pal->dim);
        break;
    case SROW_TRASH:
        folddot(g, dl, dc, g->pal->dim, r->folded);
        pen(&t, "Trash", STYLE_BOLD, g->pal->dim);
        pen(&t, finish(&c), 0, g->pal->dim);
        break;
    case SROW_FOLDER:
        folddot(g, dl, dc, g->pal->dim, r->folded);
        pen(&t, r->folder.len ? r->label : Str("No folder"), STYLE_BOLD,
            r->folder.len ? g->pal->text : g->pal->dim);
        pen(&t, finish(&c), 0, g->pal->dim);
        break;
    case SROW_DONE:
        folddot(g, dl, dc, g->pal->dim, r->folded);
        c.len = 0;
        print(&c, (i64)r->count);
        print(&c, " finished");
        pen(&t, finish(&c), 0, g->pal->dim);
        break;
    case SROW_HOST: {
        if (rt) {
            u32 dot = g->ctrrt.up ? g->pal->ok : g->pal->dim;
            folddot(g, dl, dc, dot, r->folded);
            pen(&t, ctrtitle(g->ctrrt.name), STYLE_BOLD,
                g->ctrrt.up ? g->pal->text : g->pal->dim);
            pen(&t, "  ", 0, g->pal->dim);
            pen(&t, g->ctrrt.up ? Str("new containers")
                    : !g->ctrprobed ? Str("looking")
                    : g->ctrrt.why.len ? g->ctrrt.why : Str("not running"), 0, g->pal->dim);
            break;
        }
        i64 left = h && h->state==RS_DOWN ? countdown(g, h->retryat, nowms) : 0;
        g->downshown |= h && h->state==RS_DOWN && left>0;
        u32 dot = h ? hostcolor(g, h) : g->pal->dim;
        folddot(g, dl, dc, dot, r->folded);
        pen(&t, h ? hostlabel(g, &a, h) : remote, STYLE_BOLD, g->pal->text);
        pen(&t, "  ", 0, g->pal->dim);
        Str status = h ? hoststatus(g, &a, h, left) : Str("not connected");
        if (h && h->deployfail) {
            status = "deploy failed";  // why, in the deploy toast
            dot    = g->pal->bad;
        } else if (h && h->updating!=UPD_NONE && h->updmsg.len) {
            status = h->updmsg;
            dot    = h->updating==UPD_FAILED ? g->pal->bad : g->pal->amber;
        }
        pen(&t, status, 0, dot==g->pal->ok ? g->pal->dim : dot);
        if (r->folded && r->count) {
            pen(&t, finish(&c), 0, g->pal->dim);
        }
    } break;
    }
    if (r->asks && r->folded) {
        pen(&t, "  needs an answer", STYLE_BOLD, g->pal->amber);
    }

    if (r->kind == SROW_HOST) {
        // New session here, and Connect or Disconnect
        f32 y = p.y + (lineh + 4*sc - ImGui::GetFrameHeight())/2;
        ImGui::SetCursorScreenPos(v2(p.x + w - bw + st.ItemSpacing.x, y));
        GuiCtr *hc = h ? ctrofhost(g, h) : 0;
        b32     cs = hc && !hostready(g, h) && ctrstartable(g, hc);  // started first
        b32     ok = rt ? g->ctrrt.up : cs || (h && hostready(g, h));
        ImGui::BeginDisabled(!ok);
        if (iconbutton(ICON_ADD)) {
            if (cs) {
                ctrnewafter(g, hc);  // New session once it is up
            } else {
                g->newhost = rt ? 0 : h;
                g->newctr  = rt;
                newsession(g);
            }
        }
        ImGui::EndDisabled();
        itemnamed(g, concat(&a, "new ", r->key));
        if (hc) {
            itemnamed(g, "new ctrhost");  // the checklist's, its only one
        }
        if (rt) {
            whytip(ok ? concat(&a, "A new session, in a new container: ", ctrtitle(g->ctrrt.name))
                      : g->ctrrt.why.len ? g->ctrrt.why : Str("Not running"));
        } else {
            whytip(cs ? Str("A new session here, once the container has started")
                      : ok ? Str("A new session here") : Str("Not connected"));
        }
        if (remote1) {
            ImGui::SameLine();
            b32 closing = h && h->r && h->closing;
            ImGui::BeginDisabled(closing && h->reconnect);
            b32 b = iconbutton(cbtn);
            ImGui::EndDisabled();
            itemnamed(g, concat(&a, "connect ", r->key));
            itemtip("%s", live ? "Disconnect: leave it, its sessions running "
                                               "on there"
                                             : connecttip(g, h ? h->name : remote));
            if (b && live) {
                hostleave(g, h);
            } else if (b) {
                remoteconnect(g, a, h ? h->name : remote);
            }
        }
        ImGui::SetCursorScreenPos(v2(p.x, p.y + lineh + 4*sc + st.ItemSpacing.y));
        ImGui::Dummy(v2(0, 0));
    } else if (r->kind==SROW_FOLDER && r->folder.len) {
        // New session in this folder
        f32 y = p.y + (lineh + 4*sc - ImGui::GetFrameHeight())/2;
        ImGui::SetCursorScreenPos(v2(p.x + w - bw + st.ItemSpacing.x, y));
        b32 ok = h && hostready(g, h);
        ImGui::BeginDisabled(!ok);
        if (iconbutton(ICON_ADD)) {
            g->newhost = h;
            g->newctr  = 0;
            fieldput(g->newcwd, homeshow(&a, h, r->folder));
            g->cwdhost = h==g->main ? 0 : h->uid;
            newsession(g);
        }
        ImGui::EndDisabled();
        itemnamed(g, concat(&a, "new ", r->key));
        whytip(ok ? Str("A new session in this folder") : Str("Not connected"));
        ImGui::SetCursorScreenPos(v2(p.x, p.y + lineh + 4*sc + st.ItemSpacing.y));
        ImGui::Dummy(v2(0, 0));
    }
    ImGui::PopID();
}

// A session's row: as before, now indented as its place in the tree
// says, with a fold arrow when it has subagents, its gauge, and its
// context menu.
// How long a root has left in the Trash: whole days, rounded up.
static Str trashleft(Gui *g, Arena *a, GuiSession *root, i64 nowms)
{
    i64 day  = 24*60*60*(i64)1000;
    i64 left = root->trashed + root->host->trashdays*day - nowms;
    if (left <= day) {
        return "removed within a day";
    }
    (void)g;
    Buf b(a, 32);
    print(&b, (left + day - 1)/day);
    print(&b, " days left");
    return finish(&b);
}

// Show a session, as a click on its row does: the Transcript with it,
// the box ready, its caret after the draft. One in the Trash cannot
// open, nor one deleted.
static b32 sessgo(Gui *g, GuiSession *s)
{
    if (s->gone || sessroot(g, s)->trashed) {
        return 0;
    }
    guiselect(g, s);
    setpane(g, PANE_TRANSCRIPT, 1);
    g->promptfocus = 1;
    g->promptend   = 1;
    return 1;
}

// A flat icon button, as a row's action: no fill until the mouse is on
// it, its icon dim.
static b32 rowicon(Gui *g, char const *label, Str why)
{
    Palette *p = g->pal;
    ImGui::PushStyleColor(ImGuiCol_Text, p->paneldim);
    ImGui::PushStyleColor(ImGuiCol_Button, 0);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p->rowpress);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, p->selrow);
    ImGui::BeginDisabled(why.len > 0);
    b32 r = ImGui::Button(label, v2(iconw(), 0));
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);
    return r;
}

// A root's actions, at the right of its row while the pointer is on it,
// over the end of its lines on a backdrop of the row's hover color:
// Archive (or Unarchive) and Move to Trash (or Delete, where its host
// keeps no Trash); in the Trash, Restore and Delete Permanently. Then a
// grip, which says the row drags, and drags it too. Each does what its
// context menu item does, and is left out where the row is too narrow.
// Held, only the grip is there, unseen, as its drag's source.
static void rowactions(Gui *g, Arena a, SideRow *r, GuiSession *s, ImVec2 r0, ImVec2 r1,
                       b32 held)
{
    f32         sc   = g->scale;
    ImDrawList *dl   = ImGui::GetWindowDrawList();
    f32         bw   = iconw();
    f32         bh   = ImGui::GetFrameHeight();
    f32         gap  = 2*sc;
    b32         grip = !dragwhy(g, s).len;
    f32         gw   = grip ? ImGui::CalcTextSize(ICON_GRIPPER).x + 8*sc : 0;
    f32         x1   = r1.x - 2*sc;
    f32         x0   = x1 - (2*bw + gap + gw + 4*sc);
    f32         y    = r0.y + (r1.y - r0.y - bh)/2;
    if (x0 - r0.x < 96*sc) {
        return;
    }
    ImGuiWindow *win  = ImGui::GetCurrentWindow();
    ImVec2       back = win->DC.CursorPos;     // put back as it was, the
    ImVec2       maxp = win->DC.CursorMaxPos;  // actions drawn over the row
    if (!held) {
        f32 fadew = 18*sc;
        u32 c     = g->pal->rowhover;
        u32 c0    = c & ~IM_COL32_A_MASK;
        dl->AddRectFilledMultiColor(v2(x0 - fadew, r0.y), v2(x0, r1.y), c0, c, c, c0);
        dl->AddRectFilled(v2(x0, r0.y), r1, c);

        // Copied, as trashing it closes its view, where the title is
        Str                 title = clone(&a, sesstitle(s));
        Slice<GuiSession *> one   = {};
        one = push(&a, one, s);
        ImGui::SetCursorScreenPos(v2(x0 + 4*sc, y));
        if (r->trashed) {
            Str why = sesswhy(g, s, "trash", 0, 1);
            if (rowicon(g, ICON_DISCARD "##restore", why)) {
                guitrash(g, s, 0);
            }
            itemnamed(g, concat(&a, "restore ", title));
            whytip(why.len ? why : Str("Restore: back where it was, with its subagents"));
            ImGui::SameLine(0, gap);
            why = sesswhy(g, s, "delete", 1, 0);
            if (rowicon(g, ICON_TRASH "##delete", why)) {
                askdelete(g, one, {});
            }
            itemnamed(g, concat(&a, "delete ", title));
            whytip(why.len ? why : Str("Delete Permanently: remove it and its subagents "
                                       "for good now, once you confirm"));
        } else {
            b32 arch = s->archived;
            Str why  = sesswhy(g, s, "archive", 0, 1);
            if (rowicon(g, arch ? ICON_UNARCHIVE "##archive" : ICON_ARCHIVE "##archive", why)) {
                guiarchive(g, s, !arch);
            }
            itemnamed(g, concat(&a, "archive ", title));
            whytip(why.len ? why : arch ? Str("Unarchive: back among the sessions above")
                                        : Str("Archive: under Archived, at the end of "
                                              "the sidebar"));
            ImGui::SameLine(0, gap);
            b32 on = trashon(s->host);
            why = on ? sesswhy(g, s, "trash", 1, 1) : sesswhy(g, s, "delete", 1, 0);
            if (rowicon(g, ICON_TRASH "##trash", why)) {
                if (on) {
                    trashroots(g, a, one);
                } else {
                    askdelete(g, one, {});
                }
            }
            itemnamed(g, concat(&a, "trash ", title));
            if (why.len) {
                whytip(why);
            } else if (on) {
                Buf tip(&a, 128);
                print(&tip, "Move to Trash, with its subagents, kept ");
                print(&tip, (i64)s->host->trashdays);
                print(&tip, s->host->trashdays==1 ? Str(" day") : Str(" days"));
                print(&tip, ", then removed");
                whytip(finish(&tip));
            } else {
                whytip("Delete: remove it and its subagents for good, once you confirm");
            }
        }
    }
    if (grip) {
        f32 gx = x1 - gw;
        ImGui::SetCursorScreenPos(v2(gx, r0.y));
        ImGui::InvisibleButton("##grip", v2(gw, r1.y - r0.y));
        itemnamed(g, concat(&a, "grip ", sesstitle(s)));
        if (ImGui::IsItemActive()) {
            g->gripsess = s;
        } else if (g->gripsess==s && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            g->gripsess = 0;
        }
        b32 hov = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (hov) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
        if (!held) {
            itemtip("Drag it into a group, Archived or the Trash");
            ImVec2 ts = ImGui::CalcTextSize(ICON_GRIPPER);
            dl->AddText(v2(gx + (gw - ts.x)/2, r0.y + (r1.y - r0.y - ts.y)/2),
                        hov ? g->pal->paneltext : g->pal->paneldim, ICON_GRIPPER);
        }
        dragsource(g, a, s);
    }
    win->DC.CursorPos    = back;
    win->DC.CursorMaxPos = maxp;
}

static void drawsessrow(Gui *g, Arena a, SideRow *r, GuiSession *s, b32 *other, i64 nowms)
{
    f32         sc    = g->scale;
    ImDrawList *dl    = ImGui::GetWindowDrawList();
    f32         lineh = ImGui::GetTextLineHeight();
    f32         small = smallsize(&g->fonts);
    View       *v     = s->view;
    i32         state = v ? v->state : s->state;

    pushstr(s->sid);
    ImVec2 p = ImGui::GetCursorScreenPos();
    f32    w = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemAllowOverlap();
    if (ImGui::Selectable("##s", s==g->sess, 0, v2(0, 2*lineh + 4*sc)) && !r->trashed) {
        sessgo(g, s);
    }
    ImVec2 r0 = ImGui::GetItemRectMin();
    ImVec2 r1 = ImGui::GetItemRectMax();
    s->row = {r0.x, r0.y, r1.x, r1.y};
    // A root's actions show while the pointer is on its row; a held
    // grip stays, as the source of its drag
    b32 held = g->gripsess==s && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    b32 acts = held || (!sessparent(s).len && !sessremote(s) && !dragged() &&
                        !g->dragcancel && ImGui::IsMouseHoveringRect(r0, r1) &&
                        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId |
                                                ImGuiPopupFlags_AnyPopupLevel));
    if (acts && !held && !ImGui::IsItemHovered()) {
        dl->AddRectFilled(r0, r1, g->pal->rowhover);  // the pointer on an action
    }
    if (!*other && s!=g->sess) {
        itemrect(g, RECT_OTHERSESS);
        *other = 1;
    }
    Str cwd = sessnofolder(s) ? Str("No folder") : v && v->cwd.len ? v->cwd : s->cwd;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        Buf tip(&a, 256);
        print(&tip, cwd);
        if (s->wtname.len) {
            print(&tip, "\nIn worktree ");
            print(&tip, s->wtname);
            print(&tip, ", on ");
            print(&tip, s->wtbranch);
            print(&tip, s->wtgone ? Str(" (removed: made again as it is opened)") : Str{});
        }
        putbyte(&tip, '\n');
        print(&tip, s->sid);
        if (s->lastact) {
            print(&tip, "\nActive ");
            printage(g, &tip, nowms - s->lastact);
            print(&tip, " ago");
        }
        if (r->trashed) {
            print(&tip, "\nIn the Trash: restore it to open it");
        }
        if (sessremote(s) && !sessup(g, s)) {
            print(&tip, "\nA subagent of a session on another host");
        }
        if (v && worktotal(v)>=1000) {
            print(&tip, "\nWorked ");
            printwork(&tip, worktotal(v));
        }
        if (v && gaugescaled(v)) {
            putbyte(&tip, '\n');
            gaugetip(&tip, v);
        }
        tipnow("%s", cstr0(&a, finish(&tip)));
    }
    if (ImGui::BeginPopupContextItem("##sessmenu")) {
        g->menusess = s;
        sessitems(g, a, s);
        ImGui::EndPopup();
    }
    dragsource(g, a, s);
    droptarget(g, a, s->host, r->group, r->archived,
               r->archived ? Str("Archived") : r->group.len ? r->group : Str("out of groups"));

    f32 dx = sidedotx(g, p.x, r->depth);  // its dot's middle
    if (r->kids) {
        // The dot folds its subagents (and while open, its rail does)
        ImVec2 back = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(v2(dx - 7*sc, p.y));
        ImGui::InvisibleButton("##fold", v2(14*sc, lineh + 4*sc));
        if (uiclicked() && !r->forced) {
            foldrow(g, r);
        }
        if (ImGui::IsItemHovered() && !r->forced) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            g->dothot = r->key;  // its rail lights too (drawrails)
        }
        itemnamed(g, concat(&a, "fold ", sesstitle(s)));
        whytip(r->forced ? Str("Open while a subagent works or asks") :
               r->folded ? Str("Show its subagents") : Str("Hide its subagents"));
        ImGui::SetCursorScreenPos(back);
    }

    // A dot in the color of its state, amber when it, or a subagent
    // below it, waits for the user
    b32 ro    = s->readonly || (!v && s->locked);
    b32 asks  = v && v->asks;
    s->kidask = r->asks && !asks;
    u32 color = ro || asks || s->kidask ? g->pal->amber : livecolor(g->pal, state);
    folddot(g, dl, v2(dx, p.y + 2*sc + lineh/2), color, r->kids && r->folded);
    f32 x = dx + 11*sc;

    Pen t = {dl, &g->fonts, x, p.y + 2*sc, p.x + w, g->fonts.size};
    pen(&t, sesstitle(s), 0, g->pal->text);
    if (r->folded) {
        Buf k(&a, 32);
        print(&k, "  +");
        print(&k, (i64)(r->count - 1));
        pen(&t, finish(&k), 0, g->pal->dim);
    }
    Pen b = {dl, &g->fonts, x, p.y + 2*sc + lineh, p.x + w, small};
    if (sessparent(s).len && v && v->role.len) {
        pen(&b, v->role, 0, g->pal->dim);
        pen(&b, "  ", 0, g->pal->dim);
    } else if (sessremote(s) && !sessup(g, s)) {
        pen(&b, "subagent", STYLE_BOLD, g->pal->dim);
        pen(&b, "  ", 0, g->pal->dim);
    } else if (!sessparent(s).len && sessside(s).len) {
        pen(&b, "side", STYLE_BOLD, g->pal->dim);
        pen(&b, "  ", 0, g->pal->dim);
    }
    if (r->away) {
        pen(&b, concat(&a, "on ", s->host->name), STYLE_BOLD, g->pal->active);
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
    } else if (r->trashed) {
        pen(&b, trashleft(g, &a, sessroot(g, s), nowms), 0, g->pal->dim);
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
    Str where = sesswhere(&a, s);
    if (g->sideview==SIDEVIEW_FOLDERS && !r->archived && !r->trashed) {
        // Under its folder's heading: only where in it, if not its top
        Str folder = sessfolder(s);
        Str cwd    = s->view && s->view->cwd.len ? s->view->cwd : s->cwd;
        if (folder.len && cwd==folder) {
            where = {};
        } else if (folder.len && cwd.len>folder.len && startswith(cwd, folder) &&
                   (cwd[folder.len]=='/' || cwd[folder.len]=='\\')) {
            where = cuthead(cwd, folder.len+1);
        }
    }
    if (where.len) {
        pen(&b, "  ", 0, g->pal->dim);
        pen(&b, where, 0, g->pal->dim);
    }
    // Its context gauge, thin along the row's foot, where a view has it
    if (v && gaugescaled(v) && p.x+w-4*sc-x >= 8) {
        gaugebar(g, dl, v, x, r1.y - 3*sc, p.x + w - 4*sc - x, 2*sc, sc);
    }
    if (acts) {
        rowactions(g, a, r, s, r0, r1, held);
    }
    if (dragged() == s) {
        dl->AddRectFilled(r0, r1, fade(g->pal->panel, 0.6f));  // lifted out (dragcard)
    }
    ImGui::PopID();
}

// The dragged session's card, held under the pointer where its row was
// taken hold of, as if lifted out of the list, with a shadow: its dot,
// its title, and below, where it would go if dropped now. It moves up
// and down only, level with the list, as every place to drop it is in
// the pane; and it lets what is under it show through a little. The row
// it came from stays, faded (drawsessrow). It stops above the strip at
// the pane's foot (dragstrip), so that the pointer on the strip leaves
// the strip in view.
static void dragcard(Gui *g, Arena a, GuiSession *s)
{
    f32         sc    = g->scale;
    ImDrawList *dl    = ImGui::GetForegroundDrawList();
    ImGuiIO    &io    = ImGui::GetIO();
    f32         lineh = ImGui::GetTextLineHeight();
    f32         w     = s->row.x1 - s->row.x0;
    f32         h     = s->row.y1 - s->row.y0;
    if (w<=0 || h<=0) {
        return;
    }
    ImVec2 c0 = v2(s->row.x0, io.MousePos.y - g->dragoff.y);
    ImVec4 st = g->trashrect;
    if (st.w>st.y && c0.y+h > st.y-4*sc) {
        c0.y = st.y - 4*sc - h;
    }
    ImVec2 c1 = v2(c0.x + w, c0.y + h);
    f32    rd = 6*sc;
    for (i32 i = 4; i >= 1; i--) {
        f32 d = (f32)i*2*sc;
        dl->AddRectFilled(v2(c0.x - d/2, c0.y + d/2), v2(c1.x + d/2, c1.y + d),
                          IM_COL32(0, 0, 0, 16), rd + d);
    }
    dl->AddRectFilled(c0, c1, fade(g->pal->rowhover, 0.88f), rd);
    dl->AddRect(c0, c1, fade(g->pal->active, 0.7f), rd, sc);

    View *v     = s->view;
    i32   state = v ? v->state : s->state;
    drawdot(dl, v2(c0.x + 12*sc, c0.y + 2*sc + lineh/2), 3.5f*sc, livecolor(g->pal, state));
    Pen t = {dl, &g->fonts, c0.x + 23*sc, c0.y + 2*sc, c1.x - 8*sc, g->fonts.size};
    pen(&t, sesstitle(s), 0, g->pal->text);
    Str where = {(u8 *)g->droplabel, (iz)__builtin_strlen(g->droplabel)};
    Pen b = {dl, &g->fonts, c0.x + 23*sc, c0.y + 2*sc + lineh, c1.x - 8*sc,
             smallsize(&g->fonts)};
    if (where.len) {
        b32 no   = startswith(where, "not to ");
        b32 gone = startswith(where, "\xe2\x86\x92 Trash") ||  // the strip's
                   startswith(where, "\xe2\x86\x92 delete");    // right part
        pen(&b, where, STYLE_BOLD, no ? g->pal->amber : gone ? g->pal->bad : g->pal->active);
    } else {
        pen(&b, sesswhere(&a, s), 0, g->pal->dim);
    }
}

// While a session is dragged: the space below the rows, out of its
// group; the drag's tooltip; and the pane scrolled near its edges, above
// the trash strip at its foot (foot high). It scrolls once the pointer
// has stayed at an edge a moment, not as it passes on its way to the
// strip.
static void dragrest(Gui *g, Arena a, GuiSession *s, f32 foot)
{
    f32 sc    = g->scale;
    f32 lineh = ImGui::GetTextLineHeight();
    f32 rest  = ImGui::GetContentRegionAvail().y;
    ImGui::InvisibleButton("##dropout", v2(ImGui::GetContentRegionAvail().x,
                                           rest>2*lineh ? rest : 2*lineh));
    itemnamed(g, "sessions empty");
    droptarget(g, a, s->host, {}, 0, "out of groups");


    ImGuiWindow *w    = ImGui::GetCurrentWindow();
    f32          y    = ImGui::GetIO().MousePos.y;
    f32          band = 2*lineh;
    f32          step = 0;
    if (y < w->InnerRect.Min.y+band && ImGui::GetScrollY() > 0) {
        step = -8*sc;
    } else if (y > w->InnerRect.Max.y-foot-band && y < w->InnerRect.Max.y-foot &&
               ImGui::GetScrollY() < ImGui::GetScrollMaxY()) {
        step = 8*sc;
    }
    i64 now   = g->sched.now;
    i64 dwell = 300000;  // µs
    if (step == 0) {
        g->edgeat = 0;
    } else if (!g->edgeat) {
        g->edgeat = now;
        schedat(&g->sched, now + dwell, DUE_APP);
    } else if (now - g->edgeat < dwell) {
        schedat(&g->sched, g->edgeat + dwell, DUE_APP);
    } else {
        ImGui::SetScrollY(ImGui::GetScrollY() + step);
        schedat(&g->sched, now + 16000, DUE_APP);  // on, the mouse still
    }
}

// The trash strip's rect, as x0, y0, x1, y1: pinned at the foot of the
// Sessions pane, whatever its scroll.
static ImVec4 trashstrip(Gui *g)
{
    f32          sc = g->scale;
    ImGuiWindow *w  = ImGui::GetCurrentWindow();
    f32          h  = 2*ImGui::GetTextLineHeight() + 8*sc;
    f32          x0 = w->InnerRect.Min.x + 6*sc;
    f32          x1 = w->InnerRect.Max.x - 6*sc;
    f32          y1 = w->InnerRect.Max.y - 6*sc;
    return {x0, y1-h, x1, y1};
}

// One part of the drop strip, at x0..x1 of its rect t: a place to drop
// the dragged session, named for the checklist by name, unless why says
// why not. Its label shows if it fits, else only its icon; the drag's
// tooltip says where either way. Returns 1 as the session is dropped.
static b32 stripdrop(Gui *g, Arena a, ImVec4 t, f32 x0, f32 x1, char const *icon,
                     Str label, Str to, Str why, u32 color, Str name)
{
    f32         sc = g->scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(v2(x0, t.y));
    ImGui::InvisibleButton(cstr0(&a, concat(&a, "##", name)), v2(x1-x0, t.w-t.y));
    itemnamed(g, name);
    b32 over = 0;
    b32 drop = 0;
    if (!why.len && ImGui::BeginDragDropTarget()) {
        ImGuiPayload const *p = ImGui::AcceptDragDropPayload(
            "SESS", ImGuiDragDropFlags_AcceptBeforeDelivery |
                    ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
        over = p != 0;
        if (p) {
            fieldput(g->droplabel, concat(&a, "\xe2\x86\x92 ", to));  // a rightwards arrow
        }
        drop = p && p->IsDelivery();
        ImGui::EndDragDropTarget();
    } else if (why.len && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
        fieldput(g->droplabel, why);
    }
    u32 fill = fade(color, why.len ? 0.12f : over ? 0.55f : 0.28f);
    dl->AddRectFilled(v2(x0, t.y), v2(x1, t.w), fill, 6*sc);
    dl->AddRect(v2(x0, t.y), v2(x1, t.w), fade(color, why.len ? 0.3f : 0.8f), 6*sc);
    char  *z  = cstr0(&a, concat(&a, concat(&a, Str{(u8 *)icon, (iz)__builtin_strlen(icon)}, "  "), label));
    ImVec2 ts = ImGui::CalcTextSize(z);
    if (ts.x > x1 - x0 - 8*sc) {
        z  = (char *)icon;
        ts = ImGui::CalcTextSize(z);
    }
    dl->AddText(v2(x0 + (x1-x0-ts.x)/2, t.y + (t.w-t.y-ts.y)/2),
                why.len ? g->pal->dim : g->pal->text, z);
    return drop;
}

// While a session is dragged, the strip at the pane's foot, drawn over
// the rows, so that where it can go is always in view, however long the
// list: Archive (or Unarchive), New group, and Move to Trash; or, where
// its host keeps no Trash, Delete, once confirmed. It is opaque, on the
// pane's background, so the rows under it don't muddle its labels; and
// while the list goes on below, a shadow and an arrow above it say that
// holding the drag there scrolls down (dragrest). The pane scrolls as a
// whole, so its content keeps the extent it had without the strip.
static void dragstrip(Gui *g, Arena a, GuiSession *s)
{
    ImVec4 t = g->trashrect;
    if (t.w <= t.y) {
        return;
    }
    f32          sc   = g->scale;
    ImGuiWindow *w    = ImGui::GetCurrentWindow();
    ImVec2       maxp = w->DC.CursorMaxPos;
    f32          gap  = 4*sc;
    f32          part = (t.z - t.x - 2*gap) / 3;
    f32          xa   = t.x + part;
    f32          xb   = xa + gap + part;
    ImDrawList  *dl   = ImGui::GetWindowDrawList();
    f32          top  = t.y - gap;
    if (ImGui::GetScrollY() < ImGui::GetScrollMaxY()) {
        f32 lineh = ImGui::GetTextLineHeight();
        f32 sh    = lineh;
        u32 c0    = IM_COL32(0, 0, 0, 0);
        u32 c1    = IM_COL32(0, 0, 0, g->pal->dark ? 140 : 60);
        dl->AddRectFilledMultiColor(v2(w->InnerRect.Min.x, top - sh),
                                    v2(w->InnerRect.Max.x, top), c0, c0, c1, c1);
        b32    at  = g->edgeat && ImGui::GetIO().MousePos.y > top - 3*lineh;  // held there
        f32    cx  = (t.x + t.z) / 2;
        f32    cy  = top - sh/2;
        f32    hw  = 0.45f*lineh;
        f32    hh  = 0.25f*lineh;
        ImVec2 chev[] = {v2(cx - hw, cy - hh), v2(cx, cy + hh), v2(cx + hw, cy - hh)};
        dl->AddPolyline(chev, 3, at ? g->pal->active : g->pal->text, 2*sc);
    }
    dl->AddRectFilled(v2(w->InnerRect.Min.x, top), w->InnerRect.Max, g->pal->panel);

    b32 arch = s->archived;
    if (stripdrop(g, a, t, t.x, xa, arch ? ICON_UNARCHIVE : ICON_ARCHIVE,
                  arch ? Str("Unarchive") : Str("Archive"),
                  arch ? Str("out of Archived") : Str("Archived"),
                  sesswhy(g, s, "archive", 0, 1), g->pal->active, "sessions archive")) {
        guiarchive(g, s, !arch);
    }
    if (stripdrop(g, a, t, xa + gap, xb, ICON_NEWFOLDER, "New group...",
                  "a new group, once named", sesswhy(g, s, "group", 0, 1),
                  g->pal->active, "sessions newgroup")) {
        namedialog(g, NAME_NEWGROUP, s, {}, {});
    }
    b32 on  = trashon(s->host);
    Str why = on ? sesswhy(g, s, "trash", 1, 1) : sesswhy(g, s, "delete", 1, 0);
    if (stripdrop(g, a, t, xb + gap, t.z, ICON_TRASH,
                  on ? Str("Move to Trash") : Str("Delete..."),
                  on ? Str("Trash") : Str("delete, once you confirm"),
                  why, g->pal->bad, "sessions trash")) {
        Slice<GuiSession *> one = {};
        one = push(&a, one, s);
        if (on) {
            trashroots(g, a, one);
        } else {
            askdelete(g, one, {});
        }
    }
    w->DC.CursorMaxPos = maxp;
}

// Where a sidebar row was drawn, for its rail.
struct RowAt {
    f32 x0, y0, x1, y1;
    f32 ax;  // its dot's middle (sidedotx)
};

// The fold rails: a thin line down from each open row that folds, under
// its arrow and beside all it holds. As in the Transcript, a click
// anywhere along it folds the row, and brings the row back into view if
// it was above. Not while a session is dragged, when the rows are drop
// targets; nor for a row kept open, which says why instead.
static void drawrails(Gui *g, Arena a, Slice<SideRow> rows, Slice<RowAt> at, b32 drag)
{
    f32          sc  = g->scale;
    ImDrawList  *dl  = ImGui::GetWindowDrawList();
    ImGuiWindow *w   = ImGui::GetCurrentWindow();
    f32          top = w->InnerRect.Min.y;
    f32          bot = w->InnerRect.Max.y;
    f32          fs  = ImGui::GetFontSize();
    ImVec2       back = w->DC.CursorPos;     // put back as it was, the rails
    ImVec2       maxp = w->DC.CursorMaxPos;  // drawn over the rows
    for (iz i = 0; i < rows.len; i++) {
        SideRow *r = &rows[i];
        if (!r->key.len || r->folded) {
            continue;
        }
        iz last = i;
        for (iz k = i+1; k<rows.len && rows[k].depth>r->depth; k++) {
            last = k;
        }
        if (last == i) {
            continue;
        }
        f32 x  = at[i].ax;                           // the dot's middle
        f32 ya = at[i].y0 + 2*sc + fs/2 + 6*sc;      // just below the dot
        f32 yb = at[last].y1;
        f32 c0 = ya>top ? ya : top;                  // the dot has a button of its own
        f32 c1 = yb<bot ? yb : bot;
        if (c1 - c0 < 1) {
            continue;
        }
        b32 hov = 0;
        b32 go  = 0;
        if (!drag) {
            pushstr(r->key);
            ImGui::SetCursorScreenPos(v2(x - 6*sc, c0));
            go  = ImGui::InvisibleButton("##rail", v2(12*sc, c1 - c0));
            hov = ImGui::IsItemHovered() || ImGui::IsItemActive() || r->key==g->dothot;
            itemnamed(g, concat(&a, "rail ", r->key));
            ImGui::PopID();
        }
        if (go && !r->forced) {
            if (at[i].y0 < top) {
                ImGui::SetScrollY(ImGui::GetScrollY() - (top - at[i].y0));  // its row at the top
            }
            foldrow(g, r);
        }
        if (hov && r->forced) {
            tipnow("Open while something under it needs an answer");
        } else if (hov) {
            dl->AddRectFilled(v2(x - 8*sc, at[i].y0), v2(at[i].x1, yb),
                              fade(g->pal->active, 0.06f), 4*sc);
            dl->AddRect(v2(x - 8*sc, at[i].y0), v2(at[i].x1, yb),
                        fade(g->pal->active, 0.5f), 4*sc, sc);
            dl->AddRectFilled(v2(x - 1.5f*sc, ya), v2(x + 1.5f*sc, yb), g->pal->active);
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            itemtip("Fold");
            continue;
        }
        dl->AddRectFilled(v2(x - 0.5f*sc, ya), v2(x + 0.5f*sc, yb), fade(g->pal->dim, 0.35f));
    }
    w->DC.CursorPos    = back;
    w->DC.CursorMaxPos = maxp;
}

// The sidebar: a "+" for a new session, then the tree (sidebar.cpp) of
// groups, hosts, sessions and subagents, the archived, and the Trash.
static void drawsessions(Gui *g)
{
    Arena a     = g->frame;
    i64   nowms = os_time(g->os);
    GuiSession *drag = dragged();
    g->droplabel[0] = 0;
    if (g->dragcancel && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        g->dragcancel = 0;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        g->gripsess = 0;
    }
    if (!drag) {
        g->dragoffset = 0;  // taken hold of anew by the next drag
    }
    g->dothot = {};
    if (drag && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::ClearDragDrop();  // nothing is dropped as the button comes up
        g->dragcancel = 1;
        drag = 0;
    }
    if (g->sidedirty) {
        g->sidedirty = 0;
        ImGui::MarkIniSettingsDirty();
    }
    g->trashrect = drag && !drag->trashed ? trashstrip(g) : ImVec4{};
    if (!drag) {
        g->edgeat = 0;
    }
    ImGui::BeginDisabled(!cannew(g));
    if (iconbutton(ICON_ADD)) {
        g->newhost = 0;  // the default host
        newsession(g);
    }
    ImGui::EndDisabled();
    itemnamed(g, "new session");
    whytip(cannew(g) ? Str("A new session, on this computer") : Str{});
    if (g->enginekind != ENGINE_FAKE) {
        ImGui::SameLine();
        if (iconbutton(ICON_REFRESH "##refresh")) {
            guilistall(g);
            findwsl(g);
            ctrlook(g);
        }
        itemnamed(g, "sessions refresh");
        itemtip(HOST_WINDOWS ? "Refresh: list the sessions and the WSL distros again"
                                           : "Refresh: list the sessions again");
    }
    ImGui::SameLine();  // after refresh, or new session without it
    if (iconbutton(ICON_SORT "##sort")) {
        ImGui::OpenPopup("##sortmenu");
    }
    itemnamed(g, "sessions sort");
    itemtip(g->sidesort==SIDESORT_CREATED ? "Sort: date created" : "Sort: recent activity");
    b32 resync = 0;  // the order changed: every row to its place now
    if (ImGui::BeginPopup("##sortmenu")) {
        i32 was = g->sidesort;
        if (ImGui::MenuItem("Recent activity", 0, g->sidesort==SIDESORT_ACTIVITY)) {
            g->sidesort = SIDESORT_ACTIVITY;
        }
        itemnamed(g, "sort activity");
        if (ImGui::MenuItem("Date created", 0, g->sidesort==SIDESORT_CREATED)) {
            g->sidesort = SIDESORT_CREATED;
        }
        itemnamed(g, "sort created");
        if (g->sidesort != was) {
            resync       = 1;
            g->sidedirty = 1;
            scheddirty(&g->sched);
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (iconbutton(ICON_TREE "##view")) {
        ImGui::OpenPopup("##viewmenu");
    }
    itemnamed(g, "sessions view");
    itemtip(g->sideview==SIDEVIEW_FOLDERS ? "View: by folder" : "View: by group");
    if (ImGui::BeginPopup("##viewmenu")) {
        i32 was = g->sideview;
        if (ImGui::MenuItem("Groups", 0, g->sideview==SIDEVIEW_GROUPS)) {
            g->sideview = SIDEVIEW_GROUPS;
        }
        itemnamed(g, "view groups");
        itemtip("Your groups, then the sessions in none");
        if (ImGui::MenuItem("Folders", 0, g->sideview==SIDEVIEW_FOLDERS)) {
            g->sideview = SIDEVIEW_FOLDERS;
        }
        itemnamed(g, "view folders");
        itemtip("A heading for each project folder: the git repository a "
                "session works in, else its folder");
        if (g->sideview != was) {
            resync       = 1;
            g->sidedirty = 1;
            scheddirty(&g->sched);
        }
        ImGui::EndPopup();
    }

    // Rows hold still while the pointer is over the pane, a session is
    // dragged or a menu is open, so that a turn ending elsewhere never
    // moves a row from under a click; but not as the pane shows again,
    // which would keep an order from before
    i32 frame = ImGui::GetFrameCount();
    resync |= g->sideframe != frame-1;
    g->sideframe = frame;
    b32 hold = drag || ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                                              ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
               ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);

    // The hosts to list: every one reached, and every configured remote
    // unless only while connected
    struct HostRef {
        GuiHost *h;
        Str      remote;  // a configured remote never reached
        b32      rt;      // the container runtime's row
    };
    Slice<HostRef>  refs  = {};
    SideIn          in    = {};
    for (GuiHost *h = g->hosts; h; h = h->next) {
        if (h->ctr || h->taskexec.len ||
                (h->kind==GH_REMOTE && !remotelisted(g, h, h->name))) {
            continue;
        }
        refs     = push(&a, refs, HostRef{h, {}, 0});
        in.hosts = push(&a, in.hosts, SideHost{hostkey(&a, h, {}), 1, 0});
    }
    Slice<Str> names = g->enginekind==ENGINE_FAKE ? Slice<Str>{} : remotenames(g, &a);
    for (iz i = 0; i < names.len; i++) {
        if (!findhost(g, GH_REMOTE, names[i]) && remotelisted(g, 0, names[i])) {
            refs     = push(&a, refs, HostRef{0, names[i], 0});
            in.hosts = push(&a, in.hosts, SideHost{hostkey(&a, 0, names[i]), 1, 0});
        }
    }
    // Then Docker or Podman, if installed, and the containers it made
    i32 rt = 0;  // the runtime's row + 1, which the containers go under
    if (ctrlisted(g)) {
        refs     = push(&a, refs, HostRef{0, {}, 1});
        in.hosts = push(&a, in.hosts, SideHost{"ctr", 1, 0});
        rt       = (i32)in.hosts.len;
    }
    for (GuiCtr *c = g->ctrs; c; c = c->next) {
        if (!c->removed && c->h) {
            refs     = push(&a, refs, HostRef{c->h, {}, 0});
            in.hosts = push(&a, in.hosts, SideHost{hostkey(&a, c->h, {}), 1, rt});
        }
    }
    for (GuiHost *h = g->hosts; h; h = h->next) {
        if (h->taskexec.len && h->r && !h->closing) {
            // A subagent's container, while the window is connected to it
            refs     = push(&a, refs, HostRef{h, {}, 0});
            in.hosts = push(&a, in.hosts, SideHost{hostkey(&a, h, {}), 1, rt});
        }
    }
    in.heads   = in.hosts.len > 1;
    in.byat    = g->sidesort == SIDESORT_ACTIVITY;
    in.byfolder = g->sideview == SIDEVIEW_FOLDERS;
    in.groups  = g->groups;
    in.folds   = g->folds;
    in.archive = drag != 0;

    // Its sessions, each host's newest made first, which sidetree orders
    // by sortat with byat
    Slice<GuiSession *> sess = {};
    for (iz i = 0; i < refs.len; i++) {
        Slice<GuiSession *> l = {};
        for (GuiSession *s = g->sessions; refs[i].h && s; s = s->next) {
            if (s->host==refs[i].h && !s->gone && (s->view || s->listed)) {
                l = push(&a, l, s);
            }
        }
        sort(l.data, l.len, sidafter, a);
        for (iz k = 0; k < l.len; k++) {
            GuiSession *s = l[k];
            View       *v = s->view;
            SideSess    e = {};
            e.host     = (i32)i;
            e.sid      = s->sid;
            e.parent   = sessnest(g, s);
            if (sessremote(s)) {
                // Under its parent on this machine's host, if listed
                GuiSession *up = sessup(g, s);
                e.parent = {};
                for (iz n = 0; up && n < refs.len; n++) {
                    if (refs[n].h == up->host) {
                        e.parent = up->sid;
                        e.phost  = (i32)n + 1;
                    }
                }
            }
            e.group    = s->group;
            e.folder   = sessfolder(s);
            e.archived = s->archived;
            e.trashed  = s->trashed != 0;
            e.asks     = v && v->asks && !s->readonly;
            e.active   = v ? v->busy && !s->readonly
                           : s->state!=LIVE_IDLE && s->state!=LIVE_CLOSED;
            e.shown    = s == g->sess;
            if (!hold || resync || !s->sortat) {
                s->sortat = s->lastact;  // a new one takes its place at once
            }
            e.at    = s->sortat;
            in.sess = push(&a, in.sess, e);
            sess    = push(&a, sess, s);
        }
    }

    Slice<SideRow> rows  = sidetree(&a, &in);
    b32            other = 0;  // the checklist's first session not shown
    if (g->drive) {
        // The tree as text, for the checklist: a line a row, indented,
        // G group, H host, S title, A for Archived, T for the Trash, + if
        // folded
        Buf d(&a, 1024);
        for (iz i = 0; i < rows.len; i++) {
            SideRow *r = &rows[i];
            for (i32 k = 0; k < r->depth; k++) {
                putbyte(&d, ' ');
            }
            switch (r->kind) {
            case SROW_GROUP:   print(&d, "G "); print(&d, r->group); break;
            case SROW_HOST:    print(&d, "H "); print(&d, in.hosts[r->host].key); break;
            case SROW_SESSION: print(&d, "S "); print(&d, sesstitle(sess[r->sess])); break;
            case SROW_ARCHIVED: print(&d, "A"); break;
            case SROW_TRASH:    print(&d, "T"); break;
            case SROW_DONE:     print(&d, "D "); print(&d, (i64)r->count); break;
            case SROW_FOLDER:   print(&d, "F "); print(&d, r->folder.len ? r->label
                                                                         : Str("-")); break;
            }
            print(&d, r->away ? Str("@") : Str(""));
            print(&d, r->folded ? Str("+\n") : Str("\n"));
        }
        Str t = finish(&d);
        g->sidelen = t.len<(iz)SIDEDUMP_MAX ? t.len : (iz)SIDEDUMP_MAX;
        copybytes(g->sidedump, t.data, g->sidelen);
    }
    Slice<RowAt> at = {};
    at.data = alloc<RowAt>(&a, rows.len);
    at.len  = rows.len;
    for (iz i = 0; i < rows.len; i++) {
        SideRow *r = &rows[i];
        if (r->kind!=SROW_SESSION && i && r->depth==0) {
            ImGui::Spacing();
        }
        ImVec2 c = ImGui::GetCursorScreenPos();
        at[i].x0 = c.x;
        at[i].y0 = c.y;
        at[i].x1 = c.x + ImGui::GetContentRegionAvail().x;
        at[i].ax = sidedotx(g, c.x, r->depth);
        if (r->kind == SROW_SESSION) {
            drawsessrow(g, a, r, sess[r->sess], &other, nowms);
        } else {
            b32     on  = r->kind==SROW_HOST || r->kind==SROW_FOLDER;
            HostRef ref = on ? refs[r->host] : HostRef{};
            drawheading(g, a, r, ref.h, ref.remote, nowms, ref.rt);
        }
        at[i].y1 = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y;
    }
    drawrails(g, a, rows, at, drag != 0);
    if (!sess.len) {
        ImGui::TextDisabled("No sessions yet.");
    }
    if (drag) {
        ImVec4 t = g->trashrect;
        dragrest(g, a, drag, t.w>t.y ? ImGui::GetCurrentWindow()->InnerRect.Max.y - t.y : 0);
        dragstrip(g, a, drag);
        dragcard(g, a, drag);  // after the strip, which says where it goes
    }
}

static void drawpathask(Gui *, PathAsk *, char const *, char const *, b32);
static GuiHost *filehost(Gui *, u64);
static GuiHost *fileaskhost(Gui *);
static void fetchtick(Gui *, FileTab *);
static Str  filehostname(Gui *, Arena *, u64);
static void fileaskcheck(Gui *);
static Str  fileaskok(Gui *, Str);

// The naming dialog (a session's title, a new group for it, a group's
// new name), the deletion's confirmation, the Markdown export's
// options, and the path typed where no file dialog can be shown: the
// sidebar's and the Session menu's dialogs.
static void drawsessdialogs(Gui *g)
{
    Arena a  = g->frame;
    f32   sc = g->scale;

    static char const *titles[] = {"Rename session", "New group", "Rename group"};
    if (g->opennamer) {
        g->opennamer = 0;
        ImGui::OpenPopup("##namer");
    }
    ImGui::SetNextWindowSize(v2(420*sc, 0));
    if (ImGui::BeginPopupModal("##namer", 0, ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(titles[g->nameask]);
        if (g->nameask == NAME_NEWGROUP && g->namesess) {
            ImGui::TextDisabled("For %s", cstr0(&a, shorten(&a, sesstitle(g->namesess), 60)));
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        b32 enter = ImGui::InputText("##name", g->namebuf, FIELD_MAX,
                                     ImGuiInputTextFlags_EnterReturnsTrue |
                                     ImGuiInputTextFlags_AutoSelectAll);
        itemnamed(g, "name field");
        Str name = trim({(u8 *)g->namebuf, (iz)__builtin_strlen(g->namebuf)});
        ImGui::BeginDisabled(!name.len);
        b32 ok = ImGui::Button("OK", v2(120*sc, 0));
        ImGui::EndDisabled();
        itemnamed(g, "name ok");
        ImGui::SameLine();
        b32 cancel = ImGui::Button("Cancel", v2(120*sc, 0)) || modalkey(ImGuiKey_Escape);
        if ((ok || enter) && name.len) {
            GuiSession *s = g->namesess;
            switch (g->nameask) {
            case NAME_RENAME:
                if (s && !s->gone) guirename(g, s, name);
                break;
            case NAME_NEWGROUP:
                if (s && !s->gone) guigroup(g, s, name);
                break;
            case NAME_GROUP: {
                Slice<GuiSession *> roots = grouproots(g, &a, g->namegroup);
                for (iz i = 0; i < roots.len; i++) {
                    guigroup(g, roots[i], name);
                }
                // In the old one's place in the order
                b32 dup = 0;
                for (iz i = 0; i < g->groups.len; i++) {
                    dup |= g->groups[i] == name;
                }
                for (iz i = 0; i < g->groups.len; i++) {
                    if (g->groups[i] == g->namegroup) {
                        g->groups[i] = clone(&g->perm, name);
                        break;
                    }
                }
                if (dup) {
                    groupdrop(g, g->namegroup);
                }
                g->sidedirty = 1;
            } break;
            }
            ImGui::CloseCurrentPopup();
        } else if (cancel) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (g->opendelctr) {
        g->opendelctr = 0;
        ImGui::OpenPopup("Delete container?");
    }
    ImGui::SetNextWindowSize(v2(460*sc, 0));
    if (ImGui::BeginPopupModal("Delete container?", 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        GuiCtr *c = g->delctr;
        if (!c || c->removed) {
            ImGui::CloseCurrentPopup();
        } else {
            i32 n = 0;
            for (GuiSession *s = g->sessions; s; s = s->next) {
                n += s->host==c->h && !s->gone && s->listed && !s->parent.len;
            }
            Buf b(&a, 256);
            print(&b, "Remove the container ");
            print(&b, c->c.name);
            print(&b, " (");
            print(&b, c->c.image);
            print(&b, ") and its ");
            print(&b, (i64)n);
            print(&b, n==1 ? Str(" session") : Str(" sessions"));
            print(&b, " for good?");
            if (c->c.workspace == CTR_MOUNT) {
                print(&b, " The folder it mounts stays.");
            }
            ImGui::PushTextWrapPos(440*sc);
            ImGui::TextUnformatted(cstr0(&a, finish(&b)));
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (ImGui::Button("Delete", v2(120*sc, 0))) {
                ctrdelete(g, c);
                ImGui::CloseCurrentPopup();
            }
            itemnamed(g, "delctr ok");
            ImGui::SameLine();
            if (ImGui::Button("Cancel", v2(120*sc, 0)) || modalkey(ImGuiKey_Escape)) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    if (g->opendelsess) {
        g->opendelsess = 0;
        ImGui::OpenPopup("###delsess");
    }
    ImGui::SetNextWindowSize(v2(460*sc, 0));
    char const *deltitle = g->delempty ? "Empty the Trash?###delsess"
                                       : "Delete permanently?###delsess";
    if (ImGui::BeginPopupModal(deltitle, 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        Buf b(&a, 512);
        if (g->delempty) {
            print(&b, "Empty the Trash: ");
            print(&b, (i64)g->delcount);
            print(&b, g->delcount==1 ? Str(" session") : Str(" sessions"));
        } else if (g->delgroup.len) {
            print(&b, "Delete the group ");
            print(&b, g->delgroup);
            print(&b, " and its ");
        } else {
            print(&b, "Delete ");
            print(&b, g->delroots.len ? shorten(&a, sesstitle(g->delroots[0]), 60) : Str(""));
            print(&b, g->delcount>1 ? Str(" with its subagents: ") : Str(""));
        }
        if (g->delgroup.len || g->delcount>1) {
            print(&b, (i64)g->delcount);
            print(&b, g->delcount==1 ? Str(" session") : Str(" sessions"));
            if (g->delgroup.len && g->delcount>(i32)g->delroots.len) {
                print(&b, ", subagents included");
            }
        }
        print(&b, "?");
        ImGui::PushTextWrapPos(440*sc);
        ImGui::TextUnformatted(cstr0(&a, finish(&b)));
        ImGui::TextDisabled("Their logs and files are removed for good: this cannot "
                            "be undone.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, fade(g->pal->bad, 0.6f));
        b32 del = ImGui::Button("Delete", v2(120*sc, 0));
        ImGui::PopStyleColor();
        itemnamed(g, "delete ok");
        ImGui::SameLine();
        b32 cancel = ImGui::Button("Cancel", v2(120*sc, 0)) || ImGui::Shortcut(ImGuiKey_Escape);
        itemnamed(g, "delete cancel");
        if (del && g->delempty) {
            for (GuiHost *h = g->hosts; h; h = h->next) {
                if (trashcount(g, h)) {
                    guitrashempty(g, h);
                }
            }
            g->delempty = 0;
            ImGui::CloseCurrentPopup();
        } else if (del) {
            for (iz i = 0; i < g->delroots.len; i++) {
                GuiSession *s = g->delroots[i];
                if (s->gone) continue;
                // Not held open by this client: its views go first
                for (GuiSession *k = g->sessions; k; k = k->next) {
                    if (k->view && sessroot(g, k)==s) {
                        sessclose(g, k);
                    }
                }
                guidelete(g, s);
            }
            if (g->delgroup.len) {
                groupdrop(g, g->delgroup);
            }
            g->delroots = {};
            ImGui::CloseCurrentPopup();
        } else if (cancel) {
            g->delroots = {};
            g->delempty = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (g->openexport) {
        g->openexport = 0;
        ImGui::OpenPopup("Export as Markdown");
    }
    if (ImGui::BeginPopupModal("Export as Markdown", 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        GuiSession *s     = g->exportsess;
        Export     *x     = &g->exp;
        b32         enter = modalenter();
        if (s) {
            ImGui::TextDisabled("%s", cstr0(&a, shorten(&a, sesstitle(s), 60)));
        }
        checkbox("Reasoning", &x->think);
        itemnamed(g, "export think");
        checkbox("Tool calls", &x->tools);
        itemnamed(g, "export tools");
        ImGui::BeginDisabled(!x->tools);
        ImGui::Indent();
        checkbox("Their output", &x->output);
        itemnamed(g, "export output");
        ImGui::Unindent();
        ImGui::EndDisabled();
        ImGui::Spacing();
        ImGui::BeginDisabled(!s || s->gone);
        b32 save = ImGui::Button("Save...", v2(120*sc, 0)) || (enter && s && !s->gone);
        itemnamed(g, "export save");
        ImGui::SameLine();
        b32 copy = ImGui::Button("Copy", v2(120*sc, 0));
        itemnamed(g, "export copy");
        ImGui::EndDisabled();
        ImGui::SameLine();
        b32 cancel = ImGui::Button("Cancel", v2(120*sc, 0)) || ImGui::Shortcut(ImGuiKey_Escape);
        if ((save || copy) && s && !s->gone) {
            guiexport(g, s, EXPORT_MD, copy);
            ImGui::CloseCurrentPopup();
        } else if (cancel) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    drawpathask(g, &g->pask, "pathask", "pathask", 0);
    drawpathask(g, &g->fask.p, "openfile", "openfile", 1);
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

static b32 samelineif(f32);

// The session's plan file, as plan mode writes it, fetched as it
// changes (guiplan), shown as a document (drawdoc): its text selectable,
// copied as text or as Markdown. While the file is there, it opens in
// the editor, as the waiting plan's Edit plan, else edited by hand with
// the agent not told, and in the system's file manager, both only on
// this computer.
static void drawplan(Gui *g)
{
    GuiSession *s = g->sess;
    View       *v = g->view;
    PlanFile   *f = &g->plan;
    g->planshown = 0;
    guiplan(g);
    b32 mine = s && f->s==s && f->v==v;
    if (mine && f->have && f->docgot!=f->got) {
        if (!f->doc) {
            f->doc = newdocview(&g->perm, g->os, g->vcache);
        }
        docset(f->doc, finish(&f->text));
        f->docgot = f->got;
        f->docv   = v;
    }
    if (!s || !v->planpath.len || (mine && !f->id && !f->text.len)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", mine && f->err.len ? cstr0(&g->frame, f->err) :
                            "No plan. In plan mode the agent writes one here "
                            "for you to approve before it changes anything.");
        ImGui::PopTextWrapPos();
        return;
    }
    // While the plan is there: the editor, the file manager, then how
    // plan mode is, on the same line if it fits
    Arena a     = g->frame;
    b32   shown = f->doc && f->docv==v;
    if (shown) {
        i32       k = planask(v);
        ViewTool *t = k>=0 ? v->msgs[k].tool : 0;
        if (t) {
            Str path = t->path.len ? t->path : v->planpath;
            planeditbutton(g, s, path, t->req, "Edit plan", "plan editor", planedittip);
        } else {
            planeditbutton(g, s, v->planpath, {}, "View in editor", "plan editor",
                           "Open the plan file in your editor: it shows here as the "
                           "editor leaves it, and the agent is not told");
        }
        ImGui::SameLine();
        Str why = s->host->kind==GH_REMOTE
                ? concat(&a, "The plan is on ", concat(&a, s->host->name,
                         ": only a file on this computer shows in its file manager"))
                : Str{};
        ImGui::BeginDisabled(why.len != 0);
        if (iconbutton(ICON_FOLDER "##planreveal")) {
            guishow(g, v->planpath);
        }
        ImGui::EndDisabled();
        itemnamed(g, "plan reveal");
        #if defined(__APPLE__)
        whytip(why.len ? why : Str("Reveal: show the plan file in Finder"));
        #elif defined(_WIN32)
        whytip(why.len ? why : Str("Reveal: show the plan file in File Explorer"));
        #else
        whytip(why.len ? why : Str("Reveal: show the plan file in the file manager"));
        #endif
    }
    char const *mode = v->plan ? "Plan mode: changing nothing until you approve"
                               : "Plan mode is off";
    if (shown) {
        samelineif(ImGui::CalcTextSize(mode).x);
        ImGui::AlignTextToFramePadding();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, v->plan ? g->pal->amber
                                                 : ImGui::GetColorU32(ImGuiCol_TextDisabled));
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(mode);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    itemtip("%s", cstr0(&a, v->planpath));
    if (!shown) {
        return;  // on its way
    }
    f->doc->from = filelinkfrom(s, termcwd(s));
    Str link = drawdoc(g, f->doc, "##plan");
    if (link.len) {
        followlink(g, link, &f->doc->from);
    }
    itemnamed(g, "plan text");
    g->planshown = (i32)f->doc->v->msgs[0].text.len;
}

// A changed file's status as one letter: U for a conflict, else its
// status in the index, else in the worktree.
static u8 changeletter(ChangeRow *r)
{
    b32 conflict = r->x=='U' || r->y=='U' || (r->x=='A' && r->y=='A') ||
                   (r->x=='D' && r->y=='D');
    return conflict ? 'U' : r->x!='.' ? r->x : r->y;
}

static Str changeword(u8 c)
{
    switch (c) {
    case 'M': return "modified";
    case 'T': return "type changed";
    case 'A': return "added";
    case 'D': return "deleted";
    case 'R': return "renamed";
    case 'C': return "copied";
    }
    return "changed";
}

// What a changed file's row says on hover: its path whole, where it
// came from, and how it stands in the index and in the worktree.
static Str changetip(Arena *a, ChangeRow *r)
{
    static char const *conflicts[][2] = {
        {"DD", "both deleted"},  {"AU", "added by us"},    {"UD", "deleted by them"},
        {"UA", "added by them"}, {"DU", "deleted by us"},  {"AA", "both added"},
        {"UU", "both modified"},
    };
    Buf b(a, 128 + r->path.len + r->orig.len);
    print(&b, r->path);
    if (r->orig.len) {
        print(&b, "\nfrom ");
        print(&b, r->orig);
    }
    if (changeletter(r) == 'U') {
        Str how = "in conflict";
        for (i32 i = 0; i < countof(conflicts); i++) {
            if (r->x==conflicts[i][0][0] && r->y==conflicts[i][0][1]) {
                how = Str{(u8 *)conflicts[i][1], (iz)__builtin_strlen(conflicts[i][1])};
            }
        }
        print(&b, "\nUnmerged: ");
        print(&b, how);
        return finish(&b);
    } else if (r->x == '?') {
        print(&b, "\nUntracked");
        return finish(&b);
    }
    if (r->x != '.') {
        print(&b, "\nStaged: ");
        print(&b, changeword(r->x));
    }
    if (r->y != '.') {
        print(&b, "\nNot staged: ");
        print(&b, changeword(r->y));
    }
    return finish(&b);
}

// Lines added and deleted, as "+a −d".
static Str changecounts(Arena *a, i64 add, i64 del)
{
    Buf b(a, 48);
    putbyte(&b, '+');
    print(&b, add);
    print(&b, " \xe2\x88\x92");  // a minus sign
    print(&b, del);
    return finish(&b);
}

// The Changes pane: the shown session's changes as git status has them
// (guigit, ongit), asked for only while this shows. Its branch, and its
// upstream if any, over its folder; a row a file, of its status, its
// path cut in its middle, and the lines added and deleted; the totals.
static void drawchanges(Gui *g)
{
    GuiSession *s    = g->sess;
    Changes    *c    = &g->changes;
    Arena       a    = g->frame;
    Palette    *pal  = g->pal;
    Str         why  = s ? hostwhy(g, s, "git") : Str("No session");
    GitWatch   *w    = s ? &s->git : 0;
    Str         note = why;
    if (!note.len && w->state==GITW_NONE) {
        note = "Not in a git repository";
    } else if (!note.len && (w->state==GITW_UNKNOWN || c->s!=s)) {
        note = "Reading\xe2\x80\xa6";
    }
    if (note.len) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", cstr0(&a, note));
        ImGui::PopTextWrapPos();
        return;
    }

    // The branch, its operation and upstream, then Refresh at the right
    ImGuiStyle &st = ImGui::GetStyle();
    f32         bw = iconw();
    f32         rx = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw;
    Str         name = c->branch.len ? c->branch : w->branch;
    if (!c->branch.len && w->detached) {
        name = concat(&a, "Detached at ", takehead(w->oid, 12));
    }
    Measure m = {};
    m.advance = widgetadvance;
    Str head  = concat(&a, "\xee\x82\xa0 ", name.len ? name : Str("HEAD"));
    Str cut   = midcut(&a, head, rx - st.ItemSpacing.x - ImGui::GetCursorPosX(), &m, 0);
    ImGui::AlignTextToFramePadding();
    text(cut.len ? cut : head);
    if (cut.len != head.len) {
        itemtip("%s", cstr0(&a, name));
    }
    if (w->op.len) {
        Buf b(&a, 64);
        print(&b, w->op);
        if (w->steps) {
            putbyte(&b, ' ');
            print(&b, w->step);
            putbyte(&b, '/');
            print(&b, w->steps);
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, pal->amber);
        text(finish(&b));
        ImGui::PopStyleColor();
    }
    if (c->unborn) {
        ImGui::SameLine();
        ImGui::TextDisabled("no commits yet");
    }
    if (c->upstream.len) {
        Buf b(&a, 64 + c->upstream.len);
        print(&b, "\xe2\x86\x92 ");  // an arrow
        print(&b, c->upstream);
        if (c->ab && c->ahead) {
            print(&b, "  \xe2\x86\x91");  // up: commits to push
            print(&b, c->ahead);
        }
        if (c->ab && c->behind) {
            print(&b, "  \xe2\x86\x93");  // down: to pull
            print(&b, c->behind);
        }
        Str up = finish(&b);
        samelineif(ImGui::CalcTextSize(cstr0(&a, up)).x);
        ImGui::TextDisabled("%s", cstr0(&a, up));
        if (c->ab) {
            itemtip("%lld ahead of %s, %lld behind it",
                                  (long long)c->ahead, cstr0(&a, c->upstream),
                                  (long long)c->behind);
        }
    }
    if (rx > rightof()) {
        ImGui::SameLine(rx);
    }
    b32 busy = c->id && c->id==w->id;
    ImGui::BeginDisabled(busy);
    if (iconbutton(ICON_REFRESH "##refresh")) {
        w->statusat = 0;
        gitstale(g, s);
    }
    ImGui::EndDisabled();
    itemnamed(g, "changes refresh");
    itemtip(busy ? "Reading the changes" :
                          "Refresh: read the changes again. They are read as the "
                          "agent's tools finish, and as this window takes the focus back.");
    Str root = homeshow(&a, s->host, w->root);
    cut = midcut(&a, root, ImGui::GetContentRegionAvail().x, &m, 0);
    ImGui::TextDisabled("%s", cstr0(&a, cut.len ? cut : root));
    if (cut.len != root.len) {
        itemtip("%s", cstr0(&a, root));
    }

    if (c->err.len) {
        Str err = c->err;
        if (s->host->kind == GH_REMOTE) {
            err = concat(&a, concat(&a, "On ", s->host->name), concat(&a, ": ", err));
        }
        ImGui::PushTextWrapPos(0);
        ImGui::PushStyleColor(ImGuiCol_Text, pal->bad);
        text(err);
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        return;
    } else if (!c->count) {
        ImGui::TextDisabled("No changes");
        return;
    }

    // The rows, the counts' column as wide as its widest
    i64 maxadd = 0;
    i64 maxdel = 0;
    b32 counts = 0;
    b32 bin    = 0;
    for (iz i = 0; i < c->rows.len; i++) {
        ChangeRow *r = &c->rows[i];
        maxadd  = r->add>maxadd ? r->add : maxadd;
        maxdel  = r->del>maxdel ? r->del : maxdel;
        counts |= r->counted && !r->bin;
        bin    |= r->bin;
    }
    f32 cw = counts ? ImGui::CalcTextSize(cstr0(&a, changecounts(&a, maxadd, maxdel))).x : 0;
    f32 bw2 = bin ? ImGui::CalcTextSize("bin").x : 0;
    cw = cw>bw2 ? cw : bw2;
    f32 lw   = ImGui::CalcTextSize("W").x;
    f32 foot = ImGui::GetTextLineHeightWithSpacing();
    ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##changes", cw>0 ? 3 : 2, flags, v2(0, -foot))) {
        ImGui::TableSetupColumn("##status", ImGuiTableColumnFlags_WidthFixed, lw);
        ImGui::TableSetupColumn("##path", ImGuiTableColumnFlags_WidthStretch);
        if (cw > 0) {
            ImGui::TableSetupColumn("##lines", ImGuiTableColumnFlags_WidthFixed, cw);
        }
        ImGuiListClipper clip;
        clip.Begin((i32)c->rows.len);
        while (clip.Step()) {
            for (i32 i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                ChangeRow *r = &c->rows[i];
                u8         l = changeletter(r);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                u32 col = l=='A' || l=='?' ? pal->ok : l=='D' || l=='U' ? pal->bad
                                                                       : pal->paneltext;
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                text(Str{&l, 1});
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                Str path = midcut(&a, r->path, ImGui::GetContentRegionAvail().x, &m, 0);
                text(path.len ? path : Str("\xe2\x80\xa6"));
                if (ImGui::BeginItemTooltip()) {
                    ImGui::PushTextWrapPos(tipwrap());
                    text(changetip(&a, r));
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
                if (cw<=0 || !r->counted) {
                    continue;
                }
                ImGui::TableNextColumn();
                Str n = r->bin ? Str("bin") : changecounts(&a, r->add, r->del);
                f32 x = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(cstr0(&a, n)).x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (x>0 ? x : 0));
                if (r->bin) {
                    ImGui::TextDisabled("bin");
                    continue;
                }
                // Each in its color, but dim where none
                iz  sp  = find(n, " ");
                u32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
                ImGui::PushStyleColor(ImGuiCol_Text, r->add ? pal->ok : dim);
                text(takehead(n, sp));
                ImGui::PopStyleColor();
                ImGui::SameLine(0, 0);
                ImGui::PushStyleColor(ImGuiCol_Text, r->del ? pal->bad : dim);
                text(cuthead(n, sp));
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndTable();
    }

    // The totals, and what they leave out
    Buf b(&a, 160);
    print(&b, c->count);
    print(&b, c->count==1 ? Str(" file") : Str(" files"));
    if (counts) {
        print(&b, ", ");
        print(&b, changecounts(&a, c->add, c->del));
    }
    if (c->more) {
        print(&b, "  \xc2\xb7  ");
        print(&b, c->more);
        print(&b, " more not listed");
    }
    if (c->partial) {
        print(&b, "  \xc2\xb7  new files' lines partly counted");
    }
    ImGui::TextDisabled("%s", cstr0(&a, finish(&b)));
    itemtip("git took %lld ms", (long long)c->ms);
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

static f32 textw(Str s)
{
    return ImGui::CalcTextSize(s.cdata, s.cdata+s.len).x;
}

// s cut at its end to fit w pixels in the current font, with an
// ellipsis when cut.
static Str elidewidth(Arena *a, Str s, f32 w)
{
    if (textw(s) <= w) {
        return s;
    }
    Str dots = "\xe2\x80\xa6";
    w -= textw(dots);
    iz lo = 0, hi = s.len;  // the most of s that fits, by bisection
    while (lo < hi) {
        iz mid = lo + (hi - lo + 1)/2;
        if (textw(takehead(s, mid)) <= w) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    for (; lo>0 && lo<s.len && (s[lo]&0xc0)==0x80; lo--) {}
    return concat(a, clone(a, trimright(takehead(s, lo))), dots);
}

// How a job ended, where a running one's Kill is: "exited 1", "killed".
static Str jobstate(Arena *a, ViewJob *j)
{
    Buf b(a, 64);
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
    return finish(&b);
}

// A job's item's tooltip: its number, as the agent knows it, its whole
// command, and where all its output is.
static void jobtip(Arena *a, ViewJob *j, Str spill)
{
    if (spill.len) {
        itemtip("Job %lld\n%s\nFull output: %s", (long long)j->n,
                cstr0(a, j->command), cstr0(a, spill));
    } else {
        itemtip("Job %lld\n%s", (long long)j->n, cstr0(a, j->command));
    }
}

// The session's background jobs, newest first: a dot in its state's
// color, the agent's name for it, its output size and how long,
// a Kill button while it runs (else how it ended), its command under a
// name, and its output's end. A
// running job's clock asks for a frame a second, only while shown.
static void drawjobs(Gui *g)
{
    View *v = g->view;
    g->jobsshown = 0;
    // Output held back since this pane last showed it: a few times a
    // second at most (JOBS_PERIOD), or at once, new to this view's jobs
    if (g->jobsdue || g->jobsview!=v) {
        viewjobsshow(v);
    }
    g->jobsview = v;
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
    f32         endw  = killw;  // the last column, as wide as its widest
    for (iz i = 0; i < v->jobs.len; i++) {
        if (v->jobs[i].status != VJOB_RUNNING) {
            Arena t = a;
            f32   w = textw(jobstate(&t, &v->jobs[i]));
            endw = w>endw ? w : endw;
        }
    }
    for (iz i = v->jobs.len-1; i >= 0; i--) {
        ViewJob *j = &v->jobs[i];
        ImGui::PushID((i32)i);
        b32 running = j->status == VJOB_RUNNING;
        u32 col     = jobcolor(g->pal, j);

        Str state = jobstate(&a, j);
        Str size  = j->bytes ? bytesize(&a, j->bytes) : Str{};
        Str time = {};
        if (running) {
            i64 el = nowms - j->start;
            el = el<0 ? 0 : el;
            schedat(&g->sched, g->sched.now + (1000 - el%1000)*1000 + 1000, DUE_CLOCK);
            time = secstext(&a, el/1000);
        } else if (j->ms) {
            time = duration(&a, j->ms);
        }

        // Its name left, cut to fit; size, time and Kill (or its end)
        // right, each in a column of fixed width, so neither a growing
        // clock nor a long name moves the other, and rows line up. Its
        // number, which the agent knows it by, is in the tooltip.
        f32 gap   = st.ItemSpacing.x;
        f32 x0    = ImGui::GetCursorPosX();
        f32 avail = ImGui::GetContentRegionAvail().x;
        f32 sizew = textw("888.8 KiB");
        f32 timew = textw("88 min 88 s");
        timew     = textw(time)>timew ? textw(time) : timew;
        f32 rightw = sizew + gap + timew + gap + endw;
        f32 dotw  = ImGui::GetFontSize();

        ImGui::AlignTextToFramePadding();
        ImVec2 p  = ImGui::GetCursorScreenPos();
        f32    lh = ImGui::GetFrameHeight();
        drawdot(ImGui::GetWindowDrawList(), v2(p.x + dotw*0.35f, p.y + lh/2),
                dotw*0.22f, col);
        ImGui::SetCursorPosX(x0 + dotw);
        Str  title = j->desc.len ? j->desc : j->summary;
        b32  mono  = !j->desc.len;
        if (mono) ImGui::PushFont(g->fonts.mono, 0);
        title = elidewidth(&a, title, avail - dotw - rightw - gap);
        if (mono) {
            ImGui::PushStyleColor(ImGuiCol_Text, g->pal->summary);
            text(title);
            ImGui::PopStyleColor();
            ImGui::PopFont();
        } else {
            text(title);
        }
        Str spill = j->spill.len && v->dir.len ? join(&a, v->dir, j->spill) : j->spill;
        jobtip(&a, j, spill);

        f32 x = x0 + avail - rightw;
        x = x>rightof() ? x : rightof();
        ImGui::SameLine(x + sizew - textw(size));
        ImGui::TextDisabled("%s", cstr0(&a, size));
        x += sizew + gap;
        ImGui::SameLine(x + timew - textw(time));
        ImGui::TextDisabled("%s", cstr0(&a, time));
        x += timew + gap;
        if (running && can) {
            ImGui::SameLine(x);
            ImGui::BeginDisabled(j->killing);
            b32 kill = ImGui::Button(j->killing ? "Stopping..." : "Kill", v2(killw, 0));
            ImGui::EndDisabled();
            if (first) {
                itemrect(g, RECT_JOBKILL);
                first = 0;
            }
            itemtip("Stop the job (and what it started); the agent is told");
            if (kill) {
                j->killing = 1;
                guijobkill(g, g->sess, j->n);
            }
        } else if (!running) {
            ImGui::SameLine(x);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            text(state);
            ImGui::PopStyleColor();
        }

        ImGui::PushFont(g->fonts.mono, 0);
        if (!mono) {
            // A name is the agent's word for it: what runs shows under it
            ImGui::PushStyleColor(ImGuiCol_Text, g->pal->summary);
            text(j->summary);
            ImGui::PopStyleColor();
            jobtip(&a, j, spill);
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

// Why a host cannot import a session now, or null if it can.
static Str importwhy(Gui *g, GuiHost *h)
{
    return g->imp.state!=IMP_IDLE ? Str("An import is under way") :
           !h || !hostcap(h, "import") ? Str("Its engine is too old to import") : Str{};
}

// Copy a string into a dialog's field, as much as fits.
static void fieldput(char *field, Str s)
{
    iz n = s.len<FIELD_MAX ? s.len : FIELD_MAX-1;
    copybytes(field, s.data, n);
    field[n] = 0;
}

// The new session dialog filled for an import to a host: the log's
// directory where it is known to be there (on this machine, or as
// another session's there), else the most recently active of that
// host's sessions',
// else this machine's first, or nothing for a remote with none, never
// a directory of another machine. Its model where that host has one by
// its name (its configuration's, which a host other than the main one
// is asked for), else none.
static void importfill(Gui *g, GuiHost *h)
{
    Import *x = &g->imp;
    Arena   a = g->frame;
    if (x->filled != h) {
        x->filled    = h;
        x->modelwait = 1;
        b32 there = h->kind!=GH_REMOTE && x->cwd.len &&
                    os_stat(g->os, a, x->cwd).type==STAT_DIR;
        Str newest = {};
        Slice<GuiSession *> list = sessrecent(g, &a, h);
        for (iz i = 0; i < list.len; i++) {
            GuiSession *s = list[i];
            Str         d = s->view && s->view->cwd.len ? s->view->cwd : s->cwd;
            if (sessnofolder(s)) {
                continue;  // a folder of its own, for it alone
            }
            there |= x->cwd.len && d==x->cwd;
            newest = newest.len ? newest : d;
        }
        there &= !x->nofolder;
        fieldput(g->newcwd, there              ? x->cwd     :
                            newest.len         ? newest     :
                            h->kind!=GH_REMOTE ? g->firstcwd : Str{});
        g->cwdhost = h==g->main ? 0 : h->uid;
        // A session that had a folder of its own has one again
        g->newnofolder = x->nofolder && hostcap(h, "nofolder");
    }
    if (x->modelwait && h!=g->main) {
        hostmodels(g, h);
        if (!h->modelsgot) {
            return;
        }
    }
    if (x->modelwait && hostcan(h, "models")) {
        // At the endpoint that lists it, else as configured, else none
        hostlist(g, h);
        if (!h->listgot) {
            return;
        }
        x->modelwait = 0;
        g->newcheck  = 0;
        Slice<ModelPick> picks = modelpicks(g, &a, h);
        Slice<GuiModel>  confs = h==g->main ? replyconfs(&a, g->cfg) : h->confs;
        ModelPick        got   = {};
        for (iz i = 0; !got.model.len && i < picks.len; i++) {
            got = x->model.len && !picks[i].err.len && !picks[i].dflt &&
                  picks[i].model==x->model ? picks[i] : got;
        }
        for (iz i = 0; !got.model.len && i < confs.len; i++) {
            got.model = x->model.len && confs[i].name==x->model ? x->model : Str{};
        }
        fieldput(g->newep, got.endpoint);
        fieldput(g->newmodel, got.model);
        return;
    }
    if (x->modelwait) {
        g->newep[0] = 0;
        x->modelwait = 0;
        Slice<Str> models = h==g->main ? cfgmodels(g, &a) : h->models;
        b32        known  = 0;
        for (iz i = 0; i < models.len; i++) {
            known |= x->model.len && models[i]==x->model;
        }
        fieldput(g->newmodel, known ? x->model : Str{});
    }
}

// Completion of a path typed in a field (pathpick.cpp), for the new
// session dialog's directory: a dropdown under the field of the folders
// the text picks, listed on the host, which leaves the keyboard with the
// field. It shows once the text is typed in, or Up or Down pressed, and
// hides on accepting a pick, on Esc, and as the field loses the keyboard.
// Up and Down pick; Tab accepts the pick, or the first, and Enter one
// picked, else Enter is the dialog's; a click on a row accepts it. An
// accepted folder is the text's, with a separator: its own folders are
// then listed, as soon as the text changes again.
struct PathCb {
    Gui            *g;
    PathQuery       q;
    Slice<PathPick> picks;
    Arena           scratch;   // past the picks, for pathcb and pathafter
    b32             accepted;  // Tab accepted a pick
    b32             wsl;       // a Windows path typed becomes the distro's
};

struct PathField {
    PathCb              cb;
    ImGuiID             id;
    ImGuiInputTextFlags flags;
    DirList            *list;     // the folder's listing, null if none
    b32                 visible;  // the dropdown is drawn
    b32                 enter;    // Enter accepts the pick
};

static i32 pathcb(ImGuiInputTextCallbackData *d)
{
    PathCb   *c  = (PathCb *)d->UserData;
    PathComp *pc = &c->g->pc;
    i32       n  = (i32)c->picks.len;
    if (d->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        b32 up = d->EventKey == ImGuiKey_UpArrow;
        if (n) {
            pc->sel = pc->sel<0 || !pc->shown ? (up ? n-1 : 0) : (pc->sel + (up ? n-1 : 1)) % n;
        }
        pc->shown = 1;
    } else if (d->EventFlag==ImGuiInputTextFlags_CallbackCompletion && n) {
        Arena a = c->scratch;
        Str   t = pathaccept(&a, c->q, c->picks[pc->sel>=0 ? pc->sel : 0]);
        if (t.len < d->BufSize) {
            d->DeleteChars(0, d->BufTextLen);
            d->InsertChars(0, t.cdata, t.cdata+t.len);
            c->accepted = 1;
        }
    } else if (d->EventFlag == ImGuiInputTextFlags_CallbackEdit) {
        byte  mem[FIELD_MAX+8];
        Arena a = {mem, mem+sizeof(mem)};
        Str   t = {(u8 *)d->Buf, (iz)d->BufTextLen};
        Str   w = wslpath(&a, t);
        if (w.data!=t.data && w.len<d->BufSize) {
            i32 at = d->CursorPos + (i32)(w.len - t.len);
            d->DeleteChars(0, d->BufTextLen);
            d->InsertChars(0, w.cdata, w.cdata+w.len);
            d->CursorPos = at<0 ? 0 : at>(i32)w.len ? (i32)w.len : at;
            d->ClearSelection();
        }
    }
    return 0;
}

// Before the field: what it picks, from its text as it stands, and the
// keys the dropdown takes from it and the dialog (*enter): folders, or
// with files their files too.
static PathField pathbefore(Gui *g, GuiHost *h, char *buf, b32 *enter, b32 off,
                            b32 files = 0)
{
    PathComp *pc = &g->pc;
    PathField f  = {};
    f.cb.g  = g;
    f.id    = ImGui::GetID("##cwd");
    f.flags = ImGuiInputTextFlags_AutoSelectAll;
    f.cb.wsl = hostwsl(g, h);
    if (f.cb.wsl) {
        f.flags |= ImGuiInputTextFlags_CallbackEdit;  // C:\ typed is /mnt/c/
    }
    b32 active = ImGui::GetActiveID() == f.id;
    if (!active && !pc->refocus) {
        pc->shown = 0;
        pc->sel   = -1;
    }
    if (pc->refocus && active) {
        // Back after a click on a row: the caret at the end, not all
        // selected, to type on
        ImGuiInputTextState *st = ImGui::GetInputTextState(f.id);
        if (st) {
            st->ReloadUserBufAndMoveToEnd();
        }
        pc->refocus = 0;
    } else if (pc->refocus) {
        ImGui::ActivateItemByID(f.id);
        scheddirty(&g->sched);
    }
    if (off || !hostlists(h)) {
        pc->refocus = 0;
        return f;
    }
    f.flags |= ImGuiInputTextFlags_CallbackHistory;

    if (!pc->pickbase) {
        pc->picks    = newarena(os_reserve(g->os, (iz)1<<24));
        pc->pickbase = pc->picks.beg;
    }
    pc->picks.beg = pc->pickbase;
    Arena *a    = &pc->picks;
    Str    text = {(u8 *)buf, (iz)__builtin_strlen(buf)};
    b32    win  = hostwin(h) || (text.len>=2 && text[1]==':');
    f.cb.q = pathquery(text, win);
    if (!active || !pathlistable(f.cb.q.dir, win)) {
        return f;
    }
    // Its own arena, never the frame's: the caller holds a copy of that,
    // which a fresh one would write over
    f.list       = dirget(g, *a, h, f.cb.q.dir, files);
    f.cb.picks   = pathpicks(a, f.list->ents, f.cb.q.filter, !files);
    f.cb.scratch = *a;
    i32 n      = (i32)f.cb.picks.len;
    pc->sel    = pc->sel<n ? pc->sel : -1;
    f.visible  = pc->shown && (n || f.list->err.len || !f.list->got);
    if (!f.visible) {
        return f;
    }
    if (n) {
        f.flags |= ImGuiInputTextFlags_CallbackCompletion;  // Tab, kept from nav
    }
    // Esc closes the dropdown, not the dialog, nor reverts the field
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        pc->shown = 0;
        ImGui::SetKeyOwner(ImGuiKey_Escape, ImGui::GetID("##pathkeys"),
                           ImGuiInputFlags_LockThisFrame);
    }
    // Enter with a pick accepts it, the field keeping the keyboard
    if (*enter && pc->sel>=0) {
        *enter  = 0;
        f.enter = 1;
        ImGui::SetKeyOwner(ImGuiKey_Enter, ImGui::GetID("##pathkeys"),
                           ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_KeypadEnter, ImGui::GetID("##pathkeys"),
                           ImGuiInputFlags_LockThisFrame);
    }
    return f;
}

// Put an accepted pick in the field, from outside it.
static void pathput(PathField *f, char *buf, PathPick p, Arena scratch)
{
    Str t = pathaccept(&scratch, f->cb.q, p);
    if (t.len < FIELD_MAX) {
        fieldput(buf, t);
        ImGuiInputTextState *st = ImGui::GetInputTextState(f->id);
        if (st && ImGui::GetActiveID()==f->id) {
            st->ReloadUserBufAndMoveToEnd();
        }
    }
}

// After the field: an accepted pick, then the dropdown under the field.
static void pathafter(Gui *g, PathField *f, char *buf)
{
    PathComp *pc = &g->pc;
    if (f->cb.accepted) {
        pc->shown = 0;
        pc->sel   = -1;
        pc->accepts++;
    } else if (ImGui::IsItemEdited()) {
        pc->shown = 1;
        pc->sel   = -1;
        pc->top   = 0;
    }
    Slice<PathPick> picks = f->cb.picks;
    i32             n     = (i32)picks.len;
    if (f->enter && pc->sel>=0 && pc->sel<n) {
        pathput(f, buf, picks[pc->sel], f->cb.scratch);
        pc->shown = 0;
        pc->sel   = -1;
        pc->accepts++;
    }
    pc->rows = 0;
    if (!f->visible || f->cb.accepted || f->enter) {
        DirList *l = f->list;
        if (pc->shown && l && (n || l->err.len || !l->got)) {
            scheddirty(&g->sched);  // Up or Down showed it: drawn next frame
        }
        return;
    }

    // Rows from top, which follows the pick
    pc->top = pc->sel>=0 && pc->sel<pc->top ? pc->sel :
              pc->sel>=pc->top+PICK_ROWS ? pc->sel-PICK_ROWS+1 : pc->top;
    pc->top = pc->top>n-PICK_ROWS ? n-PICK_ROWS : pc->top;
    pc->top = pc->top<0 ? 0 : pc->top;

    ImVec2 r0 = ImGui::GetItemRectMin();
    ImVec2 r1 = ImGui::GetItemRectMax();
    f32    w  = r1.x - r0.x;
    ImGui::SetNextWindowPos(v2(r0.x, r1.y));
    ImGui::SetNextWindowSizeConstraints(v2(w, 0), v2(w, 1e6f));
    ImGuiWindowFlags fl = ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoTitleBar |
                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                          ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##pathpick", 0, fl)) {
        Arena       a    = f->cb.scratch;
        DirList    *l    = f->list;
        ImDrawList *dl   = ImGui::GetWindowDrawList();
        f32         rw   = ImGui::GetContentRegionAvail().x;
        u32         dim  = g->pal->paneldim;
        u32         mark = g->pal->link;
        u32         text = ImGui::GetColorU32(ImGuiCol_Text);
        Str         sep  = f->cb.q.dir.len ? cuthead(f->cb.q.dir, f->cb.q.dir.len-1) : Str("/");
        i32         end  = pc->top+PICK_ROWS<n ? pc->top+PICK_ROWS : n;
        for (i32 i = pc->top; i < end; i++) {
            PathPick p  = picks[i];
            ImVec2   at = ImGui::GetCursorScreenPos();
            ImGui::PushID(i);
            if (ImGui::Selectable("##pick", i==pc->sel, ImGuiSelectableFlags_SelectOnClick,
                                  v2(rw, 0))) {
                pathput(f, buf, p, a);
                pc->shown   = 0;
                pc->sel     = -1;
                pc->refocus = 1;
                pc->accepts++;
                scheddirty(&g->sched);
            }
            ImGui::PopID();
            if (i == pc->top) {
                itemnamed(g, "path pick");
            }
            // The name, the parts the filter matched in the link color
            u8 *m = pathmarks(&a, p.name, f->cb.q.filter);
            f32 x = at.x;
            for (iz k = 0; k < p.name.len;) {
                iz j = k;
                for (; j<p.name.len && m[j]==m[k]; j++) {}
                Str s = {p.name.data+k, j-k};
                dl->AddText(v2(x, at.y), m[k] ? mark : text, s.cdata, s.cdata+s.len);
                x += ImGui::CalcTextSize(s.cdata, s.cdata+s.len).x;
                k = j;
            }
            if (p.dir) {
                dl->AddText(v2(x, at.y), dim, sep.cdata, sep.cdata+sep.len);
            }
        }
        if (!n && l->err.len) {
            ImGui::TextDisabled("%s", cstr0(&a, l->err));
        } else if (!n) {
            ImGui::TextDisabled("Listing\xe2\x80\xa6");
        } else if (end<n || l->more) {
            Buf b(&a, 64);
            if (end < n) {
                print(&b, (i64)(n-end));
                print(&b, " more");
            }
            print(&b, !l->more ? Str{} : end<n ? Str(", and more not listed") : Str("More not listed"));
            ImGui::TextDisabled("%s", cstr0(&a, finish(&b)));
        }
        if (n) {
            ImGui::TextDisabled("Tab completes; Up and Down pick");
        }
        pc->rows = n;
        Str first = n ? picks[0].name : Str{};
        fieldput(pc->first, first.len<countof(pc->first) ? first : Str{});
    }
    ImGui::End();
}

// A path typed in a modal (popup id ###<pid>, its items "<name> field"
// and "<name> ok"), completed as the new session's folder is. Where the
// platform has no file dialog to show (pathkeep, g->pask), it is
// answered as the dialog would have; for Open file (openfile, g->fask),
// fileaskok checks and answers it.
static void drawpathask(Gui *g, PathAsk *pa, char const *pid, char const *name, b32 openfile)
{
    Arena a  = g->frame;
    f32   sc = g->scale;
    Str   id = concat(&a, "###", Str{(u8 *)pid, (iz)__builtin_strlen(pid)});
    Str   nm = {(u8 *)name, (iz)__builtin_strlen(name)};
    pa->shown = 0;
    // Never over another popup, which opening would close: it waits,
    // open still, as one opened earlier this frame has no window yet
    b32 other = ImGui::IsPopupOpen((ImGuiID)0,
                                   ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel);
    if (pa->open && !other) {
        pa->open = 0;
        pa->err  = {};
        ImGui::OpenPopup(cstr0(&a, id));
    }
    Str ptitle = concat(&a, pa->title, id);
    if (ImGui::BeginPopupModal(cstr0(&a, ptitle), 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        b32 enter = modalenter();
        b32 save  = pa->kind == ASK_SAVE;
        pa->shown = 1;
        // Open file on another host: its paths, completed and checked there
        GuiHost *far = openfile ? filehost(g, g->fask.host) : 0;
        if (openfile && g->fask.close) {
            g->fask.close = 0;
            ImGui::CloseCurrentPopup();
        } else if (openfile) {
            fileaskcheck(g);
        }
        ImGui::TextUnformatted(openfile ? "Which file?" :
                               save ? "No file dialog can be shown here. Where should it be saved?"
                                    : "No file dialog can be shown here. Which file?");
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(560*sc);
        PathField pf = pathbefore(g, far ? far : g->main, pa->buf, &enter, 0, 1);
        if (openfile && g->fask.dropped) {
            // A file dropped filled the field (droproute): shown as typed,
            // the caret at its end, for a line after it
            g->fask.dropped = 0;
            ImGuiInputTextState *st = ImGui::GetInputTextState(pf.id);
            if (st && ImGui::GetActiveID()==pf.id) {
                st->ReloadUserBufAndMoveToEnd();
            }
        }
        Str hint = far ? concat(&a, "A path on ", filehostname(g, &a, far->uid))
                       : Str("A path on this computer");
        ImGui::InputTextWithHint("##cwd", cstr0(&a, hint), pa->buf, FIELD_MAX,
                                 pf.flags, pathcb, &pf.cb);
        itemnamed(g, concat(&a, nm, " field"));
        pathafter(g, &pf, pa->buf);
        b32 checking = openfile && g->fask.statid;
        if (checking) {
            ImGui::TextDisabled("Checking...");
        } else if (pa->err.len) {
            ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
            ImGui::TextWrapped("%s", cstr0(&a, pa->err));
            ImGui::PopStyleColor();
        }
        ImGui::Spacing();
        b32 ok = ImGui::Button(save ? "Save" : "Open", v2(120*sc, 0)) || enter;
        itemnamed(g, concat(&a, nm, " ok"));
        ImGui::SameLine();
        b32 cancel = ImGui::Button("Cancel", v2(120*sc, 0)) || ImGui::Shortcut(ImGuiKey_Escape);
        if (ok && openfile) {
            if (!checking) {  // else the answer comes first
                pa->err = fileaskok(g, trim({(u8 *)pa->buf, (iz)__builtin_strlen(pa->buf)}));
                if (!pa->err.len && !g->fask.statid) {
                    ImGui::CloseCurrentPopup();
                }
            }
        } else if (cancel && openfile) {
            g->fask.statid = 0;  // its answer, if it comes, opens nothing
            ImGui::CloseCurrentPopup();
        } else if (ok) {
            Str      t  = trim({(u8 *)pa->buf, (iz)__builtin_strlen(pa->buf)});
            HomePath hp = tildepath(&a, t, g->main->home, HOST_WINDOWS);
            i32      st = hp.err.len || !t.len ? STAT_MISSING : os_stat(g->os, a, hp.path).type;
            // Its messages are literals, tildepath's too: kept past the frame
            pa->err = hp.err.len                       ? hp.err :
                      !t.len                           ? Str("Type a path") :
                      !pathroot(hp.path, HOST_WINDOWS) ? Str("Type the whole path") :
                      st == STAT_DIR                   ? Str("That is a folder") :
                      !save && st!=STAT_FILE           ? Str("No such file") : Str{};
            if (!pa->err.len) {
                mailpost(g->tasks, pa->type, pa->id, hp.path, 1);
                ImGui::CloseCurrentPopup();
            }
        } else if (cancel) {
            mailpost(g->tasks, pa->type, pa->id, {}, 1);  // as a cancelled dialog's
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// The new session dialog's model. For a host whose engine lists its
// endpoints' models, a combo of them, the defaults first, with a field
// to filter them, or for another, MODEL or ENDPOINT/MODEL, which Enter
// takes. Its pick (newep, newmodel) holds for the run while listed;
// both empty are the defaults. An older engine's is a field to type
// a model in, by default the configured one.
static void drawnewmodel(Gui *g, Arena a, GuiHost *host)
{
    ImGui::SetNextItemWidth(-1);
    if (!hostcan(host, "models")) {
        Str dflt = {};
        for (iz i = 0; i < g->cfgrows.len; i++) {
            CfgRow *r = &g->cfgrows[i];
            if (!r->kind.len && r->key=="model") {
                dflt = r->value;
            }
        }
        char const *hint = host->kind==GH_REMOTE ? "as configured there" :
                           dflt.len ? cstr0(&a, concat(&a, "configured: ", dflt)) :
                           "the model's name";
        ImGui::InputTextWithHint("##model", hint, g->newmodel, FIELD_MAX);
        g->newep[0] = 0;
        return;
    }

    Slice<ModelPick> picks = modelpicks(g, &a, host);
    Str ep = {(u8 *)g->newep, (iz)__builtin_strlen(g->newep)};
    Str m  = {(u8 *)g->newmodel, (iz)__builtin_strlen(g->newmodel)};
    if (g->newcheck && host->listgot && !host->listid) {
        // The last pick, as the fresh listing has it, or the defaults
        g->newcheck = 0;
        b32 listed  = !m.len;
        for (iz i = 0; i < picks.len; i++) {
            ModelPick *p = &picks[i];
            listed |= !p->err.len && !p->dflt && p->endpoint==ep && p->model==m;
        }
        if (!listed) {
            g->newep[0] = g->newmodel[0] = 0;
            ep = m = {};
        }
    }
    i32 cur = -1, mark = -1;
    for (iz i = 0; i < picks.len; i++) {
        ModelPick *p  = &picks[i];
        b32        is = m.len ? !p->dflt && !p->err.len && p->endpoint==ep && p->model==m
                              : p->dflt;
        cur  = cur<0 && is ? (i32)i : cur;
        mark = mark<0 && !p->err.len && !p->dflt && p->model!=host->modeldflt ? (i32)i : mark;
    }
    if (mark >= 0 && g->pickwant!=picks[mark].label) {
        g->pickwant = clone(&g->perm, picks[mark].label);
    }
    Str shown = cur>=0 && picks[cur].dflt ? concat(&a, picks[cur].label, "  (default)") :
                cur >= 0                  ? picks[cur].label :
                m.len && ep.len           ? concat(&a, concat(&a, ep, "/"), m) :
                m.len                     ? m :
                !host->listgot            ? Str("Asking the endpoints for their models...") :
                Str("No model listed: type one");

    ImVec2 at = ImGui::GetCursorScreenPos();
    f32    w  = ImGui::GetContentRegionAvail().x;
    if (ImGui::BeginCombo("##model", cstr0(&a, shown), ImGuiComboFlags_HeightLargest)) {
        g->picksframe = ImGui::GetFrameCount();
        if (ImGui::IsWindowAppearing()) {
            g->newfilter[0] = 0;
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##modelfilter", "filter, or a model to type, Enter",
                                     g->newfilter, FIELD_MAX,
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
            Str t = trim({(u8 *)g->newfilter, (iz)__builtin_strlen(g->newfilter)});
            if (t.len) {
                ModelPick p = modeltyped(host, t);
                fieldput(g->newep, p.endpoint);
                fieldput(g->newmodel, p.model);
                ImGui::CloseCurrentPopup();
            }
        }
        itemnamed(g, "model filter");
        Str filter = trim({(u8 *)g->newfilter, (iz)__builtin_strlen(g->newfilter)});
        if (host->listid) {
            ImGui::TextDisabled("Asking the endpoints for their models...");
        }
        i32 hit = pickrows(g, a, picks, filter, cur, mark, RECT_NEWPICK);
        if (hit >= 0) {
            ModelPick *p = &picks[hit];
            fieldput(g->newep, p->dflt ? Str{} : p->endpoint);
            fieldput(g->newmodel, p->dflt ? Str{} : p->model);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndCombo();
    }
    g->rects[RECT_NEWMODEL] = {at.x, at.y, at.x+w, at.y+ImGui::GetFrameHeight()};
    itemnamed(g, "new model");
}

// The new session dialog: a working directory, typed or from those of
// earlier sessions, and a model, by default the configured one. For an
// import, it says what is imported, filled from it (importfill), and
// lists only the hosts that can import.
// A worktree's name, prefilled with the probe's suggestion, and below it
// what it starts from, as the probe of cwd on host says: the base, and
// the uncommitted changes that stay behind. Whether the probe allows it.
static b32 wtfields(Gui *g, Arena a, GuiHost *host, Str cwd)
{
    WtProbe *p    = &g->wtp;
    b32      have = p->got && p->h==host && p->cwd==cwd;
    ImGui::TextUnformatted("Name");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (g->wtfill) {
        // A field with the keyboard shows its own copy: the suggestion
        // replaces it, all selected, to be typed over
        ImGuiInputTextState *st = ImGui::GetInputTextState(ImGui::GetID("##wtname"));
        if (st) {
            st->ReloadUserBufAndSelectAll();
        }
        g->wtfill = 0;
    }
    if (ImGui::InputText("##wtname", g->wtname, FIELD_MAX)) {
        g->wtnamed = g->wtname[0] != 0;
    }
    itemnamed(g, "worktree name");
    itemtip("Its folder's name in .prospero/worktrees, and its branch's after "
                          "the prefix: lowercase letters, digits and single hyphens");
    if (!have) {
        ImGui::TextDisabled("Looking at the folder\xe2\x80\xa6");
        return 1;
    } else if (!p->ok) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
        ImGui::TextWrapped("No worktree here: %s", cstr0(&a, p->why));
        ImGui::PopStyleColor();
        return 0;
    }
    Str field = trim({(u8 *)g->wtname, (iz)__builtin_strlen(g->wtname)});
    Buf b(&a, 512);
    print(&b, "Branch ");
    print(&b, p->prefix);
    print(&b, field.len ? field : Str("NAME"));
    print(&b, ", from ");
    if (p->branch.len) {
        print(&b, p->branch);
        print(&b, " at ");
    }
    print(&b, takehead(p->oid, 7));
    if (p->dirty > 0) {
        print(&b, " \xc2\xb7 ");
        print(&b, p->dirty);
        print(&b, p->dirty==1 ? Str(" uncommitted change stays") : Str(" uncommitted changes stay"));
        print(&b, " in the folder");
    }
    ImGui::PushStyleColor(ImGuiCol_Text, g->pal->paneldim);
    ImGui::TextWrapped("%s", cstr0(&a, finish(&b)));
    ImGui::PopStyleColor();
    return 1;
}

// A worktree session of g->wtsess (guiworktree): its name, and what it
// starts from.
static void drawwt(Gui *g)
{
    f32         sc   = g->scale;
    char const *name = "Start a worktree session";
    if (g->openwt) {
        ImGui::OpenPopup(name);
        g->openwt     = 0;
        g->wtname[0]  = 0;
        g->wtnamed    = 0;
        g->wtp.cwd    = {};  // probed anew, for a fresh name
    }
    ImGui::SetNextWindowSize(v2(520*sc, 0));
    if (!ImGui::BeginPopupModal(name, 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    Arena       a     = g->frame;
    b32         enter = modalenter();
    GuiSession *s     = g->wtsess;
    Str         why   = s && !s->gone ? wtwhy(g, s) : Str("The session is gone");
    if (why.len) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
        ImGui::TextWrapped("%s", cstr0(&a, why));
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->paneldim);
        ImGui::TextWrapped(hostcap(s->host, "worktree.side")
                           ? "A fresh session under \"%s\", with none of its conversation, "
                             "that works in a new worktree, on a branch of its own, and can "
                             "report its work back to it"
                           : "A copy of \"%s\" that works in a new worktree, on a branch of "
                             "its own, while this session works on",
                           cstr0(&a, shorten(&a, sesstitle(s), 60)));
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    Str cwd = s ? (s->view && s->view->cwd.len ? s->view->cwd : s->cwd) : Str{};
    if (!why.len) {
        guiwtprobe(g, s->host, cwd);
    }
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    b32 ok = !why.len && wtfields(g, a, s ? s->host : g->main, cwd);
    ImGui::Spacing();
    ImGui::BeginDisabled(!ok);
    b32 start = ImGui::Button("Start", v2(120*sc, 0)) || (ok && enter);
    ImGui::EndDisabled();
    itemnamed(g, "worktree start");
    if (start) {
        Str field = trim({(u8 *)g->wtname, (iz)__builtin_strlen(g->wtname)});
        guiworktree(g, s, field, g->wtquote);
        g->wtquote = {};
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", v2(120*sc, 0)) || modalkey(ImGuiKey_Escape)) {
        g->wtquote = {};
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// A fork of g->mvsess into another folder (guifork with a cwd): the
// folder, on the session's host, in the new session dialog's field,
// which it starts from.
static void drawmove(Gui *g)
{
    f32         sc   = g->scale;
    char const *name = "Fork into another folder";
    GuiSession *s    = g->mvsess;
    if (g->openmv) {
        ImGui::OpenPopup(name);
        g->openmv = 0;
        g->newerr = {};
        if (s) {
            Arena a   = g->frame;
            Str   cwd = s->view && s->view->cwd.len ? s->view->cwd : s->cwd;
            fieldput(g->newcwd, homeshow(&a, s->host, cwd));
            g->cwdhost = s->host==g->main ? 0 : s->host->uid;
        }
    }
    ImGui::SetNextWindowSize(v2(560*sc, 0));
    if (!ImGui::BeginPopupModal(name, 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    Arena    a     = g->frame;
    b32      enter = modalenter();
    Str      why   = s && !s->gone ? movewhy(g, s) : Str("The session is gone");
    GuiHost *host  = s && !s->gone ? s->host : g->main;
    if (why.len) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
        ImGui::TextWrapped("%s", cstr0(&a, why));
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->paneldim);
        ImGui::TextWrapped("A copy of \"%s\" that works in this folder, while the "
                           "session goes on where it is. Its agent is told of the move, "
                           "the folder's own instructions apply, and changes made before "
                           "the fork cannot be rewound from it.",
                           cstr0(&a, shorten(&a, sesstitle(s), 60)));
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    char const *browse = "Browse\xe2\x80\xa6";
    f32 bw = ImGui::CalcTextSize(browse).x + 2*ImGui::GetStyle().FramePadding.x;
    ImGui::SetNextItemWidth(-(bw + ImGui::GetStyle().ItemSpacing.x));
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    PathField pf = pathbefore(g, host, g->newcwd, &enter, why.len>0);
    ImGui::InputText("##cwd", g->newcwd, FIELD_MAX, pf.flags, pathcb, &pf.cb);
    itemrect(g, RECT_NEWCWD);
    pathafter(g, &pf, g->newcwd);
    ImGui::SameLine();
    b32 remote = host->kind == GH_REMOTE;
    ImGui::BeginDisabled(remote);
    if (ImGui::Button(browse)) {
        browsestart(g, host);
    }
    ImGui::EndDisabled();
    itemnamed(g, "move browse");
    whytip(remote ? concat(&a, "Only for a directory on this computer: this one is on ",
                           host->name)
                  : Str("Choose the directory in a dialog"));
    if (g->newerr.len) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
        ImGui::TextWrapped("%s", cstr0(&a, g->newerr));
        ImGui::PopStyleColor();
    }

    Str dir = trim({(u8 *)g->newcwd, (iz)__builtin_strlen(g->newcwd)});
    dir     = hostwsl(g, host) ? wslpath(&a, dir) : dir;
    b32 ok  = !why.len && dir.len;
    ImGui::Spacing();
    ImGui::BeginDisabled(!ok);
    b32 go = ImGui::Button("Fork", v2(120*sc, 0)) || (ok && enter);
    ImGui::EndDisabled();
    itemnamed(g, "move fork");
    if (go) {
        guifork(g, s, -1, dir);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", v2(120*sc, 0)) || modalkey(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// The new session dialog's directory as it turns to host h, if it was
// for another: where a session there last worked, else this computer's
// first (where Prospero started), else the home directory there, ~.
static void newcwdfor(Gui *g, Arena a, GuiHost *h)
{
    u64 uid = h==g->main ? 0 : h->uid;
    if (g->cwdhost == uid) {
        return;
    }
    Str dir = h==g->main ? g->firstcwd : Str("~");
    Slice<GuiSession *> list = sessrecent(g, &a, h);
    for (iz i = 0; i < list.len; i++) {
        GuiSession *s   = list[i];
        Str         cwd = s->view && s->view->cwd.len ? s->view->cwd : s->cwd;
        if (s->host==h && cwd.len && !sessnofolder(s)) {
            dir = homeshow(&a, h, cwd);
            break;
        }
    }
    fieldput(g->newcwd, dir);
    g->cwdhost = uid;
}

// New session's fields for a new container: its image, as typed or
// picked from those listed ([container] images first, then the
// runtime's own); its workspace, with the folder here it copies or
// mounts at /work (newcwd); and its network. Returns whether Create
// may go.
static b32 drawnewctr(Gui *g, Arena a, b32 *enter)
{
    f32 sc = g->scale;
    ImGui::TextUnformatted("Image");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    *enter |= ImGui::InputText("##image", g->newimage, FIELD_MAX,
                               ImGuiInputTextFlags_EnterReturnsTrue);
    itemnamed(g, "new image");
    itemtip("A Linux image, as the runtime names it: pulled first if "
                          "it is not here");
    Slice<Str> imgs = ctrimagelist(&a, ctrset(g, "images"));
    for (iz i = 0; i < g->ctrimgs.len; i++) {
        b32 dup = g->ctrimgs[i].len>0 && g->ctrimgs[i][0]=='<';  // <none>:<none>
        for (iz k = 0; !dup && k < imgs.len; k++) {
            dup = imgs[k] == g->ctrimgs[i];
        }
        if (!dup) {
            imgs = push(&a, imgs, g->ctrimgs[i]);
        }
    }
    if (imgs.len) {
        ImGui::BeginChild("##images", v2(0, (imgs.len<6 ? (f32)imgs.len : 6.5f) *
                                            ImGui::GetTextLineHeightWithSpacing()));
        for (iz i = 0; i < imgs.len; i++) {
            ImGui::PushID((i32)i);
            Str cur = {(u8 *)g->newimage, (iz)__builtin_strlen(g->newimage)};
            if (ImGui::Selectable(cstr0(&a, imgs[i]), cur==imgs[i],
                                  ImGuiSelectableFlags_NoAutoClosePopups)) {
                fieldput(g->newimage, imgs[i]);
            }
            if (!i) {
                itemnamed(g, "new image0");
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    } else if (g->ctrimging) {
        ImGui::TextDisabled("Listing the images here...");
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Workspace");
    static char const *labels[] = {"Empty", "Copy a folder", "Mount a folder"};
    static char const *tips[]   = {
        "An empty /work, the container's alone",
        "A copy of a folder here in /work: what the agent changes stays in the container",
        "A folder here at /work: the agent changes it here, as it goes",
    };
    for (i32 i = 0; i < countof(labels); i++) {
        if (i) {
            ImGui::SameLine();
        }
        ImGui::RadioButton(labels[i], &g->newws, i);
        itemnamed(g, concat(&a, "new ws", ctrworkspaces[i]));
        itemtip("%s", tips[i]);
    }
    b32 none = g->newws == CTR_EMPTY;
    ImGui::BeginDisabled(none);
    char const *browse = "Browse\xe2\x80\xa6";
    f32 bw = ImGui::CalcTextSize(browse).x + 2*ImGui::GetStyle().FramePadding.x;
    ImGui::SetNextItemWidth(-(bw + ImGui::GetStyle().ItemSpacing.x));
    // Completed as the usual directory field is, here, where it lives
    PathField pf = pathbefore(g, g->main, g->newcwd, enter, none);
    ImGui::InputTextWithHint("##cwd", "A folder on this computer", g->newcwd, FIELD_MAX,
                             pf.flags, pathcb, &pf.cb);
    itemrect(g, RECT_NEWCWD);
    pathafter(g, &pf, g->newcwd);
    ImGui::SameLine();
    if (ImGui::Button(browse)) {
        browsestart(g, g->main);
    }
    itemnamed(g, "new browse");
    ImGui::EndDisabled();
    g->cwdoff = none;

    ImGui::Spacing();
    checkbox("Network access", &g->newnet);
    itemnamed(g, "new network");
    itemtip("Off, it reaches no network: its model calls and web searches "
                          "still go through this computer ([container] network)");
    (void)sc;
    Str image = trim({(u8 *)g->newimage, (iz)__builtin_strlen(g->newimage)});
    Str dir   = trim({(u8 *)g->newcwd, (iz)__builtin_strlen(g->newcwd)});
    return image.len && (none || dir.len);
}

static void drawnew(Gui *g)
{
    f32         sc     = g->scale;
    b32         import = g->imp.state == IMP_DIALOG;
    char const *name   = import ? "Import session" : "New session";
    if (g->opennew) {
        ImGui::OpenPopup(name);
        g->opennew = 0;
        g->newplan = 0;
        g->newnofolder = 0;
        g->newrole = {};
        if (!g->newerr.len) {
            g->newwt     = 0;  // as it was, should the engine have refused it
            g->wtname[0] = 0;
            g->wtnamed   = 0;
        }
        g->wtp.cwd = {};  // probed anew
        g->newlisted = 0;  // its host's models asked for anew
        g->newcheck  = !import;
        g->pc.shown = g->pc.refocus = 0;
        g->pc.sel = -1;
        g->newctr &= !import;
        if (g->newctr) {
            g->newnet = cfgbool(ctrset(g, "network")) == 1;
            ctrimglist(g);
        }
    }
    ImGui::SetNextWindowSize(v2(560*sc, 0));
    if (!ImGui::BeginPopupModal(name, 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    Arena a     = g->frame;
    b32   enter = modalenter();

    // Where it runs, when there is a choice: its directory is there
    // or in a new container, its models the engine's here, relayed
    b32      ctrok = !import && g->ctrrt.up && ctrlisted(g) && g->main && !g->main->ctr;
    b32      ctr   = ctrok && g->newctr;
    GuiHost *host  = !ctr && g->newhost && hostready(g, g->newhost) &&
                     (!import || hostcap(g->newhost, "import")) ? g->newhost : g->main;
    i32      ready = ctrok;
    for (GuiHost *h = g->hosts; h; h = h->next) {
        ready += hostready(g, h) && (!import || hostcap(h, "import"));
    }
    if (!import) {
        newcwdfor(g, a, host);
    }
    if (import) {
        Import *x = &g->imp;
        importfill(g, host);
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->paneldim);
        ImGui::TextWrapped("%s", cstr0(&a, x->title.len ? x->title : Str("Untitled")));
        ImGui::TextWrapped("from %s", cstr0(&a, x->path));
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    if (ready > 1) {
        ImGui::TextUnformatted("Host");
        ImGui::SetNextItemWidth(-1);
        ImVec2 at = ImGui::GetCursorScreenPos();
        Str ctrname = concat(&a, ctrtitle(g->ctrrt.name), ": a new container");
        Str shown   = ctr ? ctrname : hostlabel(g, &a, host);
        if (ImGui::BeginCombo("##host", cstr0(&a, shown))) {
            i32 n = 0;
            for (GuiHost *h = g->hosts; h; h = h->next) {
                if (!hostready(g, h) || (import && !hostcap(h, "import"))) continue;
                ImGui::PushID(h);
                if (ImGui::Selectable(cstr0(&a, hostlabel(g, &a, h)), h==host && !ctr)) {
                    g->newhost = h;
                    g->newctr  = 0;
                }
                if (n < 4) {
                    itemrect(g, RECT_HOST0 + n);
                    g->hostitems[n] = h;
                }
                n++;
                ImGui::PopID();
            }
            if (ctrok && ImGui::Selectable(cstr0(&a, ctrname), ctr)) {
                g->newctr = 1;
                g->newnet = cfgbool(ctrset(g, "network")) == 1;
                ctrimglist(g);
            }
            if (ctrok) {
                itemnamed(g, "new host ctr");
            }
            ImGui::EndCombo();
        }
        itemnamed(g, "new host");
        f32 w = ImGui::GetContentRegionAvail().x;
        g->rects[RECT_HOSTCOMBO] = {at.x, at.y, at.x+w, at.y+ImGui::GetFrameHeight()};
        ImGui::Spacing();
    }

    b32 nofolder = 0;
    b32 wt       = 0;
    b32 wtok     = 1;
    Str wtname   = {};
    if (ctr) {
        wtok = drawnewctr(g, a, &enter);
    } else {
        ImGui::TextUnformatted("Working directory");
        if (host->kind == GH_REMOTE) {
            ImGui::SameLine();
            ImGui::TextDisabled("on %s", cstr0(&a, host->name));
        }
        // Or none: a folder of the session's own, which the directory's
        // field, Browse and the recent ones then have nothing to do with
        b32 canfree  = hostcap(host, "nofolder");
        nofolder     = canfree && g->newnofolder;
        if (canfree) {
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12*sc);
            checkbox("No folder", &g->newnofolder);
            itemnamed(g, "new nofolder");
            itemtip("No project: the session works in a folder of its own, "
                                  "kept with it and deleted with it. %s, Session \xe2\x86\x92 "
                                  "New session, no folder, starts one straight away",
                                  cmdkey(g, &a, "Shift+N"));
            nofolder = g->newnofolder;
        }
        ImGui::BeginDisabled(nofolder);
        g->cwdoff = nofolder;

        // Beside it, Browse, in the platform's dialog, for a directory here
        char const *browse = "Browse\xe2\x80\xa6";
        f32 bw = ImGui::CalcTextSize(browse).x + 2*ImGui::GetStyle().FramePadding.x;
        ImGui::SetNextItemWidth(-(bw + ImGui::GetStyle().ItemSpacing.x));
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        PathField pf = pathbefore(g, host, g->newcwd, &enter, nofolder);
        ImGui::InputText("##cwd", g->newcwd, FIELD_MAX, pf.flags, pathcb, &pf.cb);
        itemrect(g, RECT_NEWCWD);
        pathafter(g, &pf, g->newcwd);
        ImGui::SameLine();
        b32 remote = host->kind == GH_REMOTE;
        g->browseoff = remote;
        ImGui::BeginDisabled(remote);
        if (ImGui::Button(browse)) {
            browsestart(g, host);
        }
        ImGui::EndDisabled();
        itemnamed(g, "new browse");
        whytip(remote ? concat(&a, "Only for a directory on this computer: this one is on ",
                               host->name)
                      : Str("Choose the directory in a dialog"));

        // Recent directories there, the most recently active first, not
        // folders of sessions' own
        Slice<GuiSession *> list   = sessrecent(g, &a, host);
        Slice<Str>          recent = {};
        for (iz i = 0; i<list.len && recent.len<8; i++) {
            Str cwd = list[i]->view && list[i]->view->cwd.len ? list[i]->view->cwd : list[i]->cwd;
            b32 dup = !cwd.len || list[i]->host!=host || sessnofolder(list[i]);
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
                // Only fills the field: a Selectable would close the dialog
                Str shown = homeshow(&a, host, recent[i]);  // ~ as the host has it
                if (ImGui::Selectable(cstr0(&a, shown), false, ImGuiSelectableFlags_NoAutoClosePopups)) {
                    fieldput(g->newcwd, shown);
                }
                if (!i) {
                    itemnamed(g, "new recent");
                }
                ImGui::PopID();
            }
        }
        ImGui::EndDisabled();

        // Or in a new worktree of its repository (worktree.cpp)
        b32 canwt  = !import && hostcap(host, "worktree");
        wt         = canwt && g->newwt && !nofolder;
        if (canwt) {
            ImGui::Spacing();
            ImGui::BeginDisabled(nofolder);
            b32 off = 0;
            checkbox("New worktree", nofolder ? &off : &g->newwt);
            ImGui::EndDisabled();
            itemnamed(g, "new worktree");
            whytip(nofolder ? Str("Not with No folder: a worktree is of a folder's repository")
                            : Str("The session works in a new git worktree of the folder's "
                                  "repository, on a branch of its own from the folder's HEAD. "
                                  "Archived or in the Trash, it is removed, if git "
                                  "calls it clean"));
            if (wt) {
                Str cwd = trim({(u8 *)g->newcwd, (iz)__builtin_strlen(g->newcwd)});
                cwd     = hostwsl(g, host) ? wslpath(&a, cwd) : cwd;
                guiwtprobe(g, host, cwd);
                ImGui::Indent();
                wtok   = wtfields(g, a, host, cwd);
                wtname = trim({(u8 *)g->wtname, (iz)__builtin_strlen(g->wtname)});
                ImGui::Unindent();
            }
        }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Model");
    if (g->newlisted!=host && hostlist(g, host)) {
        g->newlisted = host;  // asked, or else again once it can be
    }
    drawnewmodel(g, a, host);

    // A role, as the host's configuration has them, or none: the usual
    // session, with its usual tools
    Slice<GuiRole> roles = import ? Slice<GuiRole>{} :
                           host==g->main ? replyroles(&a, g->cfg) : host->roles;
    if (host != g->main) {
        hostmodels(g, host);  // its roles come with its models
    }
    b32 hasrole = 0;
    for (iz i = 0; i < roles.len; i++) {
        hasrole |= roles[i].name == g->newrole;
    }
    if (roles.len) {
        ImGui::Spacing();
        ImGui::TextUnformatted("Role");
        ImGui::SetNextItemWidth(-1);
        Str shown = hasrole ? g->newrole : Str("None");
        if (ImGui::BeginCombo("##role", cstr0(&a, shown))) {
            if (ImGui::Selectable("None", !hasrole)) {
                g->newrole = {};
            }
            itemtip("No role: the usual session, with its usual tools");
            for (iz i = 0; i < roles.len; i++) {
                GuiRole *r = &roles[i];
                ImGui::PushID((i32)i);
                if (ImGui::Selectable(cstr0(&a, r->name), r->name==g->newrole)) {
                    g->newrole = clone(&g->perm, r->name);
                }
                Buf tb(&a, 256);
                print(&tb, r->desc.len ? r->desc : Str("(no description)"));
                print(&tb, "\nTools: ");
                print(&tb, r->tools.len ? r->tools : Str("none"));
                itemtip("%s", cstr0(&a, finish(&tb)));
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        itemnamed(g, "new role");
    }
    b32 plan = !import && hostcap(host, "plan");
    if (plan) {
        // Plan mode's tools are never a role's
        ImGui::Spacing();
        ImGui::BeginDisabled(hasrole);
        b32 off = 0;
        checkbox("Plan mode", hasrole ? &off : &g->newplan);
        ImGui::EndDisabled();
        itemnamed(g, "new plan");
        whytip(hasrole ? Str("Not in a role: plan mode is for sessions without one")
                       : Str("The agent explores and writes a plan for you to approve "
                             "before it changes anything"));
    }
    if (g->newerr.len) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
        ImGui::TextWrapped("%s", cstr0(&a, g->newerr));
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(!wtok);
    b32 create = ImGui::Button(import ? "Import" : "Create", v2(120*sc, 0)) || (wtok && enter);
    ImGui::EndDisabled();
    itemrect(g, RECT_NEWCREATE);
    if (create) {
        Str cwd   = trim({(u8 *)g->newcwd, (iz)__builtin_strlen(g->newcwd)});
        cwd       = hostwsl(g, host) ? wslpath(&a, cwd) : cwd;
        Str ep    = trim({(u8 *)g->newep, (iz)__builtin_strlen(g->newep)});
        Str model = trim({(u8 *)g->newmodel, (iz)__builtin_strlen(g->newmodel)});
        Str both  = concat(&a, concat(&a, ep, "/"), model);
        g->picked = g->picked==both ? g->picked : clone(&g->perm, both);
        Str err = {};
        if (import) {
            importbegin(g, host, cwd, ep, model, nofolder);
        } else if (ctr) {
            Str image = {(u8 *)g->newimage, (iz)__builtin_strlen(g->newimage)};
            err = ctrnew(g, image, g->newws, cwd, g->newnet, ep, model,
                         hasrole ? g->newrole : Str{}, plan && g->newplan && !hasrole);
        } else {
            guinew(g, host, cwd, ep, model, hasrole ? g->newrole : Str{},
                   plan && g->newplan && !hasrole, nofolder, wt, wtname);
        }
        g->newerr = err.len ? clone(&g->perm, err) : Str{};
        if (!err.len) {
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", v2(120*sc, 0)) || modalkey(ImGuiKey_Escape)) {
        g->newerr = {};
        if (import) {
            g->imp.state = IMP_IDLE;
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Closing the window with turns, or background jobs, running asks
// first: stop them and quit, or keep them running in the background,
// this process a host of them with no window. Sessions at work on other
// hosts run on either way.
static void drawclose(Gui *g)
{
    f32 sc = g->scale;
    if (g->closereq) {
        g->closereq = 0;
        if (!g->quitting && !running(g) && !jobbing(g) && !elsewhere(g) &&
                !termbusy(g)) {
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
        i32 j = jobbing(g);
        i32 m = elsewhere(g);
        i32 t = termbusy(g);
        g->closeterms = t;
        g->closejobs  = j;
        Buf b(&a, 128);
        print(&b, "Stop ");
        print(&b, (i64)(n + j));
        print(&b, n+j==1 ? Str(" session and quit") : Str(" sessions and quit"));
        Str label = finish(&b);
        if (n) {
            ImGui::TextUnformatted("Turns are still running. Quitting stops them, and kills "
                                   "their commands; in the background they go on, with no "
                                   "window.");
        }
        if (j) {
            Buf e(&a, 256);
            print(&e, (i64)j);
            print(&e, j==1 ? Str(" session between turns has background jobs running")
                           : Str(" sessions between turns have background jobs running"));
            print(&e, ", which quitting kills. In the background they go on, and "
                      "an agent waiting for one picks up when it ends.");
            ImGui::TextUnformatted(cstr0(&a, finish(&e)));
        }
        if (!n && !j && !t) {
            ImGui::TextUnformatted("Nothing is running here any more.");
        }
        if (t) {
            Buf e(&a, 128);
            print(&e, (i64)t);
            print(&e, t==1 ? Str(" terminal tab is running a program")
                           : Str(" terminal tabs are running programs"));
            print(&e, ", which quitting hangs up.");
            ImGui::TextUnformatted(cstr0(&a, finish(&e)));
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
        b32 stop = ImGui::Button(cstr0(&a, n+j ? label : Str("Quit")));
        itemrect(g, RECT_CLOSESTOP);
        if (stop) {
            startquit(g);
        }
        if (n + j) {
            ImGui::SameLine();
            ImGui::BeginDisabled(g->whynobg.len != 0);
            b32 bg = ImGui::Button("Keep running in background");
            ImGui::EndDisabled();
            itemrect(g, RECT_CLOSEBG);
            if (g->whynobg.len) {
                itemtip("Not here: %s", cstr0(&a, g->whynobg));
            } else {
                itemtip("Close the window and its terminal tabs, and keep this "
                        "process running its sessions, unattended, with a tray icon "
                        "to bring it back");
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
    // Where the title bar's X is, for the scripted checklist: none
    // while docked, as a tab's X is elsewhere
    ImGuiWindow *win = ImGui::GetCurrentWindow();
    g->rects[RECT_OUTCLOSE] = {};
    if (!win->DockIsActive && !(win->Flags & ImGuiWindowFlags_NoTitleBar)) {
        ImGuiStyle &st = ImGui::GetStyle();
        ImRect      t  = win->TitleBarRect();
        f32         sz = ImGui::GetFontSize();
        f32         x  = t.Max.x - st.FramePadding.x - sz;
        f32         y  = t.Min.y + st.FramePadding.y;
        g->rects[RECT_OUTCLOSE] = {x, y, x+sz, y+sz};
    }
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
    Str t = finish(&o->text);
    // Copy at the right, a check mark for a moment after, as a code
    // block's (drawcodecopy)
    i64 until  = o->copiedat + COPIED_US;
    b32 copied = o->copiedat && g->sched.now<until;
    if (copied) {
        schedat(&g->sched, until + 1000, DUE_APP);
    }
    f32 right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - iconw();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", cstr0(&a, finish(&b)));
    if (o->more) {
        ImGui::SameLine();
        ImGui::BeginDisabled(o->id != 0);
        if (ImGui::Button("More")) {
            guifile(g);
        }
        ImGui::EndDisabled();
        itemtip("The next part of it");
    }
    ImGui::SameLine(right > rightof() ? right : 0);
    ImGui::BeginDisabled(!t.len);
    if (iconbutton(copied ? ICON_CHECK "###outcopy" : ICON_COPY "###outcopy")) {
        ImGui::SetClipboardText((char *)t.data);
        o->copiedat = g->sched.now;
    }
    ImGui::EndDisabled();
    itemnamed(g, "output copy");
    itemtip(o->more ? "Copy what is shown: More shows the rest"
                                  : "Copy the output");
    if (o->err.len) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
        ImGui::TextWrapped("%s", cstr0(&a, o->err));
        ImGui::PopStyleColor();
    } else if (o->id && !o->text.len) {
        ImGui::TextDisabled("Loading...");
    }
    // A read-only text field, so that the text can be selected and copied
    ImGui::PushFont(g->fonts.mono, 0);
    if (t.len) {
        ImGui::InputTextMultiline("##out", (char *)t.data, (size_t)t.len + 1, v2(-FLT_MIN, -FLT_MIN),
                                  ImGuiInputTextFlags_ReadOnly);
        g->outshown = 1;
    }
    ImGui::PopFont();
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
        itemtip("%s", cstr0(&a, finish(&b)));
    }
    return cut;
}

// Where drawimage drew its image, if it did.
struct ImgDrawn {
    b32    shown;
    ImVec2 size;  // at this size
    ImVec2 room;  // in a child this big
};

// An image whole, w by h pixels, its texture tex (null while it has
// none), at its own size in the screen's pixels, or fit to the room
// left, larger or smaller. Fit comes first, its tooltip fittip, ahead of
// a description that
// shortens to the width, so a narrow window keeps it; then a note, in
// red if bad, as "Loading..." or why it cannot be shown. The Screenshot
// window's, and a Files tab's for an image file.
static ImgDrawn drawimage(Gui *g, GuiTex *tex, i32 w, i32 h, b32 *fit, Str desc,
                          Str fittip, Str note = {}, b32 bad = 0)
{
    Arena    a = g->frame;
    ImgDrawn r = {};
    checkbox("Fit", fit);
    itemnamed(g, "Fit");
    itemtip("%s", cstr0(&a, fittip));
    ImGui::SameLine();
    textfit(a, desc, ImGui::GetContentRegionAvail().x,
            ImGui::GetColorU32(ImGuiCol_TextDisabled));
    if (note.len && bad) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
        ImGui::TextWrapped("%s", cstr0(&a, note));
        ImGui::PopStyleColor();
    } else if (note.len) {
        ImGui::TextDisabled("%s", cstr0(&a, note));
    }
    ImGui::BeginChild("##shot", v2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (tex && w>0 && h>0) {
        f32    fb   = ImGui::GetIO().DisplayFramebufferScale.x;
        ImVec2 size = v2((f32)w/(fb>0 ? fb : 1), (f32)h/(fb>0 ? fb : 1));
        ImVec2 room = ImGui::GetContentRegionAvail();
        if (*fit) {
            f32 s = room.x/size.x < room.y/size.y ? room.x/size.x : room.y/size.y;
            s    = s>0 ? s : 1;
            size = v2(size.x*s, size.y*s);
        }
        ImGui::Image(tex->td.GetTexRef(), size);
        r.shown = 1;
        r.size  = size;
        r.room  = room;
    }
    ImGui::EndChild();
    return r;
}

// The Screenshot window: a screenshot whole (drawimage). The window's
// own X closes it, as Esc does.
static void drawshot(Gui *g)
{
    // From g->frame itself: drawimage takes a copy of it for scratch
    Arena   *a = &g->frame;
    ImgView *v = &g->img.view;
    if (!v->s) {
        return;
    }
    // Where the title bar's X is, for the scripted checklist: none
    // while docked, as a tab's X is elsewhere
    ImGuiWindow *win = ImGui::GetCurrentWindow();
    g->rects[RECT_SHOTCLOSE] = {};
    if (!win->DockIsActive && !(win->Flags & ImGuiWindowFlags_NoTitleBar)) {
        ImGuiStyle &st = ImGui::GetStyle();
        ImRect      t  = win->TitleBarRect();
        f32         sz = ImGui::GetFontSize();
        f32         x  = t.Max.x - st.FramePadding.x - sz;
        f32         y  = t.Min.y + st.FramePadding.y;
        g->rects[RECT_SHOTCLOSE] = {x, y, x+sz, y+sz};
    }
    b32 close = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                ImGui::IsKeyPressed(ImGuiKey_Escape, false);

    i32 w = v->tex ? v->tex->td.Width  : v->w;
    i32 h = v->tex ? v->tex->td.Height : v->h;
    Buf b(a, 512);
    print(&b, imgstr(&v->path));
    print(&b, " of ");
    print(&b, sesstitle(v->s));
    if (v->s->host->kind != GH_ENGINE) {
        print(&b, ", on ");
        print(&b, hostlabel(g, a, v->s->host));
    }
    print(&b, ": ");
    print(&b, (i64)w);
    putbyte(&b, 'x');
    print(&b, (i64)h);
    print(&b, " pixels");
    Str desc = finish(&b);
    Str note = v->state==IMG_FAILED ? concat(a, "Cannot show the screenshot: ", imgstr(&v->err)) :
               v->state!=IMG_READY  ? Str("Loading...") : Str{};
    ImgDrawn d = drawimage(g, v->state==IMG_READY ? v->tex : 0, w, h, &v->fit, desc,
                           "Scale it to fit the window", note, v->state==IMG_FAILED);
    if (d.shown) {
        g->shotshown = 1;
        g->shotsize  = d.size;
        g->shotroom  = d.room;
    }
    if (close) {
        imgclose(g);
    }
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


static b32  cfghas(Gui *, Str, Str);
static void cfgselect(Gui *, Str, Str);

// Settings shown, with its New section dialog open for kind, a string
// that outlives the frame.
static void addsection(Gui *g, Str kind)
{
    g->addkind = kind;
    g->openadd = 1;
    setpane(g, PANE_SETTINGS, 1);
}

// Hosts

// A configured remote host's line: a dot in the color of its connection,
// its name and how it is, cut to leave room for Connect or Disconnect,
// and why it failed. The button's rectangle goes to RECT_* rect, if not
// negative. Its scratch is a, after what the caller keeps there (name).
static void drawremote(Gui *g, Arena a, Str name, i32 rect)
{
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
    // Why it is down or failed, in its tooltip: a failure a click began
    // is told in the host toast (drawhosttoast), so nothing here moves
    Str why_ = h && h->msg.len && h->state!=RS_UP ? h->msg : Str{};
    ImGui::AlignTextToFramePadding();
    textfit(a, finish(&b), ImGui::GetContentRegionAvail().x - bw - st.ItemSpacing.x, 0, why_);
    ImGui::SameLine();
    ImGui::BeginDisabled(closing && h->reconnect);
    b32 click = ImGui::SmallButton(label);
    ImGui::EndDisabled();
    if (rect >= 0) {
        itemrect(g, rect);
    }
    itemnamed(g, concat(&a, "connect ", name));
    itemtip("%s", live ? "Leave it: its sessions run on there" :
                                closing ? "Connect again once it has let go" :
                                          connecttip(g, name));
    if (click && live) {
        hostleave(g, h);
    } else if (click) {
        remoteconnect(g, a, name);
    }
    // An unconfigured WSL distro: settings of its own begin a section,
    // which says what it was until then
    LinkTo *auto_ = rect!=RECT_PAGECONNECT ? wslautohost(g, name) : 0;
    if (auto_ && !cfghas(g, "remote", name)) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Settings")) {
            guiconfigset(g, "remote", name, "wsl", auto_->wsl);
            cfgselect(g, "remote", name);
            g->cfgshowsel = 1;
            setpane(g, PANE_SETTINGS, 1);
        }
        itemnamed(g, concat(&a, "settings ", name));
        itemtip("Give it a [remote %s] section of its own, with "
                              "wsl = %s, and show it in Settings",
                              cstr0(&a, name), cstr0(&a, auto_->wsl));
    }
    if (h && live && h->thin) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->dim);
        ImGui::TextWrapped("No endpoint of its own: its sessions run on this "
                           "computer's models");
        ImGui::PopStyleColor();
        itemtip("Its configuration defines no endpoint, so this "
                              "computer makes its model calls, with its own "
                              "endpoints and keys, while connected. Define an "
                              "endpoint there to make it independent.");
    }
    // Deploying's progress, then Deploy engine whenever not connected, or
    // Update engine for an older engine there
    if (h && h->updating!=UPD_NONE && h->updmsg.len && !h->deployfail) {
        ImGui::PushStyleColor(ImGuiCol_Text, h->updating==UPD_FAILED ? g->pal->bad
                                                                     : g->pal->dim);
        ImGui::TextWrapped("%s", cstr0(&a, h->updmsg));
        ImGui::PopStyleColor();
    }
    // Not with a path in its section, which names the engine there
    Str why = deploywhy(g, h, name);
    if (!live && !closing && !(h && h->r) && !remoteset(g, name, "path").len) {
        ImGui::BeginDisabled(why.len > 0);
        if (ImGui::SmallButton("Deploy engine")) {
            guideploy(g, name);
        }
        ImGui::EndDisabled();
        itemnamed(g, concat(&a, "deploy engine ", name));
        itemtip("%s", why.len ? cstr0(&a, why)
                                            : "Copy this version's engine there, as "
                                              "prospero deploy does, then connect to it");
    } else if (live && hostolder(h)) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->amber);
        ImGui::TextWrapped("The engine there is older than this program");
        ImGui::PopStyleColor();
        ImGui::BeginDisabled(why.len > 0);
        if (ImGui::SmallButton("Update engine")) {
            guiupdate(g, h);
        }
        ImGui::EndDisabled();
        itemnamed(g, concat(&a, "update engine ", name));
        itemtip("%s", why.len ? cstr0(&a, why)
                                            : "Deploy this version's engine there, then "
                                              "start it once nothing runs");
    }
    ImGui::PopID();
}

// This process's engine, the background hosts on this machine, and the
// remote ones of the configuration: how each is, connecting and leaving
// them, and Add remote, which opens Settings' dialog for one.
static void drawhosts(Gui *g)
{
    Arena a     = g->frame;
    i64   nowms = os_time(g->os);
    if (g->enginekind == ENGINE_FAKE) {
        ImGui::TextDisabled("No engine: the fake agent has no hosts.");
        return;
    }
    if (iconbutton(ICON_REFRESH "##lookagain")) {
        findhosts(g, a);
        findwsl(g);
        guiconfigget(g);
        ctrlook(g);
    }
    itemnamed(g, "hosts look again");
    itemtip("Look again: find background hosts, WSL distros and Docker or "
                          "Podman again, and read the remotes from the configuration");

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
        itemtip("Stop its sessions' work, and the host");
        ImGui::PopID();
    }

    // Remotes, as configured, and WSL distros
    ImGui::SeparatorText("Remote hosts");
    Slice<Str> names = remotenames(g, &a);
    for (GuiHost *h = g->hosts; h; h = h->next) {
        b32 seen = h->kind!=GH_REMOTE || h->ctr || h->taskexec.len;
        for (iz n = 0; !seen && n < names.len; n++) {
            seen = names[n] == h->name;
        }
        if (!seen) {
            names = push(&a, names, h->name);  // gone from the file since
        }
    }
    if (!names.len) {
        ImGui::TextDisabled("None yet");
    }
    for (iz n = 0; n < names.len; n++) {
        // The first two's buttons, for the checklist
        drawremote(g, a, names[n], n<2 ? RECT_CONNECT+(i32)n : -1);
    }
    // A new one is a section of the configuration, added in Settings
    if (ImGui::SmallButton("Add remote...")) {
        addsection(g, "remote");
    }
    itemnamed(g, "hosts add remote");
    itemtip("Add a [remote NAME] section in Settings: a machine reached over "
            "ssh, or by a command that runs another there");

    // Containers made for sessions, and the runtime that runs them
    if (g->ctrrt.path.len || g->ctrs) {
        ImGui::SeparatorText("Containers");
        Buf rb(&a, 256);
        print(&rb, ctrtitle(g->ctrrt.name));
        if (!g->ctrrt.path.len) {
            print(&rb, ": not found");
        } else if (g->ctrrt.up) {
            print(&rb, " ");
            print(&rb, g->ctrrt.version);
            print(&rb, ", at ");
            print(&rb, g->ctrrt.path);
        } else {
            print(&rb, ": ");
            print(&rb, g->ctrprobed ? g->ctrrt.why : Str("looking"));
        }
        ImGui::TextWrapped("%s", cstr0(&a, finish(&rb)));
        for (GuiCtr *c = g->ctrs; c; c = c->next) {
            if (c->removed || !c->h) continue;
            ImGui::PushID(c);
            ImGui::Bullet();
            Buf b(&a, 256);
            print(&b, hostlabel(g, &a, c->h));
            print(&b, ": ");
            print(&b, hoststatus(g, &a, c->h, 0));
            print(&b, ", ");
            print(&b, ctrworkspaces[c->c.workspace]);
            if (c->c.folder.len) {
                print(&b, " ");
                print(&b, c->c.folder);
            }
            print(&b, c->c.network ? Str(", network") : Str(", no network"));
            ImGuiStyle &st = ImGui::GetStyle();
            f32         bw = ImGui::CalcTextSize("Delete").x + 2*st.FramePadding.x;
            textfit(a, finish(&b), ImGui::GetContentRegionAvail().x - bw - st.ItemSpacing.x, 0);
            if (c->err.len && ImGui::IsItemHovered()) {
                tipnow("%s", cstr0(&a, c->err));
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete")) {
                g->delctr     = c;
                g->opendelctr = 1;
            }
            itemnamed(g, "hosts delctr");  // the checklist's, its only one
            itemtip("Remove it and its sessions for good");
            ImGui::PopID();
        }
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
    CFG_TWOPANE = 480,      // width at scale 1 for the list beside the page
    CFG_MINVAL  = 160,      // the value column's least, at scale 1, for a
                            // column of where each is set beside it
    CFG_MARK_US = 1500000,  // how long a search's setting stays marked
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
// and booleans (true or false) are picked from their choices, paths
// edited one a line, and the rest typed. The checklist's target row
// records its field's rectangle (RECT_*, or -1 for none).
// A setting's tooltip: its description, type and default.
static Str cfgrowtip(Arena *a, CfgRow *r)
{
    Buf b(a, 128);
    print(&b, r->desc);
    print(&b, "\n(");
    print(&b, r->type);
    print(&b, ", default ");
    print(&b, r->dflt.len ? r->dflt : Str("none"));
    putbyte(&b, ')');
    return finish(&b);
}

// Whether a row is the setting a search result went to.
static b32 cfgfocused(Gui *g, CfgRow *r)
{
    return g->cfgfkey.len && r->key==g->cfgfkey && r->kind==g->cfgfkind &&
           r->name==g->cfgfname;
}

static void drawcfgrow(Gui *g, CfgRow *r, i32 target, b32 wide)
{
    Arena a = g->frame;
    ImGui::TableNextRow();
    b32 focus = cfgfocused(g, r);
    if (focus && g->sched.now<g->cfgmarkend) {
        // Marked a while, fading, so that the eye finds it
        f32 t = (f32)(g->cfgmarkend - g->sched.now) / (f32)CFG_MARK_US;
        t = t<1 ? t : 1;
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                               ImGui::GetColorU32(ImGuiCol_Header, t));
        schedat(&g->sched, g->sched.now + 16000, DUE_APP);
    }
    ImGui::TableNextColumn();
    Str tip  = cfgrowtip(&a, r);
    f32 cw   = ImGui::GetContentRegionAvail().x;
    Str from = r->valid ? provenance(g, &a, r, os_time(g->os)) : Str("invalid: default used");
    u32 fc   = r->valid ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : g->pal->bad;
    ImGui::AlignTextToFramePadding();
    textfit(a, r->key, cw, 0, tip);
    ImVec2 rowtop = ImGui::GetItemRectMin();
    if (!wide) {
        textfit(a, from, cw, fc);
    }
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1);
    // By what it is: the rows are made anew by each config.get, as after
    // an edit goes out, and an id by address would take the focus from
    // the field gone to meanwhile
    Str id = concat(&a, concat(&a, concat(&a, r->kind, "\x1f"), concat(&a, r->name, "\x1f")),
                    r->key);
    ImGui::PushID(id.cdata, id.cdata+id.len);
    if (r->choices.len) {
        // A pick goes out at once; the theme and how reasoning shows
        // and counts apply even before the file has them
        // and a boolean shows as true or false, however it was written
        b32    theme = r->kind=="ui" && r->key=="theme";
        ImVec2 at    = ImGui::GetCursorScreenPos();
        Str    pick  = {};
        i32    yes   = r->type=="bool" ? cfgbool(trim(r->value)) : -1;
        Str    shown = yes==1 ? Str("true") : yes==0 ? Str("false") : r->value;
        // Named for the checklist: "cfg KIND NAME KEY", and its choices
        // "cfg KIND NAME KEY=CHOICE"
        Str    named = concat(&a, concat(&a, concat(&a, "cfg ", r->kind), " "),
                              concat(&a, concat(&a, r->name, " "), r->key));
        f32    fw    = ImGui::GetContentRegionAvail().x;
        if (ImGui::BeginCombo("##v", cstr0(&a, shown))) {
            for (iz i = 0; i < r->choices.len; i++) {
                Str c = r->choices[i];
                if (ImGui::Selectable(cstr0(&a, c), c==shown)) {
                    pick = c;
                }
                if (theme && i<THEME_COUNT) {
                    itemrect(g, RECT_THEME0+(i32)i);
                }
                itemnamed(g, concat(&a, concat(&a, named, "="), c));
            }
            ImGui::EndCombo();
        }
        namedrect(g, named, at, v2(at.x+fw, at.y+ImGui::GetFrameHeight()));
        if (target >= 0) {
            f32 w = ImGui::GetContentRegionAvail().x;
            g->rects[target] = {at.x, at.y, at.x+w, at.y+ImGui::GetFrameHeight()};
        }
        if (pick.len && pick!=shown && !(r->sent && pick==r->sentval)) {
            if (theme) {
                settheme(g, pick);
            } else if (r->kind=="ui" && r->key=="thinking") {
                setthinking(g, thinkparse(pick));
            } else if (r->kind=="ui" && r->key=="tools") {
                settools(g, toolsparse(pick));
            } else if (r->kind=="ui" && r->key=="count") {
                setcount(g, countparse(pick));
            }
            r->sent    = 1;
            r->sentval = clone(&g->cfgmem, pick);
            guiconfigset(g, r->kind, r->name, r->key, pick);
            if (r->kind=="endpoint" && r->key=="kind" && r->name.len) {
                epurlfollow(g, r->name, pick);
            }
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
        r->typed |= ImGui::IsItemEdited();
        r->active = ImGui::IsItemActive();
        if (target >= 0) {
            itemrect(g, target);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            r->typed = 0;
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
        // A theme's color: its base's as the hint, and a swatch after the
        // field of the color typed, else the base's
        u32 swatch = 0;
        f32 sw     = ImGui::GetFrameHeight();
        if (r->kind=="theme" && r->type=="color") {
            Palette base = themebuild(cfgrowval(g, "theme", r->name, "base"),
                                      [](Str) { return Str{}; });
            for (iz i = 0; i < countof(themekeys); i++) {
                if (themekeys[i].key == r->key) {
                    swatch = *themecolor(&base, themekeys[i].off);
                }
            }
            if (swatch) {
                Buf h(&a, 8);
                putbyte(&h, '#');
                for (i32 s = 0; s < 24; s += 8) {
                    printhex(&h, swatch>>s & 255, 2);
                }
                hint = finish(&h);
            }
            CfgColor typed = cfgcolor({(u8 *)r->buf, (iz)__builtin_strlen(r->buf)});
            swatch = typed.ok ? rgb(typed.r, typed.g, typed.b) : swatch;
            ImGui::SetNextItemWidth(-(sw + ImGui::GetStyle().ItemSpacing.x));
        }
        b32 enter = ImGui::InputTextWithHint("##v", hint.len ? cstr0(&a, hint) : "", r->buf, (uz)r->cap,
                                             ImGuiInputTextFlags_EnterReturnsTrue|ImGuiInputTextFlags_AutoSelectAll);
        if (swatch) {
            ImVec2 fmin = ImGui::GetItemRectMin();
            ImVec2 fmax = ImGui::GetItemRectMax();
            ImVec2 p0   = v2(fmax.x + ImGui::GetStyle().ItemSpacing.x, fmin.y);
            ImVec2 p1   = v2(p0.x + sw, fmin.y + sw);
            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p0, p1, swatch, 3*g->scale);
            dl->AddRect(p0, p1, g->pal->rule, 3*g->scale);
        }
        itemnamed(g, concat(&a, concat(&a, concat(&a, "cfg ", r->kind), " "),
                            concat(&a, concat(&a, r->name, " "), r->key)));
        r->typed |= ImGui::IsItemEdited();
        r->active = ImGui::IsItemActive();
        if (target >= 0) {
            itemrect(g, target);
        }
        if (enter || ImGui::IsItemDeactivatedAfterEdit()) {
            r->typed = 0;
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
    if (focus) {
        // Scrolled to once if not already in view, but not in the
        // page's first frame, whose places are not settled
        ImGuiWindow *w   = ImGui::GetCurrentWindow();
        f32          bot = ImGui::GetItemRectMax().y;
        g->cfgfseen = rowtop.y>=w->InnerRect.Min.y && bot<=w->InnerRect.Max.y;
        if (g->cfgfscroll && ImGui::IsWindowAppearing()) {
            scheddirty(&g->sched);
        } else if (g->cfgfscroll) {
            if (!g->cfgfseen) {
                ImGui::SetScrollFromPosY(rowtop.y - w->Pos.y, 0.25f);
            }
            g->cfgfscroll = 0;
            g->cfgmarkend = g->sched.now + CFG_MARK_US;
        }
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

// What a page is called for the checklist and in the combo: "endpoint
// local" for a named section, else its kind, "" for the top level.
static Str cfgpagekey(Arena *a, Str kind, Str name)
{
    return name.len ? concat(a, concat(a, kind, " "), name) : kind;
}

// One page in the list: selected on a click. In the combo its label is
// whole, the popup as wide as it needs; in the list it is cut to fit,
// whole in its tooltip then. Its rectangle goes to rect, if not null.
// Its scratch is a, after what the caller keeps there (label).
static b32 cfgitem(Gui *g, Arena a, b32 combo, Str label, b32 selected, Str tip,
                   u32 color, Rect *rect)
{
    b32 click = 0;
    if (color) {
        ImGui::PushStyleColor(ImGuiCol_Text, color);
    }
    if (combo) {
        click = ImGui::Selectable(cstr0(&a, label), selected);
        if (tip.len) {
            itemtip("%s", cstr0(&a, tip));
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
            itemtip("%s", cstr0(&a, t));
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

static void cfgheading(Gui *g, Arena a, b32 combo, Str label)
{
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
    cfgheading(g, a, combo, "General");
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
        Str label = cfgkindlabel(&a, kind, 0);
        if (cfgitem(g, a, combo, label, sel, tip, 0, first ? &first->page : 0)) {
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
        Str heading = cfgkindlabel(&a, kind, 1);
        cfgheading(g, a, combo, heading);
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
            if (cfgitem(g, a, combo, r->name, sel, {}, 0, &r->page)) {
                cfgselect(g, kind, r->name);
            }
            cfgreveal(g, cfgpagekey(&a, kind, r->name), sel);
            ImGui::PopID();
        }
        if (kind == "mcp") {
            // Projects' servers no section names yet, each name once
            if (hostcan(g->main, "mcp.auth")) {
                mcplisting(g);
            }
            b32 head = 0;
            for (Json *s = g->mcplist ? g->mcplist->child : 0; s; s = s->next) {
                Str  pn  = jsonstr(jsonget(s, "name"));
                b32  dup = jsonstr(jsonget(s, "source"))!="project" || !pn.len ||
                           cfghas(g, kind, pn);
                for (Json *o = g->mcplist->child; !dup && o != s; o = o->next) {
                    dup = jsonstr(jsonget(o, "source"))=="project" &&
                          jsonstr(jsonget(o, "name"))==pn;
                }
                if (dup) {
                    continue;
                }
                if (!head && !combo) {
                    ImGui::TextDisabled("In projects");
                }
                head = 1;
                b32 sel = !g->cfgdiag && g->cfgpkind==kind && g->cfgpname==pn;
                pushstr(concat(&a, "project ", pn));
                if (cfgitem(g, a, combo, pn, sel, "A server of projects' .mcp.json: "
                            "its page turns its tools on or off, for every "
                            "project's server of this name", 0, 0)) {
                    cfgselect(g, kind, pn);
                }
                cfgreveal(g, cfgpagekey(&a, kind, pn), sel);
                ImGui::PopID();
            }
        }
        Buf ab(&a, 64);
        print(&ab, "Add ");
        print(&ab, kind=="mcp" ? Str("MCP server") : kind);
        print(&ab, "...");
        Rect at   = {};
        Str  what = finish(&ab);
        b32  add  = cfgitem(g, a, combo, what, 0, jsonstr(jsonget(k, "desc")),
                            ImGui::GetColorU32(ImGuiCol_TextDisabled), &at);
        if (group < 7) {
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
    Rect at    = {};
    Str  label = finish(&db);
    if (cfgitem(g, a, combo, label, g->cfgdiag, "Problems found in the files, "
                "with where each is", color, &at)) {
        g->cfgdiag = 1;
    }
    cfgreveal(g, "diagnostics", g->cfgdiag);
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

// A setting's page as a search result names it: "Interface", or
// "Endpoint local" for a named section.
static Str cfgfindpage(Arena *a, Str kind, Str name)
{
    Str label = cfgkindlabel(a, kind, 0);
    return name.len ? concat(a, concat(a, label, " "), name) : label;
}

// The settings a search finds, as indexes into cfgrows: the best
// matches first (cfgfindrank), and among equals in the page list's
// order, General's pages before the named sections.
static Slice<i32> cfgfindlist(Gui *g, Arena *a, Str query)
{
    Slice<i32> r = {};
    if (!g->cfgrows.len) {
        return r;
    }
    i32 *ranks = alloc<i32>(a, g->cfgrows.len);
    for (iz i = 0; i < g->cfgrows.len; i++) {
        Arena   t    = *a;
        CfgRow *row  = &g->cfgrows[i];
        Str     page = concat(&t, concat(&t, cfgfindpage(&t, row->kind, row->name), " "),
                              row->kind);
        ranks[i] = cfgfindrank(row->key, row->desc, page, query);
    }
    r.data = alloc<i32>(a, g->cfgrows.len);
    for (i32 rank = 1; rank <= 6; rank++) {
        for (i32 named = 0; named < 2; named++) {
            for (iz i = 0; i < g->cfgrows.len; i++) {
                if (ranks[i]==rank && (g->cfgrows[i].name.len>0)==named) {
                    r.data[r.len++] = (i32)i;
                }
            }
        }
    }
    r.cap = r.len;
    return r;
}

// Go to a setting: its page, scrolled to it, marked a while.
static void cfggoto(Gui *g, CfgRow *r)
{
    cfgselect(g, r->kind, r->name);
    if (r->kind.len+r->name.len+r->key.len <= 3*FIELD_MAX) {
        u8 *p = g->cfgfbuf;
        copybytes(p, r->kind.data, r->kind.len);
        g->cfgfkind = {p, r->kind.len};
        p += r->kind.len;
        copybytes(p, r->name.data, r->name.len);
        g->cfgfname = {p, r->name.len};
        p += r->name.len;
        copybytes(p, r->key.data, r->key.len);
        g->cfgfkey  = {p, r->key.len};
    }
    g->cfgfscroll = 1;
    g->cfgfseen   = 0;
    g->cfgfindhid = !g->cfgtwopane;
    scheddirty(&g->sched);
}

// Go where a settings: link points (cfglink, followlink): Diagnostics, a
// section's page, or a setting on it, a named kind's first section if
// the link names none. One not there may be in a file changed since the
// configuration came, so it is asked for once more, then the link's
// words go in the search, which lists what is nearest.
static void cfgfollow(Gui *g)
{
    Dest *d    = &g->cfglink;
    Str   kind = d->cfgkind;
    Str   name = d->cfgname;
    if (d->whole || d->diag) {
        g->cfgdiag  |= d->diag;
        g->cfglinked = 0;
        return;
    }
    for (iz i = 0; i<g->cfgrows.len && d->named && !name.len; i++) {
        name = g->cfgrows[i].kind==kind ? g->cfgrows[i].name : name;
    }
    b32     page = 0;
    CfgRow *row  = 0;
    for (iz i = 0; i<g->cfgrows.len && !row; i++) {
        CfgRow *r = &g->cfgrows[i];
        if (r->kind==kind && r->name==name) {
            page = 1;
            row  = d->cfgkey.len && r->key==d->cfgkey ? r : 0;
        }
    }
    if (d->known && page && (row || !d->cfgkey.len)) {
        if (row) {
            cfggoto(g, row);
        } else {
            cfgselect(g, kind, name);
            g->cfgshowsel = 1;
        }
    } else if (!g->cfglinkget) {
        g->cfglinkget = 1;
        guiconfigget(g);
        return;
    } else {
        iz n = d->find.len<FIELD_MAX ? d->find.len : FIELD_MAX-1;
        copybytes(g->cfgfind, d->find.data, n);
        g->cfgfind[n] = 0;
        g->cfgfindhid = 0;
    }
    g->cfglinked = 0;
    scheddirty(&g->sched);
}

// The search's results, in place of the page list: each a setting's
// key over its page, dimmed, with the setting's tooltip. Enter in the
// field (enter) goes to the first. Its scratch is a, after found.
static void drawcfgfind(Gui *g, Arena a, Slice<i32> found, b32 enter)
{
    if (!found.len) {
        ImGui::TextDisabled("No setting matches");
        return;
    }
    if (enter) {
        cfggoto(g, &g->cfgrows[found[0]]);
    }
    f32 lh  = ImGui::GetTextLineHeight();
    f32 gap = ImGui::GetStyle().ItemSpacing.y;
    u32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    for (iz i = 0; i < found.len; i++) {
        Arena   t    = a;
        CfgRow *r    = &g->cfgrows[found[i]];
        Str     page = cfgfindpage(&t, r->kind, r->name);
        ImVec2  p    = ImGui::GetCursorScreenPos();
        f32     w    = ImGui::GetContentRegionAvail().x;
        ImGui::PushID((int)i);
        if (ImGui::Selectable("##found", cfgfocused(g, r), 0, v2(0, 2*lh + gap/2))) {
            cfggoto(g, r);
        }
        itemtip("%s", cstr0(&t, cfgrowtip(&t, r)));
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImGui::RenderTextEllipsis(dl, p, v2(p.x+w, p.y+lh), p.x+w,
                                  r->key.cdata, r->key.cdata+r->key.len, 0);
        ImGui::PushStyleColor(ImGuiCol_Text, dim);
        ImVec2 q = v2(p.x, p.y + lh + gap/2);
        ImGui::RenderTextEllipsis(dl, q, v2(p.x+w, q.y+lh), p.x+w,
                                  page.cdata, page.cdata+page.len, 0);
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
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
        } else if (kind=="ui" && r->key=="notify") {
            target = RECT_NOTIFY;
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
    itemtip("Sent in chat_template_kwargs: true and false as booleans, "
                          "numbers as numbers, the rest, or anything in quotes, as a "
                          "string; empty removes what the model inherits");
    if (set) {
        Str name = trim({(u8 *)g->tplname, (iz)__builtin_strlen(g->tplname)});
        Str val  = trim({(u8 *)g->tplval, (iz)__builtin_strlen(g->tplval)});
        Str key  = startswith(name, "template.") ? name : concat(&a, "template.", name);
        if (!name.len) {
            cfgsay(g, "A template argument needs a name", 0);
        } else {
            guiconfigset(g, "model", model, key, val.len ? val : Str(""));
            g->tplname[0] = g->tplval[0] = 0;
        }
    }
}

// Text as Markdown shows it as it is: every ASCII punctuation mark
// escaped, as CommonMark allows any to be.
static void putmdtext(Buf *b, Str s)
{
    for (iz i = 0; i < s.len; i++) {
        u8 c = s[i];
        if ((c>='!' && c<='/') || (c>=':' && c<='@') || (c>='[' && c<='`') ||
                (c>='{' && c<='~')) {
            putbyte(b, '\\');
        }
        putbyte(b, c);
    }
}

// Lines as a fenced code block, its fence longer than any run of
// backticks in them.
static Str mdcodeblock(Arena *a, Slice<Str> lines)
{
    iz longest = 0;
    iz bytes   = 0;
    for (iz i = 0; i < lines.len; i++) {
        bytes += lines[i].len + 1;
        for (iz k = 0, run = 0; k < lines[i].len; k++) {
            run     = lines[i][k]=='`' ? run+1 : 0;
            longest = run>longest ? run : longest;
        }
    }
    iz  n = longest<3 ? 3 : longest+1;
    Buf b(a, bytes + 2*n + 4);
    for (iz i = 0; i < n; i++) putbyte(&b, '`');
    putbyte(&b, '\n');
    for (iz i = 0; i < lines.len; i++) {
        print(&b, lines[i]);
        putbyte(&b, '\n');
    }
    for (iz i = 0; i < n; i++) putbyte(&b, '`');
    putbyte(&b, '\n');
    return finish(&b);
}

// "Copy all" on the line with what is to its left: copies text.
static void copyallbutton(Gui *g, Arena a, Str text, char const *name, char const *tip)
{
    ImGui::SameLine();
    ImGui::BeginDisabled(!text.len);
    if (ImGui::SmallButton(cstr0(&a, concat(&a, "Copy all##", Str{(u8 *)name, (iz)__builtin_strlen(name)})))) {
        ImGui::SetClipboardText(cstr0(&a, text));
    }
    ImGui::EndDisabled();
    itemnamed(g, Str{(u8 *)name, (iz)__builtin_strlen(name)});
    itemtip("%s", tip);
}

// The configuration's diagnostics, compiler style, as a document whose
// text is selectable and copied as the Plan's is (issue #36), each led
// by its severity in bold, and Copy all for every one of them as text,
// to paste where they are wanted.
static void drawcfgdiags(Gui *g)
{
    Arena a     = g->frame;
    Json *diags = jsonget(g->cfg, "diags");
    Buf   plain(&a, 1024);
    Buf   md(&a, 1024);
    for (Json *d = diags->child; d; d = d->next) {
        Str sev = jsonstr(jsonget(d, "severity"));
        Buf b(&a, 256);
        print(&b, jsonstr(jsonget(d, "file")));
        putbyte(&b, ':');
        print(&b, jsonint(jsonget(d, "line"), 0));
        putbyte(&b, ':');
        print(&b, jsonint(jsonget(d, "col"), 0));
        print(&b, ": ");
        Str where = finish(&b);
        Str msg   = jsonstr(jsonget(d, "message"));
        print(&plain, where);
        print(&plain, sev);
        print(&plain, ": ");
        print(&plain, msg);
        putbyte(&plain, '\n');
        print(&md, "**");
        print(&md, sev);
        print(&md, "** ");
        putmdtext(&md, where);
        putmdtext(&md, msg);
        print(&md, "\n\n");
    }
    Str text = finish(&plain);
    Str doc  = finish(&md);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Diagnostics");
    copyallbutton(g, a, text, "diagnostics copy",
                  "Copy every diagnostic as text, a line each");
    if (!jsonlen(diags)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("None: every line of every file was understood.");
        ImGui::PopTextWrapPos();
        return;
    }
    if (!g->diagdoc) {
        g->diagdoc = newdocview(&g->perm, g->os, g->vcache);
    }
    if (hash(doc) != g->diaghash) {
        docset(g->diagdoc, doc);
        g->diaghash = hash(doc);
    }
    Str link = drawdoc(g, g->diagdoc, "##diags");
    if (link.len) {
        followlink(g, link);
    }
}

// What a section a web include gives the url of would send there, on its
// page, with Approve for each url: true if anything waits.
static b32 drawapprove(Gui *g, Arena a, Str kind, Str name)
{
    apprlisting(g);
    b32 any = 0;
    for (Json *x = g->apprlist ? g->apprlist->child : 0; x; x = x->next) {
        if (jsonstr(jsonget(x, "kind"))!=kind || jsonstr(jsonget(x, "name"))!=name) {
            continue;
        }
        Str url = jsonstr(jsonget(x, "url"));
        b32 seen = 0;
        for (Json *y = g->apprlist->child; y != x; y = y->next) {
            seen |= jsonstr(jsonget(y, "kind"))==kind && jsonstr(jsonget(y, "name"))==name &&
                    jsonstr(jsonget(y, "url"))==url;
        }
        if (seen) {
            continue;
        }
        Buf what(&a, 256);
        for (Json *y = x; y; y = y->next) {
            if (jsonstr(jsonget(y, "kind"))==kind && jsonstr(jsonget(y, "name"))==name &&
                    jsonstr(jsonget(y, "url"))==url) {
                print(&what, what.len ? Str(", ") : Str(""));
                print(&what, jsonstr(jsonget(y, "what")));
            }
        }
        any = 1;
        ImGui::PushID(cstr0(&a, url));
        ImGui::TextWrapped("A web include gives this url, which nothing goes "
                           "to until you approve what your configuration would "
                           "send it (%s):", cstr0(&a, finish(&what)));
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->amber);
        ImGui::TextWrapped("%s", cstr0(&a, url));
        ImGui::PopStyleColor();
        itemnamed(g, concat(&a, concat(&a, kind, Str(" url ")), name));
        b32 yes = ImGui::SmallButton("Approve");
        itemnamed(g, concat(&a, concat(&a, kind, Str(" approve ")), name));
        itemtip("Send these to this url, for every session, "
                              "until a web include changes the url");
        if (yes) {
            mcpcmd(g, CMD_APPROVE, name, url);
        }
        ImGui::PopID();
    }
    return any;
}

// Server NAME's tools on its page, as the engine last listed them
// (mcptlisting): the configuration's server's, or with only settings for
// projects' servers of the name, each running one's under its file.
// A checkbox turns one on or off, rewriting [mcp NAME] tools (mcptset);
// List tools starts a server of the configuration that never listed.
static void drawmcptools(Gui *g, Arena a, Str name, Json *m)
{
    if (!hostcan(g->main, "mcp.auth")) {
        return;
    }
    mcptlisting(g, name);
    b32 proj = !m || jsonstr(jsonget(m, "source"))=="overlay";
    b32 word = iniword(name, 0);

    // The entries with tools, and the names over all of them
    Slice<Json *> ents  = {};
    Slice<Str>    names = {};
    i64           kept  = 0;
    b32           stale = 0;
    b32           trunc = 0;
    i64           more  = 0;
    b32           incl  = 0;
    Str           cur   = mcptoolsnow(g, name, &incl);
    for (Json *s = g->mcptlist ? g->mcptlist->child : 0; s; s = s->next) {
        Str src = jsonstr(jsonget(s, "source"));
        if (jsonstr(jsonget(s, "name"))!=name || src!=(proj ? Str("project") : Str("config"))
                || !jsonlen(jsonget(s, "toollist"))) {
            continue;
        }
        ents   = push(&a, ents, s);
        stale |= jsonflag(jsonget(s, "aslisted"), 0);
        trunc |= jsonflag(jsonget(s, "truncated"), 0);
        more  += jsonint(jsonget(s, "toolsmore"), 0);
        for (Json *t = jsonget(s, "toollist")->child; t; t = t->next) {
            Str  tool = jsonstr(jsonget(t, "tool"));
            b32 seen = 0;
            for (iz k = 0; k < names.len; k++) {
                seen |= names[k] == tool;
            }
            if (!seen) {
                names = push(&a, names, tool);
                kept += mcponly(cur, tool);
            }
        }
    }

    ImGui::Spacing();
    Buf hb(&a, 64);
    print(&hb, "Tools");
    if (names.len) {
        print(&hb, " (");
        print(&hb, kept);
        print(&hb, " of ");
        print(&hb, (i64)names.len + more);
        putbyte(&hb, ')');
    }
    print(&hb, "###mcptools");
    b32 open = ImGui::CollapsingHeader(cstr0(&a, finish(&hb)), ImGuiTreeNodeFlags_DefaultOpen);
    itemnamed(g, concat(&a, "mcp tools ", name));
    if (!open) {
        return;
    }

    // What the list is, and what a checkbox changes
    ImGui::PushStyleColor(ImGuiCol_Text, g->pal->dim);
    ImGui::PushTextWrapPos(0);
    Buf nb(&a, 256);
    if (proj) {
        print(&nb, "These settings apply to the server named ");
        print(&nb, name);
        print(&nb, " in every project's .mcp.json; nothing else of this section does.");
    }
    if (stale) {
        print(&nb, nb.len ? Str(" ") : Str(""));
        print(&nb, "As last listed: it is not running now.");
    }
    if (trunc || more) {
        print(&nb, nb.len ? Str(" ") : Str(""));
        print(&nb, "It lists more tools than show here.");
    }
    if (incl) {
        print(&nb, nb.len ? Str(" ") : Str(""));
        print(&nb, "An included file sets its tools: a change here goes in your own file.");
    }
    // A list of those kept, so new tools stay off: one it never names is
    b32 allow = !mcponly(cur, "\x01");
    if (allow) {
        print(&nb, nb.len ? Str(" ") : Str(""));
        print(&nb, "Only the ticked tools are offered, so any the server adds later stay off.");
    }
    if (nb.len) {
        text(finish(&nb));
    }
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    if (!names.len) {
        Str state = jsonstr(jsonget(m, "state"));
        if (proj) {
            ImGui::TextDisabled("None runs now: their tools show while a "
                                "session runs one.");
            return;
        }
        Str why = jsonflag(jsonget(m, "enabled"), 1)==0 ? Str("It is turned off (enabled = no)") :
                  state=="approve" || jsonget(m, "approve")->child ? Str("Approve its url first") :
                  state=="auth" ? Str("Sign in first") :
                  state=="starting" ? Str("Starting...") :
                  g->mcptsent ? Str("Asked once already: Restart, then try again") : Str{};
        if (state == "ready") {
            ImGui::TextDisabled("Listing...");
            return;
        }
        ImGui::BeginDisabled(why.len > 0);
        b32 go = ImGui::SmallButton("List tools");
        ImGui::EndDisabled();
        itemnamed(g, concat(&a, "mcp list ", name));
        if (why.len) {
            whytip(why);
        } else {
            itemtip("Start the server now, as a session would, to see its tools");
        }
        if (go) {
            g->mcptstart = 1;
            mcptlisting(g, name);
        }
        return;
    }

    // One row a tool: checkbox, name, read-only, the description's head
    Str editwhy = !word ? Str("The name can't be a section's in the file: set "
                              "its tools there by hand") : Str{};
    pushstr("mcptools");
    i32 row = 0;
    for (iz e = 0; e < ents.len; e++) {
        Json *s = ents[e];
        if (proj) {
            ImGui::PushStyleColor(ImGuiCol_Text, g->pal->dim);
            Str in = concat(&a, "in ", jsonstr(jsonget(s, "file")));
            textfit(a, in, ImGui::GetContentRegionAvail().x, 0);
            ImGui::PopStyleColor();
        }
        for (Json *t = jsonget(s, "toollist")->child; t; t = t->next, row++) {
            Arena tmp  = a;
            Str   tool = jsonstr(jsonget(t, "tool"));
            Str   desc = jsonstr(jsonget(t, "desc"));
            Str   prm  = jsonstr(jsonget(t, "params"));
            b32   on   = mcponly(cur, tool);
            b32   fil  = jsonflag(jsonget(t, "filterable"), 0);
            Str   why  = jsonstr(jsonget(t, "why"));
            ImGui::PushID(row);
            bool  box  = on;
            Str   bwhy = editwhy.len ? editwhy : !fil ?
                         Str("Its name has characters a tools list can't hold: "
                             "it is always offered") : Str{};
            ImGui::BeginDisabled(bwhy.len > 0);
            b32 hit = ImGui::Checkbox("##on", &box);
            ImGui::EndDisabled();
            itemnamed(g, concat(&tmp, "mcp tool ", tool));
            if (bwhy.len) {
                whytip(bwhy);
            }
            if (hit) {
                Str err = mcptset(g, &tmp, name, tool, box);
                if (err.len) {
                    mcpsay(g, name, concat(&tmp, concat(&tmp, tool, ": "), err), 1);
                }
            }
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            Buf tb(&tmp, 256);
            print(&tb, desc);
            if (prm.len) {
                print(&tb, desc.len ? Str("\n\n") : Str(""));
                print(&tb, "Parameters: ");
                print(&tb, prm);
            }
            Str tip = finish(&tb);
            text(tool);
            if (tip.len) {
                itemtip("%s", cstr0(&tmp, tip));
            }
            if (jsonflag(jsonget(t, "ro"), 0)) {
                ImGui::SameLine();
                ImGui::TextDisabled("read-only");
            }
            if (on && why.len) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, g->pal->amber);
                Str note = concat(&tmp, "not offered: ", why);
                textfit(tmp, note, ImGui::GetContentRegionAvail().x, 0);
                ImGui::PopStyleColor();
            } else if (desc.len) {
                ImGui::SameLine();
                Str head = cut(desc, '\n').head;
                textfit(tmp, head, ImGui::GetContentRegionAvail().x,
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), tip);
            }
            ImGui::PopID();
        }
    }

    // Names a list keeps that no server lists, to take out again
    if (allow) {
        for (Str rest = cur; rest.len;) {
            iz i = 0;
            for (; i<rest.len && (whitespace(rest[i]) || rest[i]==','); i++) {}
            rest = cuthead(rest, i);
            for (i = 0; i<rest.len && !whitespace(rest[i]) && rest[i]!=','; i++) {}
            Str w = takehead(rest, i);
            rest  = cuthead(rest, i);
            b32 listed = !w.len || (w.len>1 && w[0]=='-') || !mcponly(cur, w);
            for (iz k = 0; !listed && k < names.len; k++) {
                listed = names[k] == w;
            }
            if (listed) {
                continue;
            }
            Arena tmp = a;
            ImGui::PushID(row++);
            bool box = 1;
            ImGui::BeginDisabled(editwhy.len > 0);
            b32 hit = ImGui::Checkbox("##on", &box);
            ImGui::EndDisabled();
            itemnamed(g, concat(&tmp, "mcp tool ", w));
            if (hit) {
                Str err = mcptset(g, &tmp, name, w, box);
                if (err.len) {
                    mcpsay(g, name, concat(&tmp, concat(&tmp, w, ": "), err), 1);
                }
            }
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            text(w);
            ImGui::SameLine();
            ImGui::TextDisabled("not listed by the server");
            ImGui::PopID();
        }
    }
    ImGui::PopID();
}

// An MCP server's state, on its page: as the main host's engine has it,
// with Sign in and Sign out for one over HTTP, and Restart.
static void drawmcp(Gui *g, Arena a, Str name)
{
    f32         sc  = g->scale;
    ImGuiStyle &st  = ImGui::GetStyle();
    b32         can = hostcan(g->main, "mcp.auth");
    if (can) {
        mcplisting(g);
    }
    Json *m       = mcpof(g, name);
    Str   state   = jsonstr(jsonget(m, "state"));
    b32   http    = jsonstr(jsonget(m, "url")).len || mcprow(g, name, "url").len;
    b32   signed_ = jsonint(jsonget(m, "signedin"), 0) != 0;
    u32   dot     = g->pal->dim;
    Str   line    = mcpstatus(g, &a, name, &dot);
    pushstr(concat(&a, "mcp ", name));
    ImVec2 p  = ImGui::GetCursorScreenPos();
    f32    lh = ImGui::GetTextLineHeight();
    drawdot(ImGui::GetWindowDrawList(), v2(p.x + 4*sc, p.y + lh/2 + st.FramePadding.y),
            3.5f*sc, dot);
    ImGui::Dummy(v2(10*sc, 0));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(0);
    text(line);
    ImGui::PopTextWrapPos();
    itemnamed(g, concat(&a, "mcp state ", name));

    b32 approve = drawapprove(g, a, "mcp", name);

    Str busy = g->mcpname.len ? concat(&a, concat(&a, "Signing in to ", g->mcpname),
                                       Str(" already")) : Str{};
    if (http) {
        Str why = !can ? Str("The engine is too old to sign in") :
                  approve ? Str("Approve its url first") : busy;
        ImGui::BeginDisabled(why.len > 0);
        b32 in = ImGui::SmallButton(signed_ ? "Sign in again" : "Sign in");
        ImGui::EndDisabled();
        itemnamed(g, concat(&a, "mcp signin ", name));
        if (why.len) {
            whytip(why);
        } else {
            itemtip("Sign in through the browser, for a token the "
                                  "server takes, kept in the state directory");
        }
        if (in) {
            mcpsignin(g, name);
        }
        if (signed_) {
            ImGui::SameLine();
            b32 out = ImGui::SmallButton("Sign out");
            itemnamed(g, concat(&a, "mcp signout ", name));
            itemtip("Forget the server's tokens");
            if (out) {
                mcpcmd(g, CMD_MCPLOGOUT, name);
            }
        }
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(!can || !m);
    b32 again = ImGui::SmallButton("Restart");
    ImGui::EndDisabled();
    itemnamed(g, concat(&a, "mcp restart ", name));
    itemtip("%s", http ? "Connect to it again now"
                                     : "Stop it, to start again for the next session "
                                       "that uses it");
    if (again) {
        mcpcmd(g, CMD_MCPRESTART, name);
    }
    Str err = jsonstr(jsonget(m, "err"));
    if (state=="failed" && err.len) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
        ImGui::TextWrapped("%s", cstr0(&a, err));
        ImGui::PopStyleColor();
    }
    drawmcptools(g, a, name, m);
    ImGui::PopID();
}

// A chatgpt endpoint's sign-in, on its page: who is signed in, as the
// main host's engine lists it (models, asked as the page shows), with
// Sign in and Sign out.
// A signed-in endpoint's state (kind chatgpt, or copilot), Sign in,
// and while GitHub's code waits to be entered, the code.
static void drawsignin(Gui *g, Arena a, Str name, b32 copilot)
{
    f32         sc  = g->scale;
    ImGuiStyle &st  = ImGui::GetStyle();
    GuiHost    *h   = g->main;
    b32         can = h && hostcan(h, copilot ? Str("endpoint.copilot") : Str("endpoint.auth"));
    if (can && g->eplistfor!=name && !h->listid && hostlist(g, h)) {
        g->eplistfor = clone(&g->perm, name);
    }
    GuiEndpoint *ep = 0;
    for (iz i = 0; can && i < h->eps.len; i++) {
        ep = h->eps[i].name==name ? &h->eps[i] : ep;
    }
    u32 dot  = 0;
    Str line = epstatus(g, &a, name, &dot, copilot);
    pushstr(concat(&a, "chatgpt ", name));
    ImVec2 p  = ImGui::GetCursorScreenPos();
    f32    lh = ImGui::GetTextLineHeight();
    drawdot(ImGui::GetWindowDrawList(), v2(p.x + 4*sc, p.y + lh/2 + st.FramePadding.y),
            3.5f*sc, dot);
    ImGui::Dummy(v2(10*sc, 0));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(0);
    text(line);
    ImGui::PopTextWrapPos();
    itemnamed(g, concat(&a, "endpoint state ", name));

    b32 signed_ = ep && ep->signedin;
    Str why     = !can ? Str("The engine is too old to sign in") :
                  g->mcpname.len ? concat(&a, concat(&a, "Signing in to ", g->mcpname),
                                          Str(" already")) : Str{};
    ImGui::BeginDisabled(why.len > 0);
    b32 in = ImGui::SmallButton(signed_ ? "Sign in again" : "Sign in");
    ImGui::EndDisabled();
    itemnamed(g, concat(&a, "endpoint signin ", name));
    if (why.len) {
        whytip(why);
    } else if (copilot) {
        itemtip("Sign in through the browser, with a code, with the GitHub "
                "account whose Copilot it uses, for a token kept in the state "
                "directory");
    } else {
        itemtip("Sign in through the browser with the ChatGPT account whose "
                "subscription it uses, for tokens kept in the state directory");
    }
    if (in) {
        epsignin(g, name, copilot);
    }
    if (signed_) {
        ImGui::SameLine();
        b32 out = ImGui::SmallButton("Sign out");
        itemnamed(g, concat(&a, "endpoint signout ", name));
        itemtip(copilot ? "Forget the GitHub token" : "Forget the ChatGPT tokens");
        if (out) {
            mcpcmd(g, CMD_EPLOGOUT, name);
        }
    }
    if (g->mcpname==name && g->mcpcode.len) {
        // GitHub's code, large, to enter at its page
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->dim);
        ImGui::TextWrapped("Enter this code at %s, which then asks you to "
                           "authorize Prospero:", cstr0(&a, g->mcpcodeurl));
        ImGui::PopStyleColor();
        ImGui::PushFont(g->fonts.mono, g->fonts.size*1.6f);
        text(g->mcpcode);
        ImGui::PopFont();
        itemnamed(g, concat(&a, "endpoint code ", name));
        if (ImGui::SmallButton("Copy code")) {
            ImGui::SetClipboardText(cstr0(&a, g->mcpcode));
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Open GitHub")) {
            os_guiopenurl(g->os, a, g->mcpcodeurl);
        }
    }
    ImGui::PopID();
}

// The page shown: its section's name and what it is for, Delete for a
// named one of the user's own file, Connect for a remote, an MCP
// server's state and sign-in, its settings, and a model's template
// argument.
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
        f32      bw    = iconw();
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
        } else if (!hostcap(g->main, "config.delete")) {
            print(&why, "The engine is too old to delete a section");
        }
        ImGui::BeginDisabled(why.len > 0);
        b32 del = iconbutton(ICON_TRASH "##delete");
        ImGui::EndDisabled();
        itemrect(g, RECT_DELETE);
        if (why.len) {
            itemtip("%s", cstr0(&a, finish(&why)));
        } else {
            Buf tb(&a, 256);
            print(&tb, "Delete...: remove ");
            print(&tb, title);
            print(&tb, " from ");
            print(&tb, basename(own->file));
            print(&tb, ", with the comment just above it");
            if (other) {
                print(&tb, "; an included file defines it too, which stays");
            }
            itemtip("%s", cstr0(&a, finish(&tb)));
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
        drawremote(g, a, name, RECT_PAGECONNECT);
    } else if (kind=="mcp" && name.len) {
        drawmcp(g, a, name);
    } else if (kind=="endpoint" && name.len) {
        drawapprove(g, a, kind, name);
        Str epkind = cfgepkind(g, name);
        if (epkind=="chatgpt" || epkind=="copilot") {
            drawsignin(g, a, name, epkind=="copilot");
        }
    }
    ImGui::Spacing();
    b32 projonly = 0;  // a project's server only
    for (Json *s = kind=="mcp" && g->mcplist ? g->mcplist->child : 0; s; s = s->next) {
        projonly |= jsonstr(jsonget(s, "name"))==name && jsonstr(jsonget(s, "source"))=="project";
    }
    if (named && !cfghas(g, kind, name) && projonly && name.len && !cfghead(g, kind, name, 0)) {
        // A project's server: a checkbox above writes the section
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("No [mcp %s] section yet: turning a tool off above "
                            "writes one in your file.", cstr0(&a, name));
        ImGui::PopTextWrapPos();
        return;
    } else if (named && !cfghas(g, kind, name)) {
        ImGui::TextDisabled("Loading...");
        return;
    }
    drawcfgtable(g, kind, name);
    if (kind == "model") {
        drawcfgtemplate(g, name);
    }
}

// The kind of section's first key, which a new one is written with: a
// remote's ssh and a shell's program are aliases of its name.
static void cfgseed(Str kind, Str name, Str *key, Str *val)
{
    *key = kind=="endpoint" ? Str("url") :
           kind=="model"    ? Str("reasoning") :
           kind=="remote"   ? Str("ssh") :
           kind=="shell"    ? Str("program") :
           kind=="mcp"      ? Str("url") :
           kind=="theme"    ? Str("base") : Str("tools");
    *val = kind=="endpoint" ? Str("http://localhost/") :
           kind=="model"    ? Str("reasoning_content") :
           kind=="remote"   ? name :
           kind=="shell"    ? name :
           kind=="mcp"      ? Str("https://mcp.example.com/mcp") :
           kind=="theme"    ? Str("dark") :
                              Str("read write edit shell");
}

// A new named section: its name, then its page. A remote's asks how it
// is reached as well, over ssh or by a command, and where prospero is.
static void drawcfgadd(Gui *g)
{
    Arena a  = g->frame;
    f32   sc = g->scale;
    if (g->openadd) {
        ImGui::OpenPopup("New section");
        g->openadd    = 0;
        g->addname[0] = g->remdest[0] = g->rempath[0] = 0;
        g->adderr     = {};
        g->addepkind  = EP_OPENAI;
    }
    ImGui::SetNextWindowSize(v2(380*sc, 0));
    if (!ImGui::BeginPopupModal("New section", 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    Str kind = g->addkind;
    b32 rem  = kind == "remote";
    b32 ep   = kind == "endpoint";
    Str key  = {};
    Str val  = {};
    cfgseed(kind, "NAME", &key, &val);
    ImGui::PushTextWrapPos(0);
    if (rem) {
        ImGui::TextUnformatted("A new [remote NAME] section: a machine reached "
                               "over ssh, or by a command that runs another there");
    } else if (ep) {
        ImGui::TextUnformatted("A new [endpoint NAME] section: the API it speaks, "
                               "which fills in the URL of a service of its own");
    } else {
        ImGui::Text("A new [%s NAME] section, which starts as %s = %s:",
                    cstr0(&a, kind), cstr0(&a, key), cstr0(&a, val));
    }
    ImGui::PopTextWrapPos();
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1);
    b32 enter = ImGui::InputTextWithHint("##addname", "NAME", g->addname, FIELD_MAX,
                                         ImGuiInputTextFlags_EnterReturnsTrue);
    itemrect(g, RECT_ADDNAME);
    if (ep) {
        // By kind (EP_*)
        static char const *kinds[] = {
            "openai: Chat Completions (llama.cpp, most servers)",
            "anthropic: Anthropic's Messages API",
            "responses: OpenAI's Responses API",
            "chatgpt: ChatGPT, signed in to your account",
            "copilot: GitHub Copilot, signed in to your account",
        };
        static_assert(countof(kinds) == countof(epkinds), "a label for each kind");
        i32 k = g->addepkind>=0 && g->addepkind<(i32)countof(kinds) ? g->addepkind : 0;
        ImGui::SetNextItemWidth(-1);
        ImVec2 at = ImGui::GetCursorScreenPos();
        f32    cw = ImGui::GetContentRegionAvail().x;
        if (ImGui::BeginCombo("##addkind", kinds[k])) {
            for (i32 i = 0; i < (i32)countof(kinds); i++) {
                if (ImGui::Selectable(kinds[i], i==k)) {
                    g->addepkind = i;
                }
                itemnamed(g, concat(&a, "addkind ", epkindname(i)));
            }
            ImGui::EndCombo();
        }
        namedrect(g, "addkind", at, v2(at.x+cw, at.y+ImGui::GetFrameHeight()));
        Str url = epkindurl(g->addepkind);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", cstr0(&a, epsigned(g->addepkind) ?
            concat(&a, concat(&a, "URL ", url), ": then Sign in on its page, with "
                   "the account whose subscription it uses") :
            url.len ? concat(&a, "URL ", url) :
            Str("URL http://localhost/, which you then set to the server's")));
        ImGui::PopTextWrapPos();
    }
    if (rem) {
        static char const *how[] = {"ssh", "command"};
        i32 hw = g->remexec;
        ImGui::SetNextItemWidth(100*sc);
        if (ImGui::Combo("##remhow", &hw, how, countof(how))) {
            g->remexec = hw;
        }
        itemtip("How it is reached: over ssh, or by a command that runs "
                "another there, docker exec -i NAME, say");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        enter |= ImGui::InputTextWithHint("##remdest", g->remexec ? "docker exec -i NAME"
                                                                  : "ssh destination",
                                          g->remdest, FIELD_MAX,
                                          ImGuiInputTextFlags_EnterReturnsTrue);
        itemrect(g, RECT_ADDDEST);
        itemtip(g->remexec ? "A command that runs another inside it, without a "
                             "shell; it needs /bin/sh there"
                           : "user@host, user@host:port for another port, or a "
                             "host from ~/.ssh/config");
        ImGui::SetNextItemWidth(-1);
        enter |= ImGui::InputTextWithHint("##rempath", "prospero there (optional)",
                                          g->rempath, FIELD_MAX,
                                          ImGuiInputTextFlags_EnterReturnsTrue);
        itemrect(g, RECT_ADDPATH);
    }
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
            cfgsay(g, finish(&eb), 0);
            cfgselect(g, kind, name);
            g->cfgshowsel = 1;
            ImGui::CloseCurrentPopup();
        } else if (rem) {
            Str   dest = trim({(u8 *)g->remdest, (iz)__builtin_strlen(g->remdest)});
            Str   path = trim({(u8 *)g->rempath, (iz)__builtin_strlen(g->rempath)});
            Reach l    = {};
            (g->remexec ? l.exec : l.ssh) = dest;
            Str bad = dest.len ? linkbad(a, l) : Str{};
            if (!dest.len) {
                g->adderr = g->remexec ? Str("A remote needs a command")
                                       : Str("A remote needs an ssh destination");
            } else if (bad.len) {
                Str msg = clone(&g->perm, bad);
                msg.data[0] = msg.data[0]>='a' && msg.data[0]<='z' ? msg.data[0]-32
                                                                     : msg.data[0];
                g->adderr = msg;
            } else {
                guiconfigset(g, kind, name, g->remexec ? Str("exec") : Str("ssh"), dest);
                if (path.len) {
                    guiconfigset(g, kind, name, "path", path);
                }
                cfgselect(g, kind, name);
                g->cfgshowsel = 1;
                ImGui::CloseCurrentPopup();
            }
        } else if (ep) {
            // The kind first, then its URL, so the file reads in that order
            Str url = epkindurl(g->addepkind);
            guiconfigset(g, kind, name, "kind", epkindname(g->addepkind));
            guiconfigset(g, kind, name, "url", url.len ? url : val);
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

// Why the configuration file at path cannot go out to the editor, or
// empty.
static Str cfgeditwhy(Gui *g, Str path, b32 exists, Arena *a)
{
    if (!path.len) {
        return "No configuration file location";
    } else if (g->cfgcreate) {
        return "Creating the file...";
    } else if (!exists && !hostcap(g->main, "config.create")) {
        return "The engine is too old to create the file";
    }
    return editorwhy(g, 0, a);
}

// The configuration file in the editor (cfgedit), beside Reload: its
// button, or while it is out, which editor and a Cancel. Its scratch is
// a, after what the caller keeps there.
static void cfgeditbutton(Gui *g, Arena a, Str path, b32 exists)
{
    ImGuiStyle &st = ImGui::GetStyle();
    if (g->edcfg) {
        char *what = cstr0(&a, concat(&a, concat(&a, "editing in ", g->edname), "..."));
        samelineif(ImGui::CalcTextSize(what).x + st.ItemSpacing.x +
                   ImGui::CalcTextSize("Cancel").x + 2*st.FramePadding.x);
        ImGui::TextDisabled("%s", what);
        ImGui::SameLine();
        if (ImGui::Button("Cancel##cfgedit")) {
            editcancel(g);
        }
        itemnamed(g, "cancel config editor");
        itemtip("Stop the editor: the file is as it left it, read again");
        return;
    }
    char const *label = "Open in editor";
    Str         why   = cfgeditwhy(g, path, exists, &a);
    samelineif(ImGui::CalcTextSize(label).x + 2*st.FramePadding.x);
    ImGui::BeginDisabled(why.len != 0);
    if (ImGui::Button(label)) {
        cfgedit(g, a, path, exists);
    }
    ImGui::EndDisabled();
    itemnamed(g, "config editor");
    whytip(why.len ? why : exists ? Str("Edit the file by hand in your editor: it is read "
                                        "again once the editor exits")
                                  : Str("Create the file as the first edit would (the "
                                        "documented template, for the default file), then "
                                        "edit it by hand in your editor"));
}

// What the latest change in Settings came to, as a toast over the foot
// of the page (r0 to r1), as the transcript's (drawtoast): an error stays until
// dismissed or replaced, and anything else fades.
static void drawcfgtoast(Gui *g, ImVec2 r0, ImVec2 r1)
{
    b32 fresh = !g->cfgok || g->sched.now-g->cfgstatusat<TOAST_LIFE;
    if (!g->cfgstatus.len || !fresh) {
        return;
    }
    if (g->cfgok) {
        schedat(&g->sched, g->cfgstatusat + TOAST_LIFE, DUE_CLOCK);
    }
    u32 c = g->cfgok ? g->pal->ok : g->pal->bad;
    if (toastbox(g, g->cfgstatus, c, g->cfgok ? g->pal->rule : c,
                 "\xc3\x97", "settings dismiss", "Dismiss", -1, r0, r1, 1)) {  // a multiplication sign
        g->cfgstatus = {};
    }
}

static void drawsettings(Gui *g)
{
    Arena a  = g->frame;
    f32   sc = g->scale;
    if (g->enginekind == ENGINE_FAKE) {
        ImGui::TextDisabled("No engine: the fake agent has no settings.");
        g->cfglinked = 0;
        return;
    }
    if (ImGui::IsWindowAppearing() || (!g->cfg && !g->cfgget)) {
        guiconfigget(g);  // the file may have changed by hand
        g->cfglinkget = 1;
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
    if (iconbutton(ICON_REFRESH "##reload")) {
        guiconfigget(g);
    }
    itemnamed(g, "settings reload");
    itemtip("Reload: read the configuration's files again");
    cfgeditbutton(g, a, path, jsonint(jsonget(j, "exists"), 0) != 0);
    i32 urls = (i32)jsonlen(jsonget(j, "includes"));
    if (urls) {
        // Fetching each URL include again, which the reply waits for
        char const *label = g->cfgrefresh ? "Fetching..." : "Refresh includes";
        samelineif(ImGui::CalcTextSize(label).x + 2*ImGui::GetStyle().FramePadding.x);
        ImGui::BeginDisabled(g->cfgrefresh != 0);
        b32 refresh = ImGui::Button(label);
        itemrect(g, RECT_REFRESHINC);
        ImGui::EndDisabled();
        itemtip("Fetch the %d URL include%s again, rather than wait for the "
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
        if (ImGui::Button(cstr0(&a, label))) {
            g->cfgdiag = 1;
        }
        ImGui::PopStyleColor();
        itemtip("Show the problems found in the files, with where each is");
    }

    if (g->cfglinked && !g->cfgset && !g->cfgget) {
        cfgfollow(g);
    }

    // A named section's page gone, deleted or never added, gives way to
    // the first page once nothing is in flight that would bring it
    if (!g->cfgdiag && g->cfgpname.len && !cfghas(g, g->cfgpkind, g->cfgpname) &&
            !g->cfgset && !g->cfgget) {
        cfgselect(g, "", "");
        g->cfgshowsel = 1;
    }

    // The search: Ctrl+F (Cmd+F) to it while Settings has the focus,
    // ahead of the transcript's find; Escape empties it
    if (ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_F)) {
        g->cfgfindkey = 1;
    }
    if (g->cfgfindkey) {
        ImGui::SetKeyboardFocusHere();
        g->cfgfindkey = 0;
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::SetNextItemAllowOverlap();
    b32 enter = ImGui::InputTextWithHint("##find", "Search settings", g->cfgfind, FIELD_MAX,
                                         ImGuiInputTextFlags_EnterReturnsTrue|
                                         ImGuiInputTextFlags_EscapeClearsAll);
    itemrect(g, RECT_CFGFIND);
    g->cfgfindon = ImGui::IsItemActive();
    if (ImGui::IsItemActivated() || ImGui::IsItemEdited()) {
        g->cfgfindhid = 0;
    }
    if (*g->cfgfind) {
        // An X at the field's end empties it, as Escape does, and gives
        // the field the keyboard back
        ImVec2 mx = ImGui::GetItemRectMax();
        ImVec2 mn = ImGui::GetItemRectMin();
        f32    fs = ImGui::GetFontSize();
        ImVec2 at = v2(mx.x - ImGui::GetStyle().FramePadding.x - fs,
                       mn.y + (mx.y - mn.y - fs)/2);
        if (ImGui::CloseButton(ImGui::GetID("##findclear"), at)) {
            *g->cfgfind   = 0;
            g->cfgfindkey = 1;
        }
        itemtip("Clear the search");
        g->rects[RECT_CFGCLEAR] = {at.x, at.y, at.x+fs, at.y+fs};
    }
    Str        query = trim({(u8 *)g->cfgfind, (iz)__builtin_strlen(g->cfgfind)});
    b32        find  = query.len > 0;
    Slice<i32> found = find ? cfgfindlist(g, &a, query) : Slice<i32>{};
    if (!find && g->cfgfound) {
        g->cfgshowsel = 1;  // the list back, at the page shown
    }
    g->cfgfound = find;

    f32 hs = 0;  // room to scroll sideways, for the checklist
    f32 w  = ImGui::GetContentRegionAvail().x;
    g->cfgtwopane = w >= (f32)CFG_TWOPANE*sc;
    g->cfgcombo   = 0;
    if (g->cfgtwopane) {
        f32 lw = w*0.3f;
        lw = lw<130*sc ? 130*sc : lw>220*sc ? 220*sc : lw;
        if (ImGui::BeginChild("##pages", v2(lw, 0), ImGuiChildFlags_Borders)) {
            if (find) {
                drawcfgfind(g, a, found, enter);
            } else {
                drawcfglist(g, 0);
            }
            hs = ImGui::GetScrollMaxX();
        }
        ImGui::EndChild();
        ImGui::SameLine();
    } else if (find && !g->cfgfindhid) {
        // In place of the combo and the page, until one is picked
        if (ImGui::BeginChild("##found", v2(0, 0), ImGuiChildFlags_Borders)) {
            drawcfgfind(g, a, found, enter);
        }
        ImGui::EndChild();
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
    // The toast goes over the page, clear of the list, else the window
    ImVec2 t0 = ImGui::GetWindowPos();
    ImVec2 t1 = v2(t0.x + ImGui::GetWindowSize().x, t0.y + ImGui::GetWindowSize().y);
    if (!(find && !g->cfgtwopane && !g->cfgfindhid)) {
        if (ImGui::BeginChild("##page", v2(0, 0))) {
            drawcfgpage(g);
            f32 m = ImGui::GetScrollMaxX();
            hs = hs>m ? hs : m;
        }
        ImGui::EndChild();
        t0 = ImGui::GetItemRectMin();
        t1 = ImGui::GetItemRectMax();
    }
    drawcfgtoast(g, t0, t1);
    drawcfgadd(g);
    drawcfgdel(g);
    f32 m = ImGui::GetScrollMaxX();
    g->cfghscroll = hs>m ? hs : m;
    g->cfgframe   = g->total.frames;
    if (ImGui::GetIO().WantTextInput &&
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        g->cfgtyped = g->total.frames;  // a reload would undo the typing
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
    itemtip("Render a hidden frame 250 ms after going idle, and report a missed wake if it differs");
    ImGui::SameLine();
    checkbox("Aggressive", &g->sched.aggressive);
    itemtip("Probe every second while idle");
}

// The GUI's log, its latest lines, as a code block in a document whose
// text is selectable and copied as the Plan's is, and Copy all.
static void drawlog(Gui *g)
{
    Arena a = g->frame;
    Buf   all(&a, 4096);
    for (iz i = 0; i < g->logs.len; i++) {
        print(&all, g->logs[i]);
        putbyte(&all, '\n');
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%d lines, the latest", (int)g->logs.len);
    copyallbutton(g, a, finish(&all), "log copy", "Copy the log's lines as text");
    if (!g->logs.len) {
        return;
    }
    if (!g->logdoc) {
        g->logdoc = newdocview(&g->perm, g->os, g->vcache);
    }
    if (g->logshown != g->logn) {
        docset(g->logdoc, mdcodeblock(&a, g->logs));
        g->logshown = g->logn;
    }
    drawdoc(g, g->logdoc, "##log");
}

// The fonts' licenses, which their files ask to be shown.
static void drawfontcredits()
{
    ImGui::BulletText("Roboto: Copyright 2011 Google (Apache 2.0)");
    ImGui::BulletText("JetBrains Mono: Copyright 2020 JetBrains (OFL 1.1)");
    ImGui::BulletText("Noto Sans Symbols 2, Noto Sans Math: Copyright 2022 "
                      "The Noto Project Authors, Google (OFL 1.1)");
    ImGui::BulletText("Codicons: Copyright Microsoft Corporation (CC BY 4.0)");
    ImGui::BulletText("Twemoji: Copyright 2020 Twitter, Inc and other contributors "
                      "(CC BY 4.0)");
}

// A Help topic picked, from its list or a link: shown from its top, and
// kept with the layout.
static void helppick(Gui *g, i32 page)
{
    if (page>=0 && page!=g->helppage) {
        g->helppage = page;
        ImGui::MarkIniSettingsDirty();
        scheddirty(&g->sched);
    }
}

// Help: its topics (help.cpp) as a list beside the page, or in a pane
// narrower than about 40 ems, as a combo above it. The page is a
// document, selectable and copied as the Plan window's is, and a link
// to help:NAME in it goes to that topic.
static void drawhelp(Gui *g)
{
    Arena       a   = g->frame;
    ImGuiStyle &st  = ImGui::GetStyle();
    i32         cur = g->helppage>=0 && g->helppage<HELP_COUNT ? g->helppage : 0;
    if (!g->helpdoc) {
        g->helpdoc = newdocview(&g->perm, g->os, g->vcache);
    }
    if (g->helpshown != cur+1) {
        docset(g->helpdoc, helptext(cur));
        g->helpdoc->v->sel      = {};
        g->helpdoc->scrollto    = 0;
        g->helpshown            = cur + 1;
    }
    if (g->helpsect.len) {
        g->helpdoc->section = g->helpsect;  // a link's, once laid out
        g->helpsect         = {};
    }

    // The search: Ctrl+F (Cmd+F) to it while Help has the focus, as in
    // Settings; Escape or its X empties it, Enter takes the best hit
    if (ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_F)) {
        g->helpfindkey = 1;
    }
    if (g->helpfindkey) {
        ImGui::SetKeyboardFocusHere();
        g->helpfindkey = 0;
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::SetNextItemAllowOverlap();
    b32 enter = ImGui::InputTextWithHint("##helpfind", "Search help", g->helpfind, FIELD_MAX,
                                         ImGuiInputTextFlags_EnterReturnsTrue|
                                         ImGuiInputTextFlags_EscapeClearsAll);
    itemnamed(g, "help search");
    if (*g->helpfind) {
        ImVec2 mx = ImGui::GetItemRectMax();
        ImVec2 mn = ImGui::GetItemRectMin();
        f32    fs = ImGui::GetFontSize();
        ImVec2 at = v2(mx.x - st.FramePadding.x - fs, mn.y + (mx.y - mn.y - fs)/2);
        if (ImGui::CloseButton(ImGui::GetID("##helpclear"), at)) {
            *g->helpfind   = 0;
            g->helpfindkey = 1;
        }
        itemtip("Clear the search");
        namedrect(g, "help search clear", at, v2(at.x+fs, at.y+fs));
    }
    Str            query = trim({(u8 *)g->helpfind, (iz)__builtin_strlen(g->helpfind)});
    Slice<HelpHit> hits  = helpsearch(&a, query, 50);
    i32            hit   = enter && hits.len ? 0 : -1;

    i32 pick = -1;
    f32 w    = ImGui::GetContentRegionAvail().x;
    g->helpnarrow = w < 40*ImGui::GetFontSize();
    g->helpframe  = g->total.frames;
    if (query.len) {
        // The hits in place of the topics: beside the page, or above it
        // in a narrow pane, each its heading and a line that matched
        f32    fs   = ImGui::GetFontSize();
        ImVec2 size = g->helpnarrow ? v2(0, 10*ImGui::GetTextLineHeightWithSpacing())
                                    : v2(w*0.4f < 22*fs ? w*0.4f : 22*fs, 0);
        if (ImGui::BeginChild("##hits", size, ImGuiChildFlags_Borders)) {
            if (!hits.len) {
                ImGui::TextDisabled("No matches");
            }
            for (iz i = 0; i < hits.len; i++) {
                HelpHit *h     = &hits[i];
                Str      title = helptitle(h->page);
                Str      label = h->head.len ? h->head : title;
                Str      more  = h->head.len ? title : Str{};
                if (h->snippet.len) {
                    more = more.len ? concat(&a, concat(&a, more, " \xc2\xb7 "), h->snippet)
                                    : h->snippet;
                }
                ImGui::PushID((i32)i);
                if (ImGui::Selectable(cstr0(&a, label))) {
                    hit = (i32)i;
                }
                itemnamed(g, concat(&a, "help hit ", label));
                if (more.len) {
                    ImGui::Indent();
                    ImGui::TextDisabled("%s", cstr0(&a, more));
                    ImGui::Unindent();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        if (!g->helpnarrow) {
            ImGui::SameLine();
        }
    } else if (!g->helpnarrow) {
        f32 lw = 0;
        for (i32 i = 0; i < HELP_COUNT; i++) {
            Str t = helptitle(i);
            f32 tw = ImGui::CalcTextSize(t.cdata, t.cdata+t.len).x;
            lw = tw>lw ? tw : lw;
        }
        lw += 2*st.WindowPadding.x + 2*st.FramePadding.x;
        lw  = lw>w*0.4f ? w*0.4f : lw;
        if (ImGui::BeginChild("##topics", v2(lw, 0), ImGuiChildFlags_Borders)) {
            for (i32 i = 0; i < HELP_COUNT; i++) {
                Str t = helptitle(i);
                if (ImGui::Selectable(cstr0(&a, t), i==cur)) {
                    pick = i;
                }
                itemnamed(g, concat(&a, "help topic ", t));
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
    } else {
        ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##topics", cstr0(&a, helptitle(cur)),
                              ImGuiComboFlags_HeightLarge)) {
            for (i32 i = 0; i < HELP_COUNT; i++) {
                Str t = helptitle(i);
                if (ImGui::Selectable(cstr0(&a, t), i==cur)) {
                    pick = i;
                }
                itemnamed(g, concat(&a, "help topic ", t));
            }
            ImGui::EndCombo();
        }
        namedrect(g, "help topics", at, v2(at.x+w, at.y+ImGui::GetFrameHeight()));
    }
    Str link = drawdoc(g, g->helpdoc, "##helppage");
    itemnamed(g, "help text");
    helppick(g, pick);
    if (hit >= 0) {
        // Its page, at its heading, or from the top for a page's opening;
        // the search stays, for the next hit
        HelpHit *h = &hits[hit];
        if (h->head.len) {
            g->helpsect = clone(&g->perm, h->head);
        } else if (h->page == cur) {
            g->helpdoc->scrollto = 0;
        }
        helppick(g, h->page);
        scheddirty(&g->sched);
    }
    if (link.len) {
        followlink(g, link);
    }
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
    itemtip(g->view->busy ? "Cancel the running turn"
                                        : "Send a canned prompt");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140*sc);
    if (ImGui::SliderInt("tok/s", &g->tps, 1, 2000, "%d tok/s", ImGuiSliderFlags_Logarithmic)) {
        fakeint(g, FAKE_TPS, g->tps);
    }
    itemtip("Streaming rate of the fake agent");
    ImGui::SameLine();
    if (checkbox("Bursts", &g->bursts)) {
        fakeint(g, FAKE_BURSTS, g->bursts);
    }
    itemtip("Deliver tokens in bursts of 25");
    ImGui::SameLine();
    if (checkbox("Pauses", &g->pauses)) {
        fakeint(g, FAKE_PAUSES, g->pauses);
    }
    itemtip("Stall for 5 s partway through each response");
    ImGui::SameLine();
    if (ImGui::Button("Huge code")) {
        fakecmd(g, FAKE_HUGE, "");
    }
    itemtip("Stream one response that is a single huge code block");
    ImGui::SameLine();
    b32 history = ImGui::Button("10k history");
    itemrect(g, RECT_HISTORY);
    if (history) {
        fakeint(g, FAKE_HISTORY, 10000);
    }
    itemtip("Load 10,000 messages (about 5 MB) of history");
}

// A shortcut as the menus show it: Cmd on macOS, else Ctrl.
static b32 picklayoutnum(Gui *, i32);

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

// Why a session with no folder can't start straight away, here with
// the defaults, or empty if it can.
static Str newfreewhy(Gui *g)
{
    if (!cannew(g) || !g->main) {
        return "No engine";
    }
    if (!hostcap(g->main, "nofolder")) {
        return "This engine can't start a session with no folder";
    }
    return {};
}

// A session with no folder, with the defaults and no dialog: its
// prompt takes the focus once it's made, for its first message.
static void newfree(Gui *g)
{
    if (!newfreewhy(g).len) {
        guinew(g, g->main, {}, {}, {}, {}, 0, 1);
    }
}

// The Ctrl/Cmd+K palette (palette.cpp): a modal over the window, its
// query field on top and the sessions it offers below, which it opens.

enum : i32 {
    PAL_ROWS  = 9,  // rows shown before the list scrolls
    PAL_FOUND = 8,  // settings, and Help's sections, offered at most
};

// Open it as dialogs are next drawn, remembering whether the prompt had
// the keyboard, to give it back if nothing is picked.
static void palopen(Gui *g)
{
    PalWin *p = &g->palw;
    p->open   = 1;
    p->prompt = g->sess && g->promptactive;
    if (!g->cfgrows.len) {
        guiconfigget(g);  // its settings, which come later (palbuild again)
    }
}

// Build the items again from the sessions, layouts and panes as they
// are now, and the settings and Help's sections the query finds, and
// order them for the query. The pick stays on its item if keep, else goes to
// the top.
static void palbuild(Gui *g, b32 keep)
{
    PalWin *p = &g->palw;
    if (!p->base) {
        p->mem  = newarena(os_reserve(g->os, (iz)1<<24));
        p->base = p->mem.beg;
    }
    Arena scratch = g->frame;
    Str   pick    = {};
    if (keep && p->sel>=0 && p->sel<p->order.len) {
        pick = clone(&scratch, p->items[p->order[p->sel]].id);
    }
    p->mem.beg = p->base;
    Arena *a   = &p->mem;
    p->items = {};
    p->sess  = {};
    for (GuiSession *s = g->sessions; s; s = s->next) {
        GuiSession *root = sessroot(g, s);
        if (s->gone || !(s->view || s->listed) || root->trashed) {
            continue;
        }
        View   *v  = s->view;
        PalItem it = {};
        Buf     id(a, 64);
        print(&id, (i64)s->host->uid);
        putbyte(&id, ':');
        print(&id, s->sid);
        it.kind   = PAL_SESSION;
        it.id     = finish(&id);
        it.title  = clone(a, sesstitle(s));
        it.group  = clone(a, root->group);
        it.folder = clone(a, v && v->cwd.len ? v->cwd : s->cwd);
        it.host   = s->host->kind==GH_ENGINE ? Str{} : clone(a, s->host->name);
        it.sid    = clone(a, s->sid);
        it.at     = s->lastact;
        it.seen   = s->viewseq;
        it.minor  = sessparent(s).len>0 || root->archived;
        it.shown  = s == g->sess;
        p->items  = push(a, p->items, it);
        p->sess   = push(a, p->sess, s);
    }

    // The rest have no session; each has an id of its own kind
    Str q = {(u8 *)p->q, (iz)__builtin_strlen(p->q)};
    i32 nl = PRESET_COUNT + (i32)g->layouts.len;
    for (i32 n = 0; n < nl; n++) {
        Str     name = n<PRESET_COUNT ? presetnames[n] : g->layouts[n-PRESET_COUNT].name;
        PalItem it   = {};
        it.kind  = PAL_LAYOUT;
        it.id    = concat(a, "l:", name);
        it.title = concat(a, name, " layout");
        it.note  = n<PRESET_COUNT ? Str("A layout, as View, Layout has it") : Str("A layout you saved");
        it.ref   = n;
        p->items = push(a, p->items, it);
        p->sess  = push(a, p->sess, (GuiSession *)0);
    }
    for (i32 i = 0; i < PANE_COUNT; i++) {
        if (!paneoffered(i)) {
            continue;
        }
        PalItem it = {};
        it.kind  = PAL_PANE;
        it.id    = concat(a, "p:", panestr(i));
        it.title = concat(a, panestr(i), " pane");
        it.group = "show";
        it.note  = g->panes[i] ? Str("Shown: brought to the front") : Str("Show it");
        it.ref   = i;
        p->items = push(a, p->items, it);
        p->sess  = push(a, p->sess, (GuiSession *)0);
    }
    if (trim(q).len) {
        Slice<i32> rows = cfgfindlist(g, a, q);
        for (i32 i = 0; i<(i32)rows.len && i<PAL_FOUND; i++) {
            CfgRow *r  = &g->cfgrows[rows[i]];
            PalItem it = {};
            it.kind  = PAL_SETTING;
            it.id    = concat(a, concat(a, concat(a, "c:", r->kind), concat(a, ":", r->name)),
                              concat(a, ":", r->key));
            it.title = clone(a, r->key);
            it.note  = cfgfindpage(a, r->kind, r->name);
            it.rank  = i + 1;
            it.ref   = rows[i];
            p->items = push(a, p->items, it);
            p->sess  = push(a, p->sess, (GuiSession *)0);
        }
        Slice<HelpHit> hits = helpsearch(a, q, PAL_FOUND);
        for (i32 i = 0; i<(i32)hits.len && i<PAL_FOUND; i++) {
            HelpHit *h     = &hits[i];
            Str      title = helptitle(h->page);
            PalItem  it    = {};
            it.kind  = PAL_HELP;
            it.id    = concat(a, concat(a, "h:", title), concat(a, ":", h->head));
            it.title = h->head.len ? h->head : title;
            it.note  = h->head.len ? title : Str{};
            if (h->snippet.len) {
                it.note = it.note.len ? concat(a, concat(a, it.note, " \xc2\xb7 "), h->snippet)
                                   : h->snippet;  // as Help's own search shows it
            }
            it.rank  = i + 1;
            it.ref   = h->page;
            it.arg   = h->head;
            p->items = push(a, p->items, it);
            p->sess  = push(a, p->sess, (GuiSession *)0);
        }
    }
    p->order = palorder(a, p->items, q);
    p->sel   = palkeep(p->items, p->order, pick);
}

struct PalCb {
    Gui *g;
    b32  edited;
};

static i32 palcb(ImGuiInputTextCallbackData *d)
{
    PalCb  *c = (PalCb *)d->UserData;
    PalWin *p = &c->g->palw;
    i32     n = (i32)p->order.len;
    if (d->EventFlag==ImGuiInputTextFlags_CallbackHistory && n) {
        b32 up = d->EventKey == ImGuiKey_UpArrow;
        p->sel    = (p->sel + (up ? n-1 : 1)) % n;
        p->follow = 1;
    } else if (d->EventFlag == ImGuiInputTextFlags_CallbackEdit) {
        c->edited = 1;
    }
    return 0;
}

// One row: a session's state's dot and title, and below, dim, what it
// is, its state, folder, host, group and when it was last active; for
// the rest, the title over its note.
static b32 palrow(Gui *g, Arena a, i32 i, b32 sel, i64 nowms)
{
    PalWin     *p     = &g->palw;
    GuiSession *s     = p->sess[p->order[i]];
    PalItem    *it    = &p->items[p->order[i]];
    f32         sc    = g->scale;
    f32         lineh = ImGui::GetTextLineHeight();
    f32         small = smallsize(&g->fonts);
    ImDrawList *dl    = ImGui::GetWindowDrawList();

    ImGui::PushID(i);
    ImVec2 at = ImGui::GetCursorScreenPos();
    f32    w  = ImGui::GetContentRegionAvail().x;
    b32    hit = ImGui::Selectable("##r", sel, 0, v2(0, 2*lineh + 4*sc));
    ImGui::PopID();
    if (sel && p->follow) {
        ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
        p->follow = 0;
    }
    if (!s) {
        f32 x = at.x + 6*sc + 13*sc;
        Pen t = {dl, &g->fonts, x, at.y + 2*sc, at.x + w, g->fonts.size};
        pen(&t, it->title, it->kind==PAL_SETTING ? STYLE_CODE : 0, g->pal->text);
        Pen b = {dl, &g->fonts, x, at.y + 2*sc + lineh, at.x + w, small};
        pen(&b, it->note, 0, g->pal->dim);
        return hit;
    }
    View *v     = s->view;
    i32   state = v ? v->state : s->state;

    f32 x     = at.x + 6*sc;
    b32 down  = s->host->kind!=GH_ENGINE && !s->host->up;
    b32 asks  = v && v->asks;
    u32 color = asks || s->readonly ? g->pal->amber : livecolor(g->pal, state);
    drawdot(dl, v2(x + 4*sc, at.y + 2*sc + lineh/2), 3.5f*sc, color);
    x += 13*sc;
    Pen t = {dl, &g->fonts, x, at.y + 2*sc, at.x + w, g->fonts.size};
    pen(&t, it->title, 0, g->pal->text);

    Pen b = {dl, &g->fonts, x, at.y + 2*sc + lineh, at.x + w, small};
    if (sessparent(s).len) {
        pen(&b, "subagent  ", STYLE_BOLD, g->pal->dim);
    } else if (sessside(s).len) {
        pen(&b, "side  ", STYLE_BOLD, g->pal->dim);
    }
    if (sessroot(g, s)->archived) {
        pen(&b, "archived  ", STYLE_BOLD, g->pal->dim);
    }
    if (down) {
        b32 back = s->host->r && !s->host->closing;
        pen(&b, back ? Str("reconnecting") : Str("disconnected"), STYLE_BOLD,
            back ? g->pal->amber : g->pal->dim);
    } else if (s->readonly) {
        pen(&b, "read-only", STYLE_BOLD, g->pal->amber);
    } else if (asks) {
        pen(&b, "needs an answer", STYLE_BOLD, g->pal->amber);
    } else {
        pen(&b, livelabel(v, state), STYLE_BOLD, livecolor(g->pal, state));
    }
    Buf d(&a, 256);
    print(&d, "  ");
    print(&d, sesswhere(&a, s));
    if (s->host->kind != GH_ENGINE) {
        print(&d, "  on ");
        print(&d, hostlabel(g, &a, s->host));
    }
    if (it->group.len) {
        print(&d, "  in ");
        print(&d, it->group);
    }
    if (s->lastact) {
        print(&d, "  ");
        printage(g, &d, nowms - s->lastact);
        print(&d, " ago");
    }
    pen(&b, finish(&d), 0, g->pal->dim);
    return hit;
}

// Go to an item picked, as its own pane would.
static void palgo(Gui *g, i32 i)
{
    PalWin  *p  = &g->palw;
    PalItem *it = &p->items[i];
    switch (it->kind) {
    case PAL_SESSION:
        if (!sessgo(g, p->sess[i])) {
            guilog(g, "the palette's pick could not open: in the Trash, or deleted");
        }
        break;
    case PAL_LAYOUT:
        picklayoutnum(g, it->ref);
        break;
    case PAL_PANE:
        setpane(g, it->ref, 1);
        break;
    case PAL_SETTING:
        // Its row, unless the configuration came again since (palbuild
        // then ran again too)
        if (it->ref<g->cfgrows.len && g->cfgrows[it->ref].key==it->title) {
            setpane(g, PANE_SETTINGS, 1);
            cfggoto(g, &g->cfgrows[it->ref]);
        }
        break;
    case PAL_HELP:
        helppick(g, it->ref);
        g->helpsect = it->arg.len ? clone(&g->perm, it->arg) : Str{};
        setpane(g, PANE_HELP, 1);
        break;
    }
}

static void drawpalette(Gui *g)
{
    PalWin *p  = &g->palw;
    f32     sc = g->scale;
    if (p->open) {
        p->open   = 0;
        p->close  = 0;
        p->q[0]   = 0;
        p->sel    = 0;
        p->follow = 1;
        palbuild(g, 0);
        ImGui::OpenPopup("##palette");
    }
    ImGuiViewport *vp = ImGui::GetMainViewport();
    f32 w = vp->WorkSize.x / 2;
    w = w<420*sc ? 420*sc : w>760*sc ? 760*sc : w;
    w = w>vp->WorkSize.x-32*sc ? vp->WorkSize.x-32*sc : w;
    ImGui::SetNextWindowPos(v2(vp->WorkPos.x + vp->WorkSize.x/2, vp->WorkPos.y + 48*sc),
                            ImGuiCond_Always, v2(0.5f, 0));
    ImGui::SetNextWindowSize(v2(w, 0));
    ImGuiWindowFlags f = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavInputs;
    p->shown = ImGui::BeginPopupModal("##palette", 0, f);
    p->rows  = 0;
    if (!p->shown) {
        return;
    }
    Arena a     = g->frame;
    i64   nowms = os_time(g->os);
    b32   appearing = ImGui::IsWindowAppearing();

    // Its keys, taken ahead of the field, which would otherwise let go
    // of the keyboard on Enter, and ahead of escapecancel, which would
    // cancel the shown session's turn on Esc as the palette closes. A
    // pick's Enter is held until released: the prompt has the keyboard
    // next, and a held Enter repeating there would send its draft.
    ImGuiID keys  = ImGui::GetID("##palkeys");
    b32     enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    b32     esc   = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    b32     pick  = enter && p->sel>=0 && p->sel<p->order.len;
    ImGuiInputFlags hold = pick ? ImGuiInputFlags_LockUntilRelease : ImGuiInputFlags_LockThisFrame;
    if (ImGui::IsKeyDown(ImGuiKey_Enter)) {
        ImGui::SetKeyOwner(ImGuiKey_Enter, keys, hold);
    }
    if (ImGui::IsKeyDown(ImGuiKey_KeypadEnter)) {
        ImGui::SetKeyOwner(ImGuiKey_KeypadEnter, keys, hold);
    }
    if (esc) {
        ImGui::SetKeyOwner(ImGuiKey_Escape, keys, ImGuiInputFlags_LockThisFrame);
    }
    i32 n    = (i32)p->order.len;
    i32 page = PAL_ROWS - 1;
    if (n && ImGui::IsKeyPressed(ImGuiKey_PageDown)) {
        p->sel    = p->sel+page < n ? p->sel+page : n-1;
        p->follow = 1;
    }
    if (n && ImGui::IsKeyPressed(ImGuiKey_PageUp)) {
        p->sel    = p->sel-page > 0 ? p->sel-page : 0;
        p->follow = 1;
    }

    // The field has the keyboard throughout: taken back from a field
    // elsewhere that was given it as the palette opened (Settings' search,
    // after a setting picked), but not from a row being clicked
    ImGuiContext &c    = *GImGui;
    b32           ours = c.ActiveIdWindow &&
                         c.ActiveIdWindow->RootWindow == ImGui::GetCurrentWindow()->RootWindow;
    if (appearing || !ours) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1);
    PalCb cb = {g, 0};
    ImGui::InputTextWithHint("##palq", "Go to a session, setting, help topic or layout",
                             p->q, sizeof(p->q),
                             ImGuiInputTextFlags_CallbackHistory |
                             ImGuiInputTextFlags_CallbackEdit, palcb, &cb);
    itemnamed(g, "palette query");
    if (cb.edited) {
        palbuild(g, 0);
        p->follow = 1;
        n = (i32)p->order.len;
    }

    // The list, PAL_ROWS rows high at most, scrolled to follow the pick,
    // a query's results in sections under their headings
    i32 hitrow = -1;
    if (n) {
        b32 heads = p->q[0] != 0;
        f32 gap   = ImGui::GetStyle().ItemSpacing.y;
        f32 rowh  = 2*ImGui::GetTextLineHeight() + 4*sc + gap;
        f32 headh = ImGui::GetTextLineHeight() + gap;
        f32 h     = 0;
        i32 sect  = -1;
        for (i32 i = 0; i<n && i<PAL_ROWS; i++) {
            i32 k = palsect(p->items[p->order[i]].kind);
            h    += rowh + (heads && k!=sect ? headh : 0);
            sect  = k;
        }
        ImGui::BeginChild("##palrows", v2(0, h), 0, ImGuiWindowFlags_NoNavInputs);
        sect = -1;
        for (i32 i = 0; i < n; i++) {
            i32 k = palsect(p->items[p->order[i]].kind);
            if (heads && k!=sect) {
                ImGui::TextDisabled("%s", palsectnames[k].cdata);
            }
            sect = k;
            if (palrow(g, a, i, i==p->sel, nowms)) {
                hitrow = i;
            }
        }
        ImGui::EndChild();
        Str first = p->items[p->order[0]].title;
        iz  len   = first.len<(iz)sizeof(p->first)-1 ? first.len : (iz)sizeof(p->first)-1;
        __builtin_memcpy(p->first, first.data, (uz)len);
        p->first[len] = 0;
        p->rows       = n;
    } else {
        ImGui::TextDisabled(p->q[0] ? "Nothing matches." : "No other sessions.");
    }
    ImGui::TextDisabled("Enter to open, Esc to close");

    // A click outside closes it, as Esc and Ctrl/Cmd+K do
    b32 outside = !appearing && ImGui::IsMouseClicked(0) &&
                  !ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                                          ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    i32 go = hitrow>=0 ? hitrow : pick ? p->sel : -1;
    if (go >= 0) {
        ImGui::CloseCurrentPopup();
        palgo(g, p->order[go]);
    } else if (esc || outside || p->close) {
        ImGui::CloseCurrentPopup();
        if (p->prompt) {
            g->promptfocus = 1;
            g->promptend   = 1;
        }
    }
    p->close = 0;
    ImGui::EndPopup();
}

// The menus' shortcuts, wherever the focus is (a global route, which a
// focused field's own use of a key goes ahead of): a new session, zoom
// by key or by Ctrl+wheel (Cmd on macOS), find and the session's
// folding toggles, Settings, the palette, up to a parent, and quitting on Linux, where
// no system shortcut does (macOS has Cmd+Q, Windows Alt+F4). While the
// terminal holds the keyboard, which culls global routes, the ones it
// leaves the app (termkept) route over it; the rest are its.
static void menukeys(Gui *g)
{
    ImGuiInputFlags glob = ImGuiInputFlags_RouteGlobal;
    ImGuiKeyChord   ctrl = ImGuiMod_Ctrl;
    ImGuiContext   &c    = *GImGui;
    ImGuiInputFlags keep = glob;
    if (c.ActiveId && c.ActiveId==g->termid) {
        keep |= ImGuiInputFlags_RouteOverActive;
    }
    if (cannew(g) && ImGui::Shortcut(ctrl|ImGuiKey_N, glob)) {
        newsession(g);
    }
    if (ImGui::Shortcut(ctrl|ImGuiMod_Shift|ImGuiKey_N, keep)) {
        newfree(g);
    }
    if (ImGui::Shortcut(ctrl|ImGuiKey_Equal, keep) ||
            ImGui::Shortcut(ctrl|ImGuiMod_Shift|ImGuiKey_Equal, keep) ||
            ImGui::Shortcut(ctrl|ImGuiKey_KeypadAdd, keep)) {
        zoomby(g, 1);
    }
    if (ImGui::Shortcut(ctrl|ImGuiKey_Minus, keep) ||
            ImGui::Shortcut(ctrl|ImGuiKey_KeypadSubtract, keep)) {
        zoomby(g, -1);
    }
    if (ImGui::Shortcut(ctrl|ImGuiKey_0, keep) ||
            ImGui::Shortcut(ctrl|ImGuiKey_Keypad0, keep)) {
        setzoom(g, 1);
    }
    // Find, and the session's toggles of its tool output and reasoning
    if (g->sess && ImGui::Shortcut(ctrl|ImGuiKey_F, glob)) {
        gui_find_open(g);
    }
    if (ImGui::Shortcut(ctrl|ImGuiKey_Comma, glob)) {
        opensettings(g);
    }
    // Open file, never over another dialog; unavailable, saying why
    if (ImGui::Shortcut(ctrl|ImGuiKey_O, glob) && !ImGui::GetTopMostPopupModal()) {
        Str why = fileaskwhy(g);
        if (why.len && g->sess) {
            setnote(g, g->sess, why, 0);
        } else {
            fileaskopen(g);
        }
    }
    // The palette: again to close it, and never over another dialog
    if (ImGui::Shortcut(ctrl|ImGuiKey_K, glob)) {
        if (g->palw.shown) {
            g->palw.close = 1;
        } else if (!ImGui::GetTopMostPopupModal()) {
            palopen(g);
        }
    }
    // Up from a subagent or side chat, as a file explorer goes up a
    // folder: Alt+Up, and on macOS Finder's Cmd+Up too
    if (ImGui::Shortcut(ImGuiMod_Alt|ImGuiKey_UpArrow, glob)) {
        goup(g, g->sess);
    }
    #if defined(__APPLE__)
    if (ImGui::Shortcut(ctrl|ImGuiKey_UpArrow, glob)) {
        goup(g, g->sess);
    }
    #endif
    if (ImGui::Shortcut(ctrl|ImGuiMod_Shift|ImGuiKey_S, keep)) {
        setsessionshell(g, 0, !sessionshell(g));
    }
    if (ImGui::Shortcut(ctrl|ImGuiMod_Shift|ImGuiKey_T, keep)) {
        setsessiontools(g, 0, !sessiontools(g));
    }
    if (ImGui::Shortcut(ctrl|ImGuiMod_Shift|ImGuiKey_R, keep)) {
        setsessionthink(g, 0, !sessionthink(g));
    }
    // Layouts by number, as the View menu's Layout lists them
    for (i32 n = 0; n < 9; n++) {
        if (ImGui::Shortcut(ctrl|(ImGuiKey)(ImGuiKey_1 + n), glob)) {
            picklayoutnum(g, n);
        }
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


// Layouts (dock.cpp)

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

// The id of a pane's tab, which its dock node selects
static ImGuiID panetab(i32 pane)
{
    return ImHashStr("#TAB", 0, ImHashStr(panenames[pane]));
}

// A pane's place among its dock node's tabs that appear with it, which
// ImGui sorts them by: its window's, or its settings' until it has one.
static void paneorder(i32 pane, i32 order)
{
    ImGuiWindow *w = ImGui::FindWindowByName(panenames[pane]);
    if (w) {
        w->DockOrder = (short)order;
    } else if (ImGuiWindowSettings *s = ImGui::FindWindowSettingsByID(ImHashStr(panenames[pane]))) {
        s->DockOrder = (short)order;
    }
}

// The first run's pane, the floating dock node id, centered in a dock
// space of a size at pos, most of it but not too big to read.
static void placefirst(Gui *g, ImGuiID id, ImVec2 pos, ImVec2 size)
{
    f32    sc = g->scale;
    ImVec2 fs = v2(size.x*0.6f, size.y*0.7f);
    fs.x = fs.x<1100*sc ? fs.x : 1100*sc;
    fs.y = fs.y<800*sc ? fs.y : 800*sc;
    ImVec2 at = v2(pos.x + (size.x - fs.x)/2, pos.y + (size.y - fs.y)/2);
    ImGui::DockBuilderSetNodePos(id, at);
    ImGui::DockBuilderSetNodeSize(id, fs);
    g->firstpane = id;
    g->firstspace = size;
    g->firstbox   = {at.x, at.y, at.x + fs.x, at.y + fs.y};
}

// Build the layout's subtree at part *i in the dock node id: each leaf
// the home of its panes, the front one of those shown selected (before
// DockBuilderFinish, which makes the tab bars from it). A split hands
// the central node to its side opposite the direction split toward.
static void applypart(DockLayout *l, i32 *i, ImGuiID id, u32 shown, ImGuiID *center,
                      f32 sc)
{
    i32       at = (*i)++;
    DockPart *p  = l->parts + at;
    if (p->kind == DOCK_LEAF) {
        for (i32 t = 0; t < p->ntabs; t++) {
            ImGui::DockBuilderDockWindow(panenames[p->tabs[t]], id);
            paneorder(p->tabs[t], t);
        }
        i32            front = dockfront(p, shown);
        ImGuiDockNode *n     = ImGui::DockBuilderGetNode(id);
        n->SelectedTabId = front<0 ? 0 : panetab(front);
        if (p->bare) {
            n->SetLocalFlags(n->LocalFlags | ImGuiDockNodeFlags_HiddenTabBar);
        }
        *center = p->central ? id : *center;
        return;
    }
    b32      lead = dockcentered(l, at+1);  // the center is in the first child
    ImGuiDir dir  = p->kind==DOCK_X ? (lead ? ImGuiDir_Right : ImGuiDir_Left)
                                    : (lead ? ImGuiDir_Down  : ImGuiDir_Up);
    ImGuiDockNode *n      = ImGui::DockBuilderGetNode(id);
    f32            share  = p->kind==DOCK_X ? dockcapped(p->share, p->most, n->Size.x, sc)
                                            : p->share;
    ImGui::DockBuilderSplitNode(id, dir, lead ? 1-share : share, 0, 0);
    ImGuiID        first  = n->ChildNodes[0]->ID;  // left or top
    ImGuiID        second = n->ChildNodes[1]->ID;
    applypart(l, i, first, shown, center, sc);
    applypart(l, i, second, shown, center, sc);
}

// Windows other than panes, which stay in the dock space if docked
// there when a layout is applied, in its center
static char const *dockguests[] = {"Output", "Screenshot"};

// Apply the layout picked (g->layout) in the dock space id, of a size
// at pos: its tree, each pane's home in it, shown or not, and each
// leaf's front tab. The panes it does not place float, at its place
// for them if it has one, as they are next drawn (beginpane). On a
// first run, Help and those of Settings and Hosts shown float in front
// instead, centered, Help selected.
static void applylayout(Gui *g, ImGuiID id, ImVec2 pos, ImVec2 size)
{
    DockLayout *l = &g->layout;
    b32 guests[countof(dockguests)] = {};
    for (i32 i = 0; i < countof(dockguests); i++) {
        ImGuiDockNode *n = ImGui::DockBuilderGetNode(saveddock(dockguests[i]));
        guests[i] = n && ImGui::DockNodeGetRootNode(n)->ID==id;
    }

    ImGuiID first = ImGui::GetID("FirstRun");
    ImGui::DockBuilderRemoveNode(first);
    ImGui::DockBuilderRemoveNode(id);
    ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(id, size);
    u32 shown = 0;
    for (i32 i = 0; i < PANE_COUNT; i++) {
        shown |= g->panes[i] ? 1u<<i : 0;
    }
    // A terminal shown gets its home under the prompt; hidden, it floats
    // until shown (dockwindows), so that its room stays the prompt's.
    // Files, which takes no room of its own, is a tab beside Plan's.
    b32 term  = !g->panes[PANE_TERMINAL] || !dockaddterm(l);
    b32 files = !dockaddfiles(l);
    g->homeless  = 0;
    g->homeless |= term && !(dockplaced(l)>>PANE_TERMINAL & 1) ? 1u<<PANE_TERMINAL : 0;
    g->homeless |= files && !(dockplaced(l)>>PANE_FILES & 1) ? 1u<<PANE_FILES : 0;
    i32     at     = 0;
    ImGuiID center = id;
    applypart(l, &at, id, shown, &center, g->scale);
    for (i32 i = 0; i < countof(dockguests); i++) {
        if (guests[i]) {
            ImGui::DockBuilderDockWindow(dockguests[i], center);
        }
    }
    u32 homed = dockplaced(l) & ~l->floats;
    g->floatwait = l->floats;
    for (i32 i = 0; i < PANE_COUNT; i++) {
        g->floatat[i] = l->place[i];
        if (!(homed>>i & 1)) {
            ImGui::DockBuilderDockWindow(panenames[i], 0);
        }
    }

    i32 extras[] = {PANE_HELP, PANE_SETTINGS, PANE_HOSTS};
    b32 floats   = g->firstrun && g->panes[PANE_HELP];
    g->firstpane = 0;
    if (floats) {
        ImGui::DockBuilderAddNode(first, ImGuiDockNodeFlags_None);
        placefirst(g, first, pos, size);
        for (i32 i = 0; i < countof(extras); i++) {
            if (g->panes[extras[i]]) {
                ImGui::DockBuilderDockWindow(panenames[extras[i]], first);
                paneorder(extras[i], i);
            }
        }
        ImGui::DockBuilderGetNode(first)->SelectedTabId = panetab(PANE_HELP);
        g->helpfocus = 2;
    }
    ImGui::DockBuilderFinish(id);
    if (floats) {
        ImGui::DockBuilderFinish(first);
    }
    g->firstrun = 0;
    g->layoutfocus = floats ? 0 : 4;
}

// A dock node selects the tab of the window focused, so a pane focused
// but not in front in its leaf hands the focus to the one that is. This
// runs for a few frames after a layout is applied, since the menu it
// was picked from gives the focus back as it closes.
static void layoutrefocus(Gui *g)
{
    g->layoutfocus--;
    scheddirty(&g->sched);
    DockLayout  *l     = &g->layout;
    ImGuiWindow *nav   = GImGui->NavWindow ? GImGui->NavWindow->RootWindow : 0;
    i32          pane  = -1;
    u32          shown = 0;
    for (i32 i = 0; i < PANE_COUNT; i++) {
        pane   = nav && ImGui::FindWindowByName(panenames[i])==nav ? i : pane;
        shown |= g->panes[i] ? 1u<<i : 0;
    }
    for (i32 i = 0; pane>=0 && i < l->nparts; i++) {
        DockPart *p = l->parts + i;
        for (i32 t = 0; p->kind==DOCK_LEAF && t < p->ntabs; t++) {
            i32 front = dockfront(p, shown);
            if (p->tabs[t]==pane && front>=0 && front!=pane) {
                ImGuiWindow *w = ImGui::FindWindowByName(panenames[front]);
                if (w) {
                    ImGui::FocusWindow(w);
                }
            }
        }
    }
}

// A split's first child's share of it, as ImGui sizes them: the side
// holding the central node takes what the other leaves it, else they
// share by the sizes they ask for (SizeRef, which a hidden child keeps
// while its Size goes stale).
static f32 splitshare(ImGuiDockNode *n)
{
    ImGuiStyle    &st    = ImGui::GetStyle();
    ImGuiDockNode *a     = n->ChildNodes[0];
    ImGuiDockNode *b     = n->ChildNodes[1];
    i32            axis  = n->SplitAxis;
    f32            avail = n->Size[axis] - st.DockingSeparatorSize;
    f32            least = ImTrunc(ImMin(avail, st.WindowMinSize[axis]*2)*0.5f);
    if (avail>0 && b->HasCentralNodeChild && a->SizeRef[axis]>0) {
        return ImMin(avail - least, a->SizeRef[axis]) / avail;
    } else if (avail>0 && a->HasCentralNodeChild && b->SizeRef[axis]>0) {
        return 1 - ImMin(avail - least, b->SizeRef[axis]) / avail;
    }
    f32 sum = a->SizeRef[axis] + b->SizeRef[axis];
    return sum>0 ? a->SizeRef[axis]/sum : 0.5f;
}

// The dock space's subtree at n, in pre-order: each leaf with the panes
// it is home to, those shown in their tabs' order, then the rest.
static void captureparts(DockLayout *l, ImGuiDockNode *n, ImGuiDockNode **home)
{
    if (n->IsSplitNode()) {
        docksplit(l, n->SplitAxis==ImGuiAxis_X ? DOCK_X : DOCK_Y, splitshare(n));
        captureparts(l, n->ChildNodes[0], home);
        captureparts(l, n->ChildNodes[1], home);
        return;
    }
    DockPart *p = dockpart(l, DOCK_LEAF);
    if (!p) {
        return;
    }
    p->central = n->IsCentralNode();
    p->bare    = (n->LocalFlags & ImGuiDockNodeFlags_HiddenTabBar) != 0;
    i32 ntabs  = n->TabBar ? n->TabBar->Tabs.Size : n->Windows.Size;
    for (i32 i = 0; i < ntabs; i++) {
        ImGuiWindow *w    = n->TabBar ? n->TabBar->Tabs[i].Window : n->Windows[i];
        i32          pane = w ? panenamed({(u8 *)w->Name, (iz)__builtin_strlen(w->Name)}) : -1;
        if (pane>=0 && home[pane]==n) {
            docktab(p, pane);
        }
    }
    for (i32 i = 0; i < PANE_COUNT; i++) {
        if (home[i] == n) {
            docktab(p, i);
        }
    }
    for (i32 t = 0; t < p->ntabs; t++) {
        p->front = panetab(p->tabs[t])==n->SelectedTabId ? p->tabs[t] : p->front;
    }
}

// The layout as the window has it, for Save layout...: the panes shown,
// the dock space's tree with each pane's home in it, and the others'
// places floating, as shares of the dock space: their floating dock
// node's, or their own.
static DockLayout capturelayout(Gui *g, b32 *ok)
{
    DockLayout     l    = {};
    ImGuiDockNode *root = ImGui::DockBuilderGetNode(g->dockspace);
    ImGuiDockNode *home[PANE_COUNT] = {};
    for (i32 i = 0; i < PANE_COUNT; i++) {
        ImGuiDockNode *n = ImGui::DockBuilderGetNode(saveddock(panenames[i]));
        b32 inside = root && n && n->IsLeafNode() && ImGui::DockNodeGetRootNode(n)==root;
        home[i]  = inside ? n : 0;
        l.shown |= g->panes[i] ? 1u<<i : 0;
    }
    if (root) {
        captureparts(&l, root, home);
    }

    ImVec2 vp = ImGui::GetMainViewport()->Pos;
    for (i32 i = 0; root && i < PANE_COUNT; i++) {
        if (home[i]) {
            continue;
        }
        ImGuiDockNode       *n  = ImGui::DockBuilderGetNode(saveddock(panenames[i]));
        ImGuiWindow         *w  = ImGui::FindWindowByName(panenames[i]);
        ImGuiWindowSettings *ws = ImGui::FindWindowSettingsByID(ImHashStr(panenames[i]));
        n = n ? ImGui::DockNodeGetRootNode(n) : 0;
        ImVec2 at = {}, size = {};
        if (n && n->IsFloatingNode()) {
            at   = n->Pos;
            size = n->Size;
        } else if (w) {
            at   = w->Pos;
            size = w->SizeFull;
        } else if (ws) {
            at   = v2(vp.x + ws->Pos.x, vp.y + ws->Pos.y);
            size = v2(ws->Size.x, ws->Size.y);
        }
        ImVec2 o = root->Pos, s = root->Size;
        if (size.x>0 && size.y>0 && s.x>0 && s.y>0) {
            dockfloat(&l, i, (at.x - o.x)/s.x, (at.y - o.y)/s.y, size.x/s.x, size.y/s.y);
        }
    }
    *ok = root && dockdone(&l);
    return l;
}

// Pick a layout: the panes shown and hidden as it says, then the layout
// applied the next frame, once the panes just shown are drawn
// (dockwindows).
static void picklayout(Gui *g, DockLayout *l)
{
    g->layout = *l;
    for (i32 i = 0; i < PANE_COUNT; i++) {
        if (!(l->keep>>i & 1)) {
            setpane(g, i, l->shown>>i & 1);
        }
    }
    g->panefocus     = 0;
    g->layoutpending = 1;
}

// View's Layout menu: the presets, the user's layouts, and saving and
// deleting those, each item named for the checklist.
// Layout number n from 0: the presets, then the user's saved layouts,
// as the View menu lists them and Ctrl/Cmd+1 to +9 pick them. False if
// there is none so numbered.
static b32 picklayoutnum(Gui *g, i32 n)
{
    if (n < PRESET_COUNT) {
        DockLayout l = dockpreset(n);
        picklayout(g, &l);
        return 1;
    } else if (n-PRESET_COUNT < g->layouts.len) {
        picklayout(g, &g->layouts[n-PRESET_COUNT]);
        return 1;
    }
    return 0;
}

// A layout's shortcut by its number from 0, Cmd/Ctrl+1 to +9, or null.
static char const *layoutkey(Gui *g, Arena *a, i32 n)
{
    if (n >= 9) {
        return 0;
    }
    char d[2] = {(char)('1' + n), 0};
    return cmdkey(g, a, Str{(u8 *)d, 1});
}

static void drawlayoutmenu(Gui *g)
{
    b32 menu = ImGui::BeginMenu("Layout");
    itemnamed(g, "menu Layout");
    if (!menu) {
        return;
    }
    Arena a = g->frame;
    for (i32 i = 0; i < PRESET_COUNT; i++) {
        b32 pick = ImGui::MenuItem(presetnames[i].cdata, layoutkey(g, &a, i));
        itemnamed(g, concat(&a, "layout ", presetnames[i]));
        itemtip("%s", presettips[i].cdata);
        if (pick) {
            DockLayout l = dockpreset(i);
            picklayout(g, &l);
        }
    }
    if (g->layouts.len) {
        ImGui::Separator();
    }
    for (iz i = 0; i < g->layouts.len; i++) {
        ImGui::PushID((i32)i);
        b32 pick = ImGui::MenuItem(cstr0(&a, g->layouts[i].name),
                                   layoutkey(g, &a, PRESET_COUNT + (i32)i));
        ImGui::PopID();
        itemnamed(g, concat(&a, "layout ", g->layouts[i].name));
        if (pick) {
            picklayout(g, &g->layouts[i]);
        }
    }
    ImGui::Separator();
    b32 save = ImGui::MenuItem("Save layout...");
    itemnamed(g, "Save layout...");
    if (save) {
        g->opensavelayout = 1;
    }
    b32 del = ImGui::BeginMenu("Delete layout", g->layouts.len>0);
    itemnamed(g, "menu Delete layout");
    if (del) {
        for (iz i = 0; i < g->layouts.len; i++) {
            ImGui::PushID((i32)i);
            b32 pick = ImGui::MenuItem(cstr0(&a, g->layouts[i].name));
            ImGui::PopID();
            itemnamed(g, concat(&a, "delete ", g->layouts[i].name));
            if (pick) {
                g->dellayout     = (i32)i + 1;
                g->opendellayout = 1;
            }
        }
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

// Save layout...'s dialog, which names the layout as it is now, and
// Delete layout's, which confirms.
static void drawlayoutdialogs(Gui *g)
{
    Arena a  = g->frame;
    f32   sc = g->scale;
    if (g->opensavelayout) {
        g->opensavelayout = 0;
        g->layoutbuf[0]   = 0;
        ImGui::OpenPopup("Save layout");
    }
    ImGui::SetNextWindowSize(v2(400*sc, 0));
    if (ImGui::BeginPopupModal("Save layout", 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted("The panes as they are now, to return to from the "
                               "View menu's Layout:");
        ImGui::PopTextWrapPos();
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-1);
        b32 enter = ImGui::InputTextWithHint("##layoutname", "Name", g->layoutbuf, FIELD_MAX,
                                             ImGuiInputTextFlags_EnterReturnsTrue);
        itemnamed(g, "layout name");
        Str name = trim({(u8 *)g->layoutbuf, (iz)__builtin_strlen(g->layoutbuf)});
        Str why  = docknamewhy(name);
        b32 dup  = 0;
        for (iz i = 0; i < g->layouts.len; i++) {
            dup |= equalfold(g->layouts[i].name, name);
        }
        if (name.len && why.len) {
            ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
            ImGui::TextWrapped("%s", cstr0(&a, why));
            ImGui::PopStyleColor();
        } else if (dup) {
            ImGui::TextDisabled("It replaces the layout saved by that name.");
        }
        ImGui::Spacing();
        ImGui::BeginDisabled(why.len > 0);
        b32 save = ImGui::Button(dup ? "Replace" : "Save", v2(120*sc, 0));
        ImGui::EndDisabled();
        itemnamed(g, "layout save");
        ImGui::SameLine();
        b32 cancel = ImGui::Button("Cancel", v2(120*sc, 0)) || modalkey(ImGuiKey_Escape);
        if ((save || enter) && !why.len) {
            b32        ok = 0;
            DockLayout l  = capturelayout(g, &ok);
            if (ok) {
                l.name = clone(&g->perm, name);
                iz i = 0;
                for (; i<g->layouts.len && !equalfold(g->layouts[i].name, name); i++) {}
                if (i < g->layouts.len) {
                    g->layouts[i] = l;  // in its place in the menu
                } else {
                    g->layouts = push(&g->perm, g->layouts, l);
                }
                ImGui::MarkIniSettingsDirty();
            } else {
                guilog(g, "the layout could not be saved: its dock nodes make no layout");
            }
            ImGui::CloseCurrentPopup();
        } else if (cancel) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (g->opendellayout) {
        g->opendellayout = 0;
        ImGui::OpenPopup("Delete layout?");
    }
    if (ImGui::BeginPopupModal("Delete layout?", 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        iz  i     = g->dellayout - 1;
        b32 there = i>=0 && i<g->layouts.len;
        ImGui::Text("Delete the layout %s?", there ? cstr0(&a, g->layouts[i].name) : "");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, fade(g->pal->bad, 0.6f));
        b32 del = ImGui::Button("Delete", v2(120*sc, 0));
        ImGui::PopStyleColor();
        itemnamed(g, "layout delete");
        ImGui::SameLine();
        b32 cancel = ImGui::Button("Cancel", v2(120*sc, 0)) || modalkey(ImGuiKey_Escape);
        if (del && there) {
            for (; i < g->layouts.len-1; i++) {
                g->layouts[i] = g->layouts[i+1];
            }
            g->layouts.len--;
            ImGui::MarkIniSettingsDirty();
        }
        if (del || cancel) {
            g->dellayout = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
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
        if (ImGui::MenuItem("Go to...", cmdkey(g, &a, "K"))) {
            palopen(g);
        }
        itemnamed(g, "menu Go to");
        itemtip("Find a session by its title, folder or group, or a layout, pane, setting or Help topic");
        ImGui::Separator();
        b32 add = ImGui::MenuItem("New session...", cmdkey(g, &a, "N"), false, cannew(g));
        itemrect(g, RECT_NEWBTN);
        itemtip("Start a session in a working directory");
        if (add) {
            newsession(g);
        }
        if (menutip(g, &a, "New session, no folder", newfreewhy(g),
                    "Start a session with no working folder, with the default "
                    "model, and type its prompt", cmdkey(g, &a, "Shift+N"))) {
            newfree(g);
        }
        Str iwhy = !cannew(g) ? Str("No engine") : importwhy(g, g->main);
        if (menuitem(g, a, "Import session...", iwhy)) {
            importstart(g, 0);
        }
        itemtip("A session from its raw log, exported here or elsewhere");
        GuiHost *fah = fileaskhost(g);
        Str      fat = concat(&a, concat(&a, "Show a file on ",
                                         filehostname(g, &a, fah ? fah->uid : 0)),
                          " in the Files pane, with :N after its path for a line");
        if (menutip(g, &a, "Open file...", fileaskwhy(g), fat, cmdkey(g, &a, "O"))) {
            fileaskopen(g);
        }
        itemnamed(g, "menu open file");
        if (menutip(g, &a, "Empty Trash...", emptywhy(g),
                    "Remove every session in the Trash for good now, once you confirm")) {
            askempty(g);
        }
        ImGui::Separator();
        // What the shown session's turn waits for, as Cmd+B moves it
        if (ImGui::MenuItem(bglabel, cmdkey(g, &a, "B"), false, canbackground(g->sess))) {
            guibackground(g, g->sess, {});
        }
        itemnamed(g, "menu Run in background");
        itemtip("Let the running command go on as a background job, or "
                              "the subagents on by themselves, and the agent with them: "
                              "it is told as each ends");
        // The session operations, on the session shown, as its context
        // menu in the sidebar has them
        ImGui::Separator();
        if (g->sess && !g->sess->gone && g->enginekind!=ENGINE_FAKE) {
            g->menusess = g->sess;
            sessitems(g, a, g->sess);
        } else {
            ImGui::BeginDisabled();
            ImGui::MenuItem("Rename...");
            ImGui::MenuItem("Fork");
            ImGui::MenuItem("Switch model...");
            ImGui::MenuItem("Export...");
            ImGui::Separator();
            ImGui::MenuItem("Archive");
            ImGui::MenuItem("Move to Trash");
            ImGui::Separator();
            ImGui::MenuItem("Open in external terminal");
            ImGui::MenuItem("New terminal tab", "Ctrl+Shift+`");
            ImGui::MenuItem("Close");
            ImGui::EndDisabled();
        }
        ImGui::Separator();
        char const *setkey = cmdkey(g, &a, ",");  // before a is copied
        if (menuitem(g, a, "Settings...", {}, setkey)) {
            opensettings(g);
        }
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
            copyout(g, v, COPY_TEXT);
        }
        if (ImGui::MenuItem("Copy as Markdown", cmdkey(g, &a, "Shift+C"), false, any)) {
            copyout(g, v, COPY_MARKDOWN);
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
        Arena k = a;  // let go before drawlayoutmenu takes the frame's arena
        if (ImGui::MenuItem("Go to...", cmdkey(g, &k, "K"))) {
            palopen(g);
        }
        itemtip("Find a session by its title, folder or group, or a layout, pane, setting or Help topic");
        ImGui::Separator();
        // Layouts first, the quickest to reach, then the panes
        drawlayoutmenu(g);
        ImGui::Separator();
        for (i32 i = 0; i < PANE_COUNT; i++) {
            if (!paneoffered(i)) {
                continue;
            } else if (i == PANE_HELP) {
                ImGui::Separator();
            }
            bool        on  = g->panes[i];
            char const *key = i==PANE_SETTINGS ? cmdkey(g, &a, ",") : 0;
            if (ImGui::MenuItem(panenames[i], key, &on)) {
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
        if (ImGui::MenuItem("Fold shell output", shortcutname(g, 2), sessionshell(g), has)) {
            setsessionshell(g, 0, !sessionshell(g));
        }
        if (ImGui::MenuItem("Expand tool output", shortcutname(g, 0), sessiontools(g), has)) {
            setsessiontools(g, 0, !sessiontools(g));
        }
        if (ImGui::MenuItem("Expand thinking", shortcutname(g, 1), sessionthink(g), has)) {
            setsessionthink(g, 0, !sessionthink(g));
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Theme")) {
            // The built-in themes, then the configuration's, by name
            static char const *labels[] = {"System", "Dark", "Light"};
            static_assert(countof(labels) == THEME_COUNT);
            Str pick = {};
            for (i32 i = 0; i < THEME_COUNT; i++) {
                b32 on = !g->themehas && g->themeset==i;
                if (ImGui::MenuItem(labels[i], 0, on) && !on) {
                    pick = themenames[i];
                }
            }
            b32 sep = 0;
            for (iz i = 0; i < g->cfgheads.len; i++) {
                CfgHead *h = &g->cfgheads[i];
                if (!themehead(g, i)) {
                    continue;
                } else if (!sep) {
                    ImGui::Separator();
                    sep = 1;
                }
                b32 on = g->themehas && g->themename==h->name;
                if (ImGui::MenuItem(cstr0(&a, h->name), 0, on) && !on) {
                    pick = clone(&a, h->name);
                }
            }
            if (pick.len) {
                // As the settings window picks it: at once, then kept
                settheme(g, pick);
                if (g->enginekind!=ENGINE_FAKE && engineup(g)) {
                    guiconfigset(g, "ui", "", "theme", pick);
                }
            }
            ImGui::EndMenu();
        }
        if (g->enginekind == ENGINE_FAKE) {
            ImGui::SeparatorText("Debug");
            b32 modal = ImGui::MenuItem("Modal dialog...");
            itemrect(g, RECT_MODALITEM);
            if (modal) {
                g->openmodal = 1;
            }
        }
        ImGui::EndMenu();
    }

    menu = ImGui::BeginMenu("Help");
    itemnamed(g, "menu Help");
    if (menu) {
        b32 help = ImGui::MenuItem("Help");
        itemnamed(g, "open Help");
        if (help) {
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
    drawpalette(g);
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
        b32 enter = modalenter();
        ImGui::TextUnformatted("Prospero " PROSPERO_VERSION PROSPERO_DESCRIBE);
        ImGui::TextUnformatted("A coding harness: the agent's turns, tools and sessions.");
        ImGui::Spacing();
        ImGui::SeparatorText("Third-party software");
        ImGui::BulletText("Dear ImGui: Copyright 2014 Omar Cornut (MIT)");
        ImGui::BulletText("FreeType: Copyright 1996 The FreeType Project (FTL)");
        ImGui::BulletText("MD4C: Copyright 2016 Martin Mitas (MIT)");
        ImGui::BulletText("Wuffs: Copyright 2017 The Wuffs Authors (Apache 2.0 or MIT)");
        #ifndef _WIN32
        ImGui::BulletText("SDL: Copyright 1997 Sam Lantinga (zlib)");
        ImGui::BulletText("cpp-httplib: Copyright 2017 Yuji Hirose (MIT)");
        ImGui::BulletText("Mbed TLS: Copyright The Mbed TLS Contributors (Apache 2.0)");
        #endif
        drawfontcredits();
        ImGui::Spacing();
        if (ImGui::Button("Close", v2(120*sc, 0)) || ImGui::Shortcut(ImGuiKey_Escape) || enter) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    drawnew(g);
    drawwt(g);
    drawmove(g);
    drawclose(g);
    drawsessdialogs(g);
    drawlayoutdialogs(g);
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
    if (g->floatpane == pane+1) {
        // The checklist's sizing of a pane: floating, then docked back
        g->floatpane = 0;
        ImGuiWindow *w = ImGui::FindWindowByName(panenames[pane]);
        if (g->floatsize.x > 0) {
            if (!g->floating) {
                g->floatdock = w ? w->DockId : 0;
                g->floating  = 1;
            }
            ImVec2 c = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
            ImGui::SetNextWindowSize(g->floatsize, ImGuiCond_Always);
            ImGui::SetNextWindowPos(c, ImGuiCond_Always, v2(0.5f, 0.5f));
        } else if (g->floating) {
            ImGui::SetNextWindowDockID(g->floatdock, ImGuiCond_Always);
            g->floating = 0;
        }
        ImGui::SetNextWindowFocus();
    }
    if (g->floatwait>>pane & 1) {
        // A layout's floating pane, at its place in the dock space
        g->floatwait &= ~(1u << pane);
        DockRect r = g->floatat[pane];
        ImVec2   o = g->dockpos, s = g->docksize;
        ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
        ImGui::SetNextWindowPos(v2(o.x + r.x*s.x, o.y + r.y*s.y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(v2(r.w*s.x, r.h*s.y), ImGuiCond_Always);
    }
    // A window focused as it appears selects its tab in its dock node,
    // over the tab the layout file saved, or a layout applied, which the
    // panes appearing together would otherwise take from each other, the
    // last one winning
    ImGuiWindowFlags flags = 0;
    if ((g->restored && ImGui::GetFrameCount()<=1) || g->layoutquiet) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
    }
    bool open = 1;
    b32  vis  = ImGui::Begin(panenames[pane], &open, flags);
    if (!open) {
        setpane(g, pane, 0);
    }
    // What is drawn now is this pane's, which a deployment begun from it
    // notes (deploystart); and it was shown this frame (drawhosttoast)
    g->inpane = pane + 1;
    if (vis) {
        g->panedrawn[pane] = g->total.frames;
    }
    return vis;
}

// A pane shown with no home in the dock space, the Terminal or Files:
// its first showing, or after a layout left it none (homeless). The
// layout as it is, with the pane given its home (dockaddterm,
// dockaddfiles), is applied the next frame.
static void panehome(Gui *g, i32 pane)
{
    char const *name  = panenames[pane];
    b32         known = saveddock(name) || ImGui::FindWindowByName(name) ||
                        ImGui::FindWindowSettingsByID(ImHashStr(name));
    b32         left  = g->homeless>>pane & 1;
    if ((!left && known) || g->layoutpending || g->layoutnow) {
        return;
    }
    g->homeless &= ~(1u << pane);
    b32        ok = 0;
    DockLayout l  = capturelayout(g, &ok);
    if (left) {
        // Floating only as its node went with the layout, where a pane
        // floated by hand would have kept it
        l.floats &= ~(1u << pane);
    }
    b32 added = pane==PANE_TERMINAL ? dockaddterm(&l) : dockaddfiles(&l);
    if (ok && added) {
        l.name = g->layout.name;
        l.keep = ~(u32)0;
        picklayout(g, &l);
        g->panefocus = pane + 1;
    }
}

static void dockwindows(Gui *g)
{
    termsweep(g);
    filesweep(g);
    termhotkeys(g);
    menukeys(g);
    termgrab(g);
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
    g->dockpos   = pos;
    g->docksize  = size;

    // A layout picked waits a frame, so that windows just shown again
    // are submitted before the builder places them. In neither frame do
    // the panes appearing take the focus (beginpane).
    g->layoutquiet = 0;
    if (g->layoutpending) {
        g->layoutpending = 0;
        g->layoutnow     = 1;
        g->layoutquiet   = 1;
    } else if (g->layoutnow || (g->firstlayout && !ImGui::DockBuilderGetNode(id))) {
        g->firstlayout = 0;
        g->layoutnow   = 0;
        g->layoutquiet = 1;
        applylayout(g, id, pos, size);
    } else if (g->layoutfocus > 0) {
        layoutrefocus(g);
    } else if (g->firstpane) {
        // The first run's pane stays centered while the window settles
        // (a window manager maximizing it, say), until it is moved,
        // resized, or gone
        ImGuiDockNode *n    = ImGui::DockBuilderGetNode(g->firstpane);
        Rect           b    = g->firstbox;
        f32            off  = 0;
        if (n) {
            f32 d[] = {n->Pos.x - b.x0, n->Pos.y - b.y0,
                       n->Pos.x+n->Size.x - b.x1, n->Pos.y+n->Size.y - b.y1};
            for (i32 i = 0; i < countof(d); i++) {
                off = d[i]>off ? d[i] : -d[i]>off ? -d[i] : off;
            }
        }
        b32 kept = n && !n->ParentNode && off<3;  // as rounded
        if (!kept) {
            g->firstpane = 0;
        } else if (size.x!=g->firstspace.x || size.y!=g->firstspace.y) {
            placefirst(g, g->firstpane, pos, size);
        }
    }
    ImGui::DockSpace(id, v2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);
    drawdialogs(g);
    ImGui::End();

    b32 *show = g->panes;
    if (show[PANE_SESSIONS]) {
        if (beginpane(g, PANE_SESSIONS)) {
            // Not Esc: the shown session's turn is not the rows' business
            backgroundkey(g);
            drawsessions(g);
            drawtrashtoast(g, PANE_SESSIONS, drawhosttoast(g, PANE_SESSIONS));
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
    g->planshown = -1;  // not drawn
    if (show[PANE_PLAN]) {
        ImGuiID logs = saveddock("Log");
        if (logs) {
            ImGui::SetNextWindowDockID(logs, ImGuiCond_FirstUseEver);
        }
        if (beginpane(g, PANE_PLAN)) {
            drawplan(g);
        }
        ImGui::End();
    }
    g->changesshown = 0;
    if (show[PANE_CHANGES]) {
        near = saveddock("Plan");
        near = near ? near : saveddock("Log");
        if (near) {
            ImGui::SetNextWindowDockID(near, ImGuiCond_FirstUseEver);
        }
        g->changesshown = beginpane(g, PANE_CHANGES);
        if (g->changesshown) {
            drawchanges(g);
        }
        ImGui::End();
    }
    // Submitted first, so that it is the tab selected in its dock node.
    if (show[PANE_TRANSCRIPT]) {
        if (beginpane(g, PANE_TRANSCRIPT)) {
            escapecancel(g);
            backgroundkey(g);
            drawtranscript(g);
        }
        ImGui::End();
    }
    if (show[PANE_HELP]) {
        // Selected in the first run's pane, after Jobs is in its node
        if (g->helpfocus && !--g->helpfocus) {
            ImGui::SetNextWindowFocus();
        }
        // A floating node of Help alone keeps its tab bar, and with it
        // the node, rather than leaving Help a loose window
        ImGuiWindowClass wc;
        wc.DockingAlwaysTabBar = 1;
        ImGui::SetNextWindowClass(&wc);
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
    if (PROSPERO_DEVTOOLS && show[PANE_METRICS]) {
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
    near = near ? near : saveddock("Log");
    if (show[PANE_HOSTS]) {
        if (near) {
            ImGui::SetNextWindowDockID(near, ImGuiCond_FirstUseEver);
        }
        if (beginpane(g, PANE_HOSTS)) {
            drawhosts(g);
            drawtrashtoast(g, PANE_HOSTS, drawhosttoast(g, PANE_HOSTS));
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
    // Windows that a layout saved before them lacks join a neighbor
    near = saveddock("Log");
    g->jobsvisible = 0;
    if (show[PANE_JOBS]) {
        if (near) {
            ImGui::SetNextWindowDockID(near, ImGuiCond_FirstUseEver);
        }
        g->jobsvisible = beginpane(g, PANE_JOBS);
        if (g->jobsvisible) {
            drawjobs(g);
        }
        ImGui::End();
    }
    if (!g->jobsvisible) {
        g->jobsview = 0;  // what it shows next is all new to it
    }
    if (show[PANE_LOG]) {
        if (beginpane(g, PANE_LOG)) {
            drawlog(g);
        }
        ImGui::End();
    }
    g->termfocused = 0;
    if (show[PANE_TERMINAL]) {
        // Its first showing, or after a layout left it no home: under the
        // prompt, as the layout with it applied the next frame
        panehome(g, PANE_TERMINAL);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, v2(4*g->scale, 4*g->scale));
        b32 vis = beginpane(g, PANE_TERMINAL);
        ImGui::PopStyleVar();
        if (vis) {
            drawterm(g);
        }
        ImGui::End();
    }
    g->filesdrawn = 0;
    if (show[PANE_FILES]) {
        // Its first showing, or after a layout left it no home: beside
        // Plan, as the layout with it applied the next frame
        panehome(g, PANE_FILES);
        g->filesdrawn = beginpane(g, PANE_FILES);
        if (g->filesdrawn) {
            drawfiles(g);
        }
        ImGui::End();
    }
    if (!g->filesdrawn) {
        fetchtick(g, 0);  // a far read only for the front tab shown
    }
    g->inpane = 0;
}
