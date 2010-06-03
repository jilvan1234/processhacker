/*
 * Process Hacker - 
 *   thread provider
 * 
 * Copyright (C) 2010 wj32
 * 
 * This file is part of Process Hacker.
 * 
 * Process Hacker is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Process Hacker is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Process Hacker.  If not, see <http://www.gnu.org/licenses/>.
 */

#define THRDPRV_PRIVATE
#include <phapp.h>
#include <kph.h>

typedef struct _PH_THREAD_QUERY_DATA
{
    SLIST_ENTRY ListEntry;
    PPH_THREAD_PROVIDER ThreadProvider;
    PPH_THREAD_ITEM ThreadItem;

    PPH_STRING StartAddressString;
    PH_SYMBOL_RESOLVE_LEVEL StartAddressResolveLevel;

    PPH_STRING ServiceName;
} PH_THREAD_QUERY_DATA, *PPH_THREAD_QUERY_DATA;

typedef struct _PH_THREAD_SYMBOL_LOAD_CONTEXT
{
    HANDLE ProcessId;
    PPH_THREAD_PROVIDER ThreadProvider;
    PPH_SYMBOL_PROVIDER SymbolProvider;
} PH_THREAD_SYMBOL_LOAD_CONTEXT, *PPH_THREAD_SYMBOL_LOAD_CONTEXT;

VOID NTAPI PhpThreadProviderDeleteProcedure(
    __in PVOID Object,
    __in ULONG Flags
    );

NTSTATUS PhpThreadProviderLoadSymbols(
    __in PVOID Parameter
    );

VOID NTAPI PhpThreadItemDeleteProcedure(
    __in PVOID Object,
    __in ULONG Flags
    );

BOOLEAN NTAPI PhpThreadHashtableCompareFunction(
    __in PVOID Entry1,
    __in PVOID Entry2
    );

ULONG NTAPI PhpThreadHashtableHashFunction(
    __in PVOID Entry
    );

PPH_OBJECT_TYPE PhThreadProviderType;
PPH_OBJECT_TYPE PhThreadItemType;

PH_WORK_QUEUE PhThreadProviderWorkQueue;
PH_INITONCE PhThreadProviderWorkQueueInitOnce = PH_INITONCE_INIT;

BOOLEAN PhThreadProviderInitialization()
{
    if (!NT_SUCCESS(PhCreateObjectType(
        &PhThreadProviderType,
        L"ThreadProvider",
        0,
        PhpThreadProviderDeleteProcedure
        )))
        return FALSE;

    if (!NT_SUCCESS(PhCreateObjectType(
        &PhThreadItemType,
        L"ThreadItem",
        0,
        PhpThreadItemDeleteProcedure
        )))
        return FALSE;

    return TRUE;
}

VOID PhpQueueThreadWorkQueueItem(
    __in PTHREAD_START_ROUTINE Function,
    __in PVOID Context
    )
{
    if (PhBeginInitOnce(&PhThreadProviderWorkQueueInitOnce))
    {
        PhInitializeWorkQueue(&PhThreadProviderWorkQueue, 0, 1, 1000);
        PhEndInitOnce(&PhThreadProviderWorkQueueInitOnce);
    }

    PhQueueWorkQueueItem(&PhThreadProviderWorkQueue, Function, Context);
}

PPH_THREAD_PROVIDER PhCreateThreadProvider(
    __in HANDLE ProcessId
    )
{
    PPH_THREAD_PROVIDER threadProvider;

    if (!NT_SUCCESS(PhCreateObject(
        &threadProvider,
        sizeof(PH_THREAD_PROVIDER),
        0,
        PhThreadProviderType,
        0
        )))
        return NULL;

    threadProvider->ThreadHashtable = PhCreateHashtable(
        sizeof(PPH_THREAD_ITEM),
        PhpThreadHashtableCompareFunction,
        PhpThreadHashtableHashFunction,
        20
        );
    PhInitializeFastLock(&threadProvider->ThreadHashtableLock);

    PhInitializeCallback(&threadProvider->ThreadAddedEvent);
    PhInitializeCallback(&threadProvider->ThreadModifiedEvent);
    PhInitializeCallback(&threadProvider->ThreadRemovedEvent);
    PhInitializeCallback(&threadProvider->UpdatedEvent);
    PhInitializeCallback(&threadProvider->LoadingStateChangedEvent);

    threadProvider->ProcessId = ProcessId;
    threadProvider->SymbolProvider = PhCreateSymbolProvider(ProcessId);

    if (threadProvider->SymbolProvider)
    {
        if (threadProvider->SymbolProvider->IsRealHandle)
            threadProvider->ProcessHandle = threadProvider->SymbolProvider->ProcessHandle;
    }

    PhInitializeEvent(&threadProvider->SymbolsLoadedEvent);
    threadProvider->SymbolsLoading = 0;
    RtlInitializeSListHead(&threadProvider->QueryListHead);

    // Begin loading symbols for the process' modules.
    PhReferenceObject(threadProvider);
    PhpQueueThreadWorkQueueItem(PhpThreadProviderLoadSymbols, threadProvider);

    return threadProvider;
}

VOID PhpThreadProviderDeleteProcedure(
    __in PVOID Object,
    __in ULONG Flags
    )
{
    PPH_THREAD_PROVIDER threadProvider = (PPH_THREAD_PROVIDER)Object;

    // Dereference all thread items (we referenced them 
    // when we added them to the hashtable).
    PhDereferenceAllThreadItems(threadProvider);

    PhDereferenceObject(threadProvider->ThreadHashtable);
    PhDeleteFastLock(&threadProvider->ThreadHashtableLock);
    PhDeleteCallback(&threadProvider->ThreadAddedEvent);
    PhDeleteCallback(&threadProvider->ThreadModifiedEvent);
    PhDeleteCallback(&threadProvider->ThreadRemovedEvent);
    PhDeleteCallback(&threadProvider->UpdatedEvent);
    PhDeleteCallback(&threadProvider->LoadingStateChangedEvent);

    // Destroy all queue items.
    {
        PSLIST_ENTRY entry;
        PPH_THREAD_QUERY_DATA data;

        entry = RtlInterlockedFlushSList(&threadProvider->QueryListHead);

        while (entry)
        {
            data = CONTAINING_RECORD(entry, PH_THREAD_QUERY_DATA, ListEntry);
            entry = entry->Next;

            if (data->StartAddressString) PhDereferenceObject(data->StartAddressString);
            if (data->ServiceName) PhDereferenceObject(data->ServiceName);
            PhDereferenceObject(data->ThreadItem);
            PhFree(data);
        }
    }

    // We don't close the process handle because it is owned by 
    // the symbol provider.
    if (threadProvider->SymbolProvider) PhDereferenceObject(threadProvider->SymbolProvider);
}

static BOOLEAN LoadSymbolsEnumGenericModulesCallback(
    __in PPH_MODULE_INFO Module,
    __in PVOID Context
    )
{
    PPH_THREAD_SYMBOL_LOAD_CONTEXT context = Context;
    PPH_SYMBOL_PROVIDER symbolProvider = context->SymbolProvider;

    // If we're loading kernel module symbols for a process other than 
    // System, ignore modules which are in user space. This may happen 
    // in Windows 7.
    if (
        context->ProcessId == SYSTEM_PROCESS_ID &&
        context->ThreadProvider->ProcessId != SYSTEM_PROCESS_ID &&
        (ULONG_PTR)Module->BaseAddress <= PhSystemBasicInformation.MaximumUserModeAddress
        )
        return TRUE;

    PhSymbolProviderLoadModule(
        symbolProvider,
        Module->FileName->Buffer,
        (ULONG64)Module->BaseAddress,
        Module->Size
        );

    return TRUE;
}

static BOOLEAN LoadBasicSymbolsEnumGenericModulesCallback(
    __in PPH_MODULE_INFO Module,
    __in PVOID Context
    )
{
    PPH_THREAD_SYMBOL_LOAD_CONTEXT context = Context;
    PPH_SYMBOL_PROVIDER symbolProvider = context->SymbolProvider;

    if (
        PhStringEquals2(Module->Name, L"ntdll.dll", TRUE) ||
        PhStringEquals2(Module->Name, L"kernel32.dll", TRUE)
        )
    {
        PhSymbolProviderLoadModule(
            symbolProvider,
            Module->FileName->Buffer,
            (ULONG64)Module->BaseAddress,
            Module->Size
            );
    }

    return TRUE;
}

NTSTATUS PhpThreadProviderLoadSymbols(
    __in PVOID Parameter
    )
{
    PPH_THREAD_PROVIDER threadProvider = (PPH_THREAD_PROVIDER)Parameter;
    PH_THREAD_SYMBOL_LOAD_CONTEXT loadContext;

    loadContext.ThreadProvider = threadProvider;
    loadContext.SymbolProvider = threadProvider->SymbolProvider;

    if (threadProvider->ProcessId != SYSTEM_IDLE_PROCESS_ID)
    {
        if (
            threadProvider->SymbolProvider->IsRealHandle ||
            threadProvider->ProcessId == SYSTEM_PROCESS_ID
            )
        {
            loadContext.ProcessId = threadProvider->ProcessId;
            PhEnumGenericModules(
                threadProvider->ProcessId,
                threadProvider->SymbolProvider->ProcessHandle,
                0,
                LoadSymbolsEnumGenericModulesCallback,
                &loadContext
                );
        }
        else
        {
            // We can't enumerate the process modules. Load 
            // symbols for ntdll.dll and kernel32.dll.
            loadContext.ProcessId = NtCurrentProcessId();
            PhEnumGenericModules(
                NtCurrentProcessId(),
                NtCurrentProcess(),
                0,
                LoadBasicSymbolsEnumGenericModulesCallback,
                &loadContext
                );
        }

        // Load kernel module symbols as well.
        if (threadProvider->ProcessId != SYSTEM_PROCESS_ID)
        {
            loadContext.ProcessId = SYSTEM_PROCESS_ID;
            PhEnumGenericModules(
                SYSTEM_PROCESS_ID,
                NULL,
                0,
                LoadSymbolsEnumGenericModulesCallback,
                &loadContext
                );
        }
    }
    else
    {
        // System Idle Process has one thread for each CPU,
        // each having a start address at KiIdleLoop. We 
        // need to load symbols for the kernel.

        PRTL_PROCESS_MODULES kernelModules;

        if (NT_SUCCESS(PhEnumKernelModules(&kernelModules)))
        {
            if (kernelModules->NumberOfModules > 0)
            {
                PPH_STRING fileName;
                PPH_STRING newFileName;

                fileName = PhCreateStringFromAnsi(kernelModules->Modules[0].FullPathName);
                newFileName = PhGetFileName(fileName);
                PhDereferenceObject(fileName);

                PhSymbolProviderLoadModule(
                    threadProvider->SymbolProvider,
                    newFileName->Buffer,
                    (ULONG64)kernelModules->Modules[0].ImageBase,
                    kernelModules->Modules[0].ImageSize
                    );
                PhDereferenceObject(newFileName);
            }

            PhFree(kernelModules);
        }
    }

    // Check if the process has services - we'll need to know before getting service tag/name 
    // information.
    if (WINDOWS_HAS_SERVICE_TAGS)
    {
        PPH_PROCESS_ITEM processItem;

        if (processItem = PhReferenceProcessItem(threadProvider->ProcessId))
        {
            threadProvider->HasServices = processItem->ServiceList->Count != 0;
            PhDereferenceObject(processItem);
        }
    }

    PhSetEvent(&threadProvider->SymbolsLoadedEvent);

    PhDereferenceObject(threadProvider);

    return STATUS_SUCCESS;
}

PPH_THREAD_ITEM PhCreateThreadItem(
    __in HANDLE ThreadId
    )
{
    PPH_THREAD_ITEM threadItem;

    if (!NT_SUCCESS(PhCreateObject(
        &threadItem,
        sizeof(PH_THREAD_ITEM),
        0,
        PhThreadItemType,
        0
        )))
        return NULL;

    memset(threadItem, 0, sizeof(PH_THREAD_ITEM));
    threadItem->ThreadId = ThreadId;
    PhPrintUInt32(threadItem->ThreadIdString, (ULONG)ThreadId);

    return threadItem;
}

VOID PhpThreadItemDeleteProcedure(
    __in PVOID Object,
    __in ULONG Flags
    )
{
    PPH_THREAD_ITEM threadItem = (PPH_THREAD_ITEM)Object;

    if (threadItem->ThreadHandle) NtClose(threadItem->ThreadHandle);
    if (threadItem->StartAddressString) PhDereferenceObject(threadItem->StartAddressString);
    if (threadItem->StartAddressFileName) PhDereferenceObject(threadItem->StartAddressFileName);
    if (threadItem->PriorityWin32String) PhDereferenceObject(threadItem->PriorityWin32String);
    if (threadItem->ServiceName) PhDereferenceObject(threadItem->ServiceName);
    if (threadItem->ContextSwitchesDeltaString) PhDereferenceObject(threadItem->ContextSwitchesDeltaString);
    if (threadItem->CyclesDeltaString) PhDereferenceObject(threadItem->CyclesDeltaString);
}

BOOLEAN PhpThreadHashtableCompareFunction(
    __in PVOID Entry1,
    __in PVOID Entry2
    )
{
    return
        (*(PPH_THREAD_ITEM *)Entry1)->ThreadId ==
        (*(PPH_THREAD_ITEM *)Entry2)->ThreadId;
}

ULONG PhpThreadHashtableHashFunction(
    __in PVOID Entry
    )
{
    return (ULONG)(*(PPH_THREAD_ITEM *)Entry)->ThreadId / 4;
}

PPH_THREAD_ITEM PhReferenceThreadItem(
    __in PPH_THREAD_PROVIDER ThreadProvider,
    __in HANDLE ThreadId
    )
{
    PH_THREAD_ITEM lookupThreadItem;
    PPH_THREAD_ITEM lookupThreadItemPtr = &lookupThreadItem;
    PPH_THREAD_ITEM *threadItemPtr;
    PPH_THREAD_ITEM threadItem;

    lookupThreadItem.ThreadId = ThreadId;

    PhAcquireFastLockShared(&ThreadProvider->ThreadHashtableLock);

    threadItemPtr = (PPH_THREAD_ITEM *)PhGetHashtableEntry(
        ThreadProvider->ThreadHashtable,
        &lookupThreadItemPtr
        );

    if (threadItemPtr)
    {
        threadItem = *threadItemPtr;
        PhReferenceObject(threadItem);
    }
    else
    {
        threadItem = NULL;
    }

    PhReleaseFastLockShared(&ThreadProvider->ThreadHashtableLock);

    return threadItem;
}

VOID PhDereferenceAllThreadItems(
    __in PPH_THREAD_PROVIDER ThreadProvider
    )
{
    ULONG enumerationKey = 0;
    PPH_THREAD_ITEM *threadItem;

    PhAcquireFastLockExclusive(&ThreadProvider->ThreadHashtableLock);

    while (PhEnumHashtable(ThreadProvider->ThreadHashtable, (PPVOID)&threadItem, &enumerationKey))
    {
        PhDereferenceObject(*threadItem);
    }

    PhReleaseFastLockExclusive(&ThreadProvider->ThreadHashtableLock);
}

__assumeLocked VOID PhpRemoveThreadItem(
    __in PPH_THREAD_PROVIDER ThreadProvider,
    __in PPH_THREAD_ITEM ThreadItem
    )
{
    PhRemoveHashtableEntry(ThreadProvider->ThreadHashtable, &ThreadItem);
    PhDereferenceObject(ThreadItem);
}

NTSTATUS PhpThreadQueryWorker(
    __in PVOID Parameter
    )
{
    PPH_THREAD_QUERY_DATA data = (PPH_THREAD_QUERY_DATA)Parameter;
    LONG newSymbolsLoading;

    newSymbolsLoading = _InterlockedIncrement(&data->ThreadProvider->SymbolsLoading);

    if (newSymbolsLoading == 1)
        PhInvokeCallback(&data->ThreadProvider->LoadingStateChangedEvent, (PVOID)TRUE);

    // We can't resolve the start address until symbols have 
    // been loaded.
    PhWaitForEvent(&data->ThreadProvider->SymbolsLoadedEvent, INFINITE);

    data->StartAddressString = PhGetSymbolFromAddress(
        data->ThreadProvider->SymbolProvider,
        data->ThreadItem->StartAddress,
        &data->StartAddressResolveLevel,
        &data->ThreadItem->StartAddressFileName,
        NULL,
        NULL
        );

    newSymbolsLoading = _InterlockedDecrement(&data->ThreadProvider->SymbolsLoading);

    if (newSymbolsLoading == 0)
        PhInvokeCallback(&data->ThreadProvider->LoadingStateChangedEvent, (PVOID)FALSE);

    // Get the service tag, and the service name.
    if (
        WINDOWS_HAS_SERVICE_TAGS &&
        data->ThreadProvider->SymbolProvider->IsRealHandle &&
        data->ThreadItem->ThreadHandle
        )
    {
        PVOID serviceTag;

        if (NT_SUCCESS(PhGetThreadServiceTag(
            data->ThreadItem->ThreadHandle,
            data->ThreadProvider->ProcessHandle,
            &serviceTag
            )))
        {
            data->ServiceName = PhGetServiceNameFromTag(
                data->ThreadProvider->ProcessId,
                serviceTag
                );
        }
    }

    RtlInterlockedPushEntrySList(&data->ThreadProvider->QueryListHead, &data->ListEntry);

    PhDereferenceObject(data->ThreadProvider);

    return STATUS_SUCCESS;
}

VOID PhpQueueThreadQuery(
    __in PPH_THREAD_PROVIDER ThreadProvider,
    __in PPH_THREAD_ITEM ThreadItem
    )
{
    PPH_THREAD_QUERY_DATA data;

    data = PhAllocate(sizeof(PH_THREAD_QUERY_DATA));
    memset(data, 0, sizeof(PH_THREAD_QUERY_DATA));
    data->ThreadProvider = ThreadProvider;
    data->ThreadItem = ThreadItem;

    PhReferenceObject(ThreadProvider);
    PhReferenceObject(ThreadItem);
    PhpQueueThreadWorkQueueItem(PhpThreadQueryWorker, data);
}

PPH_STRING PhpGetThreadBasicStartAddress(
    __in PPH_THREAD_PROVIDER ThreadProvider,
    __in ULONG64 Address,
    __out PPH_SYMBOL_RESOLVE_LEVEL ResolveLevel
    )
{
    ULONG64 modBase;
    PPH_STRING fileName = NULL;
    PPH_STRING baseName = NULL;
    PPH_STRING symbol;

    modBase = PhGetModuleFromAddress(
        ThreadProvider->SymbolProvider,
        Address,
        &fileName
        );

    if (fileName == NULL)
    {
        *ResolveLevel = PhsrlAddress;

        symbol = PhCreateStringEx(NULL, PH_PTR_STR_LEN * 2);
        PhPrintPointer(symbol->Buffer, (PVOID)Address);
        PhTrimStringToNullTerminator(symbol);
    }
    else
    {
        baseName = PhGetBaseName(fileName);
        *ResolveLevel = PhsrlModule;

        symbol = PhFormatString(L"%s+0x%Ix", baseName->Buffer, (PVOID)(Address - modBase));
    }

    if (fileName)
        PhDereferenceObject(fileName);
    if (baseName)
        PhDereferenceObject(baseName);

    return symbol;
}

static NTSTATUS PhpGetThreadCycleTime(
    __in PPH_THREAD_PROVIDER ThreadProvider,
    __in_opt PULARGE_INTEGER IdleThreadCycleTimes,
    __in PPH_THREAD_ITEM ThreadItem,
    __out PULONG64 CycleTime
    )
{
    if (ThreadProvider->ProcessId != SYSTEM_IDLE_PROCESS_ID)
    {
        return PhGetThreadCycleTime(ThreadItem->ThreadHandle, CycleTime);
    }
    else
    {
        if (
            IdleThreadCycleTimes && 
            (ULONG)ThreadItem->ThreadId < (ULONG)PhSystemBasicInformation.NumberOfProcessors
            )
        {
            *CycleTime = IdleThreadCycleTimes[(ULONG)ThreadItem->ThreadId].QuadPart;
            return STATUS_SUCCESS;
        }
    }

    return STATUS_INVALID_PARAMETER;
}

PPH_STRING PhGetThreadPriorityWin32String(
    __in LONG PriorityWin32
    )
{
    switch (PriorityWin32)
    {
    case THREAD_PRIORITY_TIME_CRITICAL:
        return PhCreateString(L"Time Critical");
    case THREAD_PRIORITY_HIGHEST:
        return PhCreateString(L"Highest");
    case THREAD_PRIORITY_ABOVE_NORMAL:
        return PhCreateString(L"Above Normal");
    case THREAD_PRIORITY_NORMAL:
        return PhCreateString(L"Normal");
    case THREAD_PRIORITY_BELOW_NORMAL:
        return PhCreateString(L"Below Normal");
    case THREAD_PRIORITY_LOWEST:
        return PhCreateString(L"Lowest");
    case THREAD_PRIORITY_IDLE:
        return PhCreateString(L"Idle");
    case THREAD_PRIORITY_ERROR_RETURN:
        return NULL;
    default:
        return PhFormatString(L"%d", PriorityWin32);
    }
}

VOID PhThreadProviderUpdate(
    __in PVOID Object
    )
{
    PPH_THREAD_PROVIDER threadProvider = (PPH_THREAD_PROVIDER)Object;
    PVOID processes;
    PSYSTEM_PROCESS_INFORMATION process;
    PSYSTEM_THREAD_INFORMATION threads;
    ULONG numberOfThreads;
    ULONG i;
    PULARGE_INTEGER idleThreadCycleTimes = NULL;

    if (!NT_SUCCESS(PhEnumProcesses(&processes)))
        return;

    process = PhFindProcessInformation(processes, threadProvider->ProcessId);

    if (!process)
    {
        // The process doesn't exist anymore. Pretend it does but 
        // has no threads.
        PhFree(processes);
        processes = PhAllocate(sizeof(SYSTEM_PROCESS_INFORMATION));
        process = (PSYSTEM_PROCESS_INFORMATION)processes;
        process->NumberOfThreads = 0;
    }

    threads = process->Threads;
    numberOfThreads = process->NumberOfThreads;

    // System Idle Process has one thread per CPU. 
    // They all have a TID of 0, but we can't have 
    // multiple TIDs, so we'll assign unique TIDs.
    if (threadProvider->ProcessId == SYSTEM_IDLE_PROCESS_ID)
    {
        for (i = 0; i < numberOfThreads; i++)
        {
            threads[i].ClientId.UniqueThread = (HANDLE)i;
        }

        // Get the cycle times if we're on Vista.
        if (WINDOWS_HAS_THREAD_CYCLES)
        {
            idleThreadCycleTimes = PhAllocate(
                sizeof(ULARGE_INTEGER) * (ULONG)PhSystemBasicInformation.NumberOfProcessors
                );

            if (!NT_SUCCESS(NtQuerySystemInformation(
                SystemProcessorIdleCycleTime,
                idleThreadCycleTimes,
                sizeof(ULARGE_INTEGER) * (ULONG)PhSystemBasicInformation.NumberOfProcessors,
                NULL
                )))
            {
                PhFree(idleThreadCycleTimes);
                idleThreadCycleTimes = NULL;
            }
        }
    }

    // Look for dead threads.
    {
        PPH_LIST threadsToRemove = NULL;
        ULONG enumerationKey = 0;
        PPH_THREAD_ITEM *threadItem;

        while (PhEnumHashtable(threadProvider->ThreadHashtable, (PPVOID)&threadItem, &enumerationKey))
        {
            BOOLEAN found = FALSE;

            // Check if the thread still exists.
            for (i = 0; i < numberOfThreads; i++)
            {
                PSYSTEM_THREAD_INFORMATION thread = &threads[i];

                if ((*threadItem)->ThreadId == thread->ClientId.UniqueThread)
                {
                    found = TRUE;
                    break;
                }
            }

            if (!found)
            {
                // Raise the thread removed event.
                PhInvokeCallback(&threadProvider->ThreadRemovedEvent, *threadItem);

                if (!threadsToRemove)
                    threadsToRemove = PhCreateList(2);

                PhAddListItem(threadsToRemove, *threadItem);
            }
        }

        if (threadsToRemove)
        {
            PhAcquireFastLockExclusive(&threadProvider->ThreadHashtableLock);

            for (i = 0; i < threadsToRemove->Count; i++)
            {
                PhpRemoveThreadItem(
                    threadProvider,
                    (PPH_THREAD_ITEM)threadsToRemove->Items[i]
                    );
            }

            PhReleaseFastLockExclusive(&threadProvider->ThreadHashtableLock);
            PhDereferenceObject(threadsToRemove);
        }
    }

    // Go through the queued thread query data.
    {
        PSLIST_ENTRY entry;
        PPH_THREAD_QUERY_DATA data;

        while (entry = RtlInterlockedPopEntrySList(&threadProvider->QueryListHead))
        {
            data = CONTAINING_RECORD(entry, PH_THREAD_QUERY_DATA, ListEntry);

            if (data->StartAddressResolveLevel == PhsrlFunction && data->StartAddressString)
            {
                PhSwapReference(&data->ThreadItem->StartAddressString, data->StartAddressString);
                data->ThreadItem->StartAddressResolveLevel = data->StartAddressResolveLevel;
            }

            PhSwapReference2(&data->ThreadItem->ServiceName, data->ServiceName);

            data->ThreadItem->JustResolved = TRUE;

            if (data->StartAddressString) PhDereferenceObject(data->StartAddressString);
            PhDereferenceObject(data->ThreadItem);
            PhFree(data);
        }
    }

    // Look for new threads and update existing ones.
    for (i = 0; i < numberOfThreads; i++)
    {
        PSYSTEM_THREAD_INFORMATION thread = &threads[i];
        PPH_THREAD_ITEM threadItem;

        threadItem = PhReferenceThreadItem(threadProvider, thread->ClientId.UniqueThread);

        if (!threadItem)
        {
            PVOID startAddress = NULL;

            threadItem = PhCreateThreadItem(thread->ClientId.UniqueThread);

            threadItem->CreateTime = thread->CreateTime;
            threadItem->KernelTime = thread->KernelTime;
            threadItem->UserTime = thread->UserTime;

            PhUpdateDelta(&threadItem->ContextSwitchesDelta, thread->ContextSwitches);
            threadItem->Priority = thread->Priority;
            threadItem->BasePriority = thread->BasePriority;
            threadItem->State = (KTHREAD_STATE)thread->ThreadState;
            threadItem->WaitReason = thread->WaitReason;

            // Try to open a handle to the thread.
            if (!NT_SUCCESS(PhOpenThread(
                &threadItem->ThreadHandle,
                THREAD_QUERY_INFORMATION,
                threadItem->ThreadId
                )))
            {
                PhOpenThread(
                    &threadItem->ThreadHandle,
                    ThreadQueryAccess,
                    threadItem->ThreadId
                    );
            }

            // Get the cycle count.
            if (WINDOWS_HAS_THREAD_CYCLES)
            {
                ULONG64 cycles;

                if (NT_SUCCESS(PhpGetThreadCycleTime(
                    threadProvider,
                    idleThreadCycleTimes,
                    threadItem,
                    &cycles
                    )))
                {
                    PhUpdateDelta(&threadItem->CyclesDelta, cycles);
                }
            }

            // Try to get the start address.

            if (threadItem->ThreadHandle)
            {
                NtQueryInformationThread(
                    threadItem->ThreadHandle,
                    ThreadQuerySetWin32StartAddress,
                    &startAddress,
                    sizeof(PVOID),
                    NULL
                    );
            }

            if (!startAddress)
                startAddress = thread->StartAddress;

            threadItem->StartAddress = (ULONG64)startAddress;

            // Get the Win32 priority.
            threadItem->PriorityWin32 = GetThreadPriority(threadItem->ThreadHandle);
            threadItem->PriorityWin32String = PhGetThreadPriorityWin32String(threadItem->PriorityWin32);

            if (PhWaitForEvent(&threadProvider->SymbolsLoadedEvent, 0))
            {
                threadItem->StartAddressString = PhpGetThreadBasicStartAddress(
                    threadProvider,
                    threadItem->StartAddress,
                    &threadItem->StartAddressResolveLevel
                    );
            }

            if (!threadItem->StartAddressString)
            {
                threadItem->StartAddressResolveLevel = PhsrlAddress;
                threadItem->StartAddressString = PhCreateStringEx(NULL, PH_PTR_STR_LEN * 2);
                PhPrintPointer(
                    threadItem->StartAddressString->Buffer,
                    (PVOID)threadItem->StartAddress
                    );
                PhTrimStringToNullTerminator(threadItem->StartAddressString);
            }

            PhpQueueThreadQuery(threadProvider, threadItem);

            // Is it a GUI thread?

            if (threadItem->ThreadHandle && PhKphHandle)
            {
                PVOID win32Thread;

                if (NT_SUCCESS(KphGetThreadWin32Thread(
                    PhKphHandle,
                    threadItem->ThreadHandle,
                    &win32Thread
                    )))
                {
                    threadItem->IsGuiThread = win32Thread != NULL;
                }
            }

            // Add the thread item to the hashtable.
            PhAcquireFastLockExclusive(&threadProvider->ThreadHashtableLock);
            PhAddHashtableEntry(threadProvider->ThreadHashtable, &threadItem);
            PhReleaseFastLockExclusive(&threadProvider->ThreadHashtableLock);

            // Raise the thread added event.
            PhInvokeCallback(&threadProvider->ThreadAddedEvent, threadItem);
        }
        else
        {
            BOOLEAN modified = FALSE;

            if (threadItem->JustResolved)
                modified = TRUE;

            threadItem->KernelTime = thread->KernelTime;
            threadItem->UserTime = thread->UserTime;

            threadItem->Priority = thread->Priority;
            threadItem->BasePriority = thread->BasePriority;

            threadItem->State = (KTHREAD_STATE)thread->ThreadState;

            if (threadItem->WaitReason != thread->WaitReason)
            {
                threadItem->WaitReason = thread->WaitReason;
                modified = TRUE;
            }

            // If the resolve level is only at address, it probably 
            // means symbols weren't loaded the last time we 
            // tried to get the start address. Try again.
            if (threadItem->StartAddressResolveLevel == PhsrlAddress)
            {
                if (PhWaitForEvent(&threadProvider->SymbolsLoadedEvent, 0))
                {
                    PPH_STRING newStartAddressString;

                    newStartAddressString = PhpGetThreadBasicStartAddress(
                        threadProvider,
                        threadItem->StartAddress,
                        &threadItem->StartAddressResolveLevel
                        );

                    PhSwapReference2(
                        &threadItem->StartAddressString,
                        newStartAddressString
                        );

                    modified = TRUE;
                }
            }

            // If we couldn't resolve the start address to a 
            // module+offset, use the StartAddress instead 
            // of the Win32StartAddress and try again.
            // Note that we check the resolve level again 
            // because we may have changed it in the previous 
            // block.
            if (
                threadItem->JustResolved &&
                threadItem->StartAddressResolveLevel == PhsrlAddress
                )
            {
                if (threadItem->StartAddress != (ULONG64)thread->StartAddress)
                {
                    threadItem->StartAddress = (ULONG64)thread->StartAddress;
                    PhpQueueThreadQuery(threadProvider, threadItem);
                }
            }

            // Update the context switch count.
            {
                ULONG oldDelta;

                oldDelta = threadItem->ContextSwitchesDelta.Delta;
                PhUpdateDelta(&threadItem->ContextSwitchesDelta, thread->ContextSwitches);

                if (threadItem->ContextSwitchesDelta.Delta != oldDelta)
                {
                    WCHAR deltaString[PH_INT32_STR_LEN_1];

                    if (threadItem->ContextSwitchesDelta.Delta != 0)
                    {
                        PhPrintUInt32(deltaString, threadItem->ContextSwitchesDelta.Delta);
                        PhSwapReference2(
                            &threadItem->ContextSwitchesDeltaString,
                            PhFormatDecimal(deltaString, 0, TRUE)
                            );
                    }
                    else
                    {
                        PhSwapReference2(
                            &threadItem->ContextSwitchesDeltaString,
                            PhCreateString(L"")
                            );
                    }

                    modified = TRUE;
                }
            }

            // Update the cycle count.
            if (WINDOWS_HAS_THREAD_CYCLES)
            {
                ULONG64 cycles;
                ULONG64 oldDelta;

                oldDelta = threadItem->CyclesDelta.Delta;

                if (NT_SUCCESS(PhpGetThreadCycleTime(
                    threadProvider,
                    idleThreadCycleTimes,
                    threadItem,
                    &cycles
                    )))
                {
                    PhUpdateDelta(&threadItem->CyclesDelta, cycles);

                    if (threadItem->CyclesDelta.Delta != oldDelta)
                    {
                        WCHAR deltaString[PH_INT64_STR_LEN_1];

                        if (threadItem->CyclesDelta.Delta != 0)
                        {
                            PhPrintUInt64(deltaString, threadItem->CyclesDelta.Delta);
                            PhSwapReference2(
                                &threadItem->CyclesDeltaString,
                                PhFormatDecimal(deltaString, 0, TRUE)
                                );
                        }
                        else
                        {
                            PhSwapReference2(
                                &threadItem->CyclesDeltaString,
                                PhCreateString(L"")
                                );
                        }

                        modified = TRUE;
                    }
                }
            }

            // Update the Win32 priority.
            {
                LONG oldPriorityWin32 = threadItem->PriorityWin32;

                threadItem->PriorityWin32 = GetThreadPriority(threadItem->ThreadHandle);

                if (threadItem->PriorityWin32 != oldPriorityWin32)
                {
                    PPH_STRING priorityWin32String;

                    priorityWin32String = PhGetThreadPriorityWin32String(threadItem->PriorityWin32);
                    PhSwapReference2(&threadItem->PriorityWin32String, priorityWin32String);

                    modified = TRUE;
                }
            }

            // Update the GUI thread status.

            if (threadItem->ThreadHandle && PhKphHandle)
            {
                PVOID win32Thread;

                if (NT_SUCCESS(KphGetThreadWin32Thread(
                    PhKphHandle,
                    threadItem->ThreadHandle,
                    &win32Thread
                    )))
                {
                    BOOLEAN oldIsGuiThread = threadItem->IsGuiThread;

                    threadItem->IsGuiThread = win32Thread != NULL;

                    if (threadItem->IsGuiThread != oldIsGuiThread)
                        modified = TRUE;
                }
            }

            threadItem->JustResolved = FALSE;

            if (modified)
            {
                // Raise the thread modified event.
                PhInvokeCallback(&threadProvider->ThreadModifiedEvent, threadItem);
            }

            PhDereferenceObject(threadItem);
        }
    }

    PhFree(processes);
    if (idleThreadCycleTimes) PhFree(idleThreadCycleTimes);

    PhInvokeCallback(&threadProvider->UpdatedEvent, NULL);
}
