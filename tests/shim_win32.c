// A program that runs a command named in its own file: the tests'
// stand-ins for ssh, scp and the desktop helper on Windows, where the
// harness can start only programs (CreateProcess), not #! scripts.
//
// A test copies this program, then appends a command line in UTF-8, its
// length (4 bytes, little-endian) and "SHIM": then each copy, wherever
// it is copied to or renamed, runs that command with its own arguments
// after it, as they were given, with its standard handles, and exits
// with its status. The command runs in a job that dies with the shim,
// so killing the shim, as a test kills an ssh, takes the command down
// too, as killing a real ssh ends what it ran. The command finds the
// shim's pid in PROSPERO_SHIM_PID, as the pid the harness started.
//
// This is free and unencumbered software released into the public domain.
#include <windows.h>

static void fail(const char *msg)
{
    DWORD n;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg, lstrlenA(msg), &n, 0);
    ExitProcess(127);
}

// The command line appended to this program's file, as a wide string.
static wchar_t *ownprefix(void)
{
    static wchar_t path[32768];
    if (!GetModuleFileNameW(0, path, 32768)) {
        fail("shim: cannot find its own file\n");
    }
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_DELETE, 0,
                           OPEN_EXISTING, 0, 0);
    LARGE_INTEGER size, at;
    unsigned char tail[8];
    DWORD n;
    if (f == INVALID_HANDLE_VALUE || !GetFileSizeEx(f, &size) || size.QuadPart < 8) {
        fail("shim: cannot read its own file\n");
    }
    at.QuadPart = size.QuadPart - 8;
    if (!SetFilePointerEx(f, at, 0, FILE_BEGIN) || !ReadFile(f, tail, 8, &n, 0) || n != 8 ||
        tail[4]!='S' || tail[5]!='H' || tail[6]!='I' || tail[7]!='M') {
        fail("shim: no command appended to it\n");
    }
    DWORD len = tail[0] | tail[1]<<8 | tail[2]<<16 | (DWORD)tail[3]<<24;
    if (len < 1 || len > 16384 || len > size.QuadPart - 8) {
        fail("shim: a bad command length\n");
    }
    static char utf8[16384];
    at.QuadPart = size.QuadPart - 8 - len;
    if (!SetFilePointerEx(f, at, 0, FILE_BEGIN) || !ReadFile(f, utf8, len, &n, 0) || n != len) {
        fail("shim: cannot read its command\n");
    }
    CloseHandle(f);
    static wchar_t wide[16384];
    int w = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, wide, 16383);
    if (!w) {
        fail("shim: its command is not UTF-8\n");
    }
    wide[w] = 0;
    return wide;
}

// The arguments after the program's name in a command line, as given,
// the blank before them included.
static wchar_t *args(wchar_t *s)
{
    if (*s == '"') {
        for (s++; *s && *s != '"'; s++) {}
        if (*s) s++;
    } else {
        for (; *s && *s != ' ' && *s != '\t'; s++) {}
    }
    return s;
}

int main(void)
{
    wchar_t *prefix = ownprefix();
    wchar_t *rest   = args(GetCommandLineW());
    int      plen   = lstrlenW(prefix);
    int      rlen   = lstrlenW(rest);
    wchar_t *cmd    = HeapAlloc(GetProcessHeap(), 0, (plen + rlen + 2)*sizeof(*cmd));
    if (!cmd) {
        fail("shim: out of memory\n");
    }
    CopyMemory(cmd, prefix, plen*sizeof(*cmd));
    CopyMemory(cmd + plen, rest, (rlen + 1)*sizeof(*cmd));

    wchar_t pid[16], *p = pid + 15;
    *p = 0;
    for (DWORD n = GetCurrentProcessId(); n || p == pid + 15; n /= 10) {
        *--p = (wchar_t)('0' + n%10);
    }
    SetEnvironmentVariableW(L"PROSPERO_SHIM_PID", p);

    HANDLE job = CreateJobObjectW(0, 0);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = {0};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                           JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                         &lim, sizeof(lim))) {
        fail("shim: cannot make a job\n");
    }

    STARTUPINFOW si = {0};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE std[] = {si.hStdInput, si.hStdOutput, si.hStdError};
    for (int i = 0; i < 3; i++) {
        SetHandleInformation(std[i], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    }
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(0, cmd, 0, 0, TRUE, CREATE_SUSPENDED, 0, 0, &si, &pi)) {
        fail("shim: cannot run its command\n");
    }
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 127);
        fail("shim: cannot put its command in its job\n");
    }
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD status = 127;
    GetExitCodeProcess(pi.hProcess, &status);
    ExitProcess(status);
}
