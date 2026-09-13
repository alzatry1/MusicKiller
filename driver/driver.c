/*++
Module Name:
    driver.c

Abstract:
    MusicKiller kernel driver (Windows 10 x64, WDM).

    Blocks NetEase Cloud Music and QQ Music from running.  If the file
    C:\allow.txt exists, the processes are allowed; otherwise process
    creation is rejected with STATUS_ACCESS_DENIED.

    The process-creation blocking technique is based on the Microsoft
    Windows-driver-samples sample "ObCallbackTest" (general/obcallback),
    which uses PsSetCreateProcessNotifyRoutineEx() to reject process
    creation by setting CreateInfo->CreationStatus.

    Targets are matched by image file name prefix (case-insensitive):
      cloudmusic*   NetEase Cloud Music  (cloudmusic.exe, helpers)
      qqmusic*      QQ Music             (QQMusic.exe, QQMusicExternal.exe, ...)
      qmbrowser*    QQ Music embedded browser (qmbrowser.exe)

    NOTE: drivers that call PsSetCreateProcessNotifyRoutineEx must be
    linked with /INTEGRITYCHECK, otherwise registration fails with
    STATUS_ACCESS_DENIED.
--*/

#include <ntddk.h>

DRIVER_UNLOAD MkDriverUnload;

/* Bypass switch: if this file exists, nothing is blocked. */
static const UNICODE_STRING g_AllowFilePath =
    RTL_CONSTANT_STRING(L"\\??\\C:\\allow.txt");

/* Target image name prefixes (case-insensitive). */
static PCWSTR g_TargetPrefixes[] = {
    L"cloudmusic",   /* NetEase Cloud Music: cloudmusic.exe, cloudmusic_reporter.exe, ... */
    L"qqmusic",      /* QQ Music: QQMusic.exe, QQMusicExternal.exe, ... */
    L"qmbrowser",    /* QQ Music embedded browser: qmbrowser.exe */
};

/* ------------------------------------------------------------------------ */
static BOOLEAN
MkIsTargetImageName(
    _In_ PCUNICODE_STRING ImagePath
    )
/*++
    Returns TRUE when the file name part of ImagePath starts with one of
    the target prefixes (case-insensitive).  ImagePath is the full image
    path supplied by the process notify callback.
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

    fileName.Buffer = (PWSTR)(ImagePath->Buffer + start);   /* never written through */
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
    Must be called at PASSIVE_LEVEL (the process notify callback runs at
    PASSIVE_LEVEL in the context of the thread creating the process).
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
                          FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
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
