#include "stdafx.h"
#include <chrono>
#include <thread>
#include <iostream> 
#include <string>
#include <map>
#include <NTstatus.h>
#include <windows.h>
#include <PowrProf.h>
#include <intrin.h>
#include <TlHelp32.h>
#include <pdh.h>
#include <pdhmsg.h>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "Powrprof.lib")

using namespace std;

HQUERY hQuery = NULL;
PDH_STATUS pdhStatus;
HCOUNTER hPMaxFreqCounter, hProcFreqCounter;

//CONST PWSTR COUNTER_PATH = L"\\Processor(0)\\% Processor Time";
//CONST PWSTR COUNTER_PMAXFREQ_PATH = L"\\Processor Information(0,0)\\% of Maximum Frequency";
CONST PWSTR COUNTER_PMAXFREQ_PATH = L"\\Processor Information(0,0)\\% Processor Performance";
CONST PWSTR COUNTER_PROCFREQ_PATH = L"\\Processor Information(0,0)\\Processor Frequency";

typedef struct _PROCESS_INFO {
    ///DWORD processId;
    uint64_t cycleTime;
} PROCESS_INFO, *PPROCESS_INFO;

typedef map<DWORD, PROCESS_INFO> ProcessesInfoMap;

typedef struct _PROCESSOR_POWER_INFORMATION {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
} PROCESSOR_POWER_INFORMATION, *PPROCESSOR_POWER_INFORMATION;

ULONG getCPUBaseSpeed()
{
    // get the number or processors 
    SYSTEM_INFO si = { 0 };
    ::GetSystemInfo(&si);
    ULONG result = 0;

    // allocate buffer to get info for each processor
    const int size = si.dwNumberOfProcessors * sizeof(PROCESSOR_POWER_INFORMATION);
    LPBYTE pBuffer = new BYTE[size];

    NTSTATUS status = ::CallNtPowerInformation(ProcessorInformation, NULL, 0, pBuffer, size);
    if (STATUS_SUCCESS == status)
    {
        PPROCESSOR_POWER_INFORMATION ppi = (PPROCESSOR_POWER_INFORMATION)pBuffer;
        for (DWORD nIndex = 0; nIndex < si.dwNumberOfProcessors; nIndex++)
        {
            result = max(result, ppi->MaxMhz);
            ppi++;
        }
    }
    else
    {
        std::cout << "CallNtPowerInformation failed. Status: " << status << std::endl;
    }
    delete[]pBuffer;
    return result;
}

void processChildProcesses(DWORD parentId, ProcessesInfoMap &pMap) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32First(hSnapshot, &pe)) {
        do {
            if (pe.th32ParentProcessID == parentId) {
                HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION |
                    PROCESS_VM_READ,
                    FALSE, pe.th32ProcessID);
                if (NULL != hProcess) {
                    uint64_t cycleTime = 0;
                    if (QueryProcessCycleTime(hProcess, &cycleTime)) {
                        pMap[pe.th32ProcessID].cycleTime = cycleTime;
                    }
                    CloseHandle(hProcess);
                }
            }
        } while (Process32Next(hSnapshot, &pe));
    }
    CloseHandle(hSnapshot);
}


int main(int argc, char *argv[], char *envp[])
{
    PROCESS_INFORMATION processInfo;
    STARTUPINFOA startupInfo;
    DWORD processExitCode = 0;
    ProcessesInfoMap pMap;

    // Using performance counters to get CPU frequency. Open a query object.
    pdhStatus = PdhOpenQuery(NULL, 0, &hQuery);
    if (pdhStatus != ERROR_SUCCESS)
    {
        wprintf(L"PdhOpenQuery failed with 0x%x\n", pdhStatus);
    }

    // Add counters
    pdhStatus = PdhAddEnglishCounter(hQuery, COUNTER_PMAXFREQ_PATH, 0, &hPMaxFreqCounter);
    if (pdhStatus != ERROR_SUCCESS) {
        wprintf(L"PdhAddCounter failed with 0x%x\n", pdhStatus);
    }
    /*
    pdhStatus = PdhAddEnglishCounter(hQuery, COUNTER_PROCFREQ_PATH, 0, &hProcFreqCounter);
    if (pdhStatus != ERROR_SUCCESS) {
        wprintf(L"PdhAddCounter failed with 0x%x\n", pdhStatus);
    }
    */

    // Preparing to launch target application.
    string newCmdArgs = "";
    for (auto i = 1; i < argc; i++) {
        if (newCmdArgs.length() > 0) {
            newCmdArgs += " ";
        }
        newCmdArgs += argv[i];
    }

    ZeroMemory(&startupInfo, sizeof(startupInfo));
    startupInfo.cb = sizeof(startupInfo);
    LARGE_INTEGER startCounter, startedCounter, endCounter;
    LONG lCPUBaseSpeed = getCPUBaseSpeed();
    QueryPerformanceCounter(&startCounter);
    if (newCmdArgs.length() && CreateProcessA(NULL, const_cast<char *>(newCmdArgs.c_str()),
        NULL, NULL, FALSE, 0, NULL,
        NULL, &startupInfo, &processInfo))
    {
        QueryPerformanceCounter(&startedCounter);
        pMap[processInfo.dwProcessId].cycleTime = 0;
        bool done = false;
        PDH_FMT_COUNTERVALUE pdhValue;
        while (!done) {
            // loop until process is closed and sample CPU frequency during this time and the sub processes
            switch (WaitForSingleObject(processInfo.hProcess, 250)) {
            case WAIT_TIMEOUT:
                // sample the CPU and process stats...
                pdhStatus = PdhCollectQueryData(hQuery);
                if (pdhStatus == ERROR_SUCCESS)
                {
                    pdhStatus = PdhGetFormattedCounterValue(hPMaxFreqCounter, PDH_FMT_DOUBLE | PDH_FMT_NOSCALE | PDH_FMT_NOCAP100, NULL, &pdhValue);
                    if (pdhStatus == ERROR_SUCCESS)
                    {
                        double lCPUPMaxFreq = pdhValue.doubleValue;
                        wprintf(L"CPU freq %d util %f => %f\n", lCPUBaseSpeed, lCPUPMaxFreq, lCPUBaseSpeed * lCPUPMaxFreq / 100);
                    }

                    /*
                    pdhStatus = PdhGetFormattedCounterValue(hProcFreqCounter, PDH_FMT_LONG | PDH_FMT_NOSCALE | PDH_FMT_NOCAP100, NULL, &pdhValue);
                    if (pdhStatus == ERROR_SUCCESS)
                    {
                        long lCPUFreq = pdhValue.longValue;
                    }
                    */
                } 
                else {
                    wprintf(L"PdhCollectQueryData failed with 0x%x\n", pdhStatus);
                }
                // query for the subprocesses...
                processChildProcesses(processInfo.dwProcessId, pMap);
                break;
            default:
                // we are done, break the loop
                done = true;
            }
        }        
        QueryPerformanceCounter(&endCounter);

        //GetProcessorSystemCycleTime()
        processChildProcesses(processInfo.dwProcessId, pMap);

        uint64_t cycleTime = 0;
        if (QueryProcessCycleTime(processInfo.hProcess, &cycleTime)) {
            pMap[processInfo.dwProcessId].cycleTime = cycleTime;
            
            // sum over all cycles
            cycleTime = 0;
            for (ProcessesInfoMap::iterator it = pMap.begin(); it != pMap.end(); ++it) {
                cycleTime += it->second.cycleTime;
            }

            // present the results
            wprintf(L"%I64dM %I64d %I64d\n",
                cycleTime / 1000000, 
                startedCounter.QuadPart - startCounter.QuadPart, 
                endCounter.QuadPart - startedCounter.QuadPart);
        }
        else {
            wprintf(L"Failed to query process cycle time");
        }

        GetExitCodeProcess(processInfo.hProcess, &processExitCode);

        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
    }
    else
    {
        wprintf(L"Process couldn't be started\n");
        processExitCode = -1;
    }
    
    if (hQuery) {
        PdhCloseQuery(hQuery);
    }

    return processExitCode;
}