/*++
Module Name:
    driver.c

Abstract:
    MusicKiller kernel driver (Windows 10 x64, WDM).

    Blocks NetEase Cloud Music (cloudmusic.exe) and QQ Music (QQMusic.exe)
    from running.  If the file C:\allow.txt exists, the processes are
    allowed to run; otherwise process creation is rejected and any
    already-running target processes are terminated at driver load.

    The process-creation blocking technique is based on the Microsoft
    Windows-driver-samples sample "ObCallbackTest" (general/obcallback),
    which uses PsSetCreateProcessNotifyRoutineEx() to reject process
    creation by setting CreateInfo->CreationStatus.

    NOTE: drivers that call PsSetCreateProcessNotifyRoutineEx must be
    linked with /INTEGRITYCHECK, otherwise registration fails with
    STATUS_ACCESS_DENIED.
--*/

#include <ntifs.h>

#define MK_POOL_TAG  'liKM'   /* "MKil" */

DRIVER_UNLOAD MkDriverUnload;

/* Bypass switch: if this file exists, nothing is blocked. */
static const UNICODE_STRING g_AllowFilePath =
    RTL_CONSTANT_STRING(L"\\??\\C:\\allow.txt");

/* Target image name prefixes (case-insensitive).  Prefix matching covers
   the main executables and their helper processes:
     cloudmusic.exe / cloudmusic_reporter.exe ...  (NetEase Cloud Music)
     QQMusic.exe    / QQMusicExternal.exe   ...  (QQ Music)
     qmbrowser.exe                               (QQ Music embedded browser) */
static PCWSTR g_TargetPrefixes[] = {
    L"cloudmusic",
    L"qqmusic",
    L"qmbrowser",
};

/* ------------------------------------------------------------------------ */
static BOOLEAN
MkIsTargetImageName(
    _In_ PUNICODE_STRING ImagePath
    )
/*++
    Returns TRUE when the file name part of ImagePath matches one of the
    target image names (case-insensitive).  ImagePath may be a full path
    (from the process notify callback) or a bare file name (from the
    process enumeration sweep).
--*/
{
    USHORT chars;
    USHORT start;
    USHORT i;
    UNICODE_STRING fileName;

    if (ImagePath == NULL || ImagePath->Buffer == NULL || ImagePath->Length == 0) {
        return FALSE;
    }

    chars = ImagePath->Length / sizeof(WCHAR);
    start = 0;
    for (i = chars; i > 0; --i) {
        if (ImagePath->Buffer[i - 1] == L'\\') {
            start = i;
            break;
        }
    }

    fileName.Buffer = &ImagePath->Buffer[start];
    fileName.Length = (USHORT)((chars - start) * sizeof(WCHAR));
    fileName.MaximumLength = fileName.Length;

    for (i = 0; i < ARRAYSIZE(g_TargetPrefixes); ++i) {
        UNICODE_STRING prefix;
        RtlInitUnicodeString(&prefix, g_TargetPrefixes[i]);
        if (fileName.Length >= prefix.Length &&
            RtlPrefixUnicodeString(&prefix, &fileName, TRUE)) {
            return TRUE;
        }
    }
    return FALSE;
}

/* ------------------------------------------------------------------------ */
static BOOLEAN
MkAllowFileExists(
    VOID
    )
/*++
    Returns TRUE when C:\allow.txt exists.
    Must be called at PASSIVE_LEVEL (the process notify callback and
    DriverEntry both run at PASSIVE_LEVEL).
--*/
{
    NTSTATUS status;
    HANDLE handle;
    IO_STATUS_BLOCK ioStatus;
    OBJECT_ATTRIBUTES objectAttributes;

    InitializeObjectAttributes(&objectAttributes,
                               (PUNICODE_STRING)&g_AllowFilePath,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);

    status = ZwCreateFile(&handle,
                          FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                          &objectAttributes,
                          &ioStatus,
                          NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          FILE_OPEN,
                          FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NON_ALERT,
                          NULL,
                          0);

    if (NT_SUCCESS(status)) {
        ZwClose(handle);
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------------ */
static VOID
MkProcessNotifyEx(
    _In_ PEPROCESS Process,
    _In_ HANDLE ProcessId,
    _In_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
    )
/*++
    Process creation/exit callback.  When a target process is being
    created and C:\allow.txt does not exist, creation is denied.
--*/
{
    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(ProcessId);

    if (CreateInfo == NULL) {
        /* Process exit notification: nothing to do. */
        return;
    }

    if (CreateInfo->FileOpenNameAvailable == FALSE ||
        CreateInfo->ImageFileName == NULL) {
        return;
    }

    if (!MkIsTargetImageName(CreateInfo->ImageFileName)) {
        return;
    }

    if (MkAllowFileExists()) {
        KdPrint(("MusicKiller: allow.txt present, allowing %wZ\n",
                 CreateInfo->ImageFileName));
        return;
    }

    KdPrint(("MusicKiller: blocking process creation: %wZ\n",
             CreateInfo->ImageFileName));
    CreateInfo->CreationStatus = STATUS_ACCESS_DENIED;
}

/* ------------------------------------------------------------------------ */
/* Minimal layout of SYSTEM_PROCESS_INFORMATION (info class 5).            */
typedef struct _MK_SYSTEM_PROCESS_INFORMATION {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    KPRIORITY BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
} MK_SYSTEM_PROCESS_INFORMATION, *PMK_SYSTEM_PROCESS_INFORMATION;

#define MK_SystemProcessInformation ((SYSTEM_INFORMATION_CLASS)5)

/* ------------------------------------------------------------------------ */
static VOID
MkTerminateProcessById(
    _In_ HANDLE ProcessId
    )
{
    NTSTATUS status;
    HANDLE processHandle;
    OBJECT_ATTRIBUTES objectAttributes;
    CLIENT_ID clientId;

    InitializeObjectAttributes(&objectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    clientId.UniqueProcess = ProcessId;
    clientId.UniqueThread = NULL;

    status = ZwOpenProcess(&processHandle, PROCESS_TERMINATE, &objectAttributes, &clientId);
    if (NT_SUCCESS(status)) {
        ZwTerminateProcess(processHandle, STATUS_ACCESS_DENIED);
        ZwClose(processHandle);
    } else {
        KdPrint(("MusicKiller: ZwOpenProcess(pid %p) failed 0x%08X\n",
                 ProcessId, status));
    }
}

/* ------------------------------------------------------------------------ */
static VOID
MkSweepRunningTargets(
    VOID
    )
/*++
    Terminates already-running target processes.  Called once from
    DriverEntry so targets that were started before the driver loaded
    are killed immediately (unless allow.txt exists).
--*/
{
    NTSTATUS status;
    PVOID buffer;
    ULONG bufferSize;
    ULONG returnLength;
    ULONG attempt;
    PMK_SYSTEM_PROCESS_INFORMATION entry;

    if (MkAllowFileExists()) {
        KdPrint(("MusicKiller: allow.txt present, load-time sweep skipped\n"));
        return;
    }

    buffer = NULL;
    bufferSize = 64 * 1024;
    status = STATUS_INFO_LENGTH_MISMATCH;

    for (attempt = 0;
         attempt < 4 && status == STATUS_INFO_LENGTH_MISMATCH;
         ++attempt) {

#if (NTDDI_VERSION >= NTDDI_WIN10_VB)
        buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, bufferSize, MK_POOL_TAG);
#else
        buffer = ExAllocatePoolWithTag(NonPagedPool, bufferSize, MK_POOL_TAG);
#endif
        if (buffer == NULL) {
            return;
        }

        returnLength = 0;
        status = ZwQuerySystemInformation(MK_SystemProcessInformation,
                                          buffer, bufferSize, &returnLength);
        if (status == STATUS_INFO_LENGTH_MISMATCH) {
            ExFreePoolWithTag(buffer, MK_POOL_TAG);
            buffer = NULL;
            if (returnLength > bufferSize) {
                bufferSize = returnLength + 64 * 1024;
            } else {
                bufferSize += 64 * 1024;
            }
        }
    }

    if (!NT_SUCCESS(status) || buffer == NULL) {
        if (buffer != NULL) {
            ExFreePoolWithTag(buffer, MK_POOL_TAG);
        }
        return;
    }

    entry = (PMK_SYSTEM_PROCESS_INFORMATION)buffer;
    for (;;) {
        if (entry->ImageName.Buffer != NULL &&
            MkIsTargetImageName(&entry->ImageName)) {
            KdPrint(("MusicKiller: terminating running process %wZ (pid %p)\n",
                     &entry->ImageName, entry->UniqueProcessId));
            MkTerminateProcessById(entry->UniqueProcessId);
        }

        if (entry->NextEntryOffset == 0) {
            break;
        }
        entry = (PMK_SYSTEM_PROCESS_INFORMATION)((PUCHAR)entry + entry->NextEntryOffset);
    }

    ExFreePoolWithTag(buffer, MK_POOL_TAG);
}

/* ------------------------------------------------------------------------ */
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverUnload = MkDriverUnload;

    status = PsSetCreateProcessNotifyRoutineEx(MkProcessNotifyEx, FALSE);
    if (!NT_SUCCESS(status)) {
        KdPrint(("MusicKiller: PsSetCreateProcessNotifyRoutineEx failed 0x%08X\n",
                 status));
        return status;
    }

    KdPrint(("MusicKiller: driver loaded\n"));

    MkSweepRunningTargets();
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------ */
VOID
MkDriverUnload(
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    UNREFERENCED_PARAMETER(DriverObject);

    PsSetCreateProcessNotifyRoutineEx(MkProcessNotifyEx, TRUE);
    KdPrint(("MusicKiller: driver unloaded\n"));
}
