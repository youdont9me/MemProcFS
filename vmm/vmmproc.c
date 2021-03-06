// vfsproc.c : implementation of functions related to operating system and process parsing of virtual memory.
//
// (c) Ulf Frisk, 2018-2020
// Author: Ulf Frisk, pcileech@frizk.net
//

#include "vmmdll.h"
#include "vmmproc.h"
#include "vmmwin.h"
#include "vmmwininit.h"
#include "vmmwinobj.h"
#include "vmmwinreg.h"
#include "mm_pfn.h"
#include "pluginmanager.h"
#include "statistics.h"
#include "util.h"

// ----------------------------------------------------------------------------
// GENERIC PROCESS RELATED FUNCTIONALITY BELOW:
// ----------------------------------------------------------------------------

/*
* Try initialize from user supplied CR3/PML4 supplied in parameter at startup.
* -- ctx
* -- return
*/
BOOL VmmProcUserCR3TryInitialize64()
{
    PVMM_PROCESS pObProcess;
    VmmInitializeMemoryModel(VMM_MEMORYMODEL_X64);
    pObProcess = VmmProcessCreateEntry(TRUE, 1, 0, 0, ctxMain->cfg.paCR3, 0, "unknown_process", FALSE, NULL, 0);
    VmmProcessCreateFinish();
    if(!pObProcess) {
        vmmprintfv("VmmProc: FAIL: Initialization of Process failed from user-defined CR3 %016llx.\n", ctxMain->cfg.paCR3);
        VmmInitializeMemoryModel(VMM_MEMORYMODEL_NA);
        return FALSE;
    }
    VmmTlbSpider(pObProcess);
    Ob_DECREF(pObProcess);
    ctxVmm->tpSystem = VMM_SYSTEM_UNKNOWN_X64;
    ctxVmm->kernel.paDTB = ctxMain->cfg.paCR3;
    return TRUE;
}

BOOL VmmProc_RefreshProcesses(_In_ BOOL fRefreshTotal)
{
    BOOL result;
    PVMM_PROCESS pObProcessSystem;
    // statistic count
    if(!fRefreshTotal) { InterlockedIncrement64(&ctxVmm->stat.cProcessRefreshPartial); }
    if(fRefreshTotal) { InterlockedIncrement64(&ctxVmm->stat.cProcessRefreshFull); }
    // Single user-defined X64 process
    if(fRefreshTotal) {
        if(ctxVmm->tpSystem == VMM_SYSTEM_UNKNOWN_X64) {
            VmmProcUserCR3TryInitialize64();
        }
    }
    // Windows OS
    if((ctxVmm->tpSystem == VMM_SYSTEM_WINDOWS_X64) || (ctxVmm->tpSystem == VMM_SYSTEM_WINDOWS_X86)) {
        vmmprintfvv_fn("ProcessRefresh: %s\n", (fRefreshTotal ? "Total" : "Partial"));
        pObProcessSystem = VmmProcessGet(4);
        if(!pObProcessSystem) {
            vmmprintf_fn("FAIL - SYSTEM PROCESS NOT FOUND - SHOULD NOT HAPPEN\n");
            return FALSE;
        }
        result = VmmWin_EnumerateEPROCESS(pObProcessSystem, fRefreshTotal);
        Ob_DECREF(pObProcessSystem);
    }
    return TRUE;
}

// Initial hard coded values that seems to be working nicely below. These values
// may be changed in config options or by editing files in the .status directory.

#define VMMPROC_UPDATERTHREAD_LOCAL_PERIOD              100
#define VMMPROC_UPDATERTHREAD_LOCAL_PHYSCACHE           (500 / VMMPROC_UPDATERTHREAD_LOCAL_PERIOD)                // 0.5s
#define VMMPROC_UPDATERTHREAD_LOCAL_TLB                 (5 * 1000 / VMMPROC_UPDATERTHREAD_LOCAL_PERIOD)           // 5s
#define VMMPROC_UPDATERTHREAD_LOCAL_PROC_REFRESHLIST    (5 * 1000 / VMMPROC_UPDATERTHREAD_LOCAL_PERIOD)           // 5s
#define VMMPROC_UPDATERTHREAD_LOCAL_PROC_REFRESHTOTAL   (15 * 1000 / VMMPROC_UPDATERTHREAD_LOCAL_PERIOD)          // 15s
#define VMMPROC_UPDATERTHREAD_LOCAL_REGISTRY            (5 * 60 * 1000 / VMMPROC_UPDATERTHREAD_LOCAL_PERIOD)      // 5m

#define VMMPROC_UPDATERTHREAD_REMOTE_PERIOD             100
#define VMMPROC_UPDATERTHREAD_REMOTE_PHYSCACHE          (15 * 1000 / VMMPROC_UPDATERTHREAD_REMOTE_PERIOD)        // 15s
#define VMMPROC_UPDATERTHREAD_REMOTE_TLB                (3 * 60 * 1000 / VMMPROC_UPDATERTHREAD_REMOTE_PERIOD)    // 3m
#define VMMPROC_UPDATERTHREAD_REMOTE_PROC_REFRESHLIST   (15 * 1000 / VMMPROC_UPDATERTHREAD_REMOTE_PERIOD)        // 15s
#define VMMPROC_UPDATERTHREAD_REMOTE_PROC_REFRESHTOTAL  (3 * 60 * 1000 / VMMPROC_UPDATERTHREAD_REMOTE_PERIOD)    // 3m
#define VMMPROC_UPDATERTHREAD_REMOTE_REGISTRY           (10 * 60 * 1000 / VMMPROC_UPDATERTHREAD_LOCAL_PERIOD)    // 10m

DWORD VmmProcCacheUpdaterThread()
{
    QWORD i = 0, paMax;
    BOOL fPHYS, fTLB, fProcPartial, fProcTotal, fRegistry;
    vmmprintfv("VmmProc: Start periodic cache flushing.\n");
    if(ctxMain->dev.fRemote) {
        ctxVmm->ThreadProcCache.cMs_TickPeriod = VMMPROC_UPDATERTHREAD_REMOTE_PERIOD;
        ctxVmm->ThreadProcCache.cTick_Phys = VMMPROC_UPDATERTHREAD_REMOTE_PHYSCACHE;
        ctxVmm->ThreadProcCache.cTick_TLB = VMMPROC_UPDATERTHREAD_REMOTE_TLB;
        ctxVmm->ThreadProcCache.cTick_ProcPartial = VMMPROC_UPDATERTHREAD_REMOTE_PROC_REFRESHLIST;
        ctxVmm->ThreadProcCache.cTick_ProcTotal = VMMPROC_UPDATERTHREAD_REMOTE_PROC_REFRESHTOTAL;
        ctxVmm->ThreadProcCache.cTick_Registry = VMMPROC_UPDATERTHREAD_REMOTE_REGISTRY;
    } else {
        ctxVmm->ThreadProcCache.cMs_TickPeriod = VMMPROC_UPDATERTHREAD_LOCAL_PERIOD;
        ctxVmm->ThreadProcCache.cTick_Phys = VMMPROC_UPDATERTHREAD_LOCAL_PHYSCACHE;
        ctxVmm->ThreadProcCache.cTick_TLB = VMMPROC_UPDATERTHREAD_LOCAL_TLB;
        ctxVmm->ThreadProcCache.cTick_ProcPartial = VMMPROC_UPDATERTHREAD_LOCAL_PROC_REFRESHLIST;
        ctxVmm->ThreadProcCache.cTick_ProcTotal = VMMPROC_UPDATERTHREAD_LOCAL_PROC_REFRESHTOTAL;
        ctxVmm->ThreadProcCache.cTick_Registry = VMMPROC_UPDATERTHREAD_LOCAL_REGISTRY;
    }
    while(ctxVmm->ThreadProcCache.fEnabled) {
        Sleep(ctxVmm->ThreadProcCache.cMs_TickPeriod);
        i++;
        fTLB = !(i % ctxVmm->ThreadProcCache.cTick_TLB);
        fPHYS = !(i % ctxVmm->ThreadProcCache.cTick_Phys);
        fProcTotal = !(i % ctxVmm->ThreadProcCache.cTick_ProcTotal);
        fProcPartial = !(i % ctxVmm->ThreadProcCache.cTick_ProcPartial) && !fProcTotal;
        fRegistry = !(i % ctxVmm->ThreadProcCache.cTick_Registry);
        EnterCriticalSection(&ctxVmm->MasterLock);
        // PHYS / TLB cache clear
        if(fPHYS) {
            VmmCacheClear(VMM_CACHE_TAG_PHYS);
            InterlockedIncrement64(&ctxVmm->stat.cPhysRefreshCache);
            VmmCacheClear(VMM_CACHE_TAG_PAGING);
            InterlockedIncrement64(&ctxVmm->stat.cPageRefreshCache);
            ObSet_Clear(ctxVmm->Cache.PAGING_FAILED);
        }
        if(fTLB) {
            VmmCacheClear(VMM_CACHE_TAG_TLB);
            InterlockedIncrement64(&ctxVmm->stat.cTlbRefreshCache);
        }
        // refresh proc list
        if(fProcPartial || fProcTotal) {
            if(!VmmProc_RefreshProcesses(fProcTotal)) {
                vmmprintf("VmmProc: Failed to refresh memory process file system - aborting.\n");
                LeaveCriticalSection(&ctxVmm->MasterLock);
                goto fail;
            }
            // update max physical address (if volatile).
            if(ctxMain->dev.fVolatileMaxAddress) {
                if(LeechCore_GetOption(LEECHCORE_OPT_MEMORYINFO_ADDR_MAX, &paMax) && (paMax > 0x01000000)) {
                    ctxMain->dev.paMax = paMax;
                }
            }
            // send notify
            if(fProcTotal) {
                VmmWinObj_Refresh();
                PluginManager_Notify(VMMDLL_PLUGIN_EVENT_REFRESH_PROCESS_TOTAL, NULL, 0);
            }
            // refresh pfn subsystem
            MmPfn_Refresh();
        }
        // refresh registry and user map
        if(fRegistry) {
            VmmWinReg_Refresh();
            VmmWinUser_Refresh();
            VmmWinPhysMemMap_Refresh();
            PluginManager_Notify(VMMDLL_PLUGIN_EVENT_REFRESH_REGISTRY, NULL, 0);
        }
        LeaveCriticalSection(&ctxVmm->MasterLock);
    }
fail:
    vmmprintfv("VmmProc: Exit periodic cache flushing.\n");
    if(ctxVmm->ThreadProcCache.hThread) { CloseHandle(ctxVmm->ThreadProcCache.hThread); }
    ctxVmm->ThreadProcCache.hThread = NULL;
    return 0;
}

BOOL VmmProcInitialize()
{
    BOOL result = FALSE;
    if(!VmmInitialize()) { return FALSE; }
    // 1: try initialize 'windows' with an optionally supplied CR3
    result = VmmWinInit_TryInitialize(ctxMain->cfg.paCR3);
    if(!result) {
        result = ctxMain->cfg.paCR3 && VmmProcUserCR3TryInitialize64();
        if(!result) {
            vmmprintf(
                "VmmProc: Unable to auto-identify operating system for PROC file system mount.   \n" \
                "         Specify PageDirectoryBase (DTB/CR3) in -cr3 option if value if known.  \n");
        }
    }
    // set up cache maintenance in the form of a separate worker thread in case
    // the backend is a volatile device (FPGA). If the underlying device isn't
    // volatile then there is no need to update! NB! Files are not considered
    // to be volatile.
    if(result && ctxMain->dev.fVolatile && !ctxMain->cfg.fDisableBackgroundRefresh) {
        ctxVmm->ThreadProcCache.fEnabled = TRUE;
        ctxVmm->ThreadProcCache.hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)VmmProcCacheUpdaterThread, ctxVmm, 0, NULL);
        if(!ctxVmm->ThreadProcCache.hThread) { ctxVmm->ThreadProcCache.fEnabled = FALSE; }
    }
    // allow worker threads for various functions in other parts of the code
    // NB! this only allows worker threads - it does not create them!
    ctxVmm->ThreadWorkers.fEnabled = TRUE;
    return result;
}

// ----------------------------------------------------------------------------
// SCAN/SEARCH TO IDENTIFY IMAGE:
// - Currently Windows PageDirectoryBase/CR3/PML4 detection is supported only
// ----------------------------------------------------------------------------

_Success_(return)
BOOL VmmProcPHYS_VerifyWindowsEPROCESS(_In_ PBYTE pb, _In_ QWORD cb, _In_ QWORD cbOffset, _Out_ PQWORD ppaPML4)
{
    QWORD i;
    if(cb < cbOffset + 8) { return FALSE; }
    if((cb & 0x07) || (cb < 0x500) || (cbOffset < 0x500)) { return FALSE; }
    if(*(PQWORD)(pb + cbOffset) != 0x00006D6574737953) { return FALSE; }        // not matching System00
    if(*(PQWORD)(pb + cbOffset + 8) & 0x00ffffffffffffff) { return FALSE; }     // not matching 0000000
                                                                                // maybe we have EPROCESS struct here, scan back to see if we can find
                                                                                // 4 kernel addresses in a row and a potential PML4 after that and zero
                                                                                // DWORD before that. (EPROCESS HDR).
    for(i = cbOffset; i > cbOffset - 0x500; i -= 8) {
        if((*(PQWORD)(pb + i - 0x00) & 0xfffff00000000000)) { continue; };                          // DirectoryTableBase
        if(!*(PQWORD)(pb + i - 0x00)) { continue; };                                                // DirectoryTableBase
        if((*(PQWORD)(pb + i - 0x08) & 0xffff800000000000) != 0xffff800000000000) { continue; };    // PTR
        if((*(PQWORD)(pb + i - 0x10) & 0xffff800000000000) != 0xffff800000000000) { continue; };    // PTR
        if((*(PQWORD)(pb + i - 0x18) & 0xffff800000000000) != 0xffff800000000000) { continue; };    // PTR
        if((*(PQWORD)(pb + i - 0x20) & 0xffff800000000000) != 0xffff800000000000) { continue; };    // PTR
        if((*(PDWORD)(pb + i - 0x24) != 0x00000000)) { continue; };                                 // SignalState
        *ppaPML4 = *(PQWORD)(pb + i - 0x00) & ~0xfff;
        return TRUE;
    }
    return FALSE;
}
