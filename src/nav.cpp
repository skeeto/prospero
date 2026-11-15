// Back and forward through the user's jumps, as in a browser (#58).
//
// A jump is anything of the user's that takes the focus to another
// session, pane or place: a link, a session picked, a pane brought
// forward. A spot is where the user is: the session shown, the pane
// with the focus, and that pane's place in it. A jump holds the spot it
// leaves, and the next jump or step settles it: kept if the user is now
// somewhere else, let go if the jump went nowhere. One history for the
// window, in memory only. Pure: no ImGui and no Gui, so the GUI fills
// the spots and puts them back.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    NAV_MAX  = 50,    // spots each way, the oldest dropped
    NAV_TEXT = 1024,  // bytes of a Files path or a Settings page's name
};

enum : i32 {
    NAV_BACK,
    NAV_FWD,
};

struct NavSpot {
    void *sess;       // the session shown (a GuiSession), null for none
    i32   pane;       // PANE_*, the pane with the focus

    // Transcript and Prompt: the transcript's place
    b32   stick;      // following its end
    void *view;       // else the View it was in, never reused
    i32   msg;        // and its Anchor there
    i32   off;
    f64   dy;

    // Files: the front tab, by host and path (in text)
    u64   host;
    b32   win;
    i32   mode;
    i32   top;
    f32   fdy;
    f32   scrollx;
    b32   ems;
    i32   band0;
    i32   band1;
    i32   jump;       // a jump not yet made, a line, else 0
    i32   goline;
    i32   gocol;
    u8    section[FILES_SECTION];
    i32   sectionlen;

    // Help: the topic and how far down
    i32   page;
    f32   y;

    // Settings: the page, its kind then its name in text
    b32   diag;
    i32   kindlen;

    // Terminal: the front tab
    u32   serial;

    u8    text[NAV_TEXT];
    i32   textlen;
};

struct Nav {
    NavSpot back[NAV_MAX];  // oldest first
    NavSpot fwd[NAV_MAX];
    i32     nback;
    i32     nfwd;
    NavSpot held;           // left by the last jump, until settled
    b32     hold;
};

// Whether a spot's session is still there to go back to.
typedef b32 NavLive(void *ctx, void *sess);

static Str navtext(NavSpot *s)
{
    return {s->text, s->textlen};
}

// Whether two spots are the same place, as far as going back goes.
static b32 navsame(NavSpot *a, NavSpot *b)
{
    if (a->sess!=b->sess || a->pane!=b->pane) {
        return 0;
    }
    switch (a->pane) {
    case PANE_TRANSCRIPT:
    case PANE_PROMPT:
        if (a->stick != b->stick) {
            return 0;
        }
        return a->stick ||
               (a->view==b->view && a->msg==b->msg && a->off==b->off);
    case PANE_FILES:
        return a->host==b->host &&
               filesame(navtext(a), navtext(b), a->win) &&
               a->top==b->top && a->band0==b->band0 &&
               a->band1==b->band1 && a->jump==b->jump;
    case PANE_HELP:
        return a->page==b->page && (i32)a->y==(i32)b->y;
    case PANE_SETTINGS:
        return a->diag==b->diag && a->kindlen==b->kindlen &&
               navtext(a)==navtext(b);
    case PANE_TERMINAL:
        return a->serial==b->serial;
    }
    return 1;
}

// Onto a stack: the same place as its top replaces the top, and a full
// stack drops its oldest.
static void navpush(NavSpot *stack, i32 *n, NavSpot *s)
{
    if (*n && navsame(stack+*n-1, s)) {
        stack[*n-1] = *s;
        return;
    }
    if (*n == NAV_MAX) {
        __builtin_memmove(stack, stack+1, (NAV_MAX-1)*sizeof(*stack));
        (*n)--;
    }
    stack[(*n)++] = *s;
}

// Settle a held spot against where the user is now: kept, clearing
// Forward, if somewhere else; else the jump went nowhere.
static void navsettle(Nav *n, NavSpot *now)
{
    if (n->hold) {
        n->hold = 0;
        if (!navsame(&n->held, now)) {
            navpush(n->back, &n->nback, &n->held);
            n->nfwd = 0;
        }
    }
}

// A jump is leaving this spot.
static void navhold(Nav *n, NavSpot *from)
{
    navsettle(n, from);
    n->held = *from;
    n->hold = 1;
}

// The next spot a step would reach, dropping those equal to now or
// whose session is gone; null if none. Settle first.
static NavSpot *navpeek(Nav *n, i32 dir, NavSpot *now, NavLive *live, void *ctx)
{
    NavSpot *stack = dir==NAV_BACK ? n->back : n->fwd;
    i32     *count = dir==NAV_BACK ? &n->nback : &n->nfwd;
    while (*count) {
        NavSpot *top = stack + *count - 1;
        if (!navsame(top, now) && (!live || !top->sess || live(ctx, top->sess))) {
            return top;
        }
        (*count)--;
    }
    return 0;
}

// Step to the next spot, which navpeek found: now goes onto the other
// side, and the spot comes off.
static NavSpot navstep(Nav *n, i32 dir, NavSpot *now)
{
    NavSpot *from  = dir==NAV_BACK ? n->back : n->fwd;
    i32     *nfrom = dir==NAV_BACK ? &n->nback : &n->nfwd;
    NavSpot *to    = dir==NAV_BACK ? n->fwd : n->back;
    i32     *nto   = dir==NAV_BACK ? &n->nfwd : &n->nback;
    assert(*nfrom > 0);
    NavSpot r = from[--*nfrom];
    navpush(to, nto, now);
    return r;
}

// What a step would reach, settling counted, changing nothing: for the
// menu's state and tooltip. Null if nowhere.
static NavSpot *navwhere(Nav *n, i32 dir, NavSpot *now, NavLive *live, void *ctx)
{
    b32 pending = n->hold && !navsame(&n->held, now);
    if (dir == NAV_FWD) {
        if (pending) {
            return 0;  // settling clears Forward
        }
    } else if (pending && (!n->held.sess || !live || live(ctx, n->held.sess))) {
        return &n->held;
    }
    NavSpot *stack = dir==NAV_BACK ? n->back : n->fwd;
    i32      count = dir==NAV_BACK ? n->nback : n->nfwd;
    for (i32 i = count-1; i >= 0; i--) {
        NavSpot *s = stack + i;
        if (!navsame(s, now) && (!live || !s->sess || live(ctx, s->sess))) {
            return s;
        }
    }
    return 0;
}
