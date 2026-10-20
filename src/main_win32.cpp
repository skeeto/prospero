// Win32 platform layer: no CRT and no headers
// $ cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-Mingw64.cmake
// $ c++ -std=c++20 -nostartfiles -o harness.exe src/main_win32.cpp -ladvapi32
//
// Imports only kernel32.dll and advapi32.dll (RtlGenRandom), with
// nothing newer than Windows XP, so the XP engine builds from this file.
//
// Paths are UTF-8 with either separator. Each is made absolute and
// extended-length (\\?\) before use, so MAX_PATH never applies. A path
// or name holding a null byte names nothing, rather than being cut short
// at the system call.
//
// This is free and unencumbered software released into the public domain.
#if HARNESS_GUI
#  include "imgui.h"
#  include "imgui_internal.h"
#  include "misc/freetype/imgui_freetype.h"
#  include "md4c.h"
#endif
#include "harness.cpp"
#include "wide.cpp"

enum : i32 {
    CREATE_NEW                        = 1,
    ERROR_ALREADY_EXISTS              = 183,
    ERROR_BAD_NETPATH                 = 53,
    ERROR_BAD_PATHNAME                = 161,
    ERROR_FILE_NOT_FOUND              = 2,
    ERROR_INVALID_DRIVE               = 15,
    ERROR_INVALID_NAME                = 123,
    ERROR_INVALID_PARAMETER           = 87,
    ERROR_PATH_NOT_FOUND              = 3,
    ERROR_SHARING_VIOLATION           = 32,
    FILE_ATTRIBUTE_DEVICE             = 0x40,
    FILE_ATTRIBUTE_DIRECTORY          = 0x10,
    FILE_ATTRIBUTE_NORMAL             = 0x80,
    FILE_ATTRIBUTE_REPARSE_POINT      = 0x400,
    FILE_BEGIN                        = 0,
    FILE_FLAG_BACKUP_SEMANTICS        = 0x02000000,
    FILE_SHARE_ALL                    = 7,
    FILE_TYPE_DISK                    = 1,
    FIND_FIRST_EX_LARGE_FETCH         = 2,
    FindExInfoBasic                   = 1,
    FindExInfoStandard                = 0,
    FindExSearchNameMatch             = 0,
    GENERIC_READ                      = (i32)0x80000000,
    GENERIC_WRITE                     = 0x40000000,
    GetFileExInfoStandard             = 0,
    INFINITE                          = -1,
    INVALID_FILE_ATTRIBUTES           = -1,
    IO_REPARSE_TAG_NAME_SURROGATE     = 0x20000000,  // symlinks, junctions
    MEM_COMMIT                        = 0x1000,
    MEM_RELEASE                       = 0x8000,
    MEM_RESERVE                       = 0x2000,
    MOVEFILE_REPLACE_EXISTING         = 1,
    MOVEFILE_WRITE_THROUGH            = 8,
    OPEN_ALWAYS                       = 4,
    OPEN_EXISTING                     = 3,
    PAGE_NOACCESS                     = 1,
    PAGE_READWRITE                    = 4,
    STACK_SIZE_PARAM_IS_A_RESERVATION = 0x10000,
    STD_ERROR_HANDLE                  = -12,
    WAIT_OBJECT_0                     = 0,
};

enum : uz {
    INVALID_HANDLE_VALUE = (uz)-1,
};

struct Filetime {
    u32 lo;
    u32 hi;
};

struct FileAttributes {  // WIN32_FILE_ATTRIBUTE_DATA
    u32      attr;
    Filetime ctime;
    Filetime atime;
    Filetime mtime;
    u32      sizehi;
    u32      sizelo;
};
static_assert(sizeof(FileAttributes) == 36);

struct FileInfo {  // BY_HANDLE_FILE_INFORMATION
    u32      attr;
    Filetime ctime;
    Filetime atime;
    Filetime mtime;
    u32      volume;
    u32      sizehi;
    u32      sizelo;
    u32      links;
    u32      indexhi;
    u32      indexlo;
};
static_assert(sizeof(FileInfo) == 52);

struct FindData {  // WIN32_FIND_DATAW
    u32      attr;
    Filetime ctime;
    Filetime atime;
    Filetime mtime;
    u32      sizehi;
    u32      sizelo;
    u32      reparsetag;
    u32      reserved;
    c16      name[260];
    c16      altname[14];
};
static_assert(sizeof(FindData) == 592);

struct Overlapped {  // OVERLAPPED
    uz  internal;
    uz  internalhigh;
    u32 offlo;
    u32 offhi;
    uz  event;
};
static_assert(sizeof(Overlapped) == (sizeof(uz)==8 ? 32 : 20));

struct CriticalSection {  // CRITICAL_SECTION
    uz  debuginfo;
    i32 lockcount;
    i32 recursion;
    uz  owner;
    uz  semaphore;
    uz  spincount;
};
static_assert(sizeof(CriticalSection) == (sizeof(uz)==8 ? 40 : 24));
static_assert(sizeof(CriticalSection) <= sizeof(Lock));
static_assert(alignof(CriticalSection) <= alignof(Lock));

#define W32(r, p) extern "C" __declspec(dllimport) r __stdcall p noexcept
W32(b32,    CloseHandle(uz));
W32(b32,    CreateDirectoryW(c16 *, uz));
W32(uz,     CreateEventW(uz, b32, b32, c16 *));
W32(uz,     CreateFileW(c16 *, i32, i32, uz, i32, i32, uz));
W32(uz,     CreateThread(uz, iz, u32 (__stdcall *)(void *), void *, i32,
                         i32 *));
W32(b32,    DeleteFileW(c16 *));
W32(void,   EnterCriticalSection(CriticalSection *));
W32(void,   ExitProcess[[noreturn]](i32));
W32(b32,    FindClose(uz));
W32(uz,     FindFirstFileExW(c16 *, i32, FindData *, i32, uz, i32));
W32(b32,    FindNextFileW(uz, FindData *));
W32(b32,    FlushFileBuffers(uz));
W32(c16 *,  GetCommandLineW());
W32(b32,    GetConsoleMode(uz, i32 *));
W32(i32,    GetEnvironmentVariableW(c16 *, c16 *, i32));
W32(b32,    GetFileAttributesExW(c16 *, i32, FileAttributes *));
W32(i32,    GetFileAttributesW(c16 *));
W32(b32,    GetFileInformationByHandle(uz, FileInfo *));
W32(b32,    GetFileSizeEx(uz, i64 *));
W32(i32,    GetFileType(uz));
W32(i32,    GetFullPathNameW(c16 *, i32, c16 *, c16 **));
W32(i32,    GetLastError());
W32(i32,    GetModuleFileNameW(uz, c16 *, i32));
W32(uz,     GetProcessHeap());
W32(uz,     GetStdHandle(i32));
W32(void,   GetSystemTimeAsFileTime(Filetime *));
W32(void *, HeapAlloc(uz, i32, iz));
W32(b32,    HeapFree(uz, i32, void *));
W32(b32,    InitializeCriticalSectionAndSpinCount(CriticalSection *, i32));
W32(void,   LeaveCriticalSection(CriticalSection *));
W32(b32,    MoveFileExW(c16 *, c16 *, i32));
W32(b32,    QueryPerformanceCounter(i64 *));
W32(b32,    QueryPerformanceFrequency(i64 *));
W32(b32,    ReadFile(uz, u8 *, i32, i32 *, Overlapped *));
W32(b32,    RemoveDirectoryW(c16 *));
W32(b32,    SetEndOfFile(uz));
W32(b32,    SetEvent(uz));
W32(b32,    SetFilePointerEx(uz, i64, i64 *, i32));
W32(void,   SetLastError(i32));
W32(u8,     SystemFunction036(void *, i32));  // RtlGenRandom (advapi32)
W32(byte *, VirtualAlloc(uz, iz, i32, i32));
W32(b32,    VirtualFree(uz, iz, i32));
W32(i32,    WaitForSingleObject(uz, i32));
W32(b32,    WriteConsoleW(uz, c16 *, i32, i32 *, uz));
W32(b32,    WriteFile(uz, u8 *, i32, i32 *, Overlapped *));

// GCC emits calls to these even without a CRT. Defining them keeps
// msvcrt.dll out of the imports. GUI builds have the CRT's.
#if !HARNESS_GUI && (defined(__x86_64__) || defined(__i386__))
extern "C" void *memcpy(void *dst, void const *src, uz len)
{
    void *r = dst;
    __asm volatile ("rep movsb" : "+D"(dst), "+S"(src), "+c"(len) : : "memory");
    return r;
}

extern "C" void *memset(void *dst, i32 c, uz len)
{
    void *r = dst;
    __asm volatile ("rep stosb" : "+D"(dst), "+c"(len) : "a"(c) : "memory");
    return r;
}

extern "C" void *memchr(void const *s, i32 c, uz len)
{
    u8 *p = (u8 *)s;
    for (uz i = 0; i < len; i++) {
        if (p[i] == (u8)c) {
            return p + i;
        }
    }
    return 0;
}

extern "C" i32 memcmp(void const *a, void const *b, uz len)
{
    u8 *x = (u8 *)a;
    u8 *y = (u8 *)b;
    for (uz i = 0; i < len; i++) {
        if (x[i] != y[i]) {
            return x[i] - y[i];
        }
    }
    return 0;
}
#endif

struct Os {
    i64   freq;  // performance counter ticks per second
    void *gui;   // GUI builds: the window's platform state (gui_win32.cpp)
};

enum : iz {
    THREAD_STACK = (iz)1 << 19,
};

static i32 chunk(iz len)  // bound a single ReadFile/WriteFile
{
    i32 max = 1<<21;
    return len<max ? (i32)len : max;
}

static b32 writeall(uz h, Str s)
{
    for (iz off = 0; off < s.len;) {
        i32 len = chunk(s.len - off);
        if (!WriteFile(h, s.data+off, len, &len, 0) || !len) {
            return 0;
        }
        off += len;
    }
    return 1;
}

// FILETIME (100 ns units since 1601) to Unix epoch milliseconds.
static i64 unixms(Filetime t)
{
    u64 ticks = (u64)t.hi<<32 | t.lo;
    return (i64)(ticks/10000) - 11644473600000;
}

// True if a string cannot cross the system call boundary intact.
static b32 hasnul(Str s)
{
    return cut(s, 0).ok;
}

// Absolute, extended-length, null-terminated form of a path. Falls back
// to the plain conversion if the system rejects the path, leaving the
// error for the caller's system call to report. Unlike ssg, it sizes the
// buffer exactly, since a growable arena's committed tail may be short.
static Str16 widepath(Arena *a, Str path)
{
    Str16 rel = towide(a, path);
    i32   cap = GetFullPathNameW(rel.data, 0, 0, 0);  // with terminator
    if (cap <= 0) {
        return rel;
    }
    c16 *buf = alloc<c16>(a, (iz)cap+6);  // room for \\?\UNC before it
    i32  len = GetFullPathNameW(rel.data, cap, buf+6, 0);
    if (!len || len>=cap) {
        return rel;
    }
    return extendpath(buf, len);
}

[[noreturn]] static void os_oom()
{
    writeall(GetStdHandle(STD_ERROR_HANDLE), "harness: out of memory\n");
    ExitProcess(2);
}

// Reservations are committed on demand, a page at a time at minimum.
static Region *os_reserve(Os *, iz cap)
{
    iz    page = 1<<12;
    byte *p    = VirtualAlloc(0, cap, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) {
        return 0;
    }
    if (!VirtualAlloc((uz)p, page, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree((uz)p, 0, MEM_RELEASE);
        return 0;
    }
    iz      hdr = (iz)sizeof(Region) + (-(iz)sizeof(Region) & 63);
    Region *r   = (Region *)p;
    r->base      = p + hdr;
    r->cap       = cap - hdr;
    r->committed = page - hdr;
    return r;
}

static b32 os_commit(Region *r, iz upto)
{
    if (upto <= r->committed) {
        return 1;
    }
    iz    len = upto - r->committed;
    byte *beg = r->base + r->committed;
    if (!VirtualAlloc((uz)beg, len, MEM_COMMIT, PAGE_READWRITE)) {
        return 0;
    }
    r->committed = upto;
    return 1;
}

static void os_release(Os *, Region *r)
{
    VirtualFree((uz)r, 0, MEM_RELEASE);
}


// Time and entropy

static i64 os_ticks(Os *os)
{
    i64 count = 0;
    QueryPerformanceCounter(&count);
    return count/os->freq*1000000 + count%os->freq*1000000/os->freq;
}

static i64 os_time(Os *)
{
    Filetime ft = {};
    GetSystemTimeAsFileTime(&ft);
    return unixms(ft);
}

static u64 os_random(Os *os)
{
    u64 r = 0;
    if (SystemFunction036(&r, sizeof(r))) {
        return r;
    }
    // Last resort: weak, but still differs between calls and processes.
    r = (u64)os_time(os) ^ (u64)os_ticks(os)<<20 ^ (uz)&r;
    r *= 1111111111111111111u;
    return r ^ r>>32;
}


// Files

static b32 notfound(i32 err)
{
    switch (err) {
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_PATHNAME:
    case ERROR_FILE_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_INVALID_NAME:
    case ERROR_PATH_NOT_FOUND:
        return 1;
    }
    return 0;
}

static Stat statattr(u32 attr, Filetime mtime, u32 sizehi, u32 sizelo)
{
    Stat r = {};
    r.size  = (i64)sizehi<<32 | sizelo;
    r.mtime = unixms(mtime);
    r.type  = attr&FILE_ATTRIBUTE_DIRECTORY ? STAT_DIR   :
              attr&FILE_ATTRIBUTE_DEVICE    ? STAT_OTHER :
              STAT_FILE;
    return r;
}

// Stat a null-terminated path, following links.
static Stat statwide(c16 *p)
{
    Stat r = {};

    // Opening with no access rights follows links, and backup semantics
    // permits directories.
    uz h = CreateFileW(
        p, 0, FILE_SHARE_ALL, 0, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0
    );
    if (h != INVALID_HANDLE_VALUE) {
        FileInfo fi   = {};
        b32      ok   = GetFileInformationByHandle(h, &fi);
        i32      type = GetFileType(h);
        CloseHandle(h);
        if (ok && type==FILE_TYPE_DISK) {
            return statattr(fi.attr, fi.mtime, fi.sizehi, fi.sizelo);
        }
        r.type = STAT_OTHER;  // a device, pipe, or console
        return r;
    }

    // Some system files refuse even that: fall back to the directory
    // entry, which does not follow links. The paging file refuses this
    // too, but its sharing violation shows that the path names an entry,
    // and so is no wildcard pattern, which makes it safe to search for.
    FileAttributes fa = {};
    if (GetFileAttributesExW(p, GetFileExInfoStandard, &fa)) {
        if (!(fa.attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
            return statattr(fa.attr, fa.mtime, fa.sizehi, fa.sizelo);
        }
    } else if (GetLastError() != ERROR_SHARING_VIOLATION) {
        return r;
    }

    FindData fd = {};
    uz       f  = FindFirstFileExW(
        p, FindExInfoStandard, &fd, FindExSearchNameMatch, 0, 0
    );
    if (f == INVALID_HANDLE_VALUE) {
        return r;
    }
    FindClose(f);
    if (fd.attr&FILE_ATTRIBUTE_REPARSE_POINT &&
            fd.reparsetag&IO_REPARSE_TAG_NAME_SURROGATE) {
        // A symlink or junction that would not open has a missing or
        // unreachable target, so like stat(2) report it missing. Other
        // reparse points, such as app execution aliases, are files.
        return r;
    }
    return statattr(fd.attr, fd.mtime, fd.sizehi, fd.sizelo);
}

static Read os_read(Os *, Arena *a, Str path, iz max)
{
    Read r = {};
    if (hasnul(path)) {
        r.err = READ_NOTFOUND;
        return r;
    }

    Arena tmp = *a;
    c16  *p   = widepath(&tmp, path).data;
    uz    h   = CreateFileW(
        p, GENERIC_READ, FILE_SHARE_ALL, 0, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, 0
    );
    if (h == INVALID_HANDLE_VALUE) {
        // Opening a directory fails with ERROR_ACCESS_DENIED, and so does
        // a directory link with a missing target.
        i32 err  = GetLastError();
        i32 type = statwide(p).type;
        if (type == STAT_DIR) {
            r.err = READ_NOTFILE;
        } else if (type==STAT_MISSING || notfound(err)) {
            r.err = READ_NOTFOUND;
        } else {
            r.err = READ_IO;
        }
        return r;
    }

    i64 size = 0;
    if (GetFileType(h) != FILE_TYPE_DISK) {
        CloseHandle(h);
        r.err = READ_NOTFILE;
        return r;
    } else if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        r.err = READ_IO;
        return r;
    }
    r.size = size;
    if (size > max) {
        CloseHandle(h);
        r.err = READ_TOOBIG;
        return r;
    }

    Arena save = *a;
    Str   data = {allocbytes(a, (iz)size), 0};
    while (data.len < size) {
        i32 len = chunk((iz)size - data.len);
        if (!ReadFile(h, data.data+data.len, len, &len, 0)) {
            CloseHandle(h);
            *a = save;
            r.err = READ_IO;
            return r;
        } else if (!len) {
            break;  // shrank while reading
        }
        data.len += len;
    }
    CloseHandle(h);
    a->beg = (byte *)(data.data + data.len);
    r.data = data;
    r.size = data.len;
    return r;
}

// The temporary sibling has a random name so that concurrent writers of
// the same path never share one, and it reaches the disk before the
// rename makes it visible.
static b32 os_write(Os *os, Arena scratch, Str path, Str data)
{
    if (hasnul(path)) {
        return 0;
    }
    Buf b(&scratch, path.len+32);
    print(&b, path);
    putbyte(&b, '.');
    printhex(&b, os_random(os), 16);
    print(&b, ".tmp");
    c16 *tmp = widepath(&scratch, finish(&b)).data;
    c16 *dst = widepath(&scratch, path).data;

    uz h = CreateFileW(
        tmp, GENERIC_WRITE, 0, 0, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, 0
    );
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    i32 move = MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH;
    b32 ok   = writeall(h, data) && FlushFileBuffers(h);
    ok &= !!CloseHandle(h);
    ok = ok && MoveFileExW(tmp, dst, move);
    if (!ok) {
        DeleteFileW(tmp);
    }
    return ok;
}

static b32 os_mkdirs(Os *, Arena scratch, Str path)
{
    if (!path.len || hasnul(path)) {
        return 0;
    }
    Str16 p    = widepath(&scratch, path);
    c16  *s    = p.data;
    iz    root = rootlen(p);

    // Walk up to the nearest existing ancestor, usually the path itself...
    iz end = p.len;
    while (end > root) {
        c16 save = s[end];
        s[end] = 0;
        b32 exists = GetFileAttributesW(s) != INVALID_FILE_ATTRIBUTES;
        s[end] = save;
        if (exists) {
            break;
        }
        for (end--; end>root && s[end]!='\\' && s[end]!='/'; end--) {}
    }

    // ...then create the missing directories on the way back down.
    while (end < p.len) {
        iz next = end + 1;
        for (; next<p.len && s[next]!='\\' && s[next]!='/'; next++) {}
        c16 save = s[next];
        s[next] = 0;
        b32 ok = CreateDirectoryW(s, 0) ||
                 GetLastError()==ERROR_ALREADY_EXISTS;
        s[next] = save;
        if (!ok) {
            return 0;
        }
        end = next;
    }

    // Like mkdir -p, a non-directory in the way is an error, including a
    // link to a missing directory.
    return statwide(s).type == STAT_DIR;
}

static Dirent *os_list(Os *, Arena *a, Str path)
{
    if (!path.len || hasnul(path)) {
        return 0;  // fail like opendir(""), not list \* (the drive root)
    }

    Arena tmp = *a;
    Str16 dir = widepath(&tmp, path);
    c16  *pat = alloc<c16>(&tmp, dir.len+3);
    iz    len = dir.len;
    copybytes(pat, dir.data, len*(iz)sizeof(c16));
    if (!len || (pat[len-1]!='\\' && pat[len-1]!='/')) {
        pat[len++] = '\\';
    }
    pat[len] = '*';

    FindData fd = {};  // not in the arena: entries are allocated there
    uz h = FindFirstFileExW(
        pat, FindExInfoBasic, &fd, FindExSearchNameMatch, 0,
        FIND_FIRST_EX_LARGE_FETCH
    );
    if (h==INVALID_HANDLE_VALUE && GetLastError()==ERROR_INVALID_PARAMETER) {
        // Before Windows 7: no basic info level or large fetch
        h = FindFirstFileExW(
            pat, FindExInfoStandard, &fd, FindExSearchNameMatch, 0, 0
        );
    }
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }

    Dirent *head = 0;
    do {
        c16 *name = fd.name;
        if (name[0]=='.' && (!name[1] || (name[1]=='.' && !name[2]))) {
            continue;
        }
        if (fd.attr & FILE_ATTRIBUTE_DEVICE) {
            continue;
        }
        if (fd.attr&FILE_ATTRIBUTE_REPARSE_POINT &&
                fd.reparsetag&IO_REPARSE_TAG_NAME_SURROGATE) {
            continue;
        }
        iz namelen = 0;
        for (; namelen<countof(fd.name) && name[namelen]; namelen++) {}
        Dirent *d = alloc<Dirent>(a);
        d->name  = fromwide(a, name, namelen);
        d->size  = (i64)fd.sizehi<<32 | fd.sizelo;
        d->mtime = unixms(fd.mtime);
        d->isdir = !!(fd.attr & FILE_ATTRIBUTE_DIRECTORY);
        d->next  = head;
        head = d;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return head;
}

static Stat os_stat(Os *, Arena scratch, Str path)
{
    if (hasnul(path)) {
        return {};
    }
    return statwide(widepath(&scratch, path).data);
}

static b32 os_rename(Os *, Arena scratch, Str from, Str to)
{
    if (hasnul(from) || hasnul(to)) {
        return 0;
    }
    c16 *src = widepath(&scratch, from).data;
    c16 *dst = widepath(&scratch, to).data;
    return MoveFileExW(src, dst, MOVEFILE_REPLACE_EXISTING);
}

// Also removes an empty directory, like remove(3).
static b32 os_remove(Os *, Arena scratch, Str path)
{
    if (hasnul(path)) {
        return 0;
    }
    c16 *p = widepath(&scratch, path).data;
    if (DeleteFileW(p)) {
        return 1;
    }
    i32 attr = GetFileAttributesW(p);
    return attr!=INVALID_FILE_ATTRIBUTES && attr&FILE_ATTRIBUTE_DIRECTORY &&
           RemoveDirectoryW(p);
}

static b32 os_print(Os *, i32 fd, Str s)
{
    uz  h    = GetStdHandle(-10 - fd);
    i32 mode = 0;
    if (!GetConsoleMode(h, &mode)) {
        return writeall(h, s);
    }

    // Consoles display UTF-16 correctly regardless of code page.
    c16 buf[512];
    i32 len = 0;
    for (iz i = 0; i < s.len;) {
        len += (i32)utf16encode(buf+len, utf8decode(s, &i));
        if (len>countof(buf)-2 || i==s.len) {
            i32 dummy;
            if (!WriteConsoleW(h, buf, len, &dummy, 0)) {
                return 0;
            }
            len = 0;
        }
    }
    return 1;
}


// Process environment
//
// Results are converted into space reserved at the front of the arena
// before the UTF-16 scratch behind it, then the unused tail is returned.

static Str os_getenv(Os *, Arena *a, Str name)
{
    if (hasnul(name)) {
        return {};
    }
    for (;;) {
        Arena tmp   = *a;
        c16  *wname = towide(&tmp, name).data;
        i32   cap   = GetEnvironmentVariableW(wname, 0, 0);  // with terminator
        if (!cap) {
            return {};
        }

        u8 *out = allocbytes(a, 3*(iz)cap);
        tmp   = *a;
        wname = towide(&tmp, name).data;
        c16 *buf = alloc<c16>(&tmp, cap);
        SetLastError(0);
        i32 len = GetEnvironmentVariableW(wname, buf, cap);
        if (!len && GetLastError()) {
            a->beg = (byte *)out;
            return {};  // removed meanwhile
        } else if (len < cap) {
            Arena o = {(byte *)out, (byte *)out + 3*(iz)cap};
            Str   r = fromwide(&o, buf, len);  // non-null even when empty
            a->beg = (byte *)(r.data + r.len);
            return r;
        }
        a->beg = (byte *)out;  // grew meanwhile: retry
    }
}

static Str os_exepath(Os *, Arena *a)
{
    for (i32 cap = 260; cap <= 1<<16; cap *= 4) {
        u8   *out = allocbytes(a, 3*(iz)cap);
        Arena tmp = *a;
        c16  *buf = alloc<c16>(&tmp, cap);
        i32   len = GetModuleFileNameW(0, buf, cap);
        if (len>0 && len<cap) {
            Arena o = {(byte *)out, (byte *)out + 3*(iz)cap};
            Str   r = fromwide(&o, buf, len);
            a->beg = (byte *)(r.data + r.len);
            return r;
        }
        a->beg = (byte *)out;
        if (!len) {
            break;
        }
    }
    return {};
}


// Threads, locks, and events

struct ThreadStart {
    void (*fn)(void *);
    void  *arg;
};

// The i686 ABI only promises 4-byte stack alignment at entry.
__attribute((force_align_arg_pointer))
static u32 __stdcall threadentry(void *p)
{
    ThreadStart s = *(ThreadStart *)p;
    HeapFree(GetProcessHeap(), 0, p);
    s.fn(s.arg);
    return 0;
}

// The start record is the one heap allocation in this layer: it must
// outlive this call, and the new thread frees it.
static b32 os_thread(Os *, void (*fn)(void *), void *arg)
{
    uz           heap = GetProcessHeap();
    ThreadStart *s    = (ThreadStart *)HeapAlloc(heap, 0, sizeof(ThreadStart));
    if (!s) {
        return 0;
    }
    s->fn  = fn;
    s->arg = arg;
    uz h = CreateThread(
        0, THREAD_STACK, threadentry, s, STACK_SIZE_PARAM_IS_A_RESERVATION, 0
    );
    if (!h) {
        HeapFree(heap, 0, s);
        return 0;
    }
    CloseHandle(h);
    return 1;
}

static void os_lockinit(Lock *l)
{
    InitializeCriticalSectionAndSpinCount((CriticalSection *)l->opaque, 4000);
}

static void os_lock(Lock *l)
{
    EnterCriticalSection((CriticalSection *)l->opaque);
}

static void os_unlock(Lock *l)
{
    LeaveCriticalSection((CriticalSection *)l->opaque);
}

// An auto-reset event handle in h[0].
static b32 os_eventinit(Event *e)
{
    e->h[0] = (iz)CreateEventW(0, 0, 0, 0);
    e->h[1] = 0;
    return e->h[0] != 0;
}

static void os_signal(Event *e)
{
    SetEvent((uz)e->h[0]);
}

static b32 os_wait(Event *e, i32 ms)
{
    i32 r = WaitForSingleObject((uz)e->h[0], ms<0 ? INFINITE : ms);
    return r == WAIT_OBJECT_0;
}


// Append-only logs: handles are file handles
//
// Truncation needs write access (SetEndOfFile), which FILE_APPEND_DATA
// alone lacks, so logs open with GENERIC_WRITE and get their append
// semantics per write instead: an offset of 0xffffffff:ffffffff writes
// at the current end of file atomically, as FILE_APPEND_DATA would.

static iz os_logopen(Os *, Arena scratch, Str path)
{
    if (hasnul(path)) {
        return -1;
    }
    uz h = CreateFileW(
        widepath(&scratch, path).data, GENERIC_WRITE, FILE_SHARE_ALL, 0,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0
    );
    return (iz)h;  // INVALID_HANDLE_VALUE is -1
}

static b32 os_logwrite(Os *, iz log, Str record)
{
    for (iz off = 0; off < record.len;) {
        iz         max = (iz)1 << 30;
        i32        len = record.len-off<max ? (i32)(record.len-off) : (i32)max;
        Overlapped ov  = {};
        ov.offlo = 0xffffffff;
        ov.offhi = 0xffffffff;
        if (!WriteFile((uz)log, record.data+off, len, &len, &ov) || !len) {
            return 0;
        }
        off += len;
    }
    return 1;
}

static b32 os_logsync(Os *, iz log)
{
    return FlushFileBuffers((uz)log);
}

static b32 os_logtrunc(Os *, iz log, i64 len)
{
    return SetFilePointerEx((uz)log, len, 0, FILE_BEGIN) &&
           SetEndOfFile((uz)log);
}

static void os_logclose(Os *, iz log)
{
    CloseHandle((uz)log);
}


#if HARNESS_GUI
#  include "app.cpp"
#  include "gui_win32.cpp"
#endif


#if HARNESS_GUI
// GUI builds start in the CRT, whose wWinMain (gui_win32.cpp) calls this.
extern "C" void harness_main()
#else
extern "C" __attribute((force_align_arg_pointer))
void mainCRTStartup()
#endif
{
    Os os = {};
    QueryPerformanceFrequency(&os.freq);

    iz      cap = sizeof(void *)==8 ? (iz)1<<34 : (iz)1<<28;
    Region *r   = os_reserve(&os, cap);
    if (!r) {
        os_oom();
    }
    Arena a = newarena(r);

    c16 *cmd = GetCommandLineW();
    iz   len = 0;
    for (; cmd[len]; len++) {}
    Slice<Str> args = splitcmdline(&a, fromwide(&a, cmd, len));

    #if HARNESS_GUI
    if (guiwanted(args.data, (i32)args.len)) {
        ExitProcess(gui_main(&os, a, args.data, (i32)args.len));
    }
    #endif
    ExitProcess(app_main(&os, a, args.data, (i32)args.len));
}
