#pragma once

#include <ntifs.h>

extern "C"
NTKERNELAPI
NTSTATUS
NTAPI
MmCopyVirtualMemory(
    _In_  PEPROCESS       SourceProcess,
    _In_  PVOID           SourceAddress,
    _In_  PEPROCESS       TargetProcess,
    _Out_ PVOID           TargetAddress,
    _In_  SIZE_T          BufferSize,
    _In_  KPROCESSOR_MODE PreviousMode,
    _Out_ PSIZE_T         ReturnSize);

extern "C"
NTKERNELAPI
PVOID
NTAPI
PsGetProcessWow64Process(
    _In_ PEPROCESS Process);

extern "C"
NTKERNELAPI
PPEB
NTAPI
PsGetProcessPeb(
    _In_ PEPROCESS Process);

extern "C"
NTSYSAPI
NTSTATUS
NTAPI
ZwProtectVirtualMemory(
    _In_    HANDLE  ProcessHandle,
    _Inout_ PVOID*  BaseAddress,
    _Inout_ SIZE_T* RegionSize,
    _In_    ULONG   NewProtect,
    _Out_   PULONG  OldProtect);
