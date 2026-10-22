// Desktop helper, the Wayland half of the Linux layer
//
// Included by main_desktop_x11.cpp, which picks this half at run time
// for a Wayland session. Wayland gives an ordinary client no way to see
// the screen, send input, or touch other programs' windows, so each of
// those goes through someone who may:
//
// Screen and input: the xdg-desktop-portal RemoteDesktop portal, one
// session with a ScreenCast of the monitors in it, which the desktop
// shows the user a dialog for. Its approval is kept: the session asks
// to persist (persist_mode 2), and the restore token it gets back is
// saved in Prospero's state directory ($PROSPERO_STATE, else
// $XDG_STATE_HOME/prospero, else ~/.local/state/prospero), one per
// desktop, so that later runs start without a dialog. A token is good
// for one start, which hands out the next. Input goes through libei,
// on the socket ConnectToEIS returns, with libxkbcommon to find keys in
// the desktop's keymap, or without either library, through the portal's
// Notify* calls, which take keysyms (PROSPERO_DESKTOP_EI=0 picks those
// even with libei, for testing). Screenshots read a frame of each
// monitor's PipeWire stream (libpipewire), in the stream's pixels.
// Every library is loaded with dlopen, so the helper runs without them,
// if only to say what is missing.
//
// Windows: there is no Wayland protocol for other programs' windows,
// so each desktop has its own way, or none:
// - GNOME (Mutter) keeps org.gnome.Shell.Introspect to a few programs
//   of its own unless in unsafe mode, and has nothing to activate or
//   close a window. The optional Window Calls extension (uuid
//   window-calls@domandoman.xyz, extensions.gnome.org 4724) exports
//   org.gnome.Shell.Extensions.Windows, which does all of it; when it
//   is installed and enabled, windows work as elsewhere.
// - KDE Plasma (KWin) runs scripts given over D-Bus: each request is a
//   small script, which calls back to this helper with its answer.
// - Without either, the windows of X11 programs under Xwayland still
//   work through the X11 half, since the compositor manages them as an
//   X window manager. Other windows cannot be named; "screen" and
//   "active" (whatever window has the keyboard) still work.
// Input aimed at a window is sent only once the compositor says the
// window has the keyboard, as on the other desktops.
//
// Coordinates are the compositor's logical pixels, which the portal's
// streams, the pointer and the window APIs share. A display scaled up
// has more physical pixels than that, and a screenshot is captured at
// those and scaled down, as on a Retina display.
//
// D-Bus is spoken directly (dbus.cpp): no libdbus, and passed file
// descriptors come with SCM_RIGHTS.
//
// This is free and unencumbered software released into the public domain.
#include "json.cpp"
#include "dbus.cpp"

enum : i32 {
    WL_ASKMS   = 25000,   // for the portal's dialog, within the harness's wait
    WL_ASKLONG = 300000,  // for access -ask, which waits for the user
    WL_CALLMS  = 5000,    // for an ordinary D-Bus call
};

enum : u64 {
    // The window that has the keyboard, when the desktop cannot say
    // which it is: "active" still names it
    WL_FOCUSED = 0x7fff00000000fffeull,
};

static i64 wlms()
{
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (i64)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}

static Str getenvstr(char const *name)
{
    return fromcstr(getenv(name));
}


// D-Bus transport: a Unix socket, EXTERNAL authentication

struct DbPend {
    DbPend *next;
    Str     raw;  // a whole message, in the connection's arena
    i32     fd;
};

struct DbConn {
    i32     fd;       // 0 until connected (it is never standard input)
    u32     serial;
    Str     unique;   // our name on the bus
    Arena  *perm;
    u8     *rbuf;
    iz      rlen;
    iz      rcap;
    i32     fdq[32];  // passed descriptors not yet claimed
    i32     nfdq;
    DbPend *pend;     // messages put aside while waiting for others
};

struct DbIn {
    DbMsg m;
    Str   raw;
    i32   fd;   // the first descriptor passed with it, -1 if none
    b32   ok;
};

static Str unescape(Arena *a, Str s)
{
    Buf b(a, s.len);
    for (iz i = 0; i < s.len; i++) {
        if (s[i]=='%' && i+2<s.len) {
            u8 h = lowercase(s[i+1]), l = lowercase(s[i+2]);
            i32 hv = digit(h) ? h-'0' : h>='a'&&h<='f' ? h-'a'+10 : -1;
            i32 lv = digit(l) ? l-'0' : l>='a'&&l<='f' ? l-'a'+10 : -1;
            if (hv>=0 && lv>=0) {
                putbyte(&b, (u8)(hv<<4 | lv));
                i += 2;
                continue;
            }
        }
        putbyte(&b, s[i]);
    }
    return finish(&b);
}

static b32 sendall(i32 fd, u8 *p, iz len, i32 *fds, i32 nfds)
{
    for (iz off = 0; off < len;) {
        struct iovec iov = {p+off, (uz)(len-off)};
        struct msghdr mh = {};
        mh.msg_iov    = &iov;
        mh.msg_iovlen = 1;
        alignas(struct cmsghdr) char cbuf[CMSG_SPACE(sizeof(int)*4)] = {};
        if (fds && nfds>0 && nfds<=4 && !off) {
            mh.msg_control    = cbuf;
            mh.msg_controllen = CMSG_SPACE(sizeof(int)*(uz)nfds);
            struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
            cm->cmsg_level = SOL_SOCKET;
            cm->cmsg_type  = SCM_RIGHTS;
            cm->cmsg_len   = CMSG_LEN(sizeof(int)*(uz)nfds);
            copybytes(CMSG_DATA(cm), fds, nfds*(iz)sizeof(int));
        }
        iz n = sendmsg(fd, &mh, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        off += n;
    }
    return 1;
}

// More bytes into the read buffer, waiting until deadline: 1 if some
// came, 0 at the deadline, -1 if the connection is gone.
static i32 dbfill(DbConn *c, i64 deadline)
{
    if (c->rcap - c->rlen < 1<<16) {
        iz cap  = c->rcap*2;
        u8 *buf = allocbytes(c->perm, cap);
        copybytes(buf, c->rbuf, c->rlen);
        c->rbuf = buf;
        c->rcap = cap;
    }
    for (;;) {
        i64 left = deadline - wlms();
        if (left < 0) {
            return 0;
        }
        struct pollfd p = {c->fd, POLLIN, 0};
        i32 r = poll(&p, 1, (int)(left<1000 ? left : 1000));
        if (r<0 && errno!=EINTR) {
            return -1;
        } else if (r <= 0) {
            if (quitting) return -1;
            continue;
        }
        struct iovec iov = {c->rbuf+c->rlen, (uz)(c->rcap-c->rlen)};
        struct msghdr mh = {};
        alignas(struct cmsghdr) char cbuf[CMSG_SPACE(sizeof(int)*16)];
        mh.msg_iov        = &iov;
        mh.msg_iovlen     = 1;
        mh.msg_control    = cbuf;
        mh.msg_controllen = sizeof(cbuf);
        iz n = recvmsg(c->fd, &mh, MSG_CMSG_CLOEXEC);
        if (n < 0) {
            if (errno==EINTR || errno==EAGAIN) continue;
            return -1;
        } else if (!n) {
            return -1;
        }
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
            if (cm->cmsg_level==SOL_SOCKET && cm->cmsg_type==SCM_RIGHTS) {
                iz k = (iz)(cm->cmsg_len - CMSG_LEN(0)) / (iz)sizeof(int);
                for (iz i = 0; i < k; i++) {
                    int fd = -1;
                    copybytes(&fd, CMSG_DATA(cm) + i*(iz)sizeof(int), sizeof(int));
                    if (c->nfdq < countof(c->fdq)) {
                        c->fdq[c->nfdq++] = fd;
                    } else {
                        close(fd);
                    }
                }
            }
        }
        c->rlen += n;
        return 1;
    }
}

// The next whole message already read, copied into a, or !ok.
static DbIn dbtake(DbConn *c, Arena *a)
{
    DbIn r = {};
    r.fd   = -1;
    iz n   = dbmsgsize(Str{c->rbuf, c->rlen});
    if (n<=0 || n>c->rlen) {
        return r;
    }
    r.raw.data = allocbytes(a, n);
    r.raw.len  = n;
    copybytes(r.raw.data, c->rbuf, n);
    c->rlen -= n;
    __builtin_memmove(c->rbuf, c->rbuf+n, (uz)c->rlen);
    r.m  = dbreadmsg(a, r.raw);
    r.ok = r.m.ok;
    for (i32 i = 0; i < r.m.h.nfds && c->nfdq; i++) {
        i32 fd = c->fdq[0];
        c->nfdq--;
        __builtin_memmove(c->fdq, c->fdq+1, (uz)c->nfdq*sizeof(i32));
        if (r.fd < 0) {
            r.fd = fd;
        } else {
            close(fd);
        }
    }
    return r;
}

static DbIn dbnext(DbConn *c, Arena *a, i64 deadline)
{
    for (;;) {
        DbIn r = dbtake(c, a);
        if (r.ok || dbmsgsize(Str{c->rbuf, c->rlen})<0) {
            return r;
        }
        if (dbfill(c, deadline) <= 0) {
            return r;
        }
    }
}

static b32 dbsend(DbConn *c, Arena scratch, DbHead *h, Str body, i32 *fds = 0)
{
    if (!c->fd) {
        return 0;
    }
    h->serial = ++c->serial;
    Str m = dbmessage(&scratch, h, body);
    return sendall(c->fd, m.data, m.len, fds, h->nfds);
}

static void dbreplyto(DbConn *c, Arena scratch, DbMsg *m, Str error)
{
    if (m->h.flags & DB_NOREPLY) {
        return;
    }
    DbHead h  = {};
    h.type    = error.len ? DB_ERROR : DB_RETURN;
    h.replyto = m->h.serial;
    h.dest    = m->sender;
    h.error   = error.len ? error : Str{};
    h.flags   = DB_NOREPLY;
    dbsend(c, scratch, &h, {});
}

// A message that came while waiting for another: a portal's response
// or a call to this helper is kept for later, others are answered or
// dropped.
static void dbstash(DbConn *c, Arena scratch, DbIn *in)
{
    DbMsg *m = &in->m;
    if ((m->h.type==DB_SIGNAL && m->h.member=="Response") ||
            (m->h.type==DB_CALL && m->h.path=="/prospero")) {
        DbPend *p = alloc<DbPend>(c->perm);
        p->raw = clone(c->perm, in->raw);
        p->fd  = in->fd;
        DbPend **tail = &c->pend;
        while (*tail) tail = &(*tail)->next;
        *tail = p;
        return;
    }
    if (in->fd >= 0) {
        close(in->fd);
    }
    if (m->h.type == DB_CALL) {
        dbreplyto(c, scratch, m, "org.freedesktop.DBus.Error.UnknownMethod");
    }
}

static b32 dbmatch(DbMsg *m, u8 type, Str path, Str member)
{
    return m->h.type==type && m->h.path==path && m->h.member==member;
}

// Wait for a signal or a call to this helper, with path and member.
static DbIn dbwait(DbConn *c, Arena *a, u8 type, Str path, Str member, i64 deadline)
{
    for (DbPend **p = &c->pend; *p; p = &(*p)->next) {
        DbIn r = {};
        r.raw = clone(a, (*p)->raw);
        r.m   = dbreadmsg(a, r.raw);
        r.fd  = (*p)->fd;
        if (r.m.ok && dbmatch(&r.m, type, path, member)) {
            r.ok = 1;
            *p = (*p)->next;
            return r;
        }
    }
    for (;;) {
        DbIn r = dbnext(c, a, deadline);
        if (!r.ok || dbmatch(&r.m, type, path, member)) {
            return r;
        }
        dbstash(c, *a, &r);
    }
}

// Call a method and wait for its reply (a return or an error).
static DbIn dbcall(DbConn *c, Arena *a, Str dest, Str path, Str iface,
                   Str member, Str sig, Str body, i32 ms, i32 *fds = 0, i32 nfds = 0)
{
    DbIn   r = {};
    DbHead h = {};
    h.type   = DB_CALL;
    h.dest   = dest;
    h.path   = path;
    h.iface  = iface;
    h.member = member;
    h.sig    = sig;
    h.nfds   = nfds;
    r.fd     = -1;
    if (!dbsend(c, *a, &h, body, fds)) {
        return r;
    }
    i64 deadline = wlms() + ms;
    for (;;) {
        r = dbnext(c, a, deadline);
        if (!r.ok) {
            return r;
        } else if ((r.m.h.type==DB_RETURN || r.m.h.type==DB_ERROR) &&
                   r.m.h.replyto==h.serial) {
            return r;
        }
        dbstash(c, *a, &r);
    }
}

// A reply's arguments, or null for an error or no reply.
static DbVal *dbresult(Arena *a, DbIn *in)
{
    if (!in->ok || in->m.h.type!=DB_RETURN) {
        return 0;
    }
    return dbparse(a, in->m.body, in->m.h.sig);
}

// Why a call failed: its error's name and message, or that none came.
static Str dberror(Arena *a, DbIn *in)
{
    if (!in->ok) {
        return "no reply";
    }
    DbVal *v   = dbparse(a, in->m.body, in->m.h.sig);
    Str    msg = v ? dbat(v, 0)->s : Str{};
    Buf    b(a, 128);
    print(&b, in->m.h.error);
    if (msg.len) {
        print(&b, ": ");
        print(&b, msg);
    }
    return finish(&b);
}

static b32 readline(i32 fd, Buf *b, i64 deadline)
{
    while (!(b->len>=2 && b->data[b->len-2]=='\r' && b->data[b->len-1]=='\n')) {
        struct pollfd p = {fd, POLLIN, 0};
        i64 left = deadline - wlms();
        if (left<=0 || poll(&p, 1, (int)left)<=0) {
            return 0;
        }
        u8 ch = 0;
        if (read(fd, &ch, 1) != 1 || b->len > 1024) {
            return 0;
        }
        putbyte(b, ch);
    }
    return 1;
}

// Connect to the session bus, or why not.
static Str dbopen(DbConn *c, Arena *perm, Arena scratch)
{
    Str addr = getenvstr("DBUS_SESSION_BUS_ADDRESS");
    if (!addr.len) {
        Str run = getenvstr("XDG_RUNTIME_DIR");
        if (!run.len) {
            return "no session bus: neither DBUS_SESSION_BUS_ADDRESS nor "
                   "XDG_RUNTIME_DIR is set";
        }
        addr = concat(&scratch, "unix:path=", join(&scratch, run, "bus"));
    }
    i32 fd = -1;
    for (Str rest = addr; fd<0 && rest.len;) {
        Cut one = cut(rest, ';');
        rest = one.tail;
        if (!startswith(one.head, "unix:")) {
            continue;
        }
        Str path     = {};
        b32 abstract = 0;
        for (Str kv = cuthead(one.head, 5); kv.len;) {
            Cut p = cut(kv, ',');
            kv = p.tail;
            if (startswith(p.head, "path=")) {
                path = unescape(&scratch, cuthead(p.head, 5));
            } else if (startswith(p.head, "abstract=")) {
                path     = unescape(&scratch, cuthead(p.head, 9));
                abstract = 1;
            }
        }
        struct sockaddr_un sa = {};
        if (!path.len || path.len+2 > (iz)sizeof(sa.sun_path)) {
            continue;
        }
        sa.sun_family = AF_UNIX;
        copybytes(sa.sun_path + abstract, path.data, path.len);
        socklen_t len = (socklen_t)(__builtin_offsetof(struct sockaddr_un, sun_path)
                                    + abstract + path.len + !abstract);
        fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
        if (fd>=0 && connect(fd, (struct sockaddr *)&sa, len)) {
            close(fd);
            fd = -1;
        }
    }
    if (fd < 0) {
        Buf b(perm, 64);
        print(&b, "cannot connect to the session bus at '");
        print(&b, addr);
        print(&b, "'");
        return finish(&b);
    }

    // AUTH EXTERNAL with the uid in hex digits of its decimal form
    Buf   out(&scratch, 64);
    u8    digits[16];
    iz    nd  = 0;
    for (u32 v = (u32)getuid(); nd==0 || v; v /= 10) {
        digits[nd++] = (u8)('0' + v%10);
    }
    putbyte(&out, 0);
    print(&out, "AUTH EXTERNAL ");
    while (nd) {
        u8 d = digits[--nd];
        putbyte(&out, "0123456789abcdef"[d>>4]);
        putbyte(&out, "0123456789abcdef"[d&15]);
    }
    print(&out, "\r\nNEGOTIATE_UNIX_FD\r\n");
    i64 deadline = wlms() + WL_CALLMS;
    Buf l1(&scratch, 128);
    Buf l2(&scratch, 128);
    if (!sendall(fd, out.data, out.len, 0, 0) ||
            !readline(fd, &l1, deadline) || !startswith(finish(&l1), "OK ") ||
            !readline(fd, &l2, deadline) || !startswith(finish(&l2), "AGREE_UNIX_FD") ||
            !sendall(fd, (u8 *)"BEGIN\r\n", 7, 0, 0)) {
        close(fd);
        return "the session bus refused this helper (authentication failed)";
    }
    c->fd   = fd;
    c->perm = perm;
    c->rcap = 1 << 20;
    c->rbuf = allocbytes(perm, c->rcap);
    DbIn r = dbcall(c, &scratch, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                    "org.freedesktop.DBus", "Hello", {}, {}, WL_CALLMS);
    DbVal *v = dbresult(&scratch, &r);
    if (!v) {
        close(fd);
        c->fd = 0;
        return "the session bus did not answer Hello";
    }
    c->unique = clone(perm, dbat(v, 0)->s);
    return {};
}

static void dbmatchrule(DbConn *c, Arena scratch, Str rule, b32 add)
{
    Buf b(&scratch, 256);
    dbstr(&b, rule);
    dbcall(c, &scratch, "org.freedesktop.DBus", "/org/freedesktop/DBus",
           "org.freedesktop.DBus", add ? Str("AddMatch") : Str("RemoveMatch"),
           "s", finish(&b), WL_CALLMS);
}

static b32 hasowner(DbConn *c, Arena scratch, Str name)
{
    Buf b(&scratch, 64);
    dbstr(&b, name);
    DbIn   r = dbcall(c, &scratch, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                      "org.freedesktop.DBus", "NameHasOwner", "s", finish(&b), WL_CALLMS);
    DbVal *v = dbresult(&scratch, &r);
    return v && dbat(v, 0)->u;
}


// Libraries, loaded at run time

template<typename T>
static b32 wlload(T *f, void *lib, char const *name)
{
    *f = lib ? (T)dlsym(lib, name) : 0;
    return *f != 0;
}

static void *wlopen(char const *name, char const *alt)
{
    void *lib = dlopen(name, RTLD_NOW|RTLD_LOCAL);
    return lib ? lib : dlopen(alt, RTLD_NOW|RTLD_LOCAL);
}

struct Ei;
struct EiEvent;
struct EiSeat;
struct EiDevice;
struct EiKeymap;
struct EiRegion;
struct XkbContext;
struct XkbKeymap;
struct XkbState;

enum : i32 {
    EI_CONNECT = 1, EI_DISCONNECT, EI_SEAT_ADDED, EI_SEAT_REMOVED,
    EI_DEVICE_ADDED, EI_DEVICE_REMOVED, EI_DEVICE_PAUSED, EI_DEVICE_RESUMED,
    EI_KEYBOARD_MODIFIERS,

    EI_CAP_POINTER = 1, EI_CAP_ABSOLUTE = 2, EI_CAP_KEYBOARD = 4,
    EI_CAP_TOUCH = 8, EI_CAP_SCROLL = 16, EI_CAP_BUTTON = 32,
};

struct EiLib {
    Ei       *(*ei_new_sender)(void *);
    Ei       *(*ei_unref)(Ei *);
    void      (*ei_configure_name)(Ei *, char const *);
    i32       (*ei_setup_backend_fd)(Ei *, i32);
    i32       (*ei_get_fd)(Ei *);
    void      (*ei_dispatch)(Ei *);
    EiEvent  *(*ei_get_event)(Ei *);
    u64       (*ei_now)(Ei *);
    i32       (*ei_event_get_type)(EiEvent *);
    EiSeat   *(*ei_event_get_seat)(EiEvent *);
    EiDevice *(*ei_event_get_device)(EiEvent *);
    EiEvent  *(*ei_event_unref)(EiEvent *);
    u32       (*ei_event_keyboard_get_xkb_group)(EiEvent *);
    void      (*ei_seat_bind_capabilities)(EiSeat *, ...);
    EiDevice *(*ei_device_ref)(EiDevice *);
    bool      (*ei_device_has_capability)(EiDevice *, i32);
    void      (*ei_device_start_emulating)(EiDevice *, u32);
    void      (*ei_device_frame)(EiDevice *, u64);
    void      (*ei_device_pointer_motion_absolute)(EiDevice *, f64, f64);
    void      (*ei_device_button_button)(EiDevice *, u32, bool);
    void      (*ei_device_scroll_discrete)(EiDevice *, i32, i32);
    void      (*ei_device_keyboard_key)(EiDevice *, u32, bool);
    EiKeymap *(*ei_device_keyboard_get_keymap)(EiDevice *);
    i32       (*ei_keymap_get_fd)(EiKeymap *);
    uz        (*ei_keymap_get_size)(EiKeymap *);
    EiRegion *(*ei_device_get_region)(EiDevice *, uz);
    u32       (*ei_region_get_x)(EiRegion *);
    u32       (*ei_region_get_y)(EiRegion *);
    u32       (*ei_region_get_width)(EiRegion *);
    u32       (*ei_region_get_height)(EiRegion *);

    XkbContext *(*xkb_context_new)(i32);
    XkbKeymap  *(*xkb_keymap_new_from_string)(XkbContext *, char const *, i32, i32);
    void        (*xkb_keymap_unref)(XkbKeymap *);
    u32         (*xkb_keymap_min_keycode)(XkbKeymap *);
    u32         (*xkb_keymap_max_keycode)(XkbKeymap *);
    u32         (*xkb_keymap_num_layouts_for_key)(XkbKeymap *, u32);
    u32         (*xkb_keymap_num_levels_for_key)(XkbKeymap *, u32, u32);
    i32         (*xkb_keymap_key_get_syms_by_level)(XkbKeymap *, u32, u32, u32, u32 const **);
    uz          (*xkb_keymap_key_get_mods_for_level)(XkbKeymap *, u32, u32, u32, u32 *, uz);
    XkbState   *(*xkb_state_new)(XkbKeymap *);
    void        (*xkb_state_unref)(XkbState *);
    i32         (*xkb_state_update_key)(XkbState *, u32, i32);
    u32         (*xkb_state_serialize_mods)(XkbState *, i32);
    u32         (*xkb_utf32_to_keysym)(u32);
};

static b32 loadei(EiLib *l)
{
    void *ei  = wlopen("libei.so.1", "libei.so");
    void *xkb = wlopen("libxkbcommon.so.0", "libxkbcommon.so");
    b32   ok  = 1;
    ok &= wlload(&l->ei_new_sender,                    ei, "ei_new_sender");
    ok &= wlload(&l->ei_unref,                         ei, "ei_unref");
    ok &= wlload(&l->ei_configure_name,                ei, "ei_configure_name");
    ok &= wlload(&l->ei_setup_backend_fd,              ei, "ei_setup_backend_fd");
    ok &= wlload(&l->ei_get_fd,                        ei, "ei_get_fd");
    ok &= wlload(&l->ei_dispatch,                      ei, "ei_dispatch");
    ok &= wlload(&l->ei_get_event,                     ei, "ei_get_event");
    ok &= wlload(&l->ei_now,                           ei, "ei_now");
    ok &= wlload(&l->ei_event_get_type,                ei, "ei_event_get_type");
    ok &= wlload(&l->ei_event_get_seat,                ei, "ei_event_get_seat");
    ok &= wlload(&l->ei_event_get_device,              ei, "ei_event_get_device");
    ok &= wlload(&l->ei_event_unref,                   ei, "ei_event_unref");
    ok &= wlload(&l->ei_event_keyboard_get_xkb_group,  ei, "ei_event_keyboard_get_xkb_group");
    ok &= wlload(&l->ei_seat_bind_capabilities,        ei, "ei_seat_bind_capabilities");
    ok &= wlload(&l->ei_device_ref,                    ei, "ei_device_ref");
    ok &= wlload(&l->ei_device_has_capability,         ei, "ei_device_has_capability");
    ok &= wlload(&l->ei_device_start_emulating,        ei, "ei_device_start_emulating");
    ok &= wlload(&l->ei_device_frame,                  ei, "ei_device_frame");
    ok &= wlload(&l->ei_device_pointer_motion_absolute, ei, "ei_device_pointer_motion_absolute");
    ok &= wlload(&l->ei_device_button_button,          ei, "ei_device_button_button");
    ok &= wlload(&l->ei_device_scroll_discrete,        ei, "ei_device_scroll_discrete");
    ok &= wlload(&l->ei_device_keyboard_key,           ei, "ei_device_keyboard_key");
    ok &= wlload(&l->ei_device_keyboard_get_keymap,    ei, "ei_device_keyboard_get_keymap");
    ok &= wlload(&l->ei_keymap_get_fd,                 ei, "ei_keymap_get_fd");
    ok &= wlload(&l->ei_keymap_get_size,               ei, "ei_keymap_get_size");
    ok &= wlload(&l->ei_device_get_region,             ei, "ei_device_get_region");
    ok &= wlload(&l->ei_region_get_x,                  ei, "ei_region_get_x");
    ok &= wlload(&l->ei_region_get_y,                  ei, "ei_region_get_y");
    ok &= wlload(&l->ei_region_get_width,              ei, "ei_region_get_width");
    ok &= wlload(&l->ei_region_get_height,             ei, "ei_region_get_height");
    ok &= wlload(&l->xkb_context_new,                  xkb, "xkb_context_new");
    ok &= wlload(&l->xkb_keymap_new_from_string,       xkb, "xkb_keymap_new_from_string");
    ok &= wlload(&l->xkb_keymap_unref,                 xkb, "xkb_keymap_unref");
    ok &= wlload(&l->xkb_keymap_min_keycode,           xkb, "xkb_keymap_min_keycode");
    ok &= wlload(&l->xkb_keymap_max_keycode,           xkb, "xkb_keymap_max_keycode");
    ok &= wlload(&l->xkb_keymap_num_layouts_for_key,   xkb, "xkb_keymap_num_layouts_for_key");
    ok &= wlload(&l->xkb_keymap_num_levels_for_key,    xkb, "xkb_keymap_num_levels_for_key");
    ok &= wlload(&l->xkb_keymap_key_get_syms_by_level, xkb, "xkb_keymap_key_get_syms_by_level");
    ok &= wlload(&l->xkb_keymap_key_get_mods_for_level, xkb, "xkb_keymap_key_get_mods_for_level");
    ok &= wlload(&l->xkb_state_new,                    xkb, "xkb_state_new");
    ok &= wlload(&l->xkb_state_unref,                  xkb, "xkb_state_unref");
    ok &= wlload(&l->xkb_state_update_key,             xkb, "xkb_state_update_key");
    ok &= wlload(&l->xkb_state_serialize_mods,         xkb, "xkb_state_serialize_mods");
    ok &= wlload(&l->xkb_utf32_to_keysym,              xkb, "xkb_utf32_to_keysym");
    return ok;
}

// PipeWire's types, as much of them as is used: stable ABI since 0.3.
struct PwThreadLoop;
struct PwLoop;
struct PwContext;
struct PwCore;
struct PwStream;
struct PwProperties;

struct SpaHook {
    void *link[2];
    void *funcs;
    void *data;
    void (*removed)(SpaHook *);
    void *priv;
};

struct SpaChunk {
    u32 offset;
    u32 size;
    i32 stride;
    i32 flags;
};

struct SpaData {
    u32       type;
    u32       flags;
    i64       fd;
    u32       mapoffset;
    u32       maxsize;
    void     *data;
    SpaChunk *chunk;
};

struct SpaBuffer {
    u32      nmetas;
    u32      ndatas;
    void    *metas;
    SpaData *datas;
};

struct PwBuffer {
    SpaBuffer *buffer;
    void      *userdata;
    u64        size;
    u64        requested;
};

struct PwStreamEvents {
    u32   version;
    void (*destroy)(void *);
    void (*statechanged)(void *, i32, i32, char const *);
    void *controlinfo;
    void *iochanged;
    void (*paramchanged)(void *, u32, void const *);
    void *addbuffer;
    void *removebuffer;
    void (*process)(void *);
    void *drained;
    void *command;
    void *triggerdone;
};

struct PwLib {
    void          (*pw_init)(i32 *, char ***);
    PwThreadLoop *(*pw_thread_loop_new)(char const *, void const *);
    PwLoop       *(*pw_thread_loop_get_loop)(PwThreadLoop *);
    i32           (*pw_thread_loop_start)(PwThreadLoop *);
    void          (*pw_thread_loop_stop)(PwThreadLoop *);
    void          (*pw_thread_loop_destroy)(PwThreadLoop *);
    void          (*pw_thread_loop_lock)(PwThreadLoop *);
    void          (*pw_thread_loop_unlock)(PwThreadLoop *);
    i32           (*pw_thread_loop_timed_wait)(PwThreadLoop *, i32);
    void          (*pw_thread_loop_signal)(PwThreadLoop *, bool);
    PwContext    *(*pw_context_new)(PwLoop *, PwProperties *, uz);
    void          (*pw_context_destroy)(PwContext *);
    PwCore       *(*pw_context_connect_fd)(PwContext *, i32, PwProperties *, uz);
    i32           (*pw_core_disconnect)(PwCore *);
    PwProperties *(*pw_properties_new)(char const *, ...);
    PwStream     *(*pw_stream_new)(PwCore *, char const *, PwProperties *);
    void          (*pw_stream_add_listener)(PwStream *, SpaHook *, PwStreamEvents const *, void *);
    i32           (*pw_stream_connect)(PwStream *, i32, u32, i32, void const **, u32);
    i32           (*pw_stream_disconnect)(PwStream *);
    void          (*pw_stream_destroy)(PwStream *);
    PwBuffer     *(*pw_stream_dequeue_buffer)(PwStream *);
    i32           (*pw_stream_queue_buffer)(PwStream *, PwBuffer *);
    i32           (*pw_stream_update_params)(PwStream *, void const **, u32);
};

static b32 loadpw(PwLib *l)
{
    void *pw = wlopen("libpipewire-0.3.so.0", "libpipewire-0.3.so");
    b32   ok = 1;
    ok &= wlload(&l->pw_init,                   pw, "pw_init");
    ok &= wlload(&l->pw_thread_loop_new,        pw, "pw_thread_loop_new");
    ok &= wlload(&l->pw_thread_loop_get_loop,   pw, "pw_thread_loop_get_loop");
    ok &= wlload(&l->pw_thread_loop_start,      pw, "pw_thread_loop_start");
    ok &= wlload(&l->pw_thread_loop_stop,       pw, "pw_thread_loop_stop");
    ok &= wlload(&l->pw_thread_loop_destroy,    pw, "pw_thread_loop_destroy");
    ok &= wlload(&l->pw_thread_loop_lock,       pw, "pw_thread_loop_lock");
    ok &= wlload(&l->pw_thread_loop_unlock,     pw, "pw_thread_loop_unlock");
    ok &= wlload(&l->pw_thread_loop_timed_wait, pw, "pw_thread_loop_timed_wait");
    ok &= wlload(&l->pw_thread_loop_signal,     pw, "pw_thread_loop_signal");
    ok &= wlload(&l->pw_context_new,            pw, "pw_context_new");
    ok &= wlload(&l->pw_context_destroy,        pw, "pw_context_destroy");
    ok &= wlload(&l->pw_context_connect_fd,     pw, "pw_context_connect_fd");
    ok &= wlload(&l->pw_core_disconnect,        pw, "pw_core_disconnect");
    ok &= wlload(&l->pw_properties_new,         pw, "pw_properties_new");
    ok &= wlload(&l->pw_stream_new,             pw, "pw_stream_new");
    ok &= wlload(&l->pw_stream_add_listener,    pw, "pw_stream_add_listener");
    ok &= wlload(&l->pw_stream_connect,         pw, "pw_stream_connect");
    ok &= wlload(&l->pw_stream_disconnect,      pw, "pw_stream_disconnect");
    ok &= wlload(&l->pw_stream_destroy,         pw, "pw_stream_destroy");
    ok &= wlload(&l->pw_stream_dequeue_buffer,  pw, "pw_stream_dequeue_buffer");
    ok &= wlload(&l->pw_stream_queue_buffer,    pw, "pw_stream_queue_buffer");
    ok &= wlload(&l->pw_stream_update_params,   pw, "pw_stream_update_params");
    if (ok) {
        l->pw_init(0, 0);
    }
    return ok;
}


// The helper's Wayland state

enum : i32 {
    WIN_NONE,      // nothing names windows (but maybe Xwayland's)
    WIN_GNOME,     // the Window Calls extension
    WIN_KWIN,      // KWin scripts
};

enum : i32 {
    IN_NONE,
    IN_EIS,
    IN_NOTIFY,
};

struct WlStream {
    u32 node;
    i32 x, y, w, h;  // logical
    i32 pw, ph;      // its frames' pixels, once one is seen
};

struct WlKwinWin {
    u64 id;
    Str uuid;
};

struct Wl {
    Arena    perm;
    DbConn   bus;
    Str      busfail;     // why there is no bus, for good
    Str      failed;      // why the latest call failed, for dk_why
    Str      openfail;    // why the portal failed in this command
    Str      desktop;     // XDG_CURRENT_DESKTOP's first name
    i32      winapi;      // WIN_*
    Str      winwhy;      // why windows cannot be named, if not
    i32      askms;       // for the portal's dialog

    // The portal's session, once started
    Str      session;
    WlStream streams[16];
    i32      nstreams;
    i32      input;       // IN_*
    i32      ntoken;

    // libei
    EiLib     el;
    b32       eiloaded;   // 1 loaded, -1 missing
    Ei       *ei;
    EiDevice *kbd;
    EiDevice *abs;
    b32       kbdon, abson;
    u32       seq;
    XkbContext *xctx;
    XkbKeymap  *keymap;
    u32       group;
    b32       eidead;

    // PipeWire
    PwLib    pl;
    i32      pwloaded;

    // KWin: windows seen, by the ids given out for them
    Slice<WlKwinWin> kwins;
    i32      nscript;
};

// The directory of Prospero's state, as the harness picks it.
static Str statedir(Arena *a)
{
    Str s = getenvstr("PROSPERO_STATE");
    if (s.len) {
        return clone(a, s);
    }
    s = getenvstr("XDG_STATE_HOME");
    if (s.len) {
        return join(a, s, "prospero");
    }
    s = getenvstr("HOME");
    return s.len ? join(a, join(a, s, ".local/state"), "prospero") : Str{};
}

// Where the portal's restore token for this desktop is kept.
static Str tokenpath(Wl *wl, Arena *a)
{
    Str dir = statedir(a);
    if (!dir.len) {
        return {};
    }
    Buf b(a, 64);
    print(&b, "desktop-portal-");
    for (iz i = 0; i < wl->desktop.len; i++) {
        u8 c = wl->desktop[i];
        putbyte(&b, alnum(c) ? lowercase(c) : '-');
    }
    if (!wl->desktop.len) {
        print(&b, "wayland");
    }
    print(&b, ".token");
    return join(a, dir, finish(&b));
}

static Str readtoken(Wl *wl, Arena *a)
{
    Str path = tokenpath(wl, a);
    if (!path.len) {
        return {};
    }
    i32 fd = open(cstr(a, path), O_RDONLY|O_CLOEXEC);
    if (fd < 0) {
        return {};
    }
    u8 *buf = allocbytes(a, 256);
    iz  n   = read(fd, buf, 255);
    close(fd);
    Str s = trim(Str{buf, n>0 ? n : 0});
    for (iz i = 0; i < s.len; i++) {
        if (s[i]<=' ' || s[i]>=0x7f) {
            return {};  // not a token this helper wrote
        }
    }
    return s;
}

static void mkdirs(Arena scratch, Str dir)
{
    for (iz i = 1; i <= dir.len; i++) {
        if (i==dir.len || dir[i]=='/') {
            mkdir(cstr(&scratch, takehead(dir, i)), 0700);
        }
    }
}

// Save a token, readable by the user only, replacing the old at once.
static void savetoken(Wl *wl, Arena scratch, Str token)
{
    Str path = tokenpath(wl, &scratch);
    if (!path.len) {
        return;
    }
    mkdirs(scratch, statedir(&scratch));
    Str tmp = concat(&scratch, path, ".new");
    if (!token.len) {
        unlink(cstr(&scratch, path));
        return;
    }
    i32 fd = open(cstr(&scratch, tmp), O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC, 0600);
    if (fd < 0) {
        return;
    }
    b32 ok = writeall(fd, token) && writeall(fd, "\n");
    ok &= !fsync(fd);
    close(fd);
    if (ok) {
        rename(cstr(&scratch, tmp), cstr(&scratch, path));
    } else {
        unlink(cstr(&scratch, tmp));
    }
}

static Str wlfail(Wl *wl, Str why)
{
    wl->failed = clone(&wl->perm, why);
    return wl->failed;
}


// The portal

static Str PORTAL     = "org.freedesktop.portal.Desktop";
static Str PORTALPATH = "/org/freedesktop/portal/desktop";
static Str RD         = "org.freedesktop.portal.RemoteDesktop";
static Str SC         = "org.freedesktop.portal.ScreenCast";

static Str newtoken(Wl *wl, Arena *a)
{
    Buf b(a, 32);
    print(&b, "prospero");
    print(&b, (i64)getpid());
    putbyte(&b, '_');
    print(&b, (i64)++wl->ntoken);
    return finish(&b);
}

struct PortalReply {
    DbVal *results;  // the a{sv}
    u32    response; // 0 ok, 1 cancelled by the user, 2 failed, 3 timed out
    Str    error;    // the call itself failed
};

// A portal method that answers through a Request's Response signal. The
// body's options must carry handle_token token.
static PortalReply portalcall(Wl *wl, Arena *a, Str iface, Str method, Str sig,
                              Str body, Str token, i32 ms)
{
    PortalReply r = {};
    r.results  = dbnone();
    r.response = 2;
    Str path = dbrequestpath(a, wl->bus.unique, token);
    Str rule = concat(a, concat(a, "type='signal',interface='org.freedesktop.portal."
                      "Request',member='Response',path='", path), "'");
    dbmatchrule(&wl->bus, *a, rule, 1);
    DbIn   in = dbcall(&wl->bus, a, PORTAL, PORTALPATH, iface, method, sig, body, WL_CALLMS);
    DbVal *v  = dbresult(a, &in);
    if (!v) {
        dbmatchrule(&wl->bus, *a, rule, 0);
        r.error = dberror(a, &in);
        return r;
    }
    Str handle = dbat(v, 0)->s;
    if (!(handle == path)) {
        Str other = concat(a, concat(a, "type='signal',interface='org.freedesktop."
                           "portal.Request',member='Response',path='", handle), "'");
        dbmatchrule(&wl->bus, *a, other, 1);
        path = handle;
    }
    DbIn s = dbwait(&wl->bus, a, DB_SIGNAL, path, "Response", wlms() + ms);
    dbmatchrule(&wl->bus, *a, rule, 0);
    if (!s.ok) {
        // Take the dialog away, rather than leave it to be answered
        DbHead h = {};
        h.type   = DB_CALL;
        h.flags  = DB_NOREPLY;
        h.dest   = PORTAL;
        h.path   = path;
        h.iface  = "org.freedesktop.portal.Request";
        h.member = "Close";
        dbsend(&wl->bus, *a, &h, {});
        r.response = 3;
        return r;
    }
    DbVal *sv = dbparse(a, s.m.body, s.m.h.sig);
    if (sv) {
        r.response = (u32)dbat(sv, 0)->u;
        r.results  = dbat(sv, 1);
    }
    return r;
}

// Options with a fresh handle_token, the a{sv} left open for more.
static DbArray options(Buf *b, Str token)
{
    DbArray o = dbarray(b, 8);
    dbentrystr(b, "handle_token", token);
    return o;
}

static void closesession(Wl *wl, Arena scratch)
{
    if (wl->session.len) {
        DbHead h = {};
        h.type   = DB_CALL;
        h.flags  = DB_NOREPLY;
        h.dest   = PORTAL;
        h.path   = wl->session;
        h.iface  = "org.freedesktop.portal.Session";
        h.member = "Close";
        dbsend(&wl->bus, scratch, &h, {});
    }
    wl->session  = {};
    wl->nstreams = 0;
    wl->input    = IN_NONE;
}

static Str eisopen(Wl *wl, Arena scratch, i32 fd);

// Start the remote desktop session, unless it is running: a dialog the
// first time, none with a restore token. Null if it runs, else why not.
static Str portalopen1(Wl *wl, Arena scratch);

// Once per command at most: a dialog left unanswered is not shown again
// for the next step of the same command.
static Str portalopen(Wl *wl, Arena scratch)
{
    if (wl->openfail.len) {
        return wlfail(wl, wl->openfail);
    }
    Str why = portalopen1(wl, scratch);
    if (why.data) {
        wl->openfail = why;
    }
    return why;
}

static Str portalopen1(Wl *wl, Arena scratch)
{
    if (wl->session.len && !wl->eidead) {
        return {};
    } else if (wl->session.len) {
        closesession(wl, scratch);  // libei lost it: start over
        wl->eidead = 0;
    }
    if (wl->busfail.len) {
        return wlfail(wl, wl->busfail);
    }

    // CreateSession
    Arena *a     = &scratch;
    Str    token = newtoken(wl, a);
    Buf    b(a, 256);
    DbArray o = options(&b, token);
    dbentrystr(&b, "session_handle_token", newtoken(wl, a));
    dbarrayend(&b, o);
    PortalReply r = portalcall(wl, a, RD, "CreateSession", "a{sv}", finish(&b),
                               token, WL_CALLMS);
    Str session = dbget(r.results, "session_handle")->s;
    if (r.error.len) {
        return wlfail(wl, concat(a, "the desktop has no remote desktop portal "
                                    "(org.freedesktop.portal.RemoteDesktop): ", r.error));
    } else if (r.response || !session.len) {
        return wlfail(wl, "the remote desktop portal would not create a session");
    }
    wl->session = clone(&wl->perm, session);

    // SelectDevices, with the restore token of the last approval
    token = newtoken(wl, a);
    b = Buf(a, 256);
    dbstr(&b, session);
    o = options(&b, token);
    dbentryu32(&b, "types", 3);          // keyboard and pointer
    dbentryu32(&b, "persist_mode", 2);   // until revoked
    Str saved = readtoken(wl, a);
    if (saved.len) {
        dbentrystr(&b, "restore_token", saved);
    }
    dbarrayend(&b, o);
    r = portalcall(wl, a, RD, "SelectDevices", "oa{sv}", finish(&b), token, WL_CALLMS);
    if (r.error.len || r.response) {
        closesession(wl, scratch);
        return wlfail(wl, concat(a, "the remote desktop portal refused the "
                                    "keyboard and pointer: ", r.error));
    }

    // SelectSources: every monitor, without the pointer drawn in
    token = newtoken(wl, a);
    b = Buf(a, 256);
    dbstr(&b, session);
    o = options(&b, token);
    dbentryu32(&b, "types", 1);
    dbentrybool(&b, "multiple", 1);
    dbentryu32(&b, "cursor_mode", 1);
    dbarrayend(&b, o);
    r = portalcall(wl, a, SC, "SelectSources", "oa{sv}", finish(&b), token, WL_CALLMS);
    if (r.error.len || r.response) {
        closesession(wl, scratch);
        return wlfail(wl, concat(a, "the screen cast portal refused to share "
                                    "the monitors: ", r.error));
    }

    // Start, which asks the user unless the token restores an approval
    token = newtoken(wl, a);
    b = Buf(a, 128);
    dbstr(&b, session);
    dbstr(&b, "");
    o = options(&b, token);
    dbarrayend(&b, o);
    r = portalcall(wl, a, RD, "Start", "osa{sv}", finish(&b), token, wl->askms);
    if (r.response) {
        closesession(wl, scratch);
        if (r.response == 1) {
            savetoken(wl, *a, {});
            return wlfail(wl, "the user declined the desktop's remote desktop "
                              "dialog, so the screen cannot be seen nor input "
                              "sent; nothing was done");
        } else if (r.response == 3) {
            Buf m(a, 256);
            print(&m, "the desktop's remote desktop dialog, which asks to let "
                      "prospero-desktop see the screen and control the "
                      "keyboard and pointer, was not answered within ");
            print(&m, (i64)(wl->askms/1000));
            print(&m, " seconds, so nothing was done: ask the user to approve "
                      "it (prospero-desktop access -ask waits longer), on "
                      "GNOME by turning on \"Allow Remote Interaction\" and "
                      "pressing Share; the approval is remembered");
            return wlfail(wl, finish(&m));
        }
        return wlfail(wl, concat(a, "the remote desktop portal failed to start "
                                    "its session", r.error.len ? concat(a, ": ", r.error) : Str{}));
    }
    Str next = dbget(r.results, "restore_token")->s;
    if (next.len) {
        savetoken(wl, *a, next);
    }
    u32 devices = (u32)dbget(r.results, "devices")->u;
    DbVal *streams = dbget(r.results, "streams");
    wl->nstreams = 0;
    for (DbVal *s = streams->child; s && wl->nstreams<countof(wl->streams); s = s->next) {
        WlStream *w = &wl->streams[wl->nstreams];
        DbVal *props = dbat(s, 1);
        DbVal *pos   = dbget(props, "position");
        DbVal *size  = dbget(props, "size");
        w->node = (u32)dbat(s, 0)->u;
        w->x    = (i32)dbat(pos, 0)->i;
        w->y    = (i32)dbat(pos, 1)->i;
        w->w    = (i32)dbat(size, 0)->i;
        w->h    = (i32)dbat(size, 1)->i;
        wl->nstreams += w->w>0 && w->h>0;
    }
    if (!wl->nstreams) {
        closesession(wl, scratch);
        return wlfail(wl, "the remote desktop session shares no monitor, so "
                          "the screen cannot be seen");
    } else if ((devices & 3) != 3) {
        // An approval to watch alone is no use: asked again next time
        closesession(wl, scratch);
        savetoken(wl, *a, {});
        return wlfail(wl, "the remote desktop dialog was approved without "
                          "control of the keyboard and pointer (GNOME's "
                          "\"Allow Remote Interaction\" switch was off), so "
                          "no input can be sent; it will be asked again");
    }

    // Input through libei, else the portal's own calls
    if (!wl->eiloaded) {
        wl->eiloaded = loadei(&wl->el) ? 1 : -1;
    }
    wl->input = IN_NOTIFY;
    if (wl->eiloaded==1 && !(getenvstr("PROSPERO_DESKTOP_EI") == "0")) {
        b = Buf(a, 64);
        dbstr(&b, session);
        DbArray e = dbarray(&b, 8);
        dbarrayend(&b, e);
        DbIn in = dbcall(&wl->bus, a, PORTAL, PORTALPATH, RD, "ConnectToEIS",
                         "oa{sv}", finish(&b), WL_CALLMS);
        if (dbresult(a, &in) && in.fd>=0) {
            Str why = eisopen(wl, scratch, in.fd);
            if (why.data) {
                closesession(wl, scratch);
                return wlfail(wl, why);
            }
            wl->input = IN_EIS;
        } else if (in.fd >= 0) {
            close(in.fd);
        }
    }
    return {};
}

static DkRect wlscreen(Wl *wl)
{
    DkRect r = {};
    for (i32 i = 0; i < wl->nstreams; i++) {
        WlStream *s = &wl->streams[i];
        if (!i || s->x < r.left)       r.left   = s->x;
        if (!i || s->y < r.top)        r.top    = s->y;
        if (!i || s->x+s->w > r.right)  r.right  = s->x + s->w;
        if (!i || s->y+s->h > r.bottom) r.bottom = s->y + s->h;
    }
    return r;
}


// Input through libei

static void eidevice(Wl *wl, EiDevice *d)
{
    EiLib *l = &wl->el;
    if (!wl->abs && l->ei_device_has_capability(d, EI_CAP_ABSOLUTE) &&
            l->ei_device_has_capability(d, EI_CAP_BUTTON)) {
        wl->abs = l->ei_device_ref(d);
    }
    if (!wl->kbd && l->ei_device_has_capability(d, EI_CAP_KEYBOARD)) {
        wl->kbd = l->ei_device_ref(d);
        EiKeymap *km = l->ei_device_keyboard_get_keymap(d);
        if (km) {
            uz    size = l->ei_keymap_get_size(km);
            i32   fd   = l->ei_keymap_get_fd(km);
            void *p    = mmap(0, size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (p != MAP_FAILED) {
                if (!wl->xctx) {
                    wl->xctx = l->xkb_context_new(0);
                }
                // The text may lack its terminator within size
                Arena t = wl->perm;
                char *s = (char *)allocbytes(&t, (iz)size + 1);
                copybytes(s, p, (iz)size);
                s[size] = 0;
                if (wl->xctx) {
                    wl->keymap = l->xkb_keymap_new_from_string(wl->xctx, s, 1, 0);
                }
                munmap(p, size);
            }
        }
    }
}

// Handle libei's events until cond, or ms pass: 0 if it never held.
static b32 eipump(Wl *wl, i32 ms, b32 (*cond)(Wl *))
{
    EiLib *l   = &wl->el;
    i64    end = wlms() + ms;
    for (;;) {
        l->ei_dispatch(wl->ei);
        for (EiEvent *e; (e = l->ei_get_event(wl->ei));) {
            switch (l->ei_event_get_type(e)) {
            case EI_DISCONNECT:
                wl->eidead = 1;
                break;
            case EI_SEAT_ADDED:
                l->ei_seat_bind_capabilities(l->ei_event_get_seat(e),
                    (i32)EI_CAP_POINTER, (i32)EI_CAP_ABSOLUTE, (i32)EI_CAP_BUTTON,
                    (i32)EI_CAP_SCROLL, (i32)EI_CAP_KEYBOARD, (void *)0);
                break;
            case EI_DEVICE_ADDED:
                eidevice(wl, l->ei_event_get_device(e));
                break;
            case EI_DEVICE_REMOVED:
            case EI_DEVICE_PAUSED: {
                EiDevice *d = l->ei_event_get_device(e);
                if (d == wl->abs) wl->abson = 0;
                if (d == wl->kbd) wl->kbdon = 0;
                if (l->ei_event_get_type(e) == EI_DEVICE_REMOVED) {
                    if (d == wl->abs) wl->abs = 0;
                    if (d == wl->kbd) wl->kbd = 0;
                }
            } break;
            case EI_DEVICE_RESUMED: {
                EiDevice *d = l->ei_event_get_device(e);
                if (d==wl->abs || d==wl->kbd) {
                    l->ei_device_start_emulating(d, ++wl->seq);
                    wl->abson |= d == wl->abs;
                    wl->kbdon |= d == wl->kbd;
                }
            } break;
            case EI_KEYBOARD_MODIFIERS:
                wl->group = l->ei_event_keyboard_get_xkb_group(e);
                break;
            }
            l->ei_event_unref(e);
        }
        if (wl->eidead) {
            return 0;
        } else if (cond(wl)) {
            return 1;
        }
        i64 left = end - wlms();
        if (left <= 0) {
            return 0;
        }
        struct pollfd p = {l->ei_get_fd(wl->ei), POLLIN, 0};
        poll(&p, 1, (int)(left<50 ? left : 50));
    }
}

static b32 eiready(Wl *wl)
{
    return wl->abson && wl->kbdon && wl->keymap;
}

static b32 eiany(Wl *)
{
    return 0;
}

static Str eisopen(Wl *wl, Arena scratch, i32 fd)
{
    EiLib *l = &wl->el;
    if (wl->ei) {
        l->ei_unref(wl->ei);
    }
    wl->ei    = l->ei_new_sender(0);
    wl->abs   = wl->kbd = 0;
    wl->abson = wl->kbdon = 0;
    wl->keymap = 0;
    l->ei_configure_name(wl->ei, "prospero-desktop");
    if (l->ei_setup_backend_fd(wl->ei, fd)) {
        return "cannot connect libei to the desktop's input socket";
    }
    if (!eipump(wl, 3000, eiready)) {
        Buf b(&scratch, 128);
        print(&b, "the desktop gave libei no ");
        print(&b, !wl->abs ? Str("absolute pointer") : !wl->kbd ? Str("keyboard")
                           : !wl->keymap ? Str("keymap") : Str("device it would resume"));
        return clone(&wl->perm, finish(&b));
    }
    return {};
}

static void eiframe(Wl *wl, EiDevice *d)
{
    wl->el.ei_device_frame(d, wl->el.ei_now(wl->ei));
}

// Every input call first takes what libei has to say, which may be that
// the session ended (the user stopped sharing).
static b32 eicheck(Wl *wl)
{
    eipump(wl, 0, eiany);
    if (wl->eidead || !wl->abson || !wl->kbdon) {
        wlfail(wl, "the desktop ended or paused the remote desktop session "
                   "(was sharing stopped?); no input was sent");
        return 0;
    }
    return 1;
}

struct XKey {
    u32 code;    // evdev, 0 if none
    u32 nmods;
    u32 mod[3];  // modifier keys to hold, evdev
};

// The key and modifier keys that make a keysym in the current layout.
static XKey xkbfind(Wl *wl, u32 sym)
{
    EiLib     *l  = &wl->el;
    XkbKeymap *km = wl->keymap;
    XKey       r  = {};
    u32 lo = l->xkb_keymap_min_keycode(km);
    u32 hi = l->xkb_keymap_max_keycode(km);
    for (u32 level = 0; level < 4; level++) {
        for (u32 k = lo; k <= hi && k < 4096; k++) {
            u32 nl = l->xkb_keymap_num_layouts_for_key(km, k);
            if (!nl) continue;
            u32 layout = wl->group<nl ? wl->group : 0;
            if (level >= l->xkb_keymap_num_levels_for_key(km, k, layout)) continue;
            u32 const *syms = 0;
            i32 n = l->xkb_keymap_key_get_syms_by_level(km, k, layout, level, &syms);
            if (n!=1 || syms[0]!=sym) continue;
            u32 masks[4] = {};
            uz  nm = l->xkb_keymap_key_get_mods_for_level(km, k, layout, level, masks, 4);
            if (!nm) continue;
            // Modifier keys whose masks make up the level's: shift, AltGr
            static u32 const modsyms[] = {0xffe1, 0xfe03, 0xfe11, 0xffe3, 0xffe9};
            for (uz m = 0; m < nm; m++) {
                u32 want = masks[m];
                u32 have = 0;
                r.nmods = 0;
                for (i32 j = 0; j<countof(modsyms) && have!=want && r.nmods<3; j++) {
                    XKey mk = level ? xkbfind(wl, modsyms[j]) : XKey{};
                    if (!mk.code || mk.nmods) continue;
                    XkbState *st = l->xkb_state_new(km);
                    if (!st) continue;
                    l->xkb_state_update_key(st, mk.code+8, 1);
                    u32 bits = l->xkb_state_serialize_mods(st, 1);  // depressed
                    l->xkb_state_unref(st);
                    if (bits && !(bits & ~want) && (bits & ~have)) {
                        have |= bits;
                        r.mod[r.nmods++] = mk.code;
                    }
                }
                if (have == want) {
                    r.code = k - 8;
                    return r;
                }
            }
        }
    }
    r.nmods = 0;
    return r;
}

static void eikey(Wl *wl, u32 code, b32 down)
{
    wl->el.ei_device_keyboard_key(wl->kbd, code, down);
    eiframe(wl, wl->kbd);
}


// Input through the portal's Notify* calls

// A Notify* call's body so far, the session and empty options, for the
// caller to append the rest to.
static Buf notifybody(Wl *wl, Arena *a)
{
    Buf b(a, 128);
    dbstr(&b, wl->session);
    DbArray o = dbarray(&b, 8);
    dbarrayend(&b, o);
    return b;
}

static b32 notifysend(Wl *wl, Arena scratch, Str method, Str sig, Buf *body)
{
    DbIn in = dbcall(&wl->bus, &scratch, PORTAL, PORTALPATH, RD, method,
                     concat(&scratch, "oa{sv}", sig), finish(body), WL_CALLMS);
    if (!dbresult(&scratch, &in)) {
        wlfail(wl, concat(&scratch, "the remote desktop portal refused input: ",
                          dberror(&scratch, &in)));
        return 0;
    }
    return 1;
}


// Pointer and keyboard, through either

static u8 *grab(Wl *, Arena *, WlStream *, DkSize *);

static b32 wlpointer(Wl *wl, Arena scratch, i32 x, i32 y)
{
    if (portalopen(wl, scratch).data) {
        return 0;
    }
    if (wl->input == IN_EIS) {
        if (!eicheck(wl)) return 0;
        wl->el.ei_device_pointer_motion_absolute(wl->abs, x, y);
        eiframe(wl, wl->abs);
        return 1;
    }
    // Relative to the stream of the monitor under the point. GNOME (50)
    // checks the point against the stream's logical size, but Mutter
    // then divides it by the monitor's scale, so on a scaled monitor it
    // goes as far as it may, scaled, and relative motion, which is in
    // logical pixels, makes up the rest. The scale is learned from a
    // frame of the stream.
    for (i32 i = 0; i < wl->nstreams; i++) {
        WlStream *s = &wl->streams[i];
        if (x<s->x || x>=s->x+s->w || y<s->y || y>=s->y+s->h) {
            continue;
        }
        f64 lx = x - s->x;
        f64 ly = y - s->y;
        f64 k  = 1;
        if (dkcontains(getenvstr("XDG_CURRENT_DESKTOP"), "GNOME")) {
            if (!s->pw) {
                Arena  t  = scratch;
                DkSize sz = {};
                if (!grab(wl, &t, s, &sz)) {
                    return 0;
                }
            }
            k = (f64)s->pw / s->w;
        }
        f64 ax = lx*k < s->w-1 ? lx*k : s->w-1;
        f64 ay = ly*k < s->h-1 ? ly*k : s->h-1;
        Buf b = notifybody(wl, &scratch);
        dbu32(&b, s->node);
        dbf64(&b, ax);
        dbf64(&b, ay);
        if (!notifysend(wl, scratch, "NotifyPointerMotionAbsolute", "udd", &b)) {
            return 0;
        }
        f64 dx = lx - ax/k;
        f64 dy = ly - ay/k;
        if (dx>0.5 || dy>0.5) {
            b = notifybody(wl, &scratch);
            dbf64(&b, dx);
            dbf64(&b, dy);
            return notifysend(wl, scratch, "NotifyPointerMotion", "dd", &b);
        }
        return 1;
    }
    wlfail(wl, "the point is on no monitor shared with this helper");
    return 0;
}

static b32 wlbutton(Wl *wl, Arena scratch, i32 button, b32 down)
{
    u32 code = button ? 0x111 : 0x110;  // BTN_RIGHT, BTN_LEFT
    if (portalopen(wl, scratch).data) {
        return 0;
    }
    if (wl->input == IN_EIS) {
        if (!eicheck(wl)) return 0;
        wl->el.ei_device_button_button(wl->abs, code, down);
        eiframe(wl, wl->abs);
        return 1;
    }
    Buf b = notifybody(wl, &scratch);
    dbi32(&b, (i32)code);
    dbu32(&b, down ? 1 : 0);
    return notifysend(wl, scratch, "NotifyPointerButton", "iu", &b);
}

// Positive notches scroll up, which Wayland calls negative.
static b32 wlwheel(Wl *wl, Arena scratch, i32 notches)
{
    if (portalopen(wl, scratch).data) {
        return 0;
    }
    if (wl->input == IN_EIS) {
        if (!eicheck(wl)) return 0;
        wl->el.ei_device_scroll_discrete(wl->abs, 0, -notches*120);
        eiframe(wl, wl->abs);
        return 1;
    }
    Buf b = notifybody(wl, &scratch);
    dbu32(&b, 0);  // vertical
    dbi32(&b, -notches);
    return notifysend(wl, scratch, "NotifyPointerAxisDiscrete", "ui", &b);
}

static Str keyfail(Arena *a, u32 sym, i32 c)
{
    Buf b(a, 160);
    print(&b, "no key of the desktop's keyboard layout makes ");
    if (c > 0) {
        u8 tmp[4];
        putbyte(&b, '\'');
        print(&b, Str{tmp, utf8encode(tmp, c)});
        print(&b, "' (U+");
        printhex(&b, (u64)c, 4);
        putbyte(&b, ')');
    } else {
        print(&b, "the keysym 0x");
        printhex(&b, sym, 4);
    }
    print(&b, ", and Wayland lets no program add one; no input was sent");
    return finish(&b);
}

// Keysyms pressed in order, released in reverse.
static b32 wlkeys(Wl *wl, Arena scratch, i32 *syms, i32 n)
{
    if (portalopen(wl, scratch).data) {
        return 0;
    }
    if (wl->input == IN_EIS) {
        if (!eicheck(wl)) return 0;
        u32 codes[16];
        i32 nc = 0;
        for (i32 i = 0; i < n; i++) {
            XKey k = xkbfind(wl, (u32)syms[i]);
            if (!k.code) {
                wlfail(wl, keyfail(&scratch, (u32)syms[i], 0));
                return 0;
            }
            for (u32 m = 0; m < k.nmods && nc<countof(codes); m++) {
                b32 dup = 0;
                for (i32 j = 0; j < nc; j++) dup |= codes[j] == k.mod[m];
                if (!dup) codes[nc++] = k.mod[m];
            }
            if (nc < countof(codes)) codes[nc++] = k.code;
        }
        for (i32 i = 0; i < nc; i++) eikey(wl, codes[i], 1);
        for (i32 i = nc-1; i >= 0; i--) eikey(wl, codes[i], 0);
        return 1;
    }
    b32 ok = 1;
    for (i32 i = 0; ok && i < n; i++) {
        Buf b = notifybody(wl, &scratch);
        dbi32(&b, syms[i]);
        dbu32(&b, 1);
        ok = notifysend(wl, scratch, "NotifyKeyboardKeysym", "iu", &b);
    }
    for (i32 i = n-1; i >= 0; i--) {
        Buf b = notifybody(wl, &scratch);
        dbi32(&b, syms[i]);
        dbu32(&b, 0);
        ok &= notifysend(wl, scratch, "NotifyKeyboardKeysym", "iu", &b);
    }
    return ok;
}

// The keysym of a character: Latin-1 as itself, the rest of Unicode
// offset, or as libxkbcommon names it where it has an older keysym.
static u32 wlsymof(Wl *wl, i32 c)
{
    if (wl->eiloaded==1 && c>=0x100) {
        u32 s = wl->el.xkb_utf32_to_keysym((u32)c);
        if (s) return s;
    }
    return (c>=0x20 && c<0x7f) || (c>=0xa0 && c<=0xff) ? (u32)c : 0x01000000u|(u32)c;
}

static b32 wltext(Wl *wl, Arena scratch, Str s)
{
    if (portalopen(wl, scratch).data) {
        return 0;
    }
    Slice<u32> syms  = {};
    Slice<i32> chars = {};
    for (iz i = 0; i < s.len;) {
        i32 c = utf8decode(s, &i);
        if (c == '\r') {
            if (i<s.len && s[i]=='\n') continue;
            c = '\n';
        }
        u32 sym = 0;
        if (c == '\n') {
            sym = 0xff0d;
        } else if (c == '\t') {
            sym = 0xff09;
        } else if (c>=' ' && c!=0x7f && !(c>=0x80 && c<0xa0)) {
            sym = wlsymof(wl, c);
        }
        if (sym) {
            syms  = push(&scratch, syms, sym);
            chars = push(&scratch, chars, c);
        }
    }
    if (wl->input == IN_EIS) {
        if (!eicheck(wl)) return 0;
        // Every character found before any is typed
        XKey *keys = alloc<XKey>(&scratch, syms.len);
        for (iz i = 0; i < syms.len; i++) {
            keys[i] = xkbfind(wl, syms[i]);
            if (!keys[i].code) {
                wlfail(wl, keyfail(&scratch, syms[i], chars[i]));
                return 0;
            }
        }
        for (iz i = 0; i < syms.len; i++) {
            for (u32 m = 0; m < keys[i].nmods; m++) eikey(wl, keys[i].mod[m], 1);
            eikey(wl, keys[i].code, 1);
            eikey(wl, keys[i].code, 0);
            for (u32 m = keys[i].nmods; m > 0; m--) eikey(wl, keys[i].mod[m-1], 0);
            if (i%32 == 31) {
                eipump(wl, 0, eiany);
                nap(2);
            }
        }
        return eicheck(wl);
    }
    b32 ok = 1;
    for (iz i = 0; ok && i < syms.len; i++) {
        for (i32 down = 1; ok && down >= 0; down--) {
            Buf b = notifybody(wl, &scratch);
            dbi32(&b, (i32)syms[i]);
            dbu32(&b, (u32)down);
            ok = notifysend(wl, scratch, "NotifyKeyboardKeysym", "iu", &b);
        }
    }
    return ok;
}


// Screenshots through PipeWire

enum : u32 {
    SPA_ID = 3, SPA_INT = 4, SPA_RECT = 10, SPA_FRACTION = 11,
    SPA_OBJECT = 15, SPA_CHOICE = 19,
    SPA_OBJFORMAT = 0x40003, SPA_OBJBUFFERS = 0x40004,
    SPA_PARAMENUM = 3, SPA_PARAMFORMAT = 4, SPA_PARAMBUFFERS = 5,
    SPA_FMEDIATYPE = 1, SPA_FMEDIASUB = 2,
    SPA_FVFORMAT = 0x20001, SPA_FVSIZE = 0x20003, SPA_FVRATE = 0x20004,
    SPA_BUFDATATYPE = 6,
};

static void podu32(Buf *b, u32 v)
{
    dbraw(b, &v, 4);
}

static iz podbegin(Buf *b, u32 type)
{
    iz at = b->len;
    podu32(b, 0);
    podu32(b, type);
    return at;
}

static void podend(Buf *b, iz at)
{
    u32 size = (u32)(b->len - at - 8);
    copybytes(b->data+at, &size, 4);
    while (b->len & 7) putbyte(b, 0);
}

static void podprop(Buf *b, u32 key)
{
    podu32(b, key);
    podu32(b, 0);
}

static void podid(Buf *b, u32 v)
{
    iz at = podbegin(b, SPA_ID);
    podu32(b, v);
    podend(b, at);
}

// The formats this helper reads, sizes and rates of any kind.
static Str enumformat(Arena *a)
{
    Buf b(a, 512);
    iz obj = podbegin(&b, SPA_OBJECT);
    podu32(&b, SPA_OBJFORMAT);
    podu32(&b, SPA_PARAMENUM);
    podprop(&b, SPA_FMEDIATYPE);
    podid(&b, 2);  // video
    podprop(&b, SPA_FMEDIASUB);
    podid(&b, 1);  // raw
    podprop(&b, SPA_FVFORMAT);
    iz ch = podbegin(&b, SPA_CHOICE);
    podu32(&b, 3);  // enum
    podu32(&b, 0);
    podu32(&b, 4);
    podu32(&b, SPA_ID);
    static u32 const fmts[] = {8, 8, 12, 7, 11, 9, 10, 13, 14};  // BGRx first
    for (i32 i = 0; i < countof(fmts); i++) podu32(&b, fmts[i]);
    podend(&b, ch);
    podprop(&b, SPA_FVSIZE);
    ch = podbegin(&b, SPA_CHOICE);
    podu32(&b, 1);  // range
    podu32(&b, 0);
    podu32(&b, 8);
    podu32(&b, SPA_RECT);
    podu32(&b, 1920); podu32(&b, 1080);
    podu32(&b, 1);    podu32(&b, 1);
    podu32(&b, 16384); podu32(&b, 16384);
    podend(&b, ch);
    podprop(&b, SPA_FVRATE);
    ch = podbegin(&b, SPA_CHOICE);
    podu32(&b, 1);
    podu32(&b, 0);
    podu32(&b, 8);
    podu32(&b, SPA_FRACTION);
    podu32(&b, 30); podu32(&b, 1);
    podu32(&b, 0);  podu32(&b, 1);
    podu32(&b, 1000); podu32(&b, 1);
    podend(&b, ch);
    podend(&b, obj);
    return finish(&b);
}

// Buffers in memory this helper can map: MemPtr and MemFd.
static Str parambuffers(Arena *a)
{
    Buf b(a, 64);
    iz obj = podbegin(&b, SPA_OBJECT);
    podu32(&b, SPA_OBJBUFFERS);
    podu32(&b, SPA_PARAMBUFFERS);
    podprop(&b, SPA_BUFDATATYPE);
    iz v = podbegin(&b, SPA_INT);
    podu32(&b, 1<<1 | 1<<2);
    podend(&b, v);
    podend(&b, obj);
    return finish(&b);
}

struct Grab {
    PwLib        *pl;
    PwThreadLoop *loop;
    PwStream     *stream;
    Arena        *a;
    u32           format;   // SPA_VIDEO_FORMAT_*
    i32           w, h;
    u8           *pixels;   // BGRX, once a frame came
    b32           failed;
    Str           params;   // the buffers param, built ahead
};

static u32 podget(u8 *p)
{
    u32 v = 0;
    copybytes(&v, p, 4);
    return v;
}

static void onparam(void *data, u32 id, void const *param)
{
    Grab *g = (Grab *)data;
    if (id!=SPA_PARAMFORMAT || !param) {
        return;
    }
    u8 *p    = (u8 *)param;
    u32 size = podget(p);
    if (podget(p+4) != SPA_OBJECT || size < 8) {
        return;
    }
    for (u32 off = 8; off+16 <= size;) {
        u8 *prop  = p + 8 + off;
        u32 key   = podget(prop);
        u32 vsize = podget(prop+8);
        u32 vtype = podget(prop+12);
        u8 *val   = prop + 16;
        if (vtype==SPA_CHOICE && vsize>=16) {
            vtype = podget(val+12);  // the child's type, its first value
            val  += 16;
        }
        if (key==SPA_FVFORMAT && vtype==SPA_ID) {
            g->format = podget(val);
        } else if (key==SPA_FVSIZE && vtype==SPA_RECT) {
            g->w = (i32)podget(val);
            g->h = (i32)podget(val+4);
        }
        off += 16 + ((vsize + 7) & ~7u);
    }
    void const *params[] = {g->params.data};
    g->pl->pw_stream_update_params(g->stream, params, 1);
}

static void onstate(void *data, i32, i32 state, char const *)
{
    Grab *g = (Grab *)data;
    if (state < 0) {
        g->failed = 1;
        g->pl->pw_thread_loop_signal(g->loop, 0);
    }
}

static void onprocess(void *data)
{
    Grab     *g = (Grab *)data;
    PwBuffer *b = g->pl->pw_stream_dequeue_buffer(g->stream);
    if (!b) {
        return;
    }
    SpaData *d = b->buffer->ndatas ? &b->buffer->datas[0] : 0;
    i32 w = g->w, h = g->h;
    if (!g->pixels && d && d->data && d->chunk && d->chunk->size &&
            !(d->chunk->flags & 1) && w>0 && h>0) {
        i32 stride = d->chunk->stride ? d->chunk->stride : w*4;
        iz  need   = (iz)d->chunk->offset + (iz)stride*(h-1) + (iz)w*4;
        i32 r = -1, gr = 1, bl = -1;
        switch (g->format) {
        case 7: case 11: r = 0; bl = 2; break;  // RGBx RGBA
        case 8: case 12: r = 2; bl = 0; break;  // BGRx BGRA
        case 9: case 13: r = 1; gr = 2; bl = 3; break;  // xRGB ARGB
        case 10: case 14: r = 3; gr = 2; bl = 1; break; // xBGR ABGR
        }
        if (r>=0 && stride>=w*4 && need<=(iz)d->maxsize) {
            u8 *src = (u8 *)d->data + d->chunk->offset;
            u8 *dst = allocbytes(g->a, (iz)w*h*4);
            for (i32 y = 0; y < h; y++) {
                u8 *s = src + (iz)y*stride;
                u8 *o = dst + (iz)y*w*4;
                if (g->format==8 || g->format==12) {
                    copybytes(o, s, (iz)w*4);
                    continue;
                }
                for (i32 x = 0; x < w; x++, s += 4, o += 4) {
                    o[0] = s[bl];
                    o[1] = s[gr];
                    o[2] = s[r];
                    o[3] = 0;
                }
            }
            g->pixels = dst;
            g->pl->pw_thread_loop_signal(g->loop, 0);
        }
    }
    g->pl->pw_stream_queue_buffer(g->stream, b);
}

// One frame of a stream, at its own pixels, or null and why.
static u8 *grab(Wl *wl, Arena *a, WlStream *s, DkSize *size)
{
    u32    node = s->node;
    PwLib *pl = &wl->pl;
    if (!wl->pwloaded) {
        wl->pwloaded = loadpw(pl) ? 1 : -1;
    }
    if (wl->pwloaded < 0) {
        wlfail(wl, "cannot load libpipewire-0.3, which screenshots on Wayland "
                   "need (the PipeWire client library)");
        return 0;
    }
    Buf b(a, 64);
    dbstr(&b, wl->session);
    DbArray o = dbarray(&b, 8);
    dbarrayend(&b, o);
    DbIn in = dbcall(&wl->bus, a, PORTAL, PORTALPATH, SC, "OpenPipeWireRemote",
                     "oa{sv}", finish(&b), WL_CALLMS);
    if (!dbresult(a, &in) || in.fd<0) {
        if (in.fd >= 0) close(in.fd);
        wlfail(wl, concat(a, "the screen cast portal would not open PipeWire: ",
                          dberror(a, &in)));
        return 0;
    }

    static PwStreamEvents events = {};
    events.version      = 2;
    events.statechanged = onstate;
    events.paramchanged = onparam;
    events.process      = onprocess;

    Grab g = {};
    g.pl     = pl;
    g.a      = a;
    g.params = parambuffers(a);
    Str fmt  = enumformat(a);
    SpaHook hook = {};

    g.loop = pl->pw_thread_loop_new("prospero-grab", 0);
    if (!g.loop || pl->pw_thread_loop_start(g.loop)) {
        close(in.fd);
        wlfail(wl, "cannot start PipeWire's loop");
        return 0;
    }
    pl->pw_thread_loop_lock(g.loop);
    PwContext *ctx  = pl->pw_context_new(pl->pw_thread_loop_get_loop(g.loop), 0, 0);
    PwCore    *core = ctx ? pl->pw_context_connect_fd(ctx, in.fd, 0, 0) : 0;
    if (core) {
        PwProperties *props = pl->pw_properties_new(
            "media.type", "Video", "media.category", "Capture",
            "media.role", "Screen", (char *)0);
        g.stream = pl->pw_stream_new(core, "prospero-desktop", props);
    } else if (!ctx) {
        close(in.fd);
    }
    b32 started = 0;
    if (g.stream) {
        pl->pw_stream_add_listener(g.stream, &hook, &events, &g);
        void const *params[] = {fmt.data};
        started = !pl->pw_stream_connect(g.stream, 0 /*input*/, node,
                                         1<<0 | 1<<2 /*autoconnect, map*/, params, 1);
    }
    i64 end = wlms() + 5000;
    while (started && !g.pixels && !g.failed && wlms()<end && !quitting) {
        pl->pw_thread_loop_timed_wait(g.loop, 1);
    }
    if (g.stream) {
        pl->pw_stream_disconnect(g.stream);
        pl->pw_stream_destroy(g.stream);
    }
    if (core) {
        pl->pw_core_disconnect(core);
    }
    pl->pw_thread_loop_unlock(g.loop);
    pl->pw_thread_loop_stop(g.loop);
    if (ctx) {
        pl->pw_context_destroy(ctx);
    }
    pl->pw_thread_loop_destroy(g.loop);

    if (!g.pixels) {
        wlfail(wl, !core ? Str("cannot connect to the screen cast's PipeWire remote")
                   : g.failed ? Str("the screen cast's PipeWire stream failed")
                   : Str("the screen cast sent no frame in time"));
        return 0;
    }
    size->w = s->pw = g.w;
    size->h = s->ph = g.h;
    return g.pixels;
}

// The rectangle's pixels from every monitor it overlaps, at the largest
// scale among them.
static u8 *wlcapture(Wl *wl, Arena *a, DkRect r, DkSize *size)
{
    if (portalopen(wl, *a).data) {
        return 0;
    }
    struct Part { WlStream *s; u8 *px; DkSize sz; };
    Part parts[16];
    i32  nparts = 0;
    f64  scale  = 1;
    for (i32 i = 0; i < wl->nstreams; i++) {
        WlStream *s = &wl->streams[i];
        if (s->x>=r.right || s->x+s->w<=r.left || s->y>=r.bottom || s->y+s->h<=r.top) {
            continue;
        }
        Part p = {};
        p.s  = s;
        p.px = grab(wl, a, s, &p.sz);
        if (!p.px) {
            return 0;
        }
        f64 k = (f64)p.sz.w / s->w;
        scale = k>scale ? k : scale;
        parts[nparts++] = p;
    }
    i32 w = (i32)((r.right - r.left)*scale + 0.5);
    i32 h = (i32)((r.bottom - r.top)*scale + 0.5);
    w = w<1 ? 1 : w;
    h = h<1 ? 1 : h;
    u8 *dst = allocbytes(a, (iz)w*h*4);  // zero outside every monitor
    for (i32 k = 0; k < nparts; k++) {
        Part *p = &parts[k];
        f64 sx = (f64)p->sz.w / p->s->w;
        f64 sy = (f64)p->sz.h / p->s->h;
        for (i32 y = 0; y < h; y++) {
            f64 ly = r.top + (y + 0.5)/scale;   // logical
            i32 py = (i32)((ly - p->s->y)*sy);
            if (ly<p->s->y || py<0 || py>=p->sz.h) continue;
            u8 *row = p->px + (iz)py*p->sz.w*4;
            for (i32 x = 0; x < w; x++) {
                f64 lx = r.left + (x + 0.5)/scale;
                i32 px = (i32)((lx - p->s->x)*sx);
                if (lx<p->s->x || px<0 || px>=p->sz.w) continue;
                copybytes(dst + ((iz)y*w + x)*4, row + (iz)px*4, 4);
            }
        }
    }
    size->w = w;
    size->h = h;
    return dst;
}


// Windows on GNOME, through the Window Calls extension

static Str WCPATH  = "/org/gnome/Shell/Extensions/Windows";
static Str WCIFACE = "org.gnome.Shell.Extensions.Windows";

static DbIn wccall(Wl *wl, Arena *a, Str method, u32 id)
{
    Buf b(a, 64);
    Str sig = {};
    if (id) {
        dbu32(&b, id);
        sig = "u";
    }
    return dbcall(&wl->bus, a, "org.gnome.Shell", WCPATH, WCIFACE, method,
                  sig, finish(&b), WL_CALLMS);
}

// Meta's window types that are popups: menus, tooltips, notifications.
static b32 metapopup(i64 type)
{
    return type==6 || (type>=9 && type<=14);
}

static Slice<DkWin> gnomewindows(Wl *wl, Arena *a, b32 all)
{
    Slice<DkWin> r  = {};
    DbIn         in = wccall(wl, a, "List", 0);
    DbVal       *v  = dbresult(a, &in);
    JsonDoc      d  = jsonparse(a, v ? dbat(v, 0)->s : Str{});
    if (!d.ok) {
        wlfail(wl, "the Window Calls extension did not answer List");
        return r;
    }
    // Bottom of the stack first
    for (iz i = jsonlen(d.root)-1; i >= 0; i--) {
        Json *j  = jsonat(d.root, i);
        u32   id = (u32)jsonint(jsonget(j, "id"), 0);
        DbIn  di = wccall(wl, a, "Details", id);
        DbVal *dv = dbresult(a, &di);
        JsonDoc dd = jsonparse(a, dv ? dbat(dv, 0)->s : Str{});
        if (!id || !dd.ok) {
            continue;  // gone meanwhile
        }
        Json *w = dd.root;
        b32 shown = jsonbool(jsonget(w, "in_current_workspace"), 0) &&
                    !jsonbool(jsonget(w, "minimized"), 0);
        if (!all && !shown) {
            continue;
        }
        DkWin k = {};
        i32 x  = (i32)jsonint(jsonget(w, "x"), 0);
        i32 y  = (i32)jsonint(jsonget(w, "y"), 0);
        k.id    = id;
        k.pid   = (u32)jsonint(jsonget(w, "pid"), 0);
        k.rect  = {x, y, x + (i32)jsonint(jsonget(w, "width"), 0),
                         y + (i32)jsonint(jsonget(w, "height"), 0)};
        k.title = clone(a, jsonstr(jsonget(w, "title")));
        k.cls   = clone(a, jsonstr(jsonget(w, "wm_class")));
        k.level = (i32)jsonint(jsonget(w, "window_type"), 0);
        if (jsonbool(jsonget(w, "focus"), 0)) {
            k.level |= 1 << 16;  // has the keyboard
        }
        r = push(a, r, k);
    }
    return r;
}


// Windows on KDE Plasma, through KWin scripts

// Run a script whose body returns a value, and its value as JSON.
static Str kwinrun(Wl *wl, Arena *a, Str body)
{
    Str dir = getenvstr("XDG_RUNTIME_DIR");
    if (!dir.len) {
        dir = "/tmp";
    }
    Buf name(a, 64);
    print(&name, "prospero-");
    print(&name, (i64)getpid());
    putbyte(&name, '-');
    print(&name, (i64)++wl->nscript);
    Str plugin = finish(&name);
    Str path   = join(a, dir, concat(a, plugin, ".js"));

    Buf js(a, 1024);
    print(&js, "(function () {\nvar r = (function () {\n");
    print(&js, body);
    print(&js, "\n})();\ncallDBus(\"");
    print(&js, wl->bus.unique);
    print(&js, "\", \"/prospero\", \"com.nullprogram.Prospero\", \"reply\", "
               "JSON.stringify(r === undefined ? null : r));\n})();\n");
    i32 fd = open(cstr(a, path), O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC, 0600);
    if (fd<0 || !writeall(fd, finish(&js))) {
        if (fd >= 0) close(fd);
        wlfail(wl, "cannot write a KWin script");
        return {};
    }
    close(fd);

    Buf b(a, 256);
    dbstr(&b, path);
    dbstr(&b, plugin);
    DbIn   in = dbcall(&wl->bus, a, "org.kde.KWin", "/Scripting",
                       "org.kde.kwin.Scripting", "loadScript", "ss", finish(&b), WL_CALLMS);
    DbVal *v  = dbresult(a, &in);
    if (!v) {
        unlink(cstr(a, path));
        wlfail(wl, concat(a, "KWin would not load a script: ", dberror(a, &in)));
        return {};
    }
    Buf sp(a, 64);
    print(&sp, "/Scripting/Script");
    print(&sp, dbat(v, 0)->i);
    in = dbcall(&wl->bus, a, "org.kde.KWin", finish(&sp), "org.kde.kwin.Script",
                "run", {}, {}, WL_CALLMS);
    DbIn reply = {};
    if (dbresult(a, &in)) {
        reply = dbwait(&wl->bus, a, DB_CALL, "/prospero", "reply", wlms() + WL_CALLMS);
        if (reply.ok) {
            dbreplyto(&wl->bus, *a, &reply.m, {});
        }
    }
    b = Buf(a, 64);
    dbstr(&b, plugin);
    dbcall(&wl->bus, a, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting",
           "unloadScript", "s", finish(&b), WL_CALLMS);
    unlink(cstr(a, path));  // read when it runs, not when it loads
    DbVal *rv = reply.ok ? dbparse(a, reply.m.body, reply.m.h.sig) : 0;
    if (!rv) {
        wlfail(wl, "a KWin script did not answer");
        return {};
    }
    return dbat(rv, 0)->s;
}

static Str kwinlist =
"var out = [];\n"
"var ws = workspace.stackingOrder;\n"
"for (var i = 0; i < ws.length; i++) {\n"
"    var w = ws[i];\n"
"    if (!w || w.deleted) continue;\n"
"    var g = w.frameGeometry;\n"
"    var here = w.onAllDesktops || !w.desktops ||\n"
"               w.desktops.indexOf(workspace.currentDesktop) >= 0;\n"
"    out.push({id: String(w.internalId), pid: w.pid, title: w.caption,\n"
"              cls: w.resourceClass, x: g.x, y: g.y, w: g.width, h: g.height,\n"
"              shown: here && !w.minimized && !w.hidden,\n"
"              popup: !!(w.popupWindow || w.tooltip || w.dropdownMenu ||\n"
"                        w.popupMenu || w.notification || w.criticalNotification),\n"
"              active: w === workspace.activeWindow});\n"
"}\n"
"return out;";

static u64 kwinid(Wl *wl, Str uuid)
{
    for (iz i = 0; i < wl->kwins.len; i++) {
        if (wl->kwins[i].uuid == uuid) {
            return wl->kwins[i].id;
        }
    }
    WlKwinWin k = {};
    k.id   = (hash(uuid) >> 33) | 1;  // 32 bits, as list prints ids
    k.uuid = clone(&wl->perm, uuid);
    wl->kwins = push(&wl->perm, wl->kwins, k);
    return k.id;
}

static Str kwinuuid(Wl *wl, u64 id)
{
    for (iz i = 0; i < wl->kwins.len; i++) {
        if (wl->kwins[i].id == id) {
            return wl->kwins[i].uuid;
        }
    }
    return {};
}

static Slice<DkWin> kwinwindows(Wl *wl, Arena *a, b32 all)
{
    Slice<DkWin> r = {};
    JsonDoc      d = jsonparse(a, kwinrun(wl, a, kwinlist));
    if (!d.ok) {
        return r;
    }
    for (iz i = jsonlen(d.root)-1; i >= 0; i--) {
        Json *w = jsonat(d.root, i);
        if (!all && !jsonbool(jsonget(w, "shown"), 0)) {
            continue;
        }
        DkWin k = {};
        i32 x = (i32)jsonfloat(jsonget(w, "x"), 0);
        i32 y = (i32)jsonfloat(jsonget(w, "y"), 0);
        k.id    = kwinid(wl, jsonstr(jsonget(w, "id")));
        k.pid   = (u32)jsonint(jsonget(w, "pid"), 0);
        k.rect  = {x, y, x + (i32)jsonfloat(jsonget(w, "w"), 0),
                         y + (i32)jsonfloat(jsonget(w, "h"), 0)};
        k.title = clone(a, jsonstr(jsonget(w, "title")));
        k.cls   = clone(a, jsonstr(jsonget(w, "cls")));
        k.level = jsonbool(jsonget(w, "popup"), 0) ? 6 : 0;
        if (jsonbool(jsonget(w, "active"), 0)) {
            k.level |= 1 << 16;
        }
        r = push(a, r, k);
    }
    return r;
}

// A script acting on one window by its id: BODY sees it as w.
static b32 kwinact(Wl *wl, Arena *a, u64 id, Str body)
{
    Str uuid = kwinuuid(wl, id);
    if (!uuid.len) {
        wlfail(wl, "KWin no longer has the window");
        return 0;
    }
    Buf s(a, 512);
    print(&s, "var ws = workspace.stackingOrder;\n"
              "for (var i = 0; i < ws.length; i++) {\n"
              "    var w = ws[i];\n"
              "    if (String(w.internalId) != \"");
    print(&s, uuid);
    print(&s, "\") continue;\n");
    print(&s, body);
    print(&s, "\n    return true;\n}\nreturn false;");
    Str r = kwinrun(wl, a, finish(&s));
    if (!(r == "true")) {
        if (r.len) wlfail(wl, "KWin no longer has the window");
        return 0;
    }
    return 1;
}


// The platform's calls, for main_desktop_x11.cpp

// Connect, and find what names windows here. Null if usable, else why.
static Str wlready(Wl *wl, Arena scratch)
{
    wl->openfail = {};
    if (!wl->bus.fd && !wl->busfail.len) {
        Str d = getenvstr("XDG_CURRENT_DESKTOP");
        wl->desktop = clone(&wl->perm, cut(d, ':').head);
        wl->askms   = wl->askms ? wl->askms : WL_ASKMS;
        Str why = dbopen(&wl->bus, &wl->perm, scratch);
        if (why.data) {
            wl->busfail = clone(&wl->perm, why);
        }
    }
    if (wl->busfail.len) {
        return wl->busfail;
    }

    // Asked each time, since an extension may come or go
    Arena *a = &scratch;
    Str    d = getenvstr("XDG_CURRENT_DESKTOP");
    wl->winapi = WIN_NONE;
    wl->winwhy = {};
    if (dkcontains(d, "KDE") && hasowner(&wl->bus, scratch, "org.kde.KWin")) {
        wl->winapi = WIN_KWIN;
    } else if (dkcontains(d, "GNOME")) {
        DbIn in = dbcall(&wl->bus, a, "org.gnome.Shell", WCPATH,
                         "org.freedesktop.DBus.Introspectable", "Introspect",
                         {}, {}, WL_CALLMS);
        DbVal *v = dbresult(a, &in);
        if (v && find(dbat(v, 0)->s, WCIFACE)>=0) {
            wl->winapi = WIN_GNOME;
        } else {
            wl->winwhy = "GNOME lets no other program list, raise or close its "
                         "windows; the Window Calls extension "
                         "(window-calls@domandoman.xyz) adds that";
        }
    } else {
        wl->winwhy = concat(&wl->perm, concat(&wl->perm, "this Wayland desktop ('",
                            d.len ? d : Str("unknown")), "') offers no way to "
                            "list, raise or close other programs' windows");
    }
    return {};
}

// The screen's width in logical pixels, without starting the portal if
// it can: GNOME Shell says, else the portal's monitors. 0 if unknown.
static i32 wlwidth(Wl *wl, Arena scratch)
{
    if (!wl->session.len) {
        Buf b(&scratch, 64);
        dbstr(&b, "org.gnome.Shell.Introspect");
        dbstr(&b, "ScreenSize");
        DbIn   in = dbcall(&wl->bus, &scratch, "org.gnome.Shell.Introspect",
                           "/org/gnome/Shell/Introspect", "org.freedesktop.DBus.Properties",
                           "Get", "ss", finish(&b), WL_CALLMS);
        DbVal *v  = dbresult(&scratch, &in);
        i64    w  = v ? dbat(dbunwrap(dbat(v, 0)), 0)->i : 0;
        if (w > 0) {
            return (i32)w;
        }
    }
    if (portalopen(wl, scratch).data) {
        return 0;
    }
    DkRect r = wlscreen(wl);
    return r.right - r.left;
}

static Slice<DkWin> wlwindows(Wl *wl, Arena *a, b32 all)
{
    switch (wl->winapi) {
    case WIN_GNOME: return gnomewindows(wl, a, all);
    case WIN_KWIN:  return kwinwindows(wl, a, all);
    }
    return {};
}

static DkWin wlwindow(Wl *wl, Arena *a, u64 id)
{
    Slice<DkWin> ws = wlwindows(wl, a, 1);
    for (iz i = 0; i < ws.len; i++) {
        if (ws[i].id == id) {
            return ws[i];
        }
    }
    return {};
}

static u64 wlforeground(Wl *wl, Arena scratch)
{
    Slice<DkWin> ws = wlwindows(wl, &scratch, 1);
    for (iz i = 0; i < ws.len; i++) {
        if (ws[i].level & 1<<16) {
            return ws[i].id;
        }
    }
    return 0;
}

static b32 wlactivate(Wl *wl, Arena scratch, DkWin *w)
{
    for (i32 attempt = 0; attempt < 2; attempt++) {
        if (wlforeground(wl, scratch) == w->id) {
            DkWin now = wlwindow(wl, &scratch, w->id);
            if (now.id) {
                w->rect = now.rect;
            }
            if (attempt) nap(50);  // let it repaint
            return 1;
        }
        if (wl->winapi == WIN_GNOME) {
            Arena a = scratch;
            wccall(wl, &a, "Unminimize", (u32)w->id);
            wccall(wl, &a, "Activate", (u32)w->id);
        } else if (wl->winapi == WIN_KWIN) {
            Arena a = scratch;
            kwinact(wl, &a, w->id, "workspace.activeWindow = w;");
        }
        for (i32 i = 0; i < 25 && wlforeground(wl, scratch)!=w->id; i++) {
            nap(20);
        }
    }
    // What has it instead
    Slice<DkWin> ws  = wlwindows(wl, &scratch, 1);
    Str          has = {};
    for (iz i = 0; i < ws.len; i++) {
        has = ws[i].level & 1<<16 ? ws[i].title : has;
    }
    Buf b(&wl->perm, 128);
    print(&b, "cannot give the keyboard to the window '");
    print(&b, w->title);
    print(&b, "'");
    if (has.data) {
        print(&b, ", which '");
        print(&b, has);
        print(&b, "' has");
    } else {
        print(&b, ", which the compositor would not make active");
    }
    print(&b, "; no input was sent");
    wl->failed = finish(&b);
    return 0;
}

static b32 wlclose(Wl *wl, Arena scratch, DkWin *w)
{
    if (wl->winapi == WIN_GNOME) {
        DbIn in = wccall(wl, &scratch, "Close", (u32)w->id);
        if (!dbresult(&scratch, &in)) {
            wlfail(wl, concat(&scratch, "the Window Calls extension would not "
                              "close it: ", dberror(&scratch, &in)));
            return 0;
        }
        return 1;
    }
    return kwinact(wl, &scratch, w->id, "w.closeWindow();");
}

static b32 wlmove(Wl *wl, Arena scratch, DkWin *w, i32 x, i32 y, i32 cw, i32 ch, b32 size)
{
    if (!size) {
        cw = w->rect.right - w->rect.left;
        ch = w->rect.bottom - w->rect.top;
    }
    if (wl->winapi == WIN_GNOME) {
        Buf b(&scratch, 32);
        dbu32(&b, (u32)w->id);
        dbi32(&b, x);
        dbi32(&b, y);
        dbu32(&b, (u32)cw);
        dbu32(&b, (u32)ch);
        DbIn in = dbcall(&wl->bus, &scratch, "org.gnome.Shell", WCPATH, WCIFACE,
                         "MoveResize", "uiiuu", finish(&b), WL_CALLMS);
        if (!dbresult(&scratch, &in)) {
            wlfail(wl, concat(&scratch, "the Window Calls extension would not "
                              "move it: ", dberror(&scratch, &in)));
            return 0;
        }
        return 1;
    }
    Buf s(&scratch, 128);
    print(&s, "w.frameGeometry = {x: ");
    print(&s, (i64)x);
    print(&s, ", y: ");
    print(&s, (i64)y);
    print(&s, ", width: ");
    print(&s, (i64)cw);
    print(&s, ", height: ");
    print(&s, (i64)ch);
    print(&s, "};");
    return kwinact(wl, &scratch, w->id, finish(&s));
}

// The permissions, as access prints them.
static b32 wlaccess(Wl *wl, Arena scratch, Buf *b, b32 ask)
{
    Str  token = readtoken(wl, &scratch);
    b32  ok    = token.len > 0;
    if (ask && !wl->session.len) {
        wl->askms = WL_ASKLONG;
        ok = !portalopen(wl, scratch).data;
        wl->askms = WL_ASKMS;
    } else if (wl->session.len) {
        ok = 1;
    }
    print(b, "remote-desktop\t");
    print(b, ok ? Str("granted\n") : Str("missing\n"));
    print(b, "windows\t");
    print(b, wl->winapi!=WIN_NONE ? Str("granted\n") : Str("missing\n"));
    print(b, "granted-to\t");
    print(b, tokenpath(wl, &scratch));
    putbyte(b, '\n');
    print(b, "windows-by\t");
    print(b, wl->winapi==WIN_GNOME ? Str("GNOME Window Calls extension")
           : wl->winapi==WIN_KWIN  ? Str("KWin scripts")
           : wl->winwhy);
    putbyte(b, '\n');
    if (wl->session.len) {
        print(b, "input-by\t");
        print(b, wl->input==IN_EIS ? Str("libei\n") : Str("portal\n"));
    }
    if (ask && !ok && wl->failed.len) {
        print(b, "why\t");
        print(b, wl->failed);
        putbyte(b, '\n');
    }
    return ok && wl->winapi!=WIN_NONE;
}
