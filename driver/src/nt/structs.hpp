#pragma once

#include "functions.hpp"

// --- Native (matches the process's own bitness) -----------------------------

typedef struct _LDR_DATA_TABLE_ENTRY64
{
    LIST_ENTRY     InLoadOrderLinks;
    LIST_ENTRY     InMemoryOrderLinks;
    LIST_ENTRY     InInitializationOrderLinks;
    PVOID          DllBase;
    PVOID          EntryPoint;
    ULONG          SizeOfImage;
    UNICODE_STRING FullDllName;
} LDR_DATA_TABLE_ENTRY64;

typedef struct _PEB_LDR_DATA64
{
    ULONG      Length;
    ULONG      Initialized;
    PVOID      SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
} PEB_LDR_DATA64;

// --- WOW64 (32-bit target on 64-bit Windows) --------------------------------
// LIST_ENTRY32 and UNICODE_STRING32 (ULONG Flink/Blink; USHORT+USHORT+ULONG) are
// already declared in <ntdef.h> - reused here rather than redefined.

typedef struct _LDR_DATA_TABLE_ENTRY32
{
    LIST_ENTRY32     InLoadOrderLinks;
    LIST_ENTRY32     InMemoryOrderLinks;
    LIST_ENTRY32     InInitializationOrderLinks;
    ULONG            DllBase;
    ULONG            EntryPoint;
    ULONG            SizeOfImage;
    UNICODE_STRING32 FullDllName;
} LDR_DATA_TABLE_ENTRY32;

typedef struct _PEB_LDR_DATA32
{
    ULONG        Length;
    ULONG        Initialized;
    ULONG        SsHandle;
    LIST_ENTRY32 InLoadOrderModuleList;
    LIST_ENTRY32 InMemoryOrderModuleList;
} PEB_LDR_DATA32;
