// Back and forward through jumps (#58), the window's side of nav.cpp:
// which pane has the focus, where the user is (a spot), and putting a
// spot back. A jump calls navjump before it moves; Back and Forward
// (the mouse's side buttons, the keys, View's items) call navgo.
//
// This is free and unencumbered software released into the public domain.

// Which pane has the focus, from the window with ImGui's: up its
// parents (a pane's popups and children are its) to a pane. Sessions,
// and menus, the palette and dialogs above no pane, leave it as it was,
// since a sidebar row, a menu item or a palette pick acts with its own
// window focused, after the pane the user was in.
static void navtrack(Gui *g)
{
    if (ImGui::GetTopMostPopupModal()) {
        return;
    }
    for (ImGuiWindow *w = GImGui->NavWindow; w; w = w->ParentWindow) {
        for (i32 p = 0; p < PANE_COUNT; p++) {
            if (w->ID == ImHashStr(panenames[p])) {
                g->navpane = p==PANE_SESSIONS ? g->navpane : p;
                return;
            }
        }
    }
}

// The pane a spot goes by: the focused one while shown, else the
// Transcript, the window's default.
static i32 navpaneof(Gui *g)
{
    i32 p = g->navpane;
    b32 ok = p>=0 && p<PANE_COUNT && p!=PANE_SESSIONS && g->panes[p];
    return ok ? p : (i32)PANE_TRANSCRIPT;
}

// The transcript's place, as drawtranscript reads it: following its
// end, else its anchor. (The prompt has the focus while the transcript
// is read, so its spot keeps this too.)
static void navplace(Gui *g, NavSpot *s)
{
    GuiSession  *gs = g->sess ? g->sess : &g->nosession;
    View        *v  = g->view;
    ImGuiWindow *w  = gs->win;
    s->stick = gs->stick || !v || !w || v->lastw<=0;
    if (!s->stick) {
        f32 y = w->Scroll.y;
        if (w->ScrollTarget.y < FLT_MAX) {
            y = w->ScrollTarget.y;
        }
        y = y<0 ? 0 : y>w->ScrollMax.y ? w->ScrollMax.y : y;
        Anchor an = viewanchor(v, &g->measure, v->lastw, g->scale, v->head, y);
        s->stick = an.msg < 0;
        s->view  = v;
        s->msg   = an.msg;
        s->off   = an.off;
        s->dy    = an.dy;
    }
}

// Where the user is now.
static NavSpot navnow(Gui *g)
{
    NavSpot s = {};
    s.sess = g->sess;
    s.pane = navpaneof(g);

    switch (s.pane) {
    case PANE_TRANSCRIPT:
    case PANE_PROMPT:
        navplace(g, &s);
        break;

    case PANE_FILES: {
        FileTab *ft = filefront(g);
        if (!ft || ft->path.len>NAV_TEXT) {
            break;  // the pane alone
        }
        s.host    = ft->host;
        s.win     = ft->win;
        s.mode    = ft->mode;
        s.top     = ft->top;
        s.fdy     = ft->dy;
        s.scrollx = ft->scrollx;
        s.ems     = ft->ems;
        s.band0   = ft->band0;
        s.band1   = ft->band1;
        s.jump    = ft->jumped ? 0 : ft->jump;
        s.goline  = ft->goline;
        s.gocol   = ft->gocol;
        copybytes(s.section, ft->section, ft->sectionlen);
        s.sectionlen = ft->sectionlen;
        copybytes(s.text, ft->path.data, ft->path.len);
        s.textlen = (i32)ft->path.len;
    } break;

    case PANE_HELP:
        s.page = g->helppage;
        s.y    = g->helpdoc && g->helpdoc->win ? g->helpdoc->win->Scroll.y : 0;
        break;

    case PANE_SETTINGS:
        s.diag    = g->cfgdiag;
        s.kindlen = (i32)g->cfgpkind.len;
        s.textlen = (i32)(g->cfgpkind.len + g->cfgpname.len);
        copybytes(s.text, g->cfgpkind.data, g->cfgpkind.len);
        copybytes(s.text+s.kindlen, g->cfgpname.data, g->cfgpname.len);
        break;

    case PANE_TERMINAL: {
        TermSet *set = &filesroot(g)->terms;
        s.serial = set->n ? set->tabs[set->front]->serial : 0;
    } break;
    }
    return s;
}

// A jump is about to leave where the user is. Nothing is taken from an
// empty window, nothing shown in it.
static void navjump(Gui *g)
{
    NavSpot now = navnow(g);
    if (!now.sess && (now.pane==PANE_TRANSCRIPT || now.pane==PANE_PROMPT)) {
        return;
    }
    g->navto   = {};
    g->navhelp = 0;
    navhold(&g->nav, &now);
}

// Whether a spot's session can still be shown, as sessgo asks.
static b32 navlive(void *ctx, void *sess)
{
    Gui        *g = (Gui *)ctx;
    GuiSession *s = (GuiSession *)sess;
    return !s->gone && !sessroot(g, s)->trashed;
}

// Put a spot back: its session shown (as a sidebar click does, minus
// the Transcript and box it brings), its pane shown and focused, and
// its place.
static void navapply(Gui *g, NavSpot *s)
{
    g->promptfocus = 0;
    g->termfocus   = 0;
    g->navto       = {};
    g->navhelp     = 0;
    GuiSession *to = (GuiSession *)s->sess;
    if (to != g->sess) {
        if (to) {
            guiselect(g, to);
        } else {
            setcur(g, 0);
        }
    }
    GuiSession *gs = g->sess ? g->sess : &g->nosession;

    switch (s->pane) {
    case PANE_TRANSCRIPT:
    case PANE_PROMPT:
        // The Transcript forward, as a sidebar click brings it, even for
        // the prompt, whose box takes the keyboard (below): its place is
        // the transcript's, which a tab over it would hide
        setpane(g, PANE_TRANSCRIPT, 1);
        if (s->stick) {
            gs->stick = 1;
        } else {
            // For drawtranscript, laid out there
            g->navto.sess = gs;
            g->navto.view = (View *)s->view;
            g->navto.an   = {s->msg, s->off, s->dy};
        }
        if (s->pane == PANE_PROMPT) {
            g->promptfocus = 1;
            g->promptend   = 1;
        }
        break;

    case PANE_FILES: {
        if (!s->textlen) {
            setpane(g, PANE_FILES, 1);
            break;
        }
        // Its tab again, opened by path if closed since
        FileTab *ft = filesopen(g, g->sess, s->host, navtext(s), 0, 0, {});
        FileBody *b = ft->body;
        b32 mdok    = !b || (b->md && b->body.len<=DOC_MAXSIZE);
        b32 placed  = s->mode==FMODE_TEXT || (s->mode==FMODE_MARKDOWN && mdok);
        if (placed) {
            ft->mode = s->mode;
        }
        ft->band0  = s->band0;
        ft->band1  = s->band1;
        ft->goline = s->goline;
        ft->gocol  = s->gocol;
        if (s->jump) {
            ft->jump   = s->jump;  // not made yet: made now
            ft->jumped = 0;
        } else if (placed) {
            ft->jumped  = 1;
            ft->top     = s->top;
            ft->dy      = s->fdy;
            ft->scrollx = s->scrollx;
            ft->ems     = s->ems;
        }
        copybytes(ft->section, s->section, s->sectionlen);
        ft->sectionlen = s->sectionlen;
        g->fshowing    = 0;  // drawfiles puts its place back
    } break;

    case PANE_HELP:
        helppick(g, s->page);
        g->helpsect = {};
        if (g->helpdoc) {
            g->helpdoc->section = {};
        }
        g->navhelpy = s->y;  // for drawhelp
        g->navhelp  = 1;
        setpane(g, PANE_HELP, 1);
        break;

    case PANE_SETTINGS: {
        // A page gone since gives way, as drawsettings does
        Str kind = {s->text, s->kindlen};
        Str name = {s->text+s->kindlen, s->textlen-s->kindlen};
        cfgselect(g, kind, name);
        g->cfgdiag    = s->diag;
        g->cfgshowsel = 1;
        setpane(g, PANE_SETTINGS, 1);
    } break;

    case PANE_TERMINAL: {
        // Its tab in front, without the keyboard, so that the keys go
        // back again and no shell starts
        TermSet *set = &filesroot(g)->terms;
        for (i32 i = 0; i < set->n; i++) {
            if (set->tabs[i]->serial == s->serial) {
                termsetshow(set, i);
            }
        }
        setpane(g, PANE_TERMINAL, 1);
    } break;

    default:
        setpane(g, s->pane, 1);
    }
    scheddirty(&g->sched);
}

// Back (NAV_BACK) or Forward (NAV_FWD), if there is anywhere to go:
// whether it went.
static b32 navgo(Gui *g, i32 dir)
{
    NavSpot now = navnow(g);
    navsettle(&g->nav, &now);
    if (!navpeek(&g->nav, dir, &now, navlive, g)) {
        return 0;
    }
    NavSpot to = navstep(&g->nav, dir, &now);
    navapply(g, &to);
    return 1;
}

// Where a step goes, for View's items, else null.
static NavSpot *navwhereto(Gui *g, i32 dir)
{
    NavSpot now = navnow(g);
    return navwhere(&g->nav, dir, &now, navlive, g);
}

static Str navpanename(i32 p)
{
    return {(u8 *)panenames[p], (iz)__builtin_strlen(panenames[p])};
}

// A spot's name for a tooltip: "Files: hello.c", "Help: Keyboard and
// mouse", "the Transcript of TITLE" when another session's.
static Str navname(Gui *g, Arena *a, NavSpot *s)
{
    Buf b(a, 256);
    switch (s->pane) {
    case PANE_TRANSCRIPT:
    case PANE_PROMPT:
    case PANE_TERMINAL:
        print(&b, "the ");
        print(&b, navpanename(s->pane));
        break;
    case PANE_FILES:
        print(&b, "Files");
        if (s->textlen) {
            print(&b, ": ");
            print(&b, basename(navtext(s)));
        }
        break;
    case PANE_HELP:
        print(&b, "Help");
        if (s->page>=0 && s->page<HELP_COUNT) {
            print(&b, ": ");
            print(&b, helptitle(s->page));
        }
        break;
    default:
        print(&b, navpanename(s->pane));
    }
    GuiSession *to = (GuiSession *)s->sess;
    if (to && to!=g->sess) {
        print(&b, " of ");
        print(&b, shorten(a, sesstitle(to), 60));
    }
    return finish(&b);
}
