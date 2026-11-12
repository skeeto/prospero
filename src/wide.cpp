// Portable half of the Win32 layer: UTF-16 conversion, command line
// splitting and quoting, and extended-length paths. No Win32 calls, so
// the unit tests cover it on every host. From ssg (skeeto.github.com/_ssg).
// This is free and unencumbered software released into the public domain.

using c16 = decltype(u'0');

struct Str16 {
    c16 *data = {};
    iz   len  = {};
};

// Encode a code point as UTF-16, returning the length. Surrogate code
// points (from WTF-8) encode as themselves.
static iz utf16encode(c16 *s, i32 c)
{
    if (c > 0x10ffff) {
        c = 0xfffd;
    } else if (c >= 0x10000) {
        c -= 0x10000;
        s[0] = (c16)(0xd800 + (c >> 10));
        s[1] = (c16)(0xdc00 + (c & 0x3ff));
        return 2;
    }
    s[0] = (c16)c;
    return 1;
}

// Convert UTF-8 (or WTF-8) to null-terminated UTF-16.
static Str16 towide(Arena *a, Str s)
{
    Str16 r = {};
    r.data = alloc<c16>(a, s.len+1);  // never more units than bytes
    for (iz i = 0; i < s.len;) {
        r.len += utf16encode(r.data+r.len, utf8decode(s, &i));
    }
    return r;
}

// Convert UTF-16 to UTF-8. Unpaired surrogates become WTF-8, so that
// file names round-trip through towide().
static Str fromwide(Arena *a, c16 *s, iz len)
{
    if (len > I64MAX/3) {
        os_oom();
    }
    Str r  = {};
    r.data = allocbytes(a, 3*len);  // worst case
    for (iz i = 0; i < len; i++) {
        i32 c = s[i];
        if (c>=0xd800 && c<=0xdbff && i+1<len && s[i+1]>=0xdc00 && s[i+1]<=0xdfff) {
            c = 0x10000 + ((c - 0xd800)<<10) + (s[++i] - 0xdc00);
        }
        r.len += utf8encode(r.data+r.len, c);
    }
    a->beg = (byte *)(r.data + r.len);  // return the unused tail
    return r;
}

// Split a command line into arguments like CommandLineToArgvW, quirks
// included: backslashes are literal except before a quote, a doubled
// quote inside quotes is a literal quote that also ends the quoting, and
// the program name (first argument) has no escapes at all. Unescapes in
// place, so the arguments point into cmd.
static Slice<Str> splitcmdline(Arena *a, Str cmd)
{
    Slice<Str> args = {};
    iz i = 0;  // read position
    iz w = 0;  // write position, never past i

    if (i<cmd.len && cmd[i]=='"') {
        for (i++; i<cmd.len && cmd[i]!='"'; i++) {
            cmd[w++] = cmd[i];
        }
        i += i < cmd.len;
    } else {
        for (; i<cmd.len && cmd[i]!=' ' && cmd[i]!='\t'; i++) {
            cmd[w++] = cmd[i];
        }
    }
    args = push(a, args, takehead(cmd, w));

    for (;;) {
        for (; i<cmd.len && (cmd[i]==' ' || cmd[i]=='\t'); i++) {}
        if (i == cmd.len) {
            break;
        }

        iz  beg    = w;
        b32 quoted = 0;
        while (i<cmd.len && (quoted || (cmd[i]!=' ' && cmd[i]!='\t'))) {
            if (cmd[i] == '\\') {
                iz n = 0;
                for (; i<cmd.len && cmd[i]=='\\'; i++, n++) {}
                b32 quote = i<cmd.len && cmd[i]=='"';
                for (iz k = 0; k < (quote ? n/2 : n); k++) {
                    cmd[w++] = '\\';
                }
                if (quote && n%2) {
                    cmd[w++] = cmd[i++];  // escaped quote
                }
            } else if (cmd[i] == '"') {
                i++;
                if (quoted && i<cmd.len && cmd[i]=='"') {
                    cmd[w++] = cmd[i++];
                }
                quoted = !quoted;
            } else {
                cmd[w++] = cmd[i++];
            }
        }
        args = push(a, args, span(cmd.data+beg, cmd.data+w));
    }
    return args;
}

// Join arguments into a command line that splitcmdline, and so
// CommandLineToArgvW, splits back into the same arguments. Arguments are
// quoted only when necessary. The program name has no escapes, so it must
// not contain quotes.
static Str quoteargv(Arena *a, Slice<Str> args)
{
    Buf b(a, 256);
    for (iz i = 0; i < args.len; i++) {
        Str arg = args[i];
        if (i) {
            putbyte(&b, ' ');
        }

        b32 quote = !arg.len;
        for (iz j = 0; j < arg.len; j++) {
            quote |= arg[j]==' ' || arg[j]=='\t' || arg[j]=='"';
        }
        if (!quote) {
            print(&b, arg);
            continue;
        } else if (!i) {
            putbyte(&b, '"');
            print(&b, arg);
            putbyte(&b, '"');
            continue;
        }

        // Backslashes are doubled before a quote, including the closing
        // quote, and a literal quote gets one more.
        putbyte(&b, '"');
        for (iz j = 0; j < arg.len;) {
            iz n = 0;
            for (; j<arg.len && arg[j]=='\\'; j++, n++) {}
            b32 last  = j == arg.len;
            b32 dquo  = !last && arg[j]=='"';
            iz  count = last ? 2*n : dquo ? 2*n+1 : n;
            for (iz k = 0; k < count; k++) {
                putbyte(&b, '\\');
            }
            if (!last) {
                putbyte(&b, arg[j++]);
            }
        }
        putbyte(&b, '"');
    }
    return finish(&b);
}


// Given a full path, as written by GetFullPathNameW at buf+6, prepend
// the extended-length prefix in the space before it: C:\x becomes
// \\?\C:\x, and \\host\share becomes \\?\UNC\host\share. This lifts
// the MAX_PATH limit. Paths already in that form are left alone.
static Str16 extendpath(c16 *buf, iz len)
{
    Str16 r = {buf+6, len};
    c16  *p = r.data;
    if (len>=4 && p[0]=='\\' && p[1]=='\\' && (p[2]=='?' || p[2]=='.') && p[3]=='\\') {
        return r;
    }
    b32 unc = len>=2 && p[0]=='\\' && p[1]=='\\';
    c16 prefix[] = {'\\', '\\', '?', '\\', 'U', 'N', 'C'};
    r.data -= unc ? 6 : 4;
    r.len  += unc ? 6 : 4;
    for (iz i = 0; i < (unc ? 7 : 4); i++) {
        r.data[i] = prefix[i];  // for UNC, replaces the first backslash
    }
    return r;
}

// Length of the root of an extended-length path, e.g. \\?\C:\ or
// \\?\UNC\host\share\, including its trailing separator. Zero for other
// paths.
static iz rootlen(Str16 p)
{
    c16 *s = p.data;
    if (p.len<4 || s[0]!='\\' || s[1]!='\\' || s[3]!='\\') {
        return 0;
    }
    iz  i     = 4;
    i32 parts = 1;  // drive, volume, or device
    if (p.len>=8 && s[4]=='U' && s[5]=='N' && s[6]=='C' && s[7]=='\\') {
        i     = 8;
        parts = 2;  // host and share
    }
    for (; parts; parts--) {
        for (; i<p.len && s[i]!='\\'; i++) {}
        i += i < p.len;
    }
    return i;
}

// Walking a path without following its links (fs.read, fs.stat), so as
// to refuse one that reaches off this computer's disks before anything
// opens it: on Windows opening a link to \\host\share signs in there,
// and one to a pipe or device connects to it.

enum : i32 {
    REPARSE_NOTLINK,  // not a name surrogate: dedup, cloud, app alias
    REPARSE_LINK,     // a symlink or junction, with name
    REPARSE_ODDLINK,  // another name surrogate, or one that does not parse
};

struct Reparse {
    Str16 name;      // the substitute name, as \??\C:\x, or relative
    i32   kind;      // REPARSE_*
    b32   relative;  // a symlink's target from the link's folder
};

// What reparse data (REPARSE_DATA_BUFFER, from FSCTL_GET_REPARSE_POINT)
// names. The name points into buf, which must be 2-byte aligned.
static Reparse reparsedata(u8 *buf, iz len)
{
    Reparse r = {};
    if (len < 4) {
        r.kind = REPARSE_ODDLINK;
        return r;
    }
    u32 tag = (u32)buf[0] | (u32)buf[1]<<8 | (u32)buf[2]<<16 | (u32)buf[3]<<24;
    if (!(tag & 0x20000000)) {
        return r;  // not a name surrogate
    }
    r.kind = REPARSE_ODDLINK;
    iz head = tag==0xa000000c ? 20 :  // IO_REPARSE_TAG_SYMLINK
              tag==0xa0000003 ? 16 :  // IO_REPARSE_TAG_MOUNT_POINT
              0;
    if (!head || len<head) {
        return r;
    }
    iz off = buf[8]  | buf[9]<<8;
    iz n   = buf[10] | buf[11]<<8;
    if (!n || off&1 || n&1 || head+off+n>len) {
        return r;
    }
    r.kind      = REPARSE_LINK;
    r.relative  = head==20 && buf[16]&1;  // SYMLINK_FLAG_RELATIVE
    r.name.data = (c16 *)(buf + head + off);
    r.name.len  = n / 2;
    return r;
}

// Is p an extended-length path on a disk here: \\?\C: or \\?\Volume{?
static b32 diskpath(Str16 p)
{
    c16 *s = p.data;
    if (p.len<6 || s[0]!='\\' || s[1]!='\\' || s[2]!='?' || s[3]!='\\') {
        return 0;
    }
    c16 d = (c16)(s[4] | 0x20);
    if (d>='a' && d<='z' && s[5]==':' && (p.len==6 || s[6]=='\\')) {
        return 1;
    }
    c16 vol[] = {'V', 'o', 'l', 'u', 'm', 'e', '{'};
    if (p.len < 4+countof(vol)) {
        return 0;
    }
    for (iz i = 0; i < countof(vol); i++) {
        if (s[4+i] != vol[i]) {
            return 0;
        }
    }
    return 1;
}

// Append path components to out (len units, its root the first root,
// ending in a backslash), resolving . and .. as the system does.
static void pushpath16(c16 *out, iz *len, iz root, Str16 s)
{
    for (iz i = 0; i < s.len;) {
        iz j = i;
        for (; j<s.len && s.data[j]!='\\' && s.data[j]!='/'; j++) {}
        iz n = j - i;
        if (!n || (n==1 && s.data[i]=='.')) {
            // nothing
        } else if (n==2 && s.data[i]=='.' && s.data[i+1]=='.') {
            for (; *len>root && out[*len-1]!='\\'; (*len)--) {}
            *len -= *len > root;
        } else {
            if (*len > root) {
                out[(*len)++] = '\\';
            }
            for (iz k = i; k < j; k++) {
                out[(*len)++] = s.data[k];
            }
        }
        i = j + 1;
    }
}

// The path to walk on from a link at link (\\?\C:\a\l, every folder
// before it no link) with rest after it ("\b", or empty): its target
// spliced in, in the same form. Null if the target is off this
// computer's disks (a UNC path, a device, a pipe) or of an unknown form.
static Str16 linkpath(Arena *a, Str16 link, Reparse t, Str16 rest)
{
    Str16 n    = t.name;
    iz    root = rootlen(link);
    c16  *out  = alloc<c16>(a, link.len + n.len + rest.len + 16);
    iz    len  = 0;
    if (!t.relative) {
        if (n.len<4 || n.data[0]!='\\' || n.data[1]!='?' || n.data[2]!='?' ||
                n.data[3]!='\\') {
            return {};  // \Device\..., or another NT path
        }
        // \??\C:\x, or \??\Volume{guid}\x, as \\?\...
        Str16 s = {n.data+4, n.len-4};
        iz    i = 0;
        for (; i<s.len && s.data[i]!='\\'; i++) {}
        out[len++] = '\\';
        out[len++] = '\\';
        out[len++] = '?';
        out[len++] = '\\';
        for (iz k = 0; k < i; k++) {
            out[len++] = s.data[k];
        }
        out[len++] = '\\';
        root = len;
        if (!diskpath({out, len})) {
            return {};  // UNC\, a device, a pipe
        }
        pushpath16(out, &len, root, {s.data+i, s.len-i});
    } else {
        if (!root || !diskpath(link)) {
            return {};
        }
        for (; len < root; len++) {
            out[len] = link.data[len];
        }
        if (!n.len || (n.data[0]!='\\' && n.data[0]!='/')) {
            // From the link's folder, else from its drive's root
            c16 up[] = {'.', '.'};
            pushpath16(out, &len, root, {link.data+root, link.len-root});
            pushpath16(out, &len, root, {up, 2});
        }
        pushpath16(out, &len, root, n);
    }
    pushpath16(out, &len, root, rest);
    out[len] = 0;
    return {out, len};
}
