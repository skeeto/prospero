// Win32 platform layer: no CRT and no headers
// $ cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-Mingw64.cmake
// $ c++ -std=c++20 -nostartfiles -o harness.exe src/main_win32.cpp -ladvapi32
//
// Imports only kernel32.dll, advapi32.dll (RtlGenRandom), and winhttp.dll,
// with nothing newer than Windows XP SP3, so the XP engine, an i686 build
// with HARNESS_GUI off, comes from this file too. tests/imports.py checks
// the imports against XP's own export lists. Newer functions are looked
// up at run time (GetProcAddress), and newer options tried first with a
// fallback (the automatic proxy). On XP, https cannot work, since its
// WinHTTP has no TLS 1.2, and tools leave the harness's own job when it
// has one, since jobs do not nest there (jobcheck).
//
// Each tool process runs in its own job object that kills the whole tree
// when its last handle closes, which includes this process dying by any
// means. HTTP goes through WinHTTP in synchronous mode, one thread per
// request, cancelled by closing the request handle from another thread.
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
    FILE_TYPE_CHAR                    = 2,
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
    // Processes
    CREATE_NEW_PROCESS_GROUP          = 0x00000200,
    CREATE_NO_WINDOW                  = 0x08000000,
    CREATE_SUSPENDED                  = 0x00000004,
    CREATE_UNICODE_ENVIRONMENT        = 0x00000400,
    CREATE_ALWAYS                     = 2,
    ERROR_BROKEN_PIPE                 = 109,
    ERROR_NOT_ENOUGH_MEMORY           = 8,
    FORMAT_MESSAGE_FROM_HMODULE       = 0x00000800,
    FORMAT_MESSAGE_FROM_SYSTEM        = 0x00001000,
    FORMAT_MESSAGE_IGNORE_INSERTS     = 0x00000200,
    HANDLE_FLAG_INHERIT               = 1,
    HEAP_ZERO_MEMORY                  = 8,
    JOB_KILL_ON_JOB_CLOSE             = 0x2000,
    JOB_DIE_ON_UNHANDLED_EXCEPTION    = 0x400,
    JobObjectExtendedLimitInformation = 9,
    SEM_FAILCRITICALERRORS            = 0x0001,
    SEM_NOGPFAULTERRORBOX             = 0x0002,
    SEM_NOOPENFILEERRORBOX            = 0x8000,
    STARTF_USESTDHANDLES              = 0x100,
    STD_INPUT_HANDLE                  = -10,
    STD_OUTPUT_HANDLE                 = -11,
    ATTACH_PARENT_PROCESS             = -1,
    CTRL_BREAK_EVENT                  = 1,
    // WinHTTP
    ERROR_WINHTTP_OPERATION_CANCELLED = 12017,
    WINHTTP_ACCESS_TYPE_AUTOMATIC     = 4,
    WINHTTP_ACCESS_TYPE_DEFAULT       = 0,
    WINHTTP_ACCESS_TYPE_NO_PROXY      = 1,
    WINHTTP_FLAG_SECURE               = 0x00800000,
    WINHTTP_OPTION_PROXY              = 38,
    WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT = 7,
    WINHTTP_QUERY_FLAG_NUMBER         = 0x20000000,
    WINHTTP_QUERY_RAW_HEADERS_CRLF    = 22,
    WINHTTP_QUERY_STATUS_CODE         = 19,
    // Windows XP compatibility
    CREATE_BREAKAWAY_FROM_JOB         = 0x01000000,
    ERROR_NOTLS12                     = 0x20000001,  // ours (bit 29)
    JOB_BREAKAWAY_OK                  = 0x800,
    JOB_SILENT_BREAKAWAY_OK           = 0x1000,
    WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 = 0x800,
    WINHTTP_OPTION_SECURE_PROTOCOLS   = 84,
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

struct StartupInfo {  // STARTUPINFOW
    i32  cb;
    c16 *reserved;
    c16 *desktop;
    c16 *title;
    i32  x, y, xsize, ysize, xchars, ychars, fill;
    i32  flags;
    c16  show;
    c16  reserved2;
    u8  *reserved3;
    uz   input;
    uz   output;
    uz   error;
};

struct ProcessInfo {  // PROCESS_INFORMATION
    uz  process;
    uz  thread;
    i32 pid;
    i32 tid;
};

struct JobLimits {  // JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    i64 proctime;
    i64 jobtime;
    u32 flags;
    uz  minws;
    uz  maxws;
    u32 activeprocs;
    uz  affinity;
    u32 priority;
    u32 scheduling;
    u64 io[6];
    uz  procmem;
    uz  jobmem;
    uz  peakprocmem;
    uz  peakjobmem;
};
static_assert(sizeof(JobLimits) == (sizeof(uz)==8 ? 144 : 112));

struct ProxyInfo {  // WINHTTP_PROXY_INFO
    u32  access;
    c16 *proxy;
    c16 *bypass;
};

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
W32(void,   DeleteCriticalSection(CriticalSection *));
W32(void *, GetProcAddress(uz, char *));
W32(void,   Sleep(i32));
W32(b32,    AttachConsole(i32));
W32(b32,    FreeConsole());
W32(b32,    GenerateConsoleCtrlEvent(i32, i32));
W32(b32,    SetConsoleCtrlHandler(uz, b32));
W32(b32,    SetStdHandle(i32, uz));
W32(b32,    AssignProcessToJobObject(uz, uz));
W32(uz,     CreateJobObjectW(uz, c16 *));
W32(b32,    CreatePipe(uz *, uz *, uz, i32));
W32(b32,    CreateProcessW(c16 *, c16 *, uz, uz, b32, i32, c16 *, c16 *,
                           StartupInfo *, ProcessInfo *));
W32(i32,    FormatMessageW(i32, uz, i32, i32, c16 *, i32, uz));
W32(b32,    FreeEnvironmentStringsW(c16 *));
W32(c16 *,  GetEnvironmentStringsW());
W32(b32,    GetExitCodeProcess(uz, i32 *));
W32(uz,     GetModuleHandleW(c16 *));
W32(i32,    ResumeThread(uz));
W32(i32,    SetErrorMode(i32));
W32(b32,    SetHandleInformation(uz, i32, i32));
W32(b32,    SetInformationJobObject(uz, i32, void *, i32));
W32(b32,    TerminateJobObject(uz, i32));
W32(b32,    TerminateProcess(uz, i32));
W32(b32,    WinHttpCloseHandle(uz));
W32(uz,     WinHttpConnect(uz, c16 *, c16, i32));  // INTERNET_PORT
W32(uz,     WinHttpOpen(c16 *, i32, c16 *, c16 *, i32));
W32(uz,     WinHttpOpenRequest(uz, c16 *, c16 *, c16 *, c16 *, c16 **, i32));
W32(b32,    WinHttpQueryHeaders(uz, i32, c16 *, void *, i32 *, i32 *));
W32(b32,    WinHttpQueryDataAvailable(uz, i32 *));
W32(b32,    WinHttpReadData(uz, u8 *, i32, i32 *));
W32(b32,    WinHttpReceiveResponse(uz, uz));
W32(b32,    WinHttpSendRequest(uz, c16 *, i32, void *, i32, i32, uz));
W32(b32,    WinHttpSetOption(uz, i32, void *, i32));
W32(b32,    WinHttpSetTimeouts(uz, i32, i32, i32, i32));
// Windows XP compatibility
W32(uz,     GetCurrentProcess());
W32(u32,    GetVersion());
W32(b32,    IsProcessInJob(uz, uz, b32 *));
W32(b32,    QueryInformationJobObject(uz, i32, void *, i32, i32 *));

// GCC emits calls to these even without a CRT. Defining them keeps
// msvcrt.dll out of the imports. GUI builds have the CRT's.
#if !HARNESS_GUI && (defined(__x86_64__) || defined(__i386__))
extern "C" void *memcpy(void *dst, void const *src, uz len)
{
    void *r = dst;
    __asm volatile ("rep movsb" : "+D"(dst), "+S"(src), "+c"(len) : : "memory");
    return r;
}

extern "C" void *memmove(void *dst, void const *src, uz len)
{
    void *r = dst;
    if ((uz)dst-(uz)src >= len) {  // forward is safe
        __asm volatile ("rep movsb" : "+D"(dst), "+S"(src), "+c"(len) : : "memory");
    } else {
        dst = (u8 *)dst + len - 1;
        src = (u8 *)src + len - 1;
        __asm volatile ("std; rep movsb; cld"
                        : "+D"(dst), "+S"(src), "+c"(len) : : "memory");
    }
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
    i64             freq;   // performance counter ticks per second
    CriticalSection spawn;  // held while child handles are inheritable
    CriticalSection console;  // held while attached to a child's console
    uz              std[3];   // standard handles, as at startup
    b32             stdinread;  // os_stdin was called: a reader may block
    void           *gui;    // GUI builds: the window's state (gui_win32.cpp)
    // GetFinalPathNameByHandleW, from Vista on, else null
    u32 (__stdcall *finalpath)(uz, c16 *, u32, u32);
    i32             breakaway;  // a creation flag: tools leave our job
    b32             jobless;    // tools run without jobs (jobcheck)
    i32             warned;     // atomic: jobless was reported
    i32             tls12;      // atomic: WinHTTP has TLS 1.2 (1) or not (-1)
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

// Consoles display UTF-16 correctly regardless of code page.
static b32 writeconsole(uz h, Str s)
{
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

// Standard handles come from startup, since stopping a child (ctrlbreak)
// changes those of the process for a moment.
static b32 os_print(Os *os, i32 fd, Str s)
{
    uz h = os->std[fd];
    if (GetFileType(h) != FILE_TYPE_CHAR) {
        return writeall(h, s);
    }
    // A console, which this process leaves for a moment to stop a child,
    // and whose handle it then replaces
    EnterCriticalSection(&os->console);
    h = os->std[fd];
    i32 mode = 0;
    b32 ok   = GetConsoleMode(h, &mode) ? writeconsole(h, s) : writeall(h, s);
    LeaveCriticalSection(&os->console);
    return ok;
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


// More files and the environment

static Read os_readat(Os *, Arena *a, Str path, i64 off, iz max)
{
    Read r = {};
    if (hasnul(path) || off<0 || max<0) {
        r.err = READ_NOTFOUND;
        return r;
    }
    Arena tmp = *a;
    uz    h   = CreateFileW(
        widepath(&tmp, path).data, GENERIC_READ, FILE_SHARE_ALL, 0,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0
    );
    if (h == INVALID_HANDLE_VALUE) {
        r.err = notfound(GetLastError()) ? READ_NOTFOUND : READ_IO;
        return r;
    }
    i64 size = 0;
    if (GetFileType(h)!=FILE_TYPE_DISK || !GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        r.err = READ_NOTFILE;
        return r;
    }
    r.size = size;
    i64 avail = size>off ? size-off : 0;
    iz  want  = avail<max ? (iz)avail : max;
    if (want && !SetFilePointerEx(h, off, 0, FILE_BEGIN)) {
        CloseHandle(h);
        r.err = READ_IO;
        return r;
    }

    Str data = {allocbytes(a, want), 0};
    while (data.len < want) {
        i32 len = chunk(want - data.len);
        if (!ReadFile(h, data.data+data.len, len, &len, 0)) {
            CloseHandle(h);
            r.err = READ_IO;
            return r;
        } else if (!len) {
            break;
        }
        data.len += len;
    }
    CloseHandle(h);
    a->beg = (byte *)(data.data + data.len);
    r.data = data;
    return r;
}

static Slice<Str> os_environ(Os *, Arena *a)
{
    Slice<Str> r   = {};
    c16       *env = GetEnvironmentStringsW();
    if (!env) {
        return r;
    }
    for (c16 *p = env; *p;) {
        iz len = 0;
        for (; p[len]; len++) {}
        r = push(a, r, fromwide(a, p, len));
        p += len + 1;
    }
    FreeEnvironmentStringsW(env);
    return r;
}

static iz os_stdin(Os *os, u8 *buf, iz len)
{
    __atomic_store_n(&os->stdinread, 1, __ATOMIC_RELAXED);
    i32 n = 0;
    if (!ReadFile(os->std[0], buf, chunk(len), &n, 0)) {
        return GetLastError()==ERROR_BROKEN_PIPE ? 0 : -1;
    }
    return n;
}

// Links and junctions are resolved by opening the path, which follows
// them, then asking the handle for its name (Vista and later). Where
// that cannot be done, the path is only made absolute, as by
// GetFullPathNameW. Results drop the \\?\ prefix: "C:\x", "\\h\s\x".
static Str os_realpath(Os *os, Arena *a, Str path)
{
    if (hasnul(path)) {
        return {};
    }
    Arena tmp = *a;
    c16  *rel = towide(&tmp, path).data;
    i32   cap = GetFullPathNameW(rel, 0, 0, 0);
    if (cap <= 0) {
        return {};
    }
    c16 *buf  = alloc<c16>(&tmp, cap);
    i32  len  = GetFullPathNameW(rel, cap, buf, 0);
    c16 *full = widepath(&tmp, path).data;
    if (!len || len>=cap || GetFileAttributesW(full)==INVALID_FILE_ATTRIBUTES) {
        return {};
    }

    uz h = CreateFileW(
        full, 0, FILE_SHARE_ALL, 0, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0
    );
    if (h == INVALID_HANDLE_VALUE) {
        if (notfound(GetLastError())) {
            return {};  // a link to something missing
        }
    } else if (os->finalpath) {
        u32  need = os->finalpath(h, 0, 0, 0);  // with terminator
        c16 *fin  = need ? alloc<c16>(&tmp, need) : 0;
        u32  got  = fin ? os->finalpath(h, fin, need, 0) : 0;
        if (got && got<need) {
            buf = fin;
            len = (i32)got;
            if (len>=8 && buf[2]=='?' && (buf[4]|0x20)=='u' &&
                    (buf[5]|0x20)=='n' && (buf[6]|0x20)=='c' && buf[7]=='\\') {
                buf += 6;  // \\?\UNC\h\s as \\h\s
                len -= 6;
                buf[0] = '\\';
            } else if (len>=4 && buf[2]=='?') {
                buf += 4;  // \\?\C:\x as C:\x
                len -= 4;
            }
        }
    }
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
    }
    for (; len>3 && (buf[len-1]=='\\' || buf[len-1]=='/'); len--) {}

    // Converted behind the scratch, then moved down to the front
    Str r = fromwide(&tmp, buf, len);
    u8 *out = allocbytes(a, r.len);
    __builtin_memmove(out, r.data, (uz)r.len);
    return {out, r.len};
}

// Session locks: the file held open with no sharing, which the system
// closes when the process ends. A test opens it for reading with full
// sharing, so a lock that fails where such an open succeeds met only a
// test, and tries again.
static iz os_lockfile(Os *, Arena scratch, Str path)
{
    if (hasnul(path)) {
        return -1;
    }
    c16 *p = widepath(&scratch, path).data;
    for (i32 n = 0; n < 200; n++) {
        uz h = CreateFileW(
            p, GENERIC_READ|GENERIC_WRITE, 0, 0, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, 0
        );
        if (h != INVALID_HANDLE_VALUE) {
            return (iz)h;
        } else if (GetLastError() != ERROR_SHARING_VIOLATION) {
            return -1;
        }
        uz t = CreateFileW(
            p, GENERIC_READ, FILE_SHARE_ALL, 0, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, 0
        );
        if (t == INVALID_HANDLE_VALUE) {
            return -1;  // held for real
        }
        CloseHandle(t);
        Sleep(1);
    }
    return -1;
}

static void os_unlockfile(Os *, iz lock)
{
    CloseHandle((uz)lock);
}

static b32 os_lockheld(Os *, Arena scratch, Str path)
{
    if (hasnul(path)) {
        return 0;
    }
    uz h = CreateFileW(
        widepath(&scratch, path).data, GENERIC_READ, FILE_SHARE_ALL, 0,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0
    );
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return 0;
    }
    return GetLastError() == ERROR_SHARING_VIOLATION;
}

static Str os_lasterr(Os *, Arena *a)
{
    i32 err = GetLastError();
    if (err == ERROR_NOTLS12) {
        return "this system's WinHTTP has no TLS 1.2 (Windows XP), which "
               "https endpoints need: use a plain http:// endpoint, such as "
               "a server on the LAN";
    }
    c16 buf[512];
    i32 flags = FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS;
    uz  mod   = 0;
    if (err>=12000 && err<12200) {
        mod    = GetModuleHandleW((c16 *)u"winhttp.dll");
        flags |= mod ? FORMAT_MESSAGE_FROM_HMODULE : 0;
    }
    i32 len = FormatMessageW(flags, mod, err, 0, buf, countof(buf), 0);
    for (; len>0 && (buf[len-1]=='\r' || buf[len-1]=='\n' || buf[len-1]=='.'); len--) {}
    if (len <= 0) {
        Buf b(a, 32);
        print(&b, "error ");
        print(&b, (i64)err);
        return finish(&b);
    }
    return fromwide(a, buf, len);
}


// Processes
//
// Each spawn gets a job set to kill every process in it when its last
// handle closes, and to die on an unhandled exception rather than show
// a dialog. The child starts suspended, joins the job, then resumes, so
// it cannot start anything outside it first. Its standard handles (NUL
// for input, one pipe for output and errors) are inheritable only
// inside the spawn lock, so no other spawn picks them up.
//
// Each Proc has a reader thread, pumping output into the file, and a
// waiter thread on the process handle. Both hold references, as does
// the engine until os_procfree. The job closes with the last one,
// killing whatever is left. A child from os_spawnio has a pipe for its
// standard input and a writer thread feeding it from a queue, with a
// reference of its own; its reader passes output on as records instead
// of into a file, and its errors go into the file directly.
//
// Before Windows 8 jobs do not nest: a process in a job, such as one an
// SSH server started, cannot put its children in jobs of their own
// (jobcheck). Where its job lets children leave, each tool leaves it for
// its own. That beats relaunching the harness outside the job (the
// user's sshd's escapejob): the harness stays in the job it started in,
// so it still ends with that session, and its tools with it. Where the
// job keeps its children, tools run without jobs, with a warning: a hard
// kill ends only the tool's own process, and nothing ends it with ours.

enum : iz {
    PROC_TAILMAX  = 1<<12,
    PROC_BUFLEN   = 1<<16,
    PROC_QUEUEMAX = (iz)64 << 20,  // bytes waiting for a child's input
};

struct ProcChunk {  // queued input, its bytes following
    ProcChunk *next;
    iz         len;
};

struct Proc {
    i32        refs;      // atomic
    uz         job;
    i32        pid;       // the root, leader of its console process group
    uz         process;   // owned by the waiter until the end
    uz         pipe;      // output read end, owned by the reader
    uz         file;      // the output file, owned by the reader
    Mailbox   *mbox;
    u64        id;
    i64        maxbytes;
    i64        total;     // reader only
    iz         tailpos;   // reader only
    // os_spawnio
    b32        io;
    Lock       lock;
    uz         input;     // input pipe write end, owned by the writer
    Event      wake;      // for the writer
    ProcChunk *head;      // under lock: the input queue
    ProcChunk *last;      // under lock
    iz         queued;    // under lock: bytes in the queue
    b32        closing;   // under lock: close the input after the queue
    b32        broken;    // under lock: the input is closed
    u8         tail[PROC_TAILMAX];
    u8         msg[8+PROC_TAILMAX];
    u8         buf[PROC_BUFLEN];
};

// How tools get jobs, decided once at startup: Windows 8 and later nest
// them; before, a process in a job needs its children to leave it.
static void jobcheck(Os *os)
{
    u32 ver   = GetVersion();
    u32 major = ver & 0xff;
    u32 minor = ver>>8 & 0xff;
    b32 injob = 0;
    if (major>6 || (major==6 && minor>=2)) {
        return;
    } else if (!IsProcessInJob(GetCurrentProcess(), 0, &injob) || !injob) {
        return;
    }
    JobLimits lim = {};  // a null handle asks about our own job
    if (QueryInformationJobObject(0, JobObjectExtendedLimitInformation,
                                  &lim, sizeof(lim), 0)) {
        if (lim.flags & JOB_SILENT_BREAKAWAY_OK) {
            return;  // children are outside it already
        } else if (lim.flags & JOB_BREAKAWAY_OK) {
            os->breakaway = CREATE_BREAKAWAY_FROM_JOB;
            return;
        }
    }
    os->jobless = 1;
}

// Said once, at the first tool run without a job.
static void joblesswarn(Os *os)
{
    if (!__atomic_exchange_n(&os->warned, 1, __ATOMIC_RELAXED)) {
        os_print(os, 2, "harness: warning: this process is in a job that "
                        "keeps its children, and before Windows 8 jobs do "
                        "not nest, so tools run without jobs: a tool's own "
                        "children may outlive it\n");
    }
}

// Kill the whole tree, or without a job only the process itself.
static void killproc(uz job, uz process)
{
    if (job) {
        TerminateJobObject(job, 1);
    } else {
        TerminateProcess(process, 1);
    }
}

static void procrelease(Proc *p)
{
    if (!__atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL)) {
        if (p->job) {
            CloseHandle(p->job);
        }
        CloseHandle(p->process);
        if (p->io) {
            DeleteCriticalSection((CriticalSection *)p->lock.opaque);
        }
        HeapFree(GetProcessHeap(), 0, p);
    }
}

static void put64(u8 *dst, i64 v)
{
    __builtin_memcpy(dst, &v, 8);
}

static void put32(u8 *dst, i32 v)
{
    __builtin_memcpy(dst, &v, 4);
}

static void procreader(void *arg)
{
    Proc *p    = (Proc *)arg;
    i64   last = 0;
    b32   kept = 1;  // still writing to the file
    Os    os   = {};
    QueryPerformanceFrequency(&os.freq);
    for (;;) {
        i32 n = 0;
        if (!ReadFile(p->pipe, p->buf, PROC_BUFLEN, &n, 0) || !n) {
            break;
        }
        if (kept) {
            iz keep = n;
            if (p->total+n > p->maxbytes) {
                keep = (iz)(p->maxbytes - p->total);
                kept = 0;
            }
            writeall(p->file, {p->buf, keep});
            if (!kept) {
                u8 msg[8];
                put64(msg, p->total + keep);
                mailpost(p->mbox, PROC_LIMIT, p->id, {msg, 8}, 1);
            }
        }
        p->total += n;

        for (iz i = n<PROC_TAILMAX ? 0 : n-PROC_TAILMAX; i < n; i++) {
            p->tail[p->tailpos++ & (PROC_TAILMAX-1)] = p->buf[i];
        }
        i64 now = os_ticks(&os);
        if (now-last >= 50000) {
            last = now;
            iz len = p->total<PROC_TAILMAX ? (iz)p->total : PROC_TAILMAX;
            put64(p->msg, p->total);
            for (iz i = 0; i < len; i++) {
                p->msg[8+i] = p->tail[(p->tailpos - len + i) & (PROC_TAILMAX-1)];
            }
            mailpost(p->mbox, PROC_TAIL, p->id, {p->msg, 8+len}, 0);
        }
    }
    CloseHandle(p->pipe);
    CloseHandle(p->file);
    u8 msg[8];
    put64(msg, p->total);
    mailpost(p->mbox, PROC_EOF, p->id, {msg, 8}, 1);
    procrelease(p);
}

// The reader of a child from os_spawnio: output as records, in order,
// waiting for room in the mailbox.
static void procpipe(void *arg)
{
    Proc *p   = (Proc *)arg;
    iz    max = mailmax(p->mbox);
    for (;;) {
        i32 n = 0;
        if (!ReadFile(p->pipe, p->buf, PROC_BUFLEN, &n, 0) || !n) {
            break;
        }
        for (iz off = 0; off < n; off += max) {
            iz len = n-off<max ? n-off : max;
            mailpost(p->mbox, PROC_DATA, p->id, {p->buf+off, len}, 1);
        }
        p->total += n;
    }
    CloseHandle(p->pipe);
    u8 msg[8];
    put64(msg, p->total);
    mailpost(p->mbox, PROC_EOF, p->id, {msg, 8}, 1);
    procrelease(p);
}

static void freequeue(Proc *p)  // under lock
{
    for (ProcChunk *c = p->head; c;) {
        ProcChunk *next = c->next;
        HeapFree(GetProcessHeap(), 0, c);
        c = next;
    }
    p->head   = 0;
    p->last   = 0;
    p->queued = 0;
}

// Feed queued input to the child until told to close it, or until the
// child stops reading (a broken pipe). Signals to wake it come only
// under the lock while neither closing nor broken, so once it sees
// either, nobody touches its event again and it may close it.
static void procwriter(void *arg)
{
    Proc *p = (Proc *)arg;
    for (;;) {
        os_lock(&p->lock);
        ProcChunk *c = p->head;
        if (c) {
            p->head = c->next;
            p->last = p->head ? p->last : 0;
        }
        b32 done = !c && p->closing;
        os_unlock(&p->lock);
        if (done) {
            break;
        } else if (!c) {
            os_wait(&p->wake, -1);
            continue;
        }

        b32 ok = writeall(p->input, {(u8 *)(c + 1), c->len});
        os_lock(&p->lock);
        p->queued -= c->len;
        if (!ok) {
            p->broken = 1;
            freequeue(p);
        }
        os_unlock(&p->lock);
        HeapFree(GetProcessHeap(), 0, c);
        if (!ok) {
            break;
        }
    }
    CloseHandle(p->input);
    CloseHandle((uz)p->wake.h[0]);
    procrelease(p);
}

static void procwaiter(void *arg)
{
    Proc *p    = (Proc *)arg;
    i32   code = -1;
    WaitForSingleObject(p->process, INFINITE);
    GetExitCodeProcess(p->process, &code);
    u8 msg[8];
    put32(msg, code);
    put32(msg+4, 0);
    mailpost(p->mbox, PROC_EXIT, p->id, {msg, 8}, 1);
    procrelease(p);
}

static b32 envless(Str *a, Str *b)
{
    // Sorted by name, ignoring case, as CreateProcess expects
    for (iz i = 0;; i++) {
        u8 x = i<a->len && (*a)[i]!='=' ? lowercase((*a)[i]) : 0;
        u8 y = i<b->len && (*b)[i]!='=' ? lowercase((*b)[i]) : 0;
        if (x != y) return x < y;
        if (!x) return 0;
    }
}

// A double-null-terminated UTF-16 environment block.
static c16 *envblock(Arena *a, Slice<Str> env)
{
    Str *sorted = alloc<Str>(a, env.len);
    for (iz i = 0; i < env.len; i++) {
        sorted[i] = env[i];
    }
    sort(sorted, env.len, envless, *a);
    iz units = 2;
    for (iz i = 0; i < env.len; i++) {
        units += 2*sorted[i].len + 1;  // UTF-16 is never longer
    }
    c16 *block = alloc<c16>(a, units);
    iz   len   = 0;
    for (iz i = 0; i < env.len; i++) {
        Str e = sorted[i];
        if (!e.len || hasnul(e)) continue;
        for (iz k = 0; k < e.len;) {
            len += utf16encode(block+len, utf8decode(e, &k));
        }
        block[len++] = 0;
    }
    block[len++] = 0;
    block[len]   = 0;
    return block;
}

static void closeall(uz *hs, iz n)
{
    for (iz i = 0; i < n; i++) {
        if (hs[i] && hs[i]!=INVALID_HANDLE_VALUE) {
            CloseHandle(hs[i]);
        }
    }
}

// Spawn into a new job. With io, input is a pipe from the writer
// thread, errors go to outpath (or NUL) directly, and the reader passes
// output on as records instead of into the file.
static Proc *spawnproc(Os *os, Spawn *sp, Arena scratch, b32 io)
{
    if (!sp->argv.len || hasnul(sp->cwd) || hasnul(sp->outpath)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    Str  line = sp->cmdline.len ? sp->cmdline : quoteargv(&scratch, sp->argv);
    c16 *cmd  = towide(&scratch, line).data;
    c16 *cwd  = towide(&scratch, sp->cwd).data;  // not \\?\, which cmd rejects
    c16 *env  = envblock(&scratch, sp->env);
    c16 *out  = widepath(&scratch, sp->outpath).data;

    uz  job  = os->jobless ? 0 : CreateJobObjectW(0, 0);
    uz  nul  = CreateFileW((c16 *)u"NUL", GENERIC_READ|GENERIC_WRITE, FILE_SHARE_ALL,
                           0, OPEN_EXISTING, 0, 0);
    uz  file = 0;
    b32 ok   = (job || os->jobless) && nul!=INVALID_HANDLE_VALUE;
    if (ok && (!io || sp->outpath.len)) {
        file = CreateFileW(out, GENERIC_WRITE, FILE_SHARE_ALL, 0, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, 0);
        ok   = file != INVALID_HANDLE_VALUE;
    }
    uz    pr = 0, pw = 0, ir = 0, iw = 0;
    Event wake = {};
    ok = ok && CreatePipe(&pr, &pw, 0, 1<<16) &&
         (!io || (CreatePipe(&ir, &iw, 0, 1<<16) && os_eventinit(&wake)));
    if (ok && job) {
        JobLimits lim = {};
        lim.flags = JOB_KILL_ON_JOB_CLOSE | JOB_DIE_ON_UNHANDLED_EXCEPTION;
        ok = SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                     &lim, sizeof(lim));
    }
    uz mine[] = {job, file, pr, iw, (uz)wake.h[0]};  // kept if all goes well
    if (!ok) {
        i32 err = GetLastError();
        uz hs[] = {nul, pw, ir};
        closeall(hs, countof(hs));
        closeall(mine, countof(mine));
        SetLastError(err);
        return 0;
    }

    ProcessInfo pi = {};
    StartupInfo si = {};
    si.cb     = sizeof(si);
    si.flags  = STARTF_USESTDHANDLES;
    si.input  = io ? ir : nul;
    si.output = pw;
    si.error  = !io ? pw : file ? file : nul;
    i32 flags = CREATE_SUSPENDED | CREATE_NO_WINDOW |
                CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP |
                os->breakaway;
    EnterCriticalSection(&os->spawn);
    SetHandleInformation(si.input, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(si.output, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(si.error, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    ok = CreateProcessW(0, cmd, 0, 0, 1, flags, env, cwd, &si, &pi);
    i32 err = GetLastError();
    uz child[] = {nul, pw, ir, io ? file : 0};
    closeall(child, countof(child));
    LeaveCriticalSection(&os->spawn);
    if (io) {
        file    = 0;
        mine[1] = 0;
    }
    if (!ok) {
        closeall(mine, countof(mine));
        SetLastError(err);
        return 0;
    }
    if (job && !AssignProcessToJobObject(job, pi.process)) {
        err = GetLastError();
        TerminateProcess(pi.process, 1);
        uz hs[] = {pi.thread, pi.process};
        closeall(hs, countof(hs));
        closeall(mine, countof(mine));
        SetLastError(err);
        return 0;
    } else if (!job) {
        joblesswarn(os);
    }
    ResumeThread(pi.thread);
    CloseHandle(pi.thread);

    uz    heap = GetProcessHeap();
    Proc *p    = (Proc *)HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(Proc));
    if (!p) {
        killproc(job, pi.process);
        CloseHandle(pi.process);
        closeall(mine, countof(mine));
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    p->refs     = io ? 4 : 3;
    p->job      = job;
    p->pid      = pi.pid;
    p->process  = pi.process;
    p->pipe     = pr;
    p->file     = file;
    p->mbox     = sp->mbox;
    p->id       = sp->id;
    p->maxbytes = sp->maxbytes;
    p->io       = io;
    p->input    = iw;
    p->wake     = wake;
    if (io) {
        os_lockinit(&p->lock);
    }
    if (!os_thread(os, io ? procpipe : procreader, p)) {
        killproc(job, pi.process);
        CloseHandle(pi.process);
        closeall(mine, countof(mine));
        if (io) {
            DeleteCriticalSection((CriticalSection *)p->lock.opaque);
        }
        HeapFree(heap, 0, p);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    uz input[] = {iw, (uz)wake.h[0]};
    if (!os_thread(os, procwaiter, p)) {
        killproc(job, pi.process);  // the reader then finishes
        closeall(input, countof(input));
        if (io) procrelease(p);  // the writer's
        procrelease(p);  // the waiter's
        procrelease(p);  // the caller's
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    if (io && !os_thread(os, procwriter, p)) {
        killproc(job, pi.process);  // the reader and waiter then finish
        closeall(input, countof(input));
        procrelease(p);  // the writer's
        procrelease(p);  // the caller's
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    return p;
}

static Proc *os_spawn(Os *os, Spawn *sp, Arena scratch)
{
    return spawnproc(os, sp, scratch, 0);
}

static Proc *os_spawnio(Os *os, Spawn *sp, Arena scratch)
{
    return spawnproc(os, sp, scratch, 1);
}

static b32 os_procwrite(Os *, Proc *p, Str data)
{
    if (!p->io || data.len>PROC_QUEUEMAX) {
        return 0;
    }
    uz         heap = GetProcessHeap();
    ProcChunk *c    = 0;
    if (data.len) {
        c = (ProcChunk *)HeapAlloc(heap, 0, (iz)sizeof(ProcChunk) + data.len);
        if (!c) {
            return 0;
        }
        c->next = 0;
        c->len  = data.len;
        copybytes(c+1, data.data, data.len);
    }

    os_lock(&p->lock);
    b32 ok = !p->closing && !p->broken && data.len<=PROC_QUEUEMAX-p->queued;
    if (ok && c) {
        *(p->last ? &p->last->next : &p->head) = c;
        p->last    = c;
        p->queued += c->len;
        c = 0;
    }
    if (ok && !data.data) {
        p->closing = 1;
    }
    if (ok) {
        os_signal(&p->wake);
    }
    os_unlock(&p->lock);
    if (c) {
        HeapFree(heap, 0, c);  // not queued
    }
    return ok;
}

// The soft stop: CTRL_BREAK to the child's process group, every process
// in it that shares the console the child has (CREATE_NO_WINDOW gave it
// a hidden one). Only a process attached to that console may send it,
// and a process attaches to one console at a time, so this one leaves
// its own, if any, under a lock, for the child's, then comes back.
// Attaching sets the process's standard handles to the console's, so
// they are put back after, a console's opened anew (leaving it spoiled
// the old ones). A console being read for input (os_stdin) would break,
// so then the kill that follows the grace period has to do.
static void ctrlbreak(Os *os, Proc *p)
{
    i32 mode = 0;
    if (__atomic_load_n(&os->stdinread, __ATOMIC_RELAXED) &&
            GetConsoleMode(os->std[0], &mode)) {
        return;
    }
    EnterCriticalSection(&os->console);
    b32 con[3] = {};
    for (i32 fd = 0; fd < 3; fd++) {
        con[fd] = GetConsoleMode(os->std[fd], &mode);
    }
    b32 had = FreeConsole();
    if (AttachConsole(p->pid)) {
        SetConsoleCtrlHandler(0, 1);  // should it come here, ignore it
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, p->pid);
        FreeConsole();
        SetConsoleCtrlHandler(0, 0);
    }
    b32 back = had && AttachConsole(ATTACH_PARENT_PROCESS);
    for (i32 fd = 0; fd < 3; fd++) {
        if (con[fd] && back) {
            c16 *name = fd ? (c16 *)u"CONOUT$" : (c16 *)u"CONIN$";
            uz   h    = CreateFileW(name, GENERIC_READ|GENERIC_WRITE,
                                    FILE_SHARE_ALL, 0, OPEN_EXISTING, 0, 0);
            os->std[fd] = h!=INVALID_HANDLE_VALUE ? h : os->std[fd];
        }
        SetStdHandle(-10 - fd, os->std[fd]);
    }
    LeaveCriticalSection(&os->console);
}

static void os_kill(Os *os, Proc *p, b32 hard)
{
    if (hard) {
        killproc(p->job, p->process);
    } else {
        ctrlbreak(os, p);
    }
}

static void os_procfree(Os *os, Proc *p)
{
    if (p->io) {
        os_procwrite(os, p, {});  // close the input after the queue
    }
    procrelease(p);
}


// Local endpoints: named pipes
//
// A host's pipe is \\.\pipe\harness-SID-HASH-NAME: SID the user's, HASH
// that of the state directory's full path, case folded. Its DACL allows
// this user alone, since the default DACL grants Everyone read access.
// A pipe also takes clients over SMB, which PIPE_REJECT_REMOTE_CLIENTS
// refuses from Vista on (on XP, where it is refused and left out, such
// a client needs this user's own credentials). Denying network logons
// (S-1-5-2) in the DACL instead would lock out the user's own ssh
// sessions, whose tokens carry that group. The first instance is
// created with FILE_FLAG_FIRST_PIPE_INSTANCE, which fails if another
// process holds the name, and the listener keeps an instance waiting at
// all times, making the next before it hands over a connected one.
// Clients connect at identification level only, so that a pipe server
// cannot act as them. Everything else here dates from XP.
//
// I/O is overlapped, with an event for each direction, so that a pending
// read never holds up a write, and a manual-reset stop event that ends
// either wait. The waiting thread then cancels its own I/O (CancelIo,
// as XP has nothing finer) and waits for it to finish, so that the
// handle closes, with the last reference, only once nothing uses it.
// A pipe has no half close: ending the output once the queue is written
// stops the whole connection, which the peer sees as the end once it
// has read what came before.

enum : i32 {
    DETACHED_PROCESS              = 0x00000008,
    ERROR_ACCESS_DENIED           = 5,
    ERROR_IO_PENDING              = 997,
    ERROR_PIPE_BUSY               = 231,
    ERROR_PIPE_CONNECTED          = 535,
    FILE_APPEND_DATA              = 4,
    FILE_FLAG_FIRST_PIPE_INSTANCE = 0x00080000,
    FILE_FLAG_OVERLAPPED          = 0x40000000,
    PIPE_ACCESS_DUPLEX            = 3,
    PIPE_REJECT_REMOTE_CLIENTS    = 8,
    PIPE_UNLIMITED_INSTANCES      = 255,
    SECURITY_IDENTIFICATION       = 1 << 16,
    SECURITY_SQOS_PRESENT         = 0x00100000,
    SYNCHRONIZE                   = 0x00100000,
    TOKEN_QUERY                   = 8,
    TokenUser                     = 1,
};

enum : i32 {
    EXTENDED_STARTUPINFO_PRESENT  = 0x00080000,
};

enum : uz {
    PROC_THREAD_ATTRIBUTE_HANDLE_LIST = 0x20002,
};

struct StartupInfoEx {  // STARTUPINFOEXW
    StartupInfo si;
    void       *attrs;
};

// Vista's attribute lists, looked up at run time
using InitAttrs   = b32  (__stdcall *)(void *, i32, i32, uz *);
using UpdateAttr  = b32  (__stdcall *)(void *, i32, uz, void *, uz, void *, uz *);
using DeleteAttrs = void (__stdcall *)(void *);

struct SecurityAttributes {  // SECURITY_ATTRIBUTES
    i32   len;
    void *descriptor;
    b32   inherit;
};
static_assert(sizeof(SecurityAttributes) == (sizeof(uz)==8 ? 24 : 12));

W32(b32,    CancelIo(uz));
W32(b32,    ConnectNamedPipe(uz, Overlapped *));
W32(uz,     CreateNamedPipeW(c16 *, i32, i32, i32, i32, i32, i32, SecurityAttributes *));
W32(uz,     GetCurrentProcess());
W32(i32,    GetCurrentProcessId());
W32(b32,    GetOverlappedResult(uz, Overlapped *, i32 *, b32));
W32(void *, LocalFree(void *));
W32(b32,    ResetEvent(uz));
W32(i32,    WaitForMultipleObjects(i32, uz *, b32, i32));
W32(b32,    WaitNamedPipeW(c16 *, i32));
W32(b32,    ConvertSidToStringSidW(void *, c16 **));
W32(b32,    ConvertStringSecurityDescriptorToSecurityDescriptorW(c16 *, i32, void **,
                                                                 i32 *));
W32(b32,    GetTokenInformation(uz, i32, void *, i32, i32 *));
W32(b32,    OpenProcessToken(uz, i32, uz *));

enum : iz {
    CONN_BUFLEN   = 1<<16,
    CONN_QUEUEMAX = (iz)64 << 20,  // bytes waiting to be written
};

struct ConnChunk {  // queued output, its bytes following
    ConnChunk *next;
    iz         len;
};

struct Conn {
    i32        refs;     // atomic: the owner, the reader, the writer
    i32        hrefs;    // atomic: the reader and the writer, for h
    uz         h;
    uz         stop;     // manual reset: the connection is over
    uz         rev;      // the reader's
    uz         wev;      // the writer's
    Mailbox   *mbox;
    u64        id;
    Lock       lock;
    Event      wake;     // for the writer
    ConnChunk *head;     // under lock: the output queue
    ConnChunk *last;     // under lock
    iz         queued;   // under lock: bytes in the queue
    b32        closing;  // under lock: end the connection after the queue
    b32        broken;   // under lock: the output failed, or was dropped
    u8         buf[CONN_BUFLEN];
};

struct Listener {
    i32      refs;   // atomic: the owner, the thread
    uz       pipe;   // the instance waiting for a client
    uz       stop;
    uz       ev;
    c16     *name;
    void    *sd;     // the security descriptor (LocalAlloc)
    Mailbox *mbox;
    u64      nextid;
};

// A thread is done with the pipe: the last closes it, so that the peer
// sees the end even while the owner holds on to the Conn.
static void connunpipe(Conn *c)
{
    if (!__atomic_sub_fetch(&c->hrefs, 1, __ATOMIC_ACQ_REL)) {
        CloseHandle(c->h);
    }
}

static void connrelease(Conn *c)
{
    if (!__atomic_sub_fetch(&c->refs, 1, __ATOMIC_ACQ_REL)) {
        uz hs[] = {c->hrefs ? c->h : 0, c->stop, c->rev, c->wev, (uz)c->wake.h[0]};
        closeall(hs, countof(hs));
        DeleteCriticalSection((CriticalSection *)c->lock.opaque);
        HeapFree(GetProcessHeap(), 0, c);
    }
}

static void connfreequeue(Conn *c)  // under lock
{
    for (ConnChunk *k = c->head; k;) {
        ConnChunk *next = k->next;
        HeapFree(GetProcessHeap(), 0, k);
        k = next;
    }
    c->head   = 0;
    c->last   = 0;
    c->queued = 0;
}

// One overlapped read or write, waiting for it or for stop. Returns the
// bytes moved, or -1 on failure or stop, when it is cancelled and done.
static i32 pipeio(uz h, uz ev, uz stop, u8 *buf, i32 len, b32 write)
{
    Overlapped ov = {};
    ov.event = ev;
    ResetEvent(ev);
    i32 n  = 0;
    b32 ok = write ? WriteFile(h, buf, len, &n, &ov) : ReadFile(h, buf, len, &n, &ov);
    if (!ok && GetLastError()!=ERROR_IO_PENDING) {
        return -1;
    }
    uz  hs[] = {ev, stop};
    i32 w    = WaitForMultipleObjects(2, hs, 0, INFINITE);
    if (w != WAIT_OBJECT_0) {
        CancelIo(h);
        GetOverlappedResult(h, &ov, &n, 1);
        return -1;
    }
    return GetOverlappedResult(h, &ov, &n, 0) ? n : -1;
}

static void connreader(void *arg)
{
    Conn *c   = (Conn *)arg;
    iz    max = mailmax(c->mbox);
    for (;;) {
        i32 n = pipeio(c->h, c->rev, c->stop, c->buf, CONN_BUFLEN, 0);
        if (n < 0) {
            break;
        }
        for (iz off = 0; off < n; off += max) {
            iz len = n-off<max ? n-off : max;
            mailpost(c->mbox, CONN_DATA, c->id, {c->buf+off, len}, 1);
        }
    }
    SetEvent(c->stop);  // the writer has nobody to write to
    connunpipe(c);
    mailpost(c->mbox, CONN_EOF, c->id, {}, 1);
    connrelease(c);
}

static void connwriter(void *arg)
{
    Conn *c = (Conn *)arg;
    for (;;) {
        os_lock(&c->lock);
        ConnChunk *k = c->head;
        if (k) {
            c->head = k->next;
            c->last = c->head ? c->last : 0;
        }
        b32 done = !k && (c->closing || c->broken);
        os_unlock(&c->lock);
        if (done) {
            break;
        } else if (!k) {
            uz hs[] = {(uz)c->wake.h[0], c->stop};
            WaitForMultipleObjects(2, hs, 0, INFINITE);
            if (WaitForSingleObject(c->stop, 0) == WAIT_OBJECT_0) {
                os_lock(&c->lock);
                c->broken = 1;
                connfreequeue(c);
                os_unlock(&c->lock);
            }
            continue;
        }

        b32 ok = 1;
        for (iz off = 0; ok && off<k->len;) {
            i32 n = pipeio(c->h, c->wev, c->stop, (u8 *)(k+1)+off, chunk(k->len-off), 1);
            ok   = n > 0;
            off += n;
        }
        os_lock(&c->lock);
        c->queued -= k->len;
        if (!ok) {
            c->broken = 1;
            connfreequeue(c);
        }
        os_unlock(&c->lock);
        HeapFree(GetProcessHeap(), 0, k);
    }
    SetEvent(c->stop);  // written, or broken: the connection is over
    connunpipe(c);
    connrelease(c);
}

// A connection on a connected pipe handle, its threads started.
// Announced, it is posted as CONN_NEW first, before any record of its
// own; should its threads then fail to start, its CONN_EOF follows at
// once, and the owner has a connection that is over. Otherwise null on
// failure, the handle closed.
static Conn *newconn(Os *os, uz h, Mailbox *mbox, u64 id, b32 announce)
{
    uz    heap = GetProcessHeap();
    Conn *c    = (Conn *)HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(Conn));
    if (!c) {
        CloseHandle(h);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    c->h     = h;
    c->stop  = CreateEventW(0, 1, 0, 0);
    c->rev   = CreateEventW(0, 1, 0, 0);
    c->wev   = CreateEventW(0, 1, 0, 0);
    c->mbox  = mbox;
    c->id    = id;
    c->refs  = 1;
    c->hrefs = 1;  // closing h with the Conn, until the threads take it
    os_lockinit(&c->lock);
    if (!c->stop || !c->rev || !c->wev || !os_eventinit(&c->wake)) {
        connrelease(c);
        return 0;
    }
    c->refs  = 3;
    c->hrefs = 2;
    if (announce) {
        mailpost(mbox, CONN_NEW, id, {(u8 *)&c, sizeof(c)}, 1);
    }
    b32 reader = os_thread(os, connreader, c);
    b32 writer = reader && os_thread(os, connwriter, c);
    if (writer) {
        return c;
    }
    __atomic_sub_fetch(&c->refs, 2-reader, __ATOMIC_ACQ_REL);  // not started
    os_lock(&c->lock);
    c->broken = 1;
    os_unlock(&c->lock);
    SetEvent(c->stop);  // a reader that started ends, with CONN_EOF
    connunpipe(c);      // the writer's
    if (!reader) {
        connunpipe(c);
    }
    if (announce && !reader) {
        mailpost(mbox, CONN_EOF, id, {}, 1);
    }
    if (announce) {
        return c;
    }
    connrelease(c);
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
}

static b32 os_connwrite(Os *, Conn *c, Str data)
{
    if (data.len > CONN_QUEUEMAX) {
        return 0;
    }
    uz         heap = GetProcessHeap();
    ConnChunk *k    = 0;
    if (data.len) {
        k = (ConnChunk *)HeapAlloc(heap, 0, (iz)sizeof(ConnChunk) + data.len);
        if (!k) {
            return 0;
        }
        k->next = 0;
        k->len  = data.len;
        copybytes(k+1, data.data, data.len);
    }

    os_lock(&c->lock);
    b32 ok = !c->closing && !c->broken && data.len<=CONN_QUEUEMAX-c->queued;
    if (ok && k) {
        *(c->last ? &c->last->next : &c->head) = k;
        c->last    = k;
        c->queued += k->len;
        k = 0;
    }
    if (ok && !data.data) {
        c->closing = 1;
    }
    if (ok) {
        os_signal(&c->wake);
    }
    os_unlock(&c->lock);
    if (k) {
        HeapFree(heap, 0, k);  // not queued
    }
    return ok;
}

static void os_connclose(Os *, Conn *c)
{
    os_lock(&c->lock);
    c->broken = 1;
    connfreequeue(c);
    os_unlock(&c->lock);
    SetEvent(c->stop);  // ends any wait, read or write
    connrelease(c);
}

// The user's SID as a string, in the arena, or null.
static Str usersid(Arena *a)
{
    uz tok = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        return {};
    }
    void *buf = alloc<uz>(a, 64);  // TOKEN_USER and its SID
    i32   len = 0;
    b32   ok  = GetTokenInformation(tok, TokenUser, buf, 64*(i32)sizeof(uz), &len);
    CloseHandle(tok);
    c16 *s = 0;
    if (!ok || !ConvertSidToStringSidW(*(void **)buf, &s)) {
        return {};
    }
    iz n = 0;
    for (; s[n]; n++) {}
    Str r = fromwide(a, s, n);
    LocalFree(s);
    return r;
}

static Str os_endpoint(Os *, Arena *a, Str statedir, Str name)
{
    if (hasnul(statedir) || hasnul(name)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return {};
    }
    Arena tmp  = *a;
    Str16 full = widepath(&tmp, statedir);
    u64   h    = 0x100;
    for (iz i = 0; i < full.len; i++) {
        c16 c = full.data[i];
        c = c>='A' && c<='Z' ? (c16)(c + 32) : c=='/' ? (c16)'\\' : c;
        h ^= c;
        h *= 1111111111111111111u;
    }
    Str sid = usersid(&tmp);
    if (!sid.len) {
        return {};
    }
    Buf b(a, 128 + sid.len + name.len);
    print(&b, "\\\\.\\pipe\\harness-");
    print(&b, sid);
    putbyte(&b, '-');
    printhex(&b, h, 16);
    putbyte(&b, '-');
    print(&b, name);
    return finish(&b);
}

// A new instance of a listener's pipe, the first of its name or not.
static uz pipeinstance(Listener *l, b32 first)
{
    SecurityAttributes sa = {};
    sa.len        = sizeof(sa);
    sa.descriptor = l->sd;
    i32 open = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
               (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
    for (i32 mode = PIPE_REJECT_REMOTE_CLIENTS;; mode = 0) {
        uz h = CreateNamedPipeW(l->name, open, mode, PIPE_UNLIMITED_INSTANCES,
                                CONN_BUFLEN, CONN_BUFLEN, 0, &sa);
        if (h!=INVALID_HANDLE_VALUE || !mode || GetLastError()!=ERROR_INVALID_PARAMETER) {
            return h;  // else XP: without the Vista flag
        }
    }
}

static void listenrelease(Listener *l)
{
    if (!__atomic_sub_fetch(&l->refs, 1, __ATOMIC_ACQ_REL)) {
        uz hs[] = {l->pipe, l->stop, l->ev};
        closeall(hs, countof(hs));
        LocalFree(l->sd);
        HeapFree(GetProcessHeap(), 0, l->name);
        HeapFree(GetProcessHeap(), 0, l);
    }
}

static void listenthread(void *arg)
{
    Listener *l  = (Listener *)arg;
    Os        os = {};
    while (WaitForSingleObject(l->stop, 0) != WAIT_OBJECT_0) {
        if (l->pipe == INVALID_HANDLE_VALUE) {
            l->pipe = pipeinstance(l, 0);
            if (l->pipe == INVALID_HANDLE_VALUE) {
                WaitForSingleObject(l->stop, 100);  // until handles free up
                continue;
            }
        }
        Overlapped ov = {};
        ov.event = l->ev;
        ResetEvent(l->ev);
        b32 connected = ConnectNamedPipe(l->pipe, &ov);
        i32 err       = connected ? 0 : GetLastError();
        if (err == ERROR_IO_PENDING) {
            uz  hs[] = {l->ev, l->stop};
            i32 w    = WaitForMultipleObjects(2, hs, 0, INFINITE);
            i32 n    = 0;
            if (w != WAIT_OBJECT_0) {
                CancelIo(l->pipe);
                GetOverlappedResult(l->pipe, &ov, &n, 1);
                break;
            }
            connected = GetOverlappedResult(l->pipe, &ov, &n, 0);
        } else {
            connected = connected || err==ERROR_PIPE_CONNECTED;
        }
        if (!connected) {
            CloseHandle(l->pipe);  // a client that came and went
            l->pipe = INVALID_HANDLE_VALUE;
            continue;
        }

        // The next instance waits before this one goes
        uz h = l->pipe;
        l->pipe = pipeinstance(l, 0);
        newconn(&os, h, l->mbox, ++l->nextid, 1);
    }
    listenrelease(l);
}

static Listener *os_listen(Os *os, Arena scratch, Str endpoint, Mailbox *mbox)
{
    if (hasnul(endpoint)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    Str sid = usersid(&scratch);
    if (!sid.len) {
        return 0;
    }
    Buf sddl(&scratch, 128 + sid.len);
    print(&sddl, "D:P(A;;GA;;;");
    print(&sddl, sid);
    print(&sddl, ")");
    void *sd = 0;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            towide(&scratch, finish(&sddl)).data, 1, &sd, 0)) {
        return 0;
    }

    uz        heap = GetProcessHeap();
    Listener *l    = (Listener *)HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(Listener));
    Str16     name = towide(&scratch, endpoint);
    c16      *copy = (c16 *)HeapAlloc(heap, 0, (name.len+1)*(iz)sizeof(c16));
    if (!l || !copy) {
        if (l) HeapFree(heap, 0, l);
        if (copy) HeapFree(heap, 0, copy);
        LocalFree(sd);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    copybytes(copy, name.data, (name.len+1)*(iz)sizeof(c16));
    l->refs   = 1;
    l->name   = copy;
    l->sd     = sd;
    l->mbox   = mbox;
    l->stop   = CreateEventW(0, 1, 0, 0);
    l->ev     = CreateEventW(0, 1, 0, 0);
    l->pipe   = pipeinstance(l, 1);  // fails if another process has it
    if (l->pipe==INVALID_HANDLE_VALUE || !l->stop || !l->ev) {
        i32 err = GetLastError();
        listenrelease(l);
        SetLastError(err);
        return 0;
    }
    l->refs = 2;
    if (!os_thread(os, listenthread, l)) {
        l->refs = 1;
        listenrelease(l);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    return l;
}

static void os_unlisten(Os *, Listener *l)
{
    SetEvent(l->stop);
    listenrelease(l);
}

// Open the client end of a pipe, waiting a moment while every instance
// is busy. INVALID_HANDLE_VALUE if nobody listens.
static uz pipeopen(Arena scratch, Str endpoint)
{
    if (hasnul(endpoint)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    c16 *name = towide(&scratch, endpoint).data;
    for (i32 tries = 0;; tries++) {
        uz h = CreateFileW(name, GENERIC_READ|GENERIC_WRITE, 0, 0, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|
                           SECURITY_IDENTIFICATION, 0);
        if (h!=INVALID_HANDLE_VALUE || GetLastError()!=ERROR_PIPE_BUSY || tries==20) {
            return h;
        }
        WaitNamedPipeW(name, 250);
    }
}

static Conn *os_connect(Os *os, Arena scratch, Str endpoint, Mailbox *mbox, u64 id)
{
    uz h = pipeopen(scratch, endpoint);
    return h==INVALID_HANDLE_VALUE ? 0 : newconn(os, h, mbox, id, 0);
}

static b32 os_listening(Os *, Arena scratch, Str endpoint)
{
    uz h = pipeopen(scratch, endpoint);
    if (h == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_PIPE_BUSY;
    }
    CloseHandle(h);
    return 1;
}

static i32 os_pid(Os *)
{
    return GetCurrentProcessId();
}

// No console, a process group of its own, and out of this process's job,
// unless the job forbids that (ERROR_ACCESS_DENIED), as an ssh session's
// may: then in the job, attached. Its standard handles are inheritable
// only inside the spawn lock, as for os_spawn, and from Vista on they
// are all it inherits (PROC_THREAD_ATTRIBUTE_HANDLE_LIST): an ssh
// session hands its programs inheritable handles besides their standard
// ones, and a host holding any of them would hold the session open for
// as long as it runs. On XP it inherits those, but not this process's
// standard handles.
static Detached os_detach(Os *os, Arena scratch, Slice<Str> argv, Str cwd, Str errpath)
{
    Detached r = {};
    if (!argv.len || hasnul(cwd) || hasnul(errpath)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return r;
    }
    Str exe = os_exepath(os, &scratch);
    if (exe.len) {
        Slice<Str> args = {};
        args = push(&scratch, args, exe);  // this executable, whatever argv[0]
        for (iz i = 1; i < argv.len; i++) {
            args = push(&scratch, args, argv[i]);
        }
        argv = args;
    }
    c16 *cmd = towide(&scratch, quoteargv(&scratch, argv)).data;
    c16 *dir = towide(&scratch, cwd).data;
    uz   nul = CreateFileW((c16 *)u"NUL", GENERIC_READ|GENERIC_WRITE, FILE_SHARE_ALL,
                           0, OPEN_EXISTING, 0, 0);
    uz   err = CreateFileW(widepath(&scratch, errpath).data, FILE_APPEND_DATA|SYNCHRONIZE,
                           FILE_SHARE_ALL, 0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (nul == INVALID_HANDLE_VALUE) {
        if (err != INVALID_HANDLE_VALUE) CloseHandle(err);
        return r;
    }
    err = err!=INVALID_HANDLE_VALUE ? err : nul;

    ProcessInfo   pi  = {};
    StartupInfoEx six = {};
    StartupInfo  *si  = &six.si;
    si->cb     = sizeof(*si);
    si->flags  = STARTF_USESTDHANDLES;
    si->input  = nul;
    si->output = nul;
    si->error  = err;
    i32 flags = CREATE_NEW_PROCESS_GROUP;
    flags |= DETACHED_PROCESS;
    flags |= CREATE_BREAKAWAY_FROM_JOB;

    uz          k32    = GetModuleHandleW((c16 *)u"kernel32.dll");
    InitAttrs   init   = (InitAttrs)
        GetProcAddress(k32, (char *)"InitializeProcThreadAttributeList");
    UpdateAttr  update = (UpdateAttr)
        GetProcAddress(k32, (char *)"UpdateProcThreadAttribute");
    DeleteAttrs del    = (DeleteAttrs)
        GetProcAddress(k32, (char *)"DeleteProcThreadAttributeList");
    uz          list[] = {nul, err};
    uz          size   = 0;
    if (init && update && del && !init(0, 1, 0, &size) && size) {
        void *attrs = alloc<uz>(&scratch, (iz)(size/sizeof(uz) + 1));
        if (init(attrs, 1, 0, &size)) {
            uz n = err!=nul ? 2 : 1;
            if (update(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list,
                       n*sizeof(uz), 0, 0)) {
                six.attrs = attrs;
                si->cb    = sizeof(six);
                flags    |= EXTENDED_STARTUPINFO_PRESENT;
            } else {
                del(attrs);
            }
        }
    }

    EnterCriticalSection(&os->spawn);
    for (i32 fd = 0; fd < 3; fd++) {
        // This process's own, an ssh session's pipes perhaps, which an
        // heir would hold open, and the session with them, for good
        SetHandleInformation(os->std[fd], HANDLE_FLAG_INHERIT, 0);
    }
    SetHandleInformation(nul, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(err, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    b32 ok = CreateProcessW(0, cmd, 0, 0, 1, flags, 0, dir, si, &pi);
    if (!ok && GetLastError()==ERROR_ACCESS_DENIED) {
        flags &= ~CREATE_BREAKAWAY_FROM_JOB;
        ok = CreateProcessW(0, cmd, 0, 0, 1, flags, 0, dir, si, &pi);
        r.attached = ok;
    }
    i32 e = GetLastError();
    uz  hs[] = {nul, err!=nul ? err : 0};
    closeall(hs, countof(hs));
    LeaveCriticalSection(&os->spawn);
    if (six.attrs) {
        del(six.attrs);
    }
    if (!ok) {
        SetLastError(e);
        r.attached = 0;
        return r;
    }
    CloseHandle(pi.thread);
    CloseHandle(pi.process);
    r.pid = pi.pid;
    return r;
}


// HTTP: WinHTTP, synchronous, one thread per request
//
// A cancel closes the request handle, which makes the worker's blocked
// call fail. The worker checks under the lock before each call, so it
// does not start a call on a handle a cancel already closed, though a
// close can always land just after the check, which WinHTTP tolerates.

struct Http {
    i32      refs;      // atomic: the engine and the worker
    Lock     lock;
    uz       request;   // under lock, 0 once closed
    b32      cancelled; // under lock
    Mailbox *mbox;
    u64      id;
    i64      maxbytes;
    c16     *host;
    c16     *path;
    c16     *method;
    c16     *headers;
    Str      body;
    i32      port;
    i32      connectms;
    i32      readms;
    b32      secure;
    b32      direct;    // never through a proxy
};

static void httprelease(Http *h)
{
    if (!__atomic_sub_fetch(&h->refs, 1, __ATOMIC_ACQ_REL)) {
        HeapFree(GetProcessHeap(), 0, h);
    }
}

static b32 httplive(Http *h)
{
    os_lock(&h->lock);
    b32 live = !h->cancelled;
    os_unlock(&h->lock);
    return live;
}

static void httpdone(Http *h, i32 err, Str text)
{
    u8  msg[260];
    iz  len = text.len<256 ? text.len : 256;
    put32(msg, err);
    copybytes(msg+4, text.data, len);
    mailpost(h->mbox, HTTP_DONE, h->id, {msg, 4+len}, 1);
}

// The worker's error: a cancel, or the WinHTTP error text.
static void httpfail(Http *h, i32 err, u8 *scratch, iz cap)
{
    i32 code = GetLastError();
    if (!httplive(h) || code==ERROR_WINHTTP_OPERATION_CANCELLED) {
        httpdone(h, HTTP_ECANCEL, "cancelled");
        return;
    }
    Arena a  = {(byte *)scratch, (byte *)scratch + cap};
    Os    os = {};
    SetLastError(code);
    httpdone(h, err, os_lasterr(&os, &a));
}

static void httpworker(void *arg)
{
    Http *h       = (Http *)arg;
    uz    session = 0;
    uz    connect = 0;
    uz    request = 0;
    u8   *mem     = (u8 *)HeapAlloc(GetProcessHeap(), 0, 1<<16);

    if (!mem) {
        httpdone(h, HTTP_EIO, "out of memory");
        httprelease(h);
        return;
    }

    c16 *agent = (c16 *)u"harness/" HARNESS_VERSION;
    session = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC, 0, 0, 0);
    if (!session) {
        // Before Windows 8.1: proxy settings from netsh winhttp
        session = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_DEFAULT, 0, 0, 0);
    }
    if (!session) {
        httpfail(h, HTTP_ECONNECT, mem, 1<<16);
        goto done;
    }
    WinHttpSetTimeouts(session, 0, h->connectms, 60000, h->readms);
    connect = WinHttpConnect(session, h->host, (c16)h->port, 0);
    if (!connect) {
        httpfail(h, HTTP_ECONNECT, mem, 1<<16);
        goto done;
    }
    request = WinHttpOpenRequest(connect, h->method, h->path, 0, 0, 0,
                                 h->secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        httpfail(h, HTTP_ECONNECT, mem, 1<<16);
        goto done;
    }
    os_lock(&h->lock);
    if (h->cancelled) {
        os_unlock(&h->lock);
        WinHttpCloseHandle(request);
        request = 0;
        httpdone(h, HTTP_ECANCEL, "cancelled");
        goto done;
    }
    h->request = request;
    os_unlock(&h->lock);

    if (h->direct) {
        ProxyInfo none = {WINHTTP_ACCESS_TYPE_NO_PROXY, 0, 0};
        WinHttpSetOption(request, WINHTTP_OPTION_PROXY, &none, sizeof(none));
    }
    {
        // Waiting for the head belongs to the engine's watchdogs
        i32 wait = h->readms ? h->readms : 0x7fffffff;
        WinHttpSetOption(request, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT,
                         &wait, sizeof(wait));
    }

    if (!httplive(h) ||
            !WinHttpSendRequest(request, h->headers, -1, h->body.data,
                                (i32)h->body.len, (i32)h->body.len, 0)) {
        httpfail(h, HTTP_ECONNECT, mem, 1<<16);
        goto done;
    }
    if (!httplive(h) || !WinHttpReceiveResponse(request, 0)) {
        httpfail(h, HTTP_EIO, mem, 1<<16);
        goto done;
    }

    {
        i32 status = 0;
        i32 size   = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
                            0, &status, &size, 0);
        c16 *wide = (c16 *)mem;
        i32  hlen = 1<<16;  // in bytes
        iz   n    = 0;
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, 0,
                                wide, &hlen, 0)) {
            n = hlen / 2;
        }
        u8 *msg = (u8 *)HeapAlloc(GetProcessHeap(), 0, (uz)(4 + 3*n));
        if (!msg) {
            httpdone(h, HTTP_EIO, "out of memory");
            goto done;
        }
        Arena c   = {(byte *)msg + 4, (byte *)msg + 4 + 3*n};
        Str   raw = fromwide(&c, wide, n);
        put32(msg, status);
        mailpost(h->mbox, HTTP_HEAD, h->id, {msg, 4+raw.len}, 1);
        HeapFree(GetProcessHeap(), 0, msg);
    }

    // A synchronous read waits to fill its buffer, so ask what is ready
    // first (waiting for at least a byte), then read only that much.
    for (i64 got = 0;;) {
        i32 avail = 0;
        i32 n     = 0;
        if (!httplive(h) || !WinHttpQueryDataAvailable(request, &avail)) {
            httpfail(h, HTTP_EIO, mem, 1<<16);
            goto done;
        } else if (!avail) {
            break;
        }
        avail = avail<(1<<16) ? avail : 1<<16;
        if (!httplive(h) || !WinHttpReadData(request, mem, avail, &n)) {
            httpfail(h, HTTP_EIO, mem, 1<<16);
            goto done;
        } else if (!n) {
            break;
        }
        if (n > h->maxbytes-got) {
            httpdone(h, HTTP_ETOOBIG, "response too large");
            goto done;
        }
        got += n;
        mailpost(h->mbox, HTTP_BODY, h->id, {mem, n}, 1);
    }
    httpdone(h, HTTP_OK, {});

  done:
    os_lock(&h->lock);
    if (h->request) {
        WinHttpCloseHandle(h->request);
        h->request = 0;
    }
    os_unlock(&h->lock);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    HeapFree(GetProcessHeap(), 0, mem);
    httprelease(h);
}

// Whether WinHTTP can do TLS 1.2, which every https endpoint worth
// reaching requires. XP's cannot, and rejects even being asked for it,
// so https there fails at once with a clear error (ERROR_NOTLS12)
// rather than in a handshake after the connect. Asked once, of a
// session of its own, which no request then inherits.
static b32 hastls12(Os *os)
{
    i32 r = __atomic_load_n(&os->tls12, __ATOMIC_RELAXED);
    if (!r) {
        i32 tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        uz  s   = WinHttpOpen((c16 *)u"harness", WINHTTP_ACCESS_TYPE_DEFAULT, 0, 0, 0);
        b32 no  = s && !WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS,
                                         &tls, sizeof(tls)) &&
                  GetLastError()==ERROR_INVALID_PARAMETER;
        if (s) {
            WinHttpCloseHandle(s);
        }
        r = no ? -1 : 1;
        __atomic_store_n(&os->tls12, r, __ATOMIC_RELAXED);
    }
    return r > 0;
}

static Http *os_http(Os *os, HttpReq *req, Arena scratch)
{
    Url u = parseurl(req->url);
    if (!u.ok || hasnul(req->method) || hasnul(u.path) || hasnul(req->headers)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    } else if (u.scheme=="https" && !hastls12(os)) {
        SetLastError(ERROR_NOTLS12);
        return 0;
    }
    Str host = u.host;
    if (host.len>=2 && host[0]=='[') {
        host = cuthead(cuttail(host, 1), 1);
    }
    Str16 whost    = towide(&scratch, host);
    Str16 wpath    = towide(&scratch, u.path);
    Str16 wmethod  = towide(&scratch, req->method);
    Str16 wheaders = towide(&scratch, req->headers);

    // One block: the record, then the strings
    iz units = whost.len + wpath.len + wmethod.len + wheaders.len + 4;
    iz size  = (iz)sizeof(Http) + units*2 + req->body.len + 8;
    Http *h  = (Http *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
    if (!h) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    c16 *w = (c16 *)(h + 1);
    Str16 parts[] = {whost, wpath, wmethod, wheaders};
    c16 **dst[]   = {&h->host, &h->path, &h->method, &h->headers};
    for (iz i = 0; i < countof(parts); i++) {
        *dst[i] = w;
        copybytes(w, parts[i].data, parts[i].len*2);
        w += parts[i].len;
        *w++ = 0;
    }
    u8 *body = (u8 *)w;
    copybytes(body, req->body.data, req->body.len);
    h->body      = {body, req->body.len};
    h->refs      = 2;
    h->mbox      = req->mbox;
    h->id        = req->id;
    h->maxbytes  = req->maxbytes;
    h->port      = u.port;
    h->connectms = req->connectms;
    h->readms    = req->readms;
    h->secure    = u.scheme == "https";
    h->direct    = localhost(u.host);
    os_lockinit(&h->lock);
    if (!os_thread(os, httpworker, h)) {
        HeapFree(GetProcessHeap(), 0, h);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    return h;
}

static void os_httpcancel(Os *, Http *h)
{
    os_lock(&h->lock);
    h->cancelled = 1;
    if (h->request) {
        WinHttpCloseHandle(h->request);
        h->request = 0;
    }
    os_unlock(&h->lock);
}

static void os_httpfree(Os *, Http *h)
{
    httprelease(h);
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
    InitializeCriticalSectionAndSpinCount(&os.spawn, 4000);
    InitializeCriticalSectionAndSpinCount(&os.console, 4000);
    for (i32 fd = 0; fd < 3; fd++) {
        os.std[fd] = GetStdHandle(-10 - fd);
    }
    uz kernel32  = GetModuleHandleW((c16 *)u"kernel32.dll");
    os.finalpath = (u32 (__stdcall *)(uz, c16 *, u32, u32))
                   GetProcAddress(kernel32, (char *)"GetFinalPathNameByHandleW");
    jobcheck(&os);

    // No dialogs from a crashing child, or from a missing disk
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);

    // A 32-bit process has 2 GiB of address space, which XP fragments
    // into pieces of at most 1 GiB: every reservation is small there.
    iz      cap = sizeof(void *)==8 ? (iz)1<<34 : (iz)1<<26;
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
