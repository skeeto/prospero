#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Check the headless Windows engine's imports against Windows XP's.

  uv run tests/imports.py build-xp/prospero.exe
  uv run tests/imports.py --exports kernel32.dll advapi32.dll winhttp.dll

The headless engine (PROSPERO_GUI=OFF), i686 or x86_64, may import only
from KERNEL32, ADVAPI32 and WINHTTP, only by name, and only functions
that Windows XP SP3's copies of those DLLs export. Anything newer must be
looked up at run time (GetProcAddress) with a fallback. It must also
declare an OS and subsystem version that XP loads (5.1 at most, or 5.2
for x86_64, XP x64), and have no delay-loaded imports, which would dodge
this check.

The export lists below come from the DLLs of an XP SP3 machine (version
5.1.2600, all three dated 2008-04-14) through --exports, which prints
them in this file's format. The PE file is parsed here, with no objdump.
Exits 1 on any violation, listing each.
"""

import argparse
import struct
import sys

sys.dont_write_bytecode = True


class Pe:
    """The parts of a PE image this check reads."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:2] != b"MZ":
            raise ValueError(f"{path}: not an executable")
        pe = struct.unpack_from("<I", d, 0x3c)[0]
        if d[pe:pe+4] != b"PE\0\0":
            raise ValueError(f"{path}: no PE header")
        nsections, optsize = struct.unpack_from("<H12xH", d, pe+6)
        opt = pe + 24
        magic = struct.unpack_from("<H", d, opt)[0]
        if magic not in (0x10b, 0x20b):
            raise ValueError(f"{path}: unknown optional header {magic:#x}")
        self.wide = magic == 0x20b
        self.osver = struct.unpack_from("<HH", d, opt+40)
        self.subver = struct.unpack_from("<HH", d, opt+48)
        ndirs = struct.unpack_from("<I", d, opt + (108 if self.wide else 92))[0]
        dirs = opt + (112 if self.wide else 96)
        self.dirs = [struct.unpack_from("<II", d, dirs + 8*i) for i in range(ndirs)]
        self.sections = []
        for i in range(nsections):
            s = opt + optsize + 40*i
            vsize, va, rawsize, raw = struct.unpack_from("<IIII", d, s+8)
            self.sections.append((va, max(vsize, rawsize), raw))

    def offset(self, rva):
        for va, size, raw in self.sections:
            if va <= rva < va+size:
                return raw + rva - va
        raise ValueError(f"RVA {rva:#x} outside every section")

    def u32(self, rva):
        return struct.unpack_from("<I", self.data, self.offset(rva))[0]

    def cstr(self, rva):
        off = self.offset(rva)
        end = self.data.index(b"\0", off)
        return self.data[off:end].decode("ascii", "replace")

    def directory(self, index):
        return self.dirs[index] if index < len(self.dirs) else (0, 0)

    def imports(self):
        """{dll name: [function name, or "#ordinal"]}"""
        r = {}
        rva, size = self.directory(1)
        if not size:
            return r
        step = 8 if self.wide else 4
        flag = 1 << (63 if self.wide else 31)
        fmt = "<Q" if self.wide else "<I"
        for k in range(0, 1 << 16):
            names, _, _, name, first = struct.unpack_from(
                "<IIIII", self.data, self.offset(rva + 20*k))
            if not name:
                break
            funcs = r.setdefault(self.cstr(name), [])
            thunk = names or first
            for i in range(0, 1 << 16):
                v = struct.unpack_from(fmt, self.data, self.offset(thunk + step*i))[0]
                if not v:
                    break
                if v & flag:
                    funcs.append(f"#{v & 0xffff}")
                else:
                    funcs.append(self.cstr((v & 0x7fffffff) + 2))
        return r

    def delayed(self):
        """Names of delay-loaded DLLs."""
        r = []
        rva, size = self.directory(13)
        for k in range(size // 32):
            name = struct.unpack_from("<I", self.data, self.offset(rva + 32*k) + 4)[0]
            if name:
                r.append(self.cstr(name))
        return r

    def exports(self):
        rva, size = self.directory(0)
        if not size:
            return []
        nnames, _, names = struct.unpack_from("<III", self.data, self.offset(rva) + 24)
        return [self.cstr(self.u32(names + 4*i)) for i in range(nnames)]


def check(path):
    """Violations of the XP rules in the image at path."""
    pe = Pe(path)
    bad = []
    # XP is 5.1, and its x64 edition (a Server 2003 kernel) 5.2
    most = (5, 2) if pe.wide else (5, 1)
    for what, ver in (("OS", pe.osver), ("subsystem", pe.subver)):
        if ver > most:
            bad.append(f"{what} version {ver[0]}.{ver[1]}: "
                       f"XP loads {most[0]}.{most[1]} at most")
    for dll in pe.delayed():
        bad.append(f"{dll}: delay-loaded")
    allowed = {k: set(v.split()) for k, v in XP.items()}
    for dll, funcs in pe.imports().items():
        names = allowed.get(dll.lower())
        if names is None:
            bad.append(f"{dll}: not one of {', '.join(sorted(XP))}")
            continue
        for f in funcs:
            if f.startswith("#"):
                bad.append(f"{dll} {f}: imported by ordinal")
            elif f not in names:
                bad.append(f"{dll} {f}: not exported by Windows XP SP3")
    return pe, bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exports", action="store_true",
                    help="print the export lists of the given DLLs instead")
    ap.add_argument("-v", action="store_true", help="list every import")
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()

    if args.exports:
        print("XP = {")
        for path in args.files:
            names = sorted(Pe(path).exports())
            lines, line = [], ""
            for n in names:
                if len(line) + 1 + len(n) > 76:
                    lines.append(line)
                    line = ""
                line = f"{line} {n}" if line else n
            lines.append(line)
            name = path.replace("\\", "/").rsplit("/", 1)[-1].lower()
            print(f'    "{name}": """')
            for line in lines:
                print(line)
            print('""",')
        print("}")
        return 0

    status = 0
    for path in args.files:
        pe, bad = check(path)
        imports = pe.imports()
        count = sum(len(v) for v in imports.values())
        if args.v:
            for dll, funcs in imports.items():
                print(f"{dll}: {' '.join(funcs)}")
        for b in bad:
            print(f"{path}: {b}")
        print(f"{path}: {count} imports from {', '.join(imports)}: "
              f"{'ok' if not bad else f'{len(bad)} problems'}")
        status |= 1 if bad else 0
    return status


# Exports of Windows XP SP3 (5.1.2600, 2008-04-14), from --exports
XP = {
    "kernel32.dll": """
ActivateActCtx AddAtomA AddAtomW AddConsoleAliasA AddConsoleAliasW
AddLocalAlternateComputerNameA AddLocalAlternateComputerNameW AddRefActCtx
AddVectoredExceptionHandler AllocConsole AllocateUserPhysicalPages
AreFileApisANSI AssignProcessToJobObject AttachConsole BackupRead BackupSeek
BackupWrite BaseCheckAppcompatCache BaseCleanupAppcompatCache
BaseCleanupAppcompatCacheSupport BaseDumpAppcompatCache
BaseFlushAppcompatCache BaseInitAppcompatCache BaseInitAppcompatCacheSupport
BaseProcessInitPostImport BaseQueryModuleData BaseUpdateAppcompatCache
BasepCheckWinSaferRestrictions Beep BeginUpdateResourceA
BeginUpdateResourceW BindIoCompletionCallback BuildCommDCBA
BuildCommDCBAndTimeoutsA BuildCommDCBAndTimeoutsW BuildCommDCBW
CallNamedPipeA CallNamedPipeW CancelDeviceWakeupRequest CancelIo
CancelTimerQueueTimer CancelWaitableTimer ChangeTimerQueueTimer
CheckNameLegalDOS8Dot3A CheckNameLegalDOS8Dot3W CheckRemoteDebuggerPresent
ClearCommBreak ClearCommError CloseConsoleHandle CloseHandle
CloseProfileUserMapping CmdBatNotification CommConfigDialogA
CommConfigDialogW CompareFileTime CompareStringA CompareStringW
ConnectNamedPipe ConsoleMenuControl ContinueDebugEvent ConvertDefaultLocale
ConvertFiberToThread ConvertThreadToFiber CopyFileA CopyFileExA CopyFileExW
CopyFileW CopyLZFile CreateActCtxA CreateActCtxW CreateConsoleScreenBuffer
CreateDirectoryA CreateDirectoryExA CreateDirectoryExW CreateDirectoryW
CreateEventA CreateEventW CreateFiber CreateFiberEx CreateFileA
CreateFileMappingA CreateFileMappingW CreateFileW CreateHardLinkA
CreateHardLinkW CreateIoCompletionPort CreateJobObjectA CreateJobObjectW
CreateJobSet CreateMailslotA CreateMailslotW
CreateMemoryResourceNotification CreateMutexA CreateMutexW CreateNamedPipeA
CreateNamedPipeW CreateNlsSecurityDescriptor CreatePipe CreateProcessA
CreateProcessInternalA CreateProcessInternalW CreateProcessInternalWSecure
CreateProcessW CreateRemoteThread CreateSemaphoreA CreateSemaphoreW
CreateSocketHandle CreateTapePartition CreateThread CreateTimerQueue
CreateTimerQueueTimer CreateToolhelp32Snapshot CreateVirtualBuffer
CreateWaitableTimerA CreateWaitableTimerW DeactivateActCtx
DebugActiveProcess DebugActiveProcessStop DebugBreak DebugBreakProcess
DebugSetProcessKillOnExit DecodePointer DecodeSystemPointer DefineDosDeviceA
DefineDosDeviceW DelayLoadFailureHook DeleteAtom DeleteCriticalSection
DeleteFiber DeleteFileA DeleteFileW DeleteTimerQueue DeleteTimerQueueEx
DeleteTimerQueueTimer DeleteVolumeMountPointA DeleteVolumeMountPointW
DeviceIoControl DisableThreadLibraryCalls DisconnectNamedPipe
DnsHostnameToComputerNameA DnsHostnameToComputerNameW DosDateTimeToFileTime
DosPathToSessionPathA DosPathToSessionPathW DuplicateConsoleHandle
DuplicateHandle EncodePointer EncodeSystemPointer EndUpdateResourceA
EndUpdateResourceW EnterCriticalSection EnumCalendarInfoA
EnumCalendarInfoExA EnumCalendarInfoExW EnumCalendarInfoW EnumDateFormatsA
EnumDateFormatsExA EnumDateFormatsExW EnumDateFormatsW
EnumLanguageGroupLocalesA EnumLanguageGroupLocalesW EnumResourceLanguagesA
EnumResourceLanguagesW EnumResourceNamesA EnumResourceNamesW
EnumResourceTypesA EnumResourceTypesW EnumSystemCodePagesA
EnumSystemCodePagesW EnumSystemGeoID EnumSystemLanguageGroupsA
EnumSystemLanguageGroupsW EnumSystemLocalesA EnumSystemLocalesW
EnumTimeFormatsA EnumTimeFormatsW EnumUILanguagesA EnumUILanguagesW
EnumerateLocalComputerNamesA EnumerateLocalComputerNamesW EraseTape
EscapeCommFunction ExitProcess ExitThread ExitVDM ExpandEnvironmentStringsA
ExpandEnvironmentStringsW ExpungeConsoleCommandHistoryA
ExpungeConsoleCommandHistoryW ExtendVirtualBuffer FatalAppExitA
FatalAppExitW FatalExit FileTimeToDosDateTime FileTimeToLocalFileTime
FileTimeToSystemTime FillConsoleOutputAttribute FillConsoleOutputCharacterA
FillConsoleOutputCharacterW FindActCtxSectionGuid FindActCtxSectionStringA
FindActCtxSectionStringW FindAtomA FindAtomW FindClose
FindCloseChangeNotification FindFirstChangeNotificationA
FindFirstChangeNotificationW FindFirstFileA FindFirstFileExA
FindFirstFileExW FindFirstFileW FindFirstVolumeA FindFirstVolumeMountPointA
FindFirstVolumeMountPointW FindFirstVolumeW FindNextChangeNotification
FindNextFileA FindNextFileW FindNextVolumeA FindNextVolumeMountPointA
FindNextVolumeMountPointW FindNextVolumeW FindResourceA FindResourceExA
FindResourceExW FindResourceW FindVolumeClose FindVolumeMountPointClose
FlushConsoleInputBuffer FlushFileBuffers FlushInstructionCache
FlushViewOfFile FoldStringA FoldStringW FormatMessageA FormatMessageW
FreeConsole FreeEnvironmentStringsA FreeEnvironmentStringsW FreeLibrary
FreeLibraryAndExitThread FreeResource FreeUserPhysicalPages
FreeVirtualBuffer GenerateConsoleCtrlEvent GetACP GetAtomNameA GetAtomNameW
GetBinaryType GetBinaryTypeA GetBinaryTypeW GetCPFileNameFromRegistry
GetCPInfo GetCPInfoExA GetCPInfoExW GetCalendarInfoA GetCalendarInfoW
GetComPlusPackageInstallStatus GetCommConfig GetCommMask GetCommModemStatus
GetCommProperties GetCommState GetCommTimeouts GetCommandLineA
GetCommandLineW GetCompressedFileSizeA GetCompressedFileSizeW
GetComputerNameA GetComputerNameExA GetComputerNameExW GetComputerNameW
GetConsoleAliasA GetConsoleAliasExesA GetConsoleAliasExesLengthA
GetConsoleAliasExesLengthW GetConsoleAliasExesW GetConsoleAliasW
GetConsoleAliasesA GetConsoleAliasesLengthA GetConsoleAliasesLengthW
GetConsoleAliasesW GetConsoleCP GetConsoleCharType GetConsoleCommandHistoryA
GetConsoleCommandHistoryLengthA GetConsoleCommandHistoryLengthW
GetConsoleCommandHistoryW GetConsoleCursorInfo GetConsoleCursorMode
GetConsoleDisplayMode GetConsoleFontInfo GetConsoleFontSize
GetConsoleHardwareState GetConsoleInputExeNameA GetConsoleInputExeNameW
GetConsoleInputWaitHandle GetConsoleKeyboardLayoutNameA
GetConsoleKeyboardLayoutNameW GetConsoleMode GetConsoleNlsMode
GetConsoleOutputCP GetConsoleProcessList GetConsoleScreenBufferInfo
GetConsoleSelectionInfo GetConsoleTitleA GetConsoleTitleW GetConsoleWindow
GetCurrencyFormatA GetCurrencyFormatW GetCurrentActCtx GetCurrentConsoleFont
GetCurrentDirectoryA GetCurrentDirectoryW GetCurrentProcess
GetCurrentProcessId GetCurrentThread GetCurrentThreadId GetDateFormatA
GetDateFormatW GetDefaultCommConfigA GetDefaultCommConfigW
GetDefaultSortkeySize GetDevicePowerState GetDiskFreeSpaceA
GetDiskFreeSpaceExA GetDiskFreeSpaceExW GetDiskFreeSpaceW GetDllDirectoryA
GetDllDirectoryW GetDriveTypeA GetDriveTypeW GetEnvironmentStrings
GetEnvironmentStringsA GetEnvironmentStringsW GetEnvironmentVariableA
GetEnvironmentVariableW GetExitCodeProcess GetExitCodeThread
GetExpandedNameA GetExpandedNameW GetFileAttributesA GetFileAttributesExA
GetFileAttributesExW GetFileAttributesW GetFileInformationByHandle
GetFileSize GetFileSizeEx GetFileTime GetFileType
GetFirmwareEnvironmentVariableA GetFirmwareEnvironmentVariableW
GetFullPathNameA GetFullPathNameW GetGeoInfoA GetGeoInfoW GetHandleContext
GetHandleInformation GetLargestConsoleWindowSize GetLastError
GetLinguistLangSize GetLocalTime GetLocaleInfoA GetLocaleInfoW
GetLogicalDriveStringsA GetLogicalDriveStringsW GetLogicalDrives
GetLogicalProcessorInformation GetLongPathNameA GetLongPathNameW
GetMailslotInfo GetModuleFileNameA GetModuleFileNameW GetModuleHandleA
GetModuleHandleExA GetModuleHandleExW GetModuleHandleW
GetNamedPipeHandleStateA GetNamedPipeHandleStateW GetNamedPipeInfo
GetNativeSystemInfo GetNextVDMCommand GetNlsSectionName
GetNumaAvailableMemory GetNumaAvailableMemoryNode GetNumaHighestNodeNumber
GetNumaNodeProcessorMask GetNumaProcessorMap GetNumaProcessorNode
GetNumberFormatA GetNumberFormatW GetNumberOfConsoleFonts
GetNumberOfConsoleInputEvents GetNumberOfConsoleMouseButtons GetOEMCP
GetOverlappedResult GetPriorityClass GetPrivateProfileIntA
GetPrivateProfileIntW GetPrivateProfileSectionA
GetPrivateProfileSectionNamesA GetPrivateProfileSectionNamesW
GetPrivateProfileSectionW GetPrivateProfileStringA GetPrivateProfileStringW
GetPrivateProfileStructA GetPrivateProfileStructW GetProcAddress
GetProcessAffinityMask GetProcessDEPPolicy GetProcessHandleCount
GetProcessHeap GetProcessHeaps GetProcessId GetProcessIoCounters
GetProcessPriorityBoost GetProcessShutdownParameters GetProcessTimes
GetProcessVersion GetProcessWorkingSetSize GetProfileIntA GetProfileIntW
GetProfileSectionA GetProfileSectionW GetProfileStringA GetProfileStringW
GetQueuedCompletionStatus GetShortPathNameA GetShortPathNameW
GetStartupInfoA GetStartupInfoW GetStdHandle GetStringTypeA GetStringTypeExA
GetStringTypeExW GetStringTypeW GetSystemDEPPolicy GetSystemDefaultLCID
GetSystemDefaultLangID GetSystemDefaultUILanguage GetSystemDirectoryA
GetSystemDirectoryW GetSystemInfo GetSystemPowerStatus
GetSystemRegistryQuota GetSystemTime GetSystemTimeAdjustment
GetSystemTimeAsFileTime GetSystemTimes GetSystemWindowsDirectoryA
GetSystemWindowsDirectoryW GetSystemWow64DirectoryA GetSystemWow64DirectoryW
GetTapeParameters GetTapePosition GetTapeStatus GetTempFileNameA
GetTempFileNameW GetTempPathA GetTempPathW GetThreadContext
GetThreadIOPendingFlag GetThreadLocale GetThreadPriority
GetThreadPriorityBoost GetThreadSelectorEntry GetThreadTimes GetTickCount
GetTimeFormatA GetTimeFormatW GetTimeZoneInformation GetUserDefaultLCID
GetUserDefaultLangID GetUserDefaultUILanguage GetUserGeoID
GetVDMCurrentDirectories GetVersion GetVersionExA GetVersionExW
GetVolumeInformationA GetVolumeInformationW
GetVolumeNameForVolumeMountPointA GetVolumeNameForVolumeMountPointW
GetVolumePathNameA GetVolumePathNameW GetVolumePathNamesForVolumeNameA
GetVolumePathNamesForVolumeNameW GetWindowsDirectoryA GetWindowsDirectoryW
GetWriteWatch GlobalAddAtomA GlobalAddAtomW GlobalAlloc GlobalCompact
GlobalDeleteAtom GlobalFindAtomA GlobalFindAtomW GlobalFix GlobalFlags
GlobalFree GlobalGetAtomNameA GlobalGetAtomNameW GlobalHandle GlobalLock
GlobalMemoryStatus GlobalMemoryStatusEx GlobalReAlloc GlobalSize
GlobalUnWire GlobalUnfix GlobalUnlock GlobalWire Heap32First Heap32ListFirst
Heap32ListNext Heap32Next HeapAlloc HeapCompact HeapCreate HeapCreateTagsW
HeapDestroy HeapExtend HeapFree HeapLock HeapQueryInformation HeapQueryTagW
HeapReAlloc HeapSetInformation HeapSize HeapSummary HeapUnlock HeapUsage
HeapValidate HeapWalk InitAtomTable InitializeCriticalSection
InitializeCriticalSectionAndSpinCount InitializeSListHead
InterlockedCompareExchange InterlockedDecrement InterlockedExchange
InterlockedExchangeAdd InterlockedFlushSList InterlockedIncrement
InterlockedPopEntrySList InterlockedPushEntrySList InvalidateConsoleDIBits
IsBadCodePtr IsBadHugeReadPtr IsBadHugeWritePtr IsBadReadPtr IsBadStringPtrA
IsBadStringPtrW IsBadWritePtr IsDBCSLeadByte IsDBCSLeadByteEx
IsDebuggerPresent IsProcessInJob IsProcessorFeaturePresent
IsSystemResumeAutomatic IsValidCodePage IsValidLanguageGroup IsValidLocale
IsValidUILanguage IsWow64Process LCMapStringA LCMapStringW LZClose
LZCloseFile LZCopy LZCreateFileW LZDone LZInit LZOpenFileA LZOpenFileW
LZRead LZSeek LZStart LeaveCriticalSection LoadLibraryA LoadLibraryExA
LoadLibraryExW LoadLibraryW LoadModule LoadResource LocalAlloc LocalCompact
LocalFileTimeToFileTime LocalFlags LocalFree LocalHandle LocalLock
LocalReAlloc LocalShrink LocalSize LocalUnlock LockFile LockFileEx
LockResource MapUserPhysicalPages MapUserPhysicalPagesScatter MapViewOfFile
MapViewOfFileEx Module32First Module32FirstW Module32Next Module32NextW
MoveFileA MoveFileExA MoveFileExW MoveFileW MoveFileWithProgressA
MoveFileWithProgressW MulDiv MultiByteToWideChar NlsConvertIntegerToString
NlsGetCacheUpdateCount NlsResetProcessLocale NumaVirtualQueryNode
OpenConsoleW OpenDataFile OpenEventA OpenEventW OpenFile OpenFileMappingA
OpenFileMappingW OpenJobObjectA OpenJobObjectW OpenMutexA OpenMutexW
OpenProcess OpenProfileUserMapping OpenSemaphoreA OpenSemaphoreW OpenThread
OpenWaitableTimerA OpenWaitableTimerW OutputDebugStringA OutputDebugStringW
PeekConsoleInputA PeekConsoleInputW PeekNamedPipe PostQueuedCompletionStatus
PrepareTape PrivCopyFileExW PrivMoveFileIdentityW Process32First
Process32FirstW Process32Next Process32NextW ProcessIdToSessionId PulseEvent
PurgeComm QueryActCtxW QueryDepthSList QueryDosDeviceA QueryDosDeviceW
QueryInformationJobObject QueryMemoryResourceNotification
QueryPerformanceCounter QueryPerformanceFrequency
QueryWin31IniFilesMappedToRegistry QueueUserAPC QueueUserWorkItem
RaiseException ReadConsoleA ReadConsoleInputA ReadConsoleInputExA
ReadConsoleInputExW ReadConsoleInputW ReadConsoleOutputA
ReadConsoleOutputAttribute ReadConsoleOutputCharacterA
ReadConsoleOutputCharacterW ReadConsoleOutputW ReadConsoleW
ReadDirectoryChangesW ReadFile ReadFileEx ReadFileScatter ReadProcessMemory
RegisterConsoleIME RegisterConsoleOS2 RegisterConsoleVDM
RegisterWaitForInputIdle RegisterWaitForSingleObject
RegisterWaitForSingleObjectEx RegisterWowBaseHandlers RegisterWowExec
ReleaseActCtx ReleaseMutex ReleaseSemaphore RemoveDirectoryA
RemoveDirectoryW RemoveLocalAlternateComputerNameA
RemoveLocalAlternateComputerNameW RemoveVectoredExceptionHandler ReplaceFile
ReplaceFileA ReplaceFileW RequestDeviceWakeup RequestWakeupLatency
ResetEvent ResetWriteWatch RestoreLastError ResumeThread RtlCaptureContext
RtlCaptureStackBackTrace RtlFillMemory RtlMoveMemory RtlUnwind RtlZeroMemory
ScrollConsoleScreenBufferA ScrollConsoleScreenBufferW SearchPathA
SearchPathW SetCPGlobal SetCalendarInfoA SetCalendarInfoW
SetClientTimeZoneInformation SetComPlusPackageInstallStatus SetCommBreak
SetCommConfig SetCommMask SetCommState SetCommTimeouts SetComputerNameA
SetComputerNameExA SetComputerNameExW SetComputerNameW
SetConsoleActiveScreenBuffer SetConsoleCP SetConsoleCommandHistoryMode
SetConsoleCtrlHandler SetConsoleCursor SetConsoleCursorInfo
SetConsoleCursorMode SetConsoleCursorPosition SetConsoleDisplayMode
SetConsoleFont SetConsoleHardwareState SetConsoleIcon
SetConsoleInputExeNameA SetConsoleInputExeNameW SetConsoleKeyShortcuts
SetConsoleLocalEUDC SetConsoleMaximumWindowSize SetConsoleMenuClose
SetConsoleMode SetConsoleNlsMode SetConsoleNumberOfCommandsA
SetConsoleNumberOfCommandsW SetConsoleOS2OemFormat SetConsoleOutputCP
SetConsolePalette SetConsoleScreenBufferSize SetConsoleTextAttribute
SetConsoleTitleA SetConsoleTitleW SetConsoleWindowInfo
SetCriticalSectionSpinCount SetCurrentDirectoryA SetCurrentDirectoryW
SetDefaultCommConfigA SetDefaultCommConfigW SetDllDirectoryA
SetDllDirectoryW SetEndOfFile SetEnvironmentVariableA
SetEnvironmentVariableW SetErrorMode SetEvent SetFileApisToANSI
SetFileApisToOEM SetFileAttributesA SetFileAttributesW SetFilePointer
SetFilePointerEx SetFileShortNameA SetFileShortNameW SetFileTime
SetFileValidData SetFirmwareEnvironmentVariableA
SetFirmwareEnvironmentVariableW SetHandleContext SetHandleCount
SetHandleInformation SetInformationJobObject SetLastConsoleEventActive
SetLastError SetLocalPrimaryComputerNameA SetLocalPrimaryComputerNameW
SetLocalTime SetLocaleInfoA SetLocaleInfoW SetMailslotInfo
SetMessageWaitingIndicator SetNamedPipeHandleState SetPriorityClass
SetProcessAffinityMask SetProcessDEPPolicy SetProcessPriorityBoost
SetProcessShutdownParameters SetProcessWorkingSetSize SetStdHandle
SetSystemPowerState SetSystemTime SetSystemTimeAdjustment SetTapeParameters
SetTapePosition SetTermsrvAppInstallMode SetThreadAffinityMask
SetThreadContext SetThreadExecutionState SetThreadIdealProcessor
SetThreadLocale SetThreadPriority SetThreadPriorityBoost SetThreadUILanguage
SetTimeZoneInformation SetTimerQueueTimer SetUnhandledExceptionFilter
SetUserGeoID SetVDMCurrentDirectories SetVolumeLabelA SetVolumeLabelW
SetVolumeMountPointA SetVolumeMountPointW SetWaitableTimer SetupComm
ShowConsoleCursor SignalObjectAndWait SizeofResource Sleep SleepEx
SuspendThread SwitchToFiber SwitchToThread SystemTimeToFileTime
SystemTimeToTzSpecificLocalTime TerminateJobObject TerminateProcess
TerminateThread TermsrvAppInstallMode Thread32First Thread32Next TlsAlloc
TlsFree TlsGetValue TlsSetValue Toolhelp32ReadProcessMemory
TransactNamedPipe TransmitCommChar TrimVirtualBuffer TryEnterCriticalSection
TzSpecificLocalTimeToSystemTime UTRegister UTUnRegister
UnhandledExceptionFilter UnlockFile UnlockFileEx UnmapViewOfFile
UnregisterConsoleIME UnregisterWait UnregisterWaitEx UpdateResourceA
UpdateResourceW VDMConsoleOperation VDMOperationStarted ValidateLCType
ValidateLocale VerLanguageNameA VerLanguageNameW VerSetConditionMask
VerifyConsoleIoHandle VerifyVersionInfoA VerifyVersionInfoW VirtualAlloc
VirtualAllocEx VirtualBufferExceptionHandler VirtualFree VirtualFreeEx
VirtualLock VirtualProtect VirtualProtectEx VirtualQuery VirtualQueryEx
VirtualUnlock WTSGetActiveConsoleSessionId WaitCommEvent WaitForDebugEvent
WaitForMultipleObjects WaitForMultipleObjectsEx WaitForSingleObject
WaitForSingleObjectEx WaitNamedPipeA WaitNamedPipeW WideCharToMultiByte
WinExec WriteConsoleA WriteConsoleInputA WriteConsoleInputVDMA
WriteConsoleInputVDMW WriteConsoleInputW WriteConsoleOutputA
WriteConsoleOutputAttribute WriteConsoleOutputCharacterA
WriteConsoleOutputCharacterW WriteConsoleOutputW WriteConsoleW WriteFile
WriteFileEx WriteFileGather WritePrivateProfileSectionA
WritePrivateProfileSectionW WritePrivateProfileStringA
WritePrivateProfileStringW WritePrivateProfileStructA
WritePrivateProfileStructW WriteProcessMemory WriteProfileSectionA
WriteProfileSectionW WriteProfileStringA WriteProfileStringW WriteTapemark
ZombifyActCtx _hread _hwrite _lclose _lcreat _llseek _lopen _lread _lwrite
lstrcat lstrcatA lstrcatW lstrcmp lstrcmpA lstrcmpW lstrcmpi lstrcmpiA
lstrcmpiW lstrcpy lstrcpyA lstrcpyW lstrcpyn lstrcpynA lstrcpynW lstrlen
lstrlenA lstrlenW
""",
    "advapi32.dll": """
A_SHAFinal A_SHAInit A_SHAUpdate AbortSystemShutdownA AbortSystemShutdownW
AccessCheck AccessCheckAndAuditAlarmA AccessCheckAndAuditAlarmW
AccessCheckByType AccessCheckByTypeAndAuditAlarmA
AccessCheckByTypeAndAuditAlarmW AccessCheckByTypeResultList
AccessCheckByTypeResultListAndAuditAlarmA
AccessCheckByTypeResultListAndAuditAlarmByHandleA
AccessCheckByTypeResultListAndAuditAlarmByHandleW
AccessCheckByTypeResultListAndAuditAlarmW AddAccessAllowedAce
AddAccessAllowedAceEx AddAccessAllowedObjectAce AddAccessDeniedAce
AddAccessDeniedAceEx AddAccessDeniedObjectAce AddAce AddAuditAccessAce
AddAuditAccessAceEx AddAuditAccessObjectAce AddUsersToEncryptedFile
AdjustTokenGroups AdjustTokenPrivileges AllocateAndInitializeSid
AllocateLocallyUniqueId AreAllAccessesGranted AreAnyAccessesGranted
BackupEventLogA BackupEventLogW BuildExplicitAccessWithNameA
BuildExplicitAccessWithNameW BuildImpersonateExplicitAccessWithNameA
BuildImpersonateExplicitAccessWithNameW BuildImpersonateTrusteeA
BuildImpersonateTrusteeW BuildSecurityDescriptorA BuildSecurityDescriptorW
BuildTrusteeWithNameA BuildTrusteeWithNameW BuildTrusteeWithObjectsAndNameA
BuildTrusteeWithObjectsAndNameW BuildTrusteeWithObjectsAndSidA
BuildTrusteeWithObjectsAndSidW BuildTrusteeWithSidA BuildTrusteeWithSidW
CancelOverlappedAccess ChangeServiceConfig2A ChangeServiceConfig2W
ChangeServiceConfigA ChangeServiceConfigW CheckTokenMembership
ClearEventLogA ClearEventLogW CloseCodeAuthzLevel CloseEncryptedFileRaw
CloseEventLog CloseServiceHandle CloseTrace CommandLineFromMsiDescriptor
ComputeAccessTokenFromCodeAuthzLevel ControlService ControlTraceA
ControlTraceW ConvertAccessToSecurityDescriptorA
ConvertAccessToSecurityDescriptorW ConvertSDToStringSDRootDomainA
ConvertSDToStringSDRootDomainW ConvertSecurityDescriptorToAccessA
ConvertSecurityDescriptorToAccessNamedA
ConvertSecurityDescriptorToAccessNamedW ConvertSecurityDescriptorToAccessW
ConvertSecurityDescriptorToStringSecurityDescriptorA
ConvertSecurityDescriptorToStringSecurityDescriptorW ConvertSidToStringSidA
ConvertSidToStringSidW ConvertStringSDToSDDomainA ConvertStringSDToSDDomainW
ConvertStringSDToSDRootDomainA ConvertStringSDToSDRootDomainW
ConvertStringSecurityDescriptorToSecurityDescriptorA
ConvertStringSecurityDescriptorToSecurityDescriptorW ConvertStringSidToSidA
ConvertStringSidToSidW ConvertToAutoInheritPrivateObjectSecurity CopySid
CreateCodeAuthzLevel CreatePrivateObjectSecurity
CreatePrivateObjectSecurityEx
CreatePrivateObjectSecurityWithMultipleInheritance CreateProcessAsUserA
CreateProcessAsUserSecure CreateProcessAsUserW CreateProcessWithLogonW
CreateRestrictedToken CreateServiceA CreateServiceW CreateTraceInstanceId
CreateWellKnownSid CredDeleteA CredDeleteW CredEnumerateA CredEnumerateW
CredFree CredGetSessionTypes CredGetTargetInfoA CredGetTargetInfoW
CredIsMarshaledCredentialA CredIsMarshaledCredentialW CredMarshalCredentialA
CredMarshalCredentialW CredProfileLoaded CredReadA
CredReadDomainCredentialsA CredReadDomainCredentialsW CredReadW CredRenameA
CredRenameW CredUnmarshalCredentialA CredUnmarshalCredentialW CredWriteA
CredWriteDomainCredentialsA CredWriteDomainCredentialsW CredWriteW
CredpConvertCredential CredpConvertTargetInfo CredpDecodeCredential
CredpEncodeCredential CryptAcquireContextA CryptAcquireContextW
CryptContextAddRef CryptCreateHash CryptDecrypt CryptDeriveKey
CryptDestroyHash CryptDestroyKey CryptDuplicateHash CryptDuplicateKey
CryptEncrypt CryptEnumProviderTypesA CryptEnumProviderTypesW
CryptEnumProvidersA CryptEnumProvidersW CryptExportKey CryptGenKey
CryptGenRandom CryptGetDefaultProviderA CryptGetDefaultProviderW
CryptGetHashParam CryptGetKeyParam CryptGetProvParam CryptGetUserKey
CryptHashData CryptHashSessionKey CryptImportKey CryptReleaseContext
CryptSetHashParam CryptSetKeyParam CryptSetProvParam CryptSetProviderA
CryptSetProviderExA CryptSetProviderExW CryptSetProviderW CryptSignHashA
CryptSignHashW CryptVerifySignatureA CryptVerifySignatureW DecryptFileA
DecryptFileW DeleteAce DeleteService DeregisterEventSource
DestroyPrivateObjectSecurity DuplicateEncryptionInfoFile DuplicateToken
DuplicateTokenEx ElfBackupEventLogFileA ElfBackupEventLogFileW
ElfChangeNotify ElfClearEventLogFileA ElfClearEventLogFileW ElfCloseEventLog
ElfDeregisterEventSource ElfFlushEventLog ElfNumberOfRecords ElfOldestRecord
ElfOpenBackupEventLogA ElfOpenBackupEventLogW ElfOpenEventLogA
ElfOpenEventLogW ElfReadEventLogA ElfReadEventLogW ElfRegisterEventSourceA
ElfRegisterEventSourceW ElfReportEventA ElfReportEventW EnableTrace
EncryptFileA EncryptFileW EncryptedFileKeyInfo EncryptionDisable
EnumDependentServicesA EnumDependentServicesW EnumServiceGroupW
EnumServicesStatusA EnumServicesStatusExA EnumServicesStatusExW
EnumServicesStatusW EnumerateTraceGuids EqualDomainSid EqualPrefixSid
EqualSid FileEncryptionStatusA FileEncryptionStatusW FindFirstFreeAce
FlushTraceA FlushTraceW FreeEncryptedFileKeyInfo
FreeEncryptionCertificateHashList FreeInheritedFromArray FreeSid
GetAccessPermissionsForObjectA GetAccessPermissionsForObjectW GetAce
GetAclInformation GetAuditedPermissionsFromAclA
GetAuditedPermissionsFromAclW GetCurrentHwProfileA GetCurrentHwProfileW
GetEffectiveRightsFromAclA GetEffectiveRightsFromAclW GetEventLogInformation
GetExplicitEntriesFromAclA GetExplicitEntriesFromAclW GetFileSecurityA
GetFileSecurityW GetInformationCodeAuthzLevelW
GetInformationCodeAuthzPolicyW GetInheritanceSourceA GetInheritanceSourceW
GetKernelObjectSecurity GetLengthSid GetLocalManagedApplicationData
GetLocalManagedApplications GetManagedApplicationCategories
GetManagedApplications GetMultipleTrusteeA GetMultipleTrusteeOperationA
GetMultipleTrusteeOperationW GetMultipleTrusteeW GetNamedSecurityInfoA
GetNamedSecurityInfoExA GetNamedSecurityInfoExW GetNamedSecurityInfoW
GetNumberOfEventLogRecords GetOldestEventLogRecord
GetOverlappedAccessResults GetPrivateObjectSecurity
GetSecurityDescriptorControl GetSecurityDescriptorDacl
GetSecurityDescriptorGroup GetSecurityDescriptorLength
GetSecurityDescriptorOwner GetSecurityDescriptorRMControl
GetSecurityDescriptorSacl GetSecurityInfo GetSecurityInfoExA
GetSecurityInfoExW GetServiceDisplayNameA GetServiceDisplayNameW
GetServiceKeyNameA GetServiceKeyNameW GetSidIdentifierAuthority
GetSidLengthRequired GetSidSubAuthority GetSidSubAuthorityCount
GetTokenInformation GetTraceEnableFlags GetTraceEnableLevel
GetTraceLoggerHandle GetTrusteeFormA GetTrusteeFormW GetTrusteeNameA
GetTrusteeNameW GetTrusteeTypeA GetTrusteeTypeW GetUserNameA GetUserNameW
GetWindowsAccountDomainSid I_ScGetCurrentGroupStateW I_ScIsSecurityProcess
I_ScPnPGetServiceName I_ScSendTSMessage I_ScSetServiceBitsA
I_ScSetServiceBitsW IdentifyCodeAuthzLevelW ImpersonateAnonymousToken
ImpersonateLoggedOnUser ImpersonateNamedPipeClient ImpersonateSelf
InitializeAcl InitializeSecurityDescriptor InitializeSid
InitiateSystemShutdownA InitiateSystemShutdownExA InitiateSystemShutdownExW
InitiateSystemShutdownW InstallApplication IsTextUnicode IsTokenRestricted
IsTokenUntrusted IsValidAcl IsValidSecurityDescriptor IsValidSid
IsWellKnownSid LockServiceDatabase LogonUserA LogonUserExA LogonUserExW
LogonUserW LookupAccountNameA LookupAccountNameW LookupAccountSidA
LookupAccountSidW LookupPrivilegeDisplayNameA LookupPrivilegeDisplayNameW
LookupPrivilegeNameA LookupPrivilegeNameW LookupPrivilegeValueA
LookupPrivilegeValueW LookupSecurityDescriptorPartsA
LookupSecurityDescriptorPartsW LsaAddAccountRights LsaAddPrivilegesToAccount
LsaClearAuditLog LsaClose LsaCreateAccount LsaCreateSecret
LsaCreateTrustedDomain LsaCreateTrustedDomainEx LsaDelete
LsaDeleteTrustedDomain LsaEnumerateAccountRights LsaEnumerateAccounts
LsaEnumerateAccountsWithUserRight LsaEnumeratePrivileges
LsaEnumeratePrivilegesOfAccount LsaEnumerateTrustedDomains
LsaEnumerateTrustedDomainsEx LsaFreeMemory LsaGetQuotasForAccount
LsaGetRemoteUserName LsaGetSystemAccessAccount LsaGetUserName
LsaICLookupNames LsaICLookupNamesWithCreds LsaICLookupSids
LsaICLookupSidsWithCreds LsaLookupNames LsaLookupNames2
LsaLookupPrivilegeDisplayName LsaLookupPrivilegeName LsaLookupPrivilegeValue
LsaLookupSids LsaNtStatusToWinError LsaOpenAccount LsaOpenPolicy
LsaOpenPolicySce LsaOpenSecret LsaOpenTrustedDomain
LsaOpenTrustedDomainByName LsaQueryDomainInformationPolicy
LsaQueryForestTrustInformation LsaQueryInfoTrustedDomain
LsaQueryInformationPolicy LsaQuerySecret LsaQuerySecurityObject
LsaQueryTrustedDomainInfo LsaQueryTrustedDomainInfoByName
LsaRemoveAccountRights LsaRemovePrivilegesFromAccount LsaRetrievePrivateData
LsaSetDomainInformationPolicy LsaSetForestTrustInformation
LsaSetInformationPolicy LsaSetInformationTrustedDomain
LsaSetQuotasForAccount LsaSetSecret LsaSetSecurityObject
LsaSetSystemAccessAccount LsaSetTrustedDomainInfoByName
LsaSetTrustedDomainInformation LsaStorePrivateData MD4Final MD4Init
MD4Update MD5Final MD5Init MD5Update MSChapSrvChangePassword
MSChapSrvChangePassword2 MakeAbsoluteSD MakeAbsoluteSD2 MakeSelfRelativeSD
MapGenericMask NotifyBootConfigStatus NotifyChangeEventLog
ObjectCloseAuditAlarmA ObjectCloseAuditAlarmW ObjectDeleteAuditAlarmA
ObjectDeleteAuditAlarmW ObjectOpenAuditAlarmA ObjectOpenAuditAlarmW
ObjectPrivilegeAuditAlarmA ObjectPrivilegeAuditAlarmW OpenBackupEventLogA
OpenBackupEventLogW OpenEncryptedFileRawA OpenEncryptedFileRawW
OpenEventLogA OpenEventLogW OpenProcessToken OpenSCManagerA OpenSCManagerW
OpenServiceA OpenServiceW OpenThreadToken OpenTraceA OpenTraceW
PrivilegeCheck PrivilegedServiceAuditAlarmA PrivilegedServiceAuditAlarmW
ProcessIdleTasks ProcessTrace QueryAllTracesA QueryAllTracesW
QueryRecoveryAgentsOnEncryptedFile QueryServiceConfig2A QueryServiceConfig2W
QueryServiceConfigA QueryServiceConfigW QueryServiceLockStatusA
QueryServiceLockStatusW QueryServiceObjectSecurity QueryServiceStatus
QueryServiceStatusEx QueryTraceA QueryTraceW QueryUsersOnEncryptedFile
QueryWindows31FilesMigration ReadEncryptedFileRaw ReadEventLogA
ReadEventLogW RegCloseKey RegConnectRegistryA RegConnectRegistryW
RegCreateKeyA RegCreateKeyExA RegCreateKeyExW RegCreateKeyW RegDeleteKeyA
RegDeleteKeyW RegDeleteValueA RegDeleteValueW RegDisablePredefinedCache
RegDisablePredefinedCacheEx RegEnumKeyA RegEnumKeyExA RegEnumKeyExW
RegEnumKeyW RegEnumValueA RegEnumValueW RegFlushKey RegGetKeySecurity
RegLoadKeyA RegLoadKeyW RegNotifyChangeKeyValue RegOpenCurrentUser
RegOpenKeyA RegOpenKeyExA RegOpenKeyExW RegOpenKeyW RegOpenUserClassesRoot
RegOverridePredefKey RegQueryInfoKeyA RegQueryInfoKeyW
RegQueryMultipleValuesA RegQueryMultipleValuesW RegQueryValueA
RegQueryValueExA RegQueryValueExW RegQueryValueW RegReplaceKeyA
RegReplaceKeyW RegRestoreKeyA RegRestoreKeyW RegSaveKeyA RegSaveKeyExA
RegSaveKeyExW RegSaveKeyW RegSetKeySecurity RegSetValueA RegSetValueExA
RegSetValueExW RegSetValueW RegUnLoadKeyA RegUnLoadKeyW RegisterEventSourceA
RegisterEventSourceW RegisterIdleTask RegisterServiceCtrlHandlerA
RegisterServiceCtrlHandlerExA RegisterServiceCtrlHandlerExW
RegisterServiceCtrlHandlerW RegisterTraceGuidsA RegisterTraceGuidsW
RemoveTraceCallback RemoveUsersFromEncryptedFile ReportEventA ReportEventW
RevertToSelf SaferCloseLevel SaferComputeTokenFromLevel SaferCreateLevel
SaferGetLevelInformation SaferGetPolicyInformation SaferIdentifyLevel
SaferRecordEventLogEntry SaferSetLevelInformation SaferSetPolicyInformation
SaferiChangeRegistryScope SaferiCompareTokenLevels
SaferiIsExecutableFileType SaferiPopulateDefaultsInRegistry
SaferiRecordEventLogEntry SaferiReplaceProcessThreadTokens
SaferiSearchMatchingHashRules SetAclInformation SetEntriesInAccessListA
SetEntriesInAccessListW SetEntriesInAclA SetEntriesInAclW
SetEntriesInAuditListA SetEntriesInAuditListW SetFileSecurityA
SetFileSecurityW SetInformationCodeAuthzLevelW
SetInformationCodeAuthzPolicyW SetKernelObjectSecurity SetNamedSecurityInfoA
SetNamedSecurityInfoExA SetNamedSecurityInfoExW SetNamedSecurityInfoW
SetPrivateObjectSecurity SetPrivateObjectSecurityEx
SetSecurityDescriptorControl SetSecurityDescriptorDacl
SetSecurityDescriptorGroup SetSecurityDescriptorOwner
SetSecurityDescriptorRMControl SetSecurityDescriptorSacl SetSecurityInfo
SetSecurityInfoExA SetSecurityInfoExW SetServiceBits
SetServiceObjectSecurity SetServiceStatus SetThreadToken SetTokenInformation
SetTraceCallback SetUserFileEncryptionKey StartServiceA
StartServiceCtrlDispatcherA StartServiceCtrlDispatcherW StartServiceW
StartTraceA StartTraceW StopTraceA StopTraceW
SynchronizeWindows31FilesAndWindowsNTRegistry SystemFunction001
SystemFunction002 SystemFunction003 SystemFunction004 SystemFunction005
SystemFunction006 SystemFunction007 SystemFunction008 SystemFunction009
SystemFunction010 SystemFunction011 SystemFunction012 SystemFunction013
SystemFunction014 SystemFunction015 SystemFunction016 SystemFunction017
SystemFunction018 SystemFunction019 SystemFunction020 SystemFunction021
SystemFunction022 SystemFunction023 SystemFunction024 SystemFunction025
SystemFunction026 SystemFunction027 SystemFunction028 SystemFunction029
SystemFunction030 SystemFunction031 SystemFunction032 SystemFunction033
SystemFunction034 SystemFunction035 SystemFunction036 SystemFunction040
SystemFunction041 TraceEvent TraceEventInstance TraceMessage TraceMessageVa
TreeResetNamedSecurityInfoA TreeResetNamedSecurityInfoW
TrusteeAccessToObjectA TrusteeAccessToObjectW UninstallApplication
UnlockServiceDatabase UnregisterIdleTask UnregisterTraceGuids UpdateTraceA
UpdateTraceW WdmWmiServiceMain WmiCloseBlock WmiCloseTraceWithCursor
WmiConvertTimestamp WmiDevInstToInstanceNameA WmiDevInstToInstanceNameW
WmiEnumerateGuids WmiExecuteMethodA WmiExecuteMethodW
WmiFileHandleToInstanceNameA WmiFileHandleToInstanceNameW WmiFreeBuffer
WmiGetFirstTraceOffset WmiGetNextEvent WmiGetTraceHeader
WmiMofEnumerateResourcesA WmiMofEnumerateResourcesW
WmiNotificationRegistrationA WmiNotificationRegistrationW WmiOpenBlock
WmiOpenTraceWithCursor WmiParseTraceEvent WmiQueryAllDataA
WmiQueryAllDataMultipleA WmiQueryAllDataMultipleW WmiQueryAllDataW
WmiQueryGuidInformation WmiQuerySingleInstanceA
WmiQuerySingleInstanceMultipleA WmiQuerySingleInstanceMultipleW
WmiQuerySingleInstanceW WmiReceiveNotificationsA WmiReceiveNotificationsW
WmiSetSingleInstanceA WmiSetSingleInstanceW WmiSetSingleItemA
WmiSetSingleItemW Wow64Win32ApiEntry WriteEncryptedFileRaw
""",
    "winhttp.dll": """
DllCanUnloadNow DllGetClassObject DllRegisterServer DllUnregisterServer
WinHttpAddRequestHeaders WinHttpCheckPlatform WinHttpCloseHandle
WinHttpConnect WinHttpCrackUrl WinHttpCreateUrl
WinHttpDetectAutoProxyConfigUrl WinHttpGetDefaultProxyConfiguration
WinHttpGetIEProxyConfigForCurrentUser WinHttpGetProxyForUrl WinHttpOpen
WinHttpOpenRequest WinHttpQueryAuthSchemes WinHttpQueryDataAvailable
WinHttpQueryHeaders WinHttpQueryOption WinHttpReadData
WinHttpReceiveResponse WinHttpSendRequest WinHttpSetCredentials
WinHttpSetDefaultProxyConfiguration WinHttpSetOption
WinHttpSetStatusCallback WinHttpSetTimeouts WinHttpTimeFromSystemTime
WinHttpTimeToSystemTime WinHttpWriteData
""",
}


if __name__ == "__main__":
    sys.exit(main())
