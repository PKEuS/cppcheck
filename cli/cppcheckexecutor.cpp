/*
 * Cppcheck - A tool for static C/C++ code analysis
 * Copyright (C) 2007-2020 Cppcheck team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "cppcheckexecutor.h"

#include "analyzerinfo.h"
#include "cmdlineparser.h"
#include "config.h"
#include "cppcheck.h"
#include "filelister.h"
#include "library.h"
#include "path.h"
#include "pathmatch.h"
#include "preprocessor.h"
#include "settings.h"
#include "suppressions.h"
#include "threadexecutor.h"
#include "timer.h"
#include "utils.h"
#include "checkunusedfunctions.h"
#include "version.h"

#include <csignal>
#include <cstdio>
#include <cstdlib> // EXIT_SUCCESS and EXIT_FAILURE
#include <cstring>
#include <fstream>
#include <iostream>
#include <list>
#include <utility>
#include <vector>

#if !defined(NO_UNIX_SIGNAL_HANDLING) && defined(__GNUC__) && !defined(__MINGW32__) && !defined(__OS2__)
#define USE_UNIX_SIGNAL_HANDLING
#include <unistd.h>
#if defined(__APPLE__)
#   define _XOPEN_SOURCE // ucontext.h APIs can only be used on Mac OSX >= 10.7 if _XOPEN_SOURCE is defined
#   include <ucontext.h>

#   undef _XOPEN_SOURCE
#elif !defined(__OpenBSD__)
#   include <ucontext.h>
#endif
#ifdef __linux__
#include <sys/syscall.h>
#include <sys/types.h>
#endif
#endif

#if !defined(NO_UNIX_BACKTRACE_SUPPORT) && defined(USE_UNIX_SIGNAL_HANDLING) && defined(__GNUC__) && defined(__GLIBC__) && !defined(__CYGWIN__) && !defined(__MINGW32__) && !defined(__NetBSD__) && !defined(__SVR4) && !defined(__QNX__)
#define USE_UNIX_BACKTRACE_SUPPORT
#include <cxxabi.h>
#include <execinfo.h>
#endif

#if defined(_MSC_VER)
#define USE_WINDOWS_SEH
#include <Windows.h>
#include <DbgHelp.h>
#include <TCHAR.H>
#include <excpt.h>
#endif


/*static*/ FILE* CppCheckExecutor::mExceptionOutput = stdout;

CppCheckExecutor::CppCheckExecutor()
    : mLatestProgressOutputTime(0), mErrorOutput(nullptr), mShowAllErrors(false)
{
}

CppCheckExecutor::~CppCheckExecutor()
{
    delete mErrorOutput;
}

bool CppCheckExecutor::parseFromArgs(CppCheck *cppcheck, int argc, const char* const argv[])
{
    CmdLineParser parser(&mSettings, &mProject);
    const bool success = parser.parseFromArgs(argc, argv);

    if (success) {
        if (parser.getShowVersion() && !parser.getShowErrorMessages()) {
            const char * const extraVersion = CppCheck::extraVersion();
            if (*extraVersion != 0)
                std::cout << PROGRAMNAME " " << CppCheck::version() << " ("
                          << extraVersion << ')' << std::endl;
            else
                std::cout << PROGRAMNAME " " << CppCheck::version() << std::endl;
        }

        if (parser.getShowErrorMessages()) {
            mShowAllErrors = true;
            std::cout << ErrorMessage::getXMLHeader();
            cppcheck->getErrorMessages();
            std::cout << ErrorMessage::getXMLFooter() << std::endl;
        }

        if (parser.exitAfterPrinting()) {
            Settings::terminate();
            return true;
        }
    } else {
        return false;
    }

    // Check that all include paths exist
    {
        for (std::vector<std::string>::iterator iter = mProject.includePaths.begin();
             iter != mProject.includePaths.end();
            ) {
            const std::string path(Path::toNativeSeparators(*iter));
            if (FileLister::isDirectory(path))
                ++iter;
            else {
                // If the include path is not found, warn user and remove the non-existing path from the list.
                if (mProject.severity.isEnabled(Severity::information))
                    std::cout << "(information) Couldn't find path given by -I '" << path << '\'' << std::endl;
                iter = mProject.includePaths.erase(iter);
            }
        }
    }

    // Output a warning for the user if he tries to exclude headers
    bool warn = false;
    const std::vector<std::string>& ignored = parser.getIgnoredPaths();
    for (const std::string &i : ignored) {
        if (Path::isHeader(i)) {
            warn = true;
            break;
        }
    }
    if (warn) {
        std::cout << "cppcheck: filename exclusion does not apply to header (.h and .hpp) files." << std::endl;
        std::cout << "cppcheck: Please use --suppress for ignoring results from the header files." << std::endl;
    }

    const std::vector<std::string>& pathnames = parser.getPathNames();

#if defined(_WIN32)
    // For Windows we want case-insensitive path matching
    const bool caseSensitive = false;
#else
    const bool caseSensitive = true;
#endif
    std::map<std::string, std::size_t> files;
    if (!pathnames.empty()) {
        // Execute recursiveAddFiles() to each given file parameter
        const PathMatch matcher(ignored, caseSensitive);
        for (const std::string &pathname : pathnames)
            FileLister::recursiveAddFiles(files, Path::toNativeSeparators(pathname), mProject.library.markupExtensions(), matcher);
    }

    if (files.empty()) {
        std::cout << "cppcheck: error: could not find or open any of the paths given." << std::endl;
        if (!ignored.empty())
            std::cout << "cppcheck: Maybe all paths were ignored?" << std::endl;
        return false;
    } else if (!mProject.fileFilter.empty()) {
        for (std::map<std::string, std::size_t>::const_iterator i = files.begin(); i != files.end();) {
            if (matchglob(mProject.fileFilter, i->first))
                i = files.erase(i);
            else
                ++i;
        }
        if (files.empty()) {
            std::cout << "cppcheck: error: could not find any files matching the filter." << std::endl;
            return false;
        }
    }

    mAnalyzerInformation.createCTUs(mProject.buildDir, files);

    return true;
}

int CppCheckExecutor::check(int argc, const char* const argv[])
{
    Preprocessor::missingIncludeFlag = false;
    Preprocessor::missingSystemIncludeFlag = false;

    CppCheck cppCheck(*this, mSettings, mProject, true);

    if (!parseFromArgs(&cppCheck, argc, argv)) {
        return EXIT_FAILURE;
    }
    if (Settings::terminated()) {
        return EXIT_SUCCESS;
    }

    int ret;

    if (cppCheck.settings().exceptionHandling)
        ret = check_wrapper(cppCheck, argc, argv);
    else
        ret = check_internal(cppCheck, argc, argv);

    showTimerResults();

    return ret;
}

void CppCheckExecutor::showTimerResults()
{
    if (mSettings.showtime == Settings::SHOWTIME_NONE)
        return;

    std::cout << "\nTimings: exclusive / inclusive (averages), all in seconds\n";

    TimerResults::Data overallData;

    typedef std::pair<std::string, struct TimerResults::Data> dataElementType;
    std::vector<dataElementType> data(Timer::results.mResults.begin(), Timer::results.mResults.end());
    std::sort(data.begin(), data.end(), [](const dataElementType& lhs, const dataElementType& rhs) -> bool {
        return lhs.second.seconds() > rhs.second.seconds();
    });

    std::cout.precision(3);
    std::cout.setf(std::ios::fixed);
    std::cout.setf(std::ios::showpoint);

    size_t width = 0;

    for (std::vector<dataElementType>::const_iterator iter = data.begin(); iter != data.end(); ++iter)
        width = std::max(width, iter->first.size());

    size_t ordinal = 1; // maybe it would be nice to have an ordinal in output later!
    for (std::vector<dataElementType>::const_iterator iter = data.begin(); iter != data.end(); ++iter) {
        const double sec1 = iter->second.seconds();
        const double secAverage1 = sec1 / (double)(iter->second.mNumberOfResults);
        const double sec2 = iter->second.fullSeconds();
        const double secAverage2 = sec2 / (double)(iter->second.mNumberOfResults);
        overallData.mClocks += iter->second.mClocks;
        if ((mSettings.showtime != Settings::SHOWTIME_TOP5) || (ordinal <= 5)) {
            std::cout << iter->first << ": " << std::string(width - iter->first.size(), ' ');
            std::cout << sec1 << " / " << sec2 << " (" << secAverage1 << " / " << secAverage2 << " - " << iter->second.mNumberOfResults;
            std::cout << (iter->second.mNumberOfResults == 1 ? " result)" : " results)") << std::endl;
        }
        ++ordinal;
    }

    const double secOverall = overallData.seconds();
    std::cout << "Overall time: " << secOverall << "s" << std::endl;
}

/**
 *  Simple helper function:
 * \return size of array
 * */
template<typename T, int size>
std::size_t getArrayLength(const T(&)[size])
{
    return size;
}


#if defined(USE_UNIX_SIGNAL_HANDLING)
/*
 * Try to print the callstack.
 * That is very sensitive to the operating system, hardware, compiler and runtime.
 * The code is not meant for production environment!
 * One reason is named first: it's using functions not whitelisted for usage in a signal handler function.
 */
static void print_stacktrace(FILE* output, bool demangling, int maxdepth, bool lowMem)
{
#if defined(USE_UNIX_BACKTRACE_SUPPORT)
// 32 vs. 64bit
#define ADDRESSDISPLAYLENGTH ((sizeof(long)==8)?12:8)
    const int fd = fileno(output);
    void *callstackArray[32]= {nullptr}; // the less resources the better...
    const int currentdepth = backtrace(callstackArray, (int)getArrayLength(callstackArray));
    const int offset=2; // some entries on top are within our own exception handling code or libc
    if (maxdepth<0)
        maxdepth=currentdepth-offset;
    else
        maxdepth = std::min(maxdepth, currentdepth);
    if (lowMem) {
        fputs("Callstack (symbols only):\n", output);
        backtrace_symbols_fd(callstackArray+offset, maxdepth, fd);
    } else {
        char **symbolStringList = backtrace_symbols(callstackArray, currentdepth);
        if (symbolStringList) {
            fputs("Callstack:\n", output);
            for (int i = offset; i < maxdepth; ++i) {
                const char * const symbolString = symbolStringList[i];
                char * realnameString = nullptr;
                const char * const firstBracketName     = strchr(symbolString, '(');
                const char * const firstBracketAddress  = strchr(symbolString, '[');
                const char * const secondBracketAddress = strchr(firstBracketAddress, ']');
                const char * const beginAddress         = firstBracketAddress+3;
                const int addressLen = int(secondBracketAddress-beginAddress);
                const int padLen     = int(ADDRESSDISPLAYLENGTH-addressLen);
                if (demangling && firstBracketName) {
                    const char * const plus = strchr(firstBracketName, '+');
                    if (plus && (plus>(firstBracketName+1))) {
                        char input_buffer[1024]= {0};
                        strncpy(input_buffer, firstBracketName+1, plus-firstBracketName-1);
                        char output_buffer[2048]= {0};
                        size_t length = getArrayLength(output_buffer);
                        int status=0;
                        // We're violating the specification - passing stack address instead of malloc'ed heap.
                        // Benefit is that no further heap is required, while there is sufficient stack...
                        realnameString = abi::__cxa_demangle(input_buffer, output_buffer, &length, &status); // non-NULL on success
                    }
                }
                const int ordinal=i-offset;
                fprintf(output, "#%-2d 0x",
                        ordinal);
                if (padLen>0)
                    fprintf(output, "%0*d",
                            padLen, 0);
                if (realnameString) {
                    fprintf(output, "%.*s in %s\n",
                            (int)(secondBracketAddress-firstBracketAddress-3), firstBracketAddress+3,
                            realnameString);
                } else {
                    fprintf(output, "%.*s in %.*s\n",
                            (int)(secondBracketAddress-firstBracketAddress-3), firstBracketAddress+3,
                            (int)(firstBracketAddress-symbolString), symbolString);
                }
            }
            free(symbolStringList);
        } else {
            fputs("Callstack could not be obtained\n", output);
        }
    }
#undef ADDRESSDISPLAYLENGTH
#else
    UNUSED(output);
    UNUSED(demangling);
    UNUSED(maxdepth);
    UNUSED(lowMem);
#endif
}

static const size_t MYSTACKSIZE = 16*1024+SIGSTKSZ; // wild guess about a reasonable buffer
static char mytstack[MYSTACKSIZE]= {0}; // alternative stack for signal handler
static bool bStackBelowHeap=false; // lame attempt to locate heap vs. stack address space. See CppCheckExecutor::check_wrapper()

/**
 * \param[in] ptr address to be examined.
 * \return true if address is supposed to be on stack (contrary to heap). If ptr is 0 false will be returned.
 * If unknown better return false.
 */
static bool IsAddressOnStack(const void* ptr)
{
    if (nullptr==ptr)
        return false;
    char a;
    if (bStackBelowHeap)
        return ptr < &a;
    else
        return ptr > &a;
}

/* (declare this list here, so it may be used in signal handlers in addition to main())
 * A list of signals available in ISO C
 * Check out http://pubs.opengroup.org/onlinepubs/009695399/basedefs/signal.h.html
 * For now we only want to detect abnormal behaviour for a few selected signals:
 */

#define DECLARE_SIGNAL(x) std::make_pair(x, #x)
typedef std::map<int, std::string> Signalmap_t;
static const Signalmap_t listofsignals = {
    DECLARE_SIGNAL(SIGABRT),
    DECLARE_SIGNAL(SIGBUS),
    DECLARE_SIGNAL(SIGFPE),
    DECLARE_SIGNAL(SIGILL),
    DECLARE_SIGNAL(SIGINT),
    DECLARE_SIGNAL(SIGQUIT),
    DECLARE_SIGNAL(SIGSEGV),
    DECLARE_SIGNAL(SIGSYS),
    // don't care: SIGTERM
    DECLARE_SIGNAL(SIGUSR1),
    //DECLARE_SIGNAL(SIGUSR2) no usage currently
};
#undef DECLARE_SIGNAL
/*
 * Entry pointer for signal handlers
 * It uses functions which are not safe to be called from a signal handler,
 * (http://pubs.opengroup.org/onlinepubs/9699919799/functions/V2_chap02.html#tag_15_04 has a whitelist)
 * but when ending up here something went terribly wrong anyway.
 * And all which is left is just printing some information and terminate.
 */
static void CppcheckSignalHandler(int signo, siginfo_t * info, void * context)
{
    int type = -1;
    pid_t killid;
#if defined(__linux__) && defined(REG_ERR)
    const ucontext_t* const uc = reinterpret_cast<const ucontext_t*>(context);
    killid = (pid_t) syscall(SYS_gettid);
    if (uc) {
        type = (int)uc->uc_mcontext.gregs[REG_ERR] & 2;
    }
#else
    UNUSED(context);
    killid = getpid();
#endif

    const Signalmap_t::const_iterator it=listofsignals.find(signo);
    const char * const signame = (it==listofsignals.end()) ? "unknown" : it->second.c_str();
    bool printCallstack=true; // try to print a callstack?
    bool lowMem=false; // was low-memory condition detected? Be careful then! Avoid allocating much more memory then.
    bool unexpectedSignal=true; // unexpected indicates program failure
    bool terminate=true; // exit process/thread
    const bool isAddressOnStack = IsAddressOnStack(info->si_addr);
    FILE* output = CppCheckExecutor::getExceptionOutput();
    switch (signo) {
    case SIGABRT:
        fputs("Internal error: cppcheck received signal ", output);
        fputs(signame, output);
        fputs(
#ifdef NDEBUG
            " - out of memory?\n",
#else
            " - out of memory or assertion?\n",
#endif
            output);
        lowMem=true; // educated guess
        break;
    case SIGBUS:
        fputs("Internal error: cppcheck received signal ", output);
        fputs(signame, output);
        switch (info->si_code) {
        case BUS_ADRALN: // invalid address alignment
            fputs(" - BUS_ADRALN", output);
            break;
        case BUS_ADRERR: // nonexistent physical address
            fputs(" - BUS_ADRERR", output);
            break;
        case BUS_OBJERR: // object-specific hardware error
            fputs(" - BUS_OBJERR", output);
            break;
#ifdef BUS_MCEERR_AR
        case BUS_MCEERR_AR: // Hardware memory error consumed on a machine check;
            fputs(" - BUS_MCEERR_AR", output);
            break;
#endif
#ifdef BUS_MCEERR_AO
        case BUS_MCEERR_AO: // Hardware memory error detected in process but not consumed
            fputs(" - BUS_MCEERR_AO", output);
            break;
#endif
        default:
            break;
        }
        fprintf(output, " (at 0x%lx).\n",
                (unsigned long)info->si_addr);
        break;
    case SIGFPE:
        fputs("Internal error: cppcheck received signal ", output);
        fputs(signame, output);
        switch (info->si_code) {
        case FPE_INTDIV: //     integer divide by zero
            fputs(" - FPE_INTDIV", output);
            break;
        case FPE_INTOVF: //     integer overflow
            fputs(" - FPE_INTOVF", output);
            break;
        case FPE_FLTDIV: //     floating-point divide by zero
            fputs(" - FPE_FLTDIV", output);
            break;
        case FPE_FLTOVF: //     floating-point overflow
            fputs(" - FPE_FLTOVF", output);
            break;
        case FPE_FLTUND: //     floating-point underflow
            fputs(" - FPE_FLTUND", output);
            break;
        case FPE_FLTRES: //     floating-point inexact result
            fputs(" - FPE_FLTRES", output);
            break;
        case FPE_FLTINV: //     floating-point invalid operation
            fputs(" - FPE_FLTINV", output);
            break;
        case FPE_FLTSUB: //     subscript out of range
            fputs(" - FPE_FLTSUB", output);
            break;
        default:
            break;
        }
        fprintf(output, " (at 0x%lx).\n",
                (unsigned long)info->si_addr);
        break;
    case SIGILL:
        fputs("Internal error: cppcheck received signal ", output);
        fputs(signame, output);
        switch (info->si_code) {
        case ILL_ILLOPC: //     illegal opcode
            fputs(" - ILL_ILLOPC", output);
            break;
        case ILL_ILLOPN: //    illegal operand
            fputs(" - ILL_ILLOPN", output);
            break;
        case ILL_ILLADR: //    illegal addressing mode
            fputs(" - ILL_ILLADR", output);
            break;
        case ILL_ILLTRP: //    illegal trap
            fputs(" - ILL_ILLTRP", output);
            break;
        case ILL_PRVOPC: //    privileged opcode
            fputs(" - ILL_PRVOPC", output);
            break;
        case ILL_PRVREG: //    privileged register
            fputs(" - ILL_PRVREG", output);
            break;
        case ILL_COPROC: //    coprocessor error
            fputs(" - ILL_COPROC", output);
            break;
        case ILL_BADSTK: //    internal stack error
            fputs(" - ILL_BADSTK", output);
            break;
        default:
            break;
        }
        fprintf(output, " (at 0x%lx).%s\n",
                (unsigned long)info->si_addr,
                (isAddressOnStack)?" Stackoverflow?":"");
        break;
    case SIGINT:
        unexpectedSignal=false; // legal usage: interrupt application via CTRL-C
        fputs("cppcheck received signal ", output);
        fputs(signame, output);
        printCallstack=true;
        fputs(".\n", output);
        break;
    case SIGSEGV:
        fputs("Internal error: cppcheck received signal ", output);
        fputs(signame, output);
        switch (info->si_code) {
        case SEGV_MAPERR: //    address not mapped to object
            fputs(" - SEGV_MAPERR", output);
            break;
        case SEGV_ACCERR: //    invalid permissions for mapped object
            fputs(" - SEGV_ACCERR", output);
            break;
        default:
            break;
        }
        fprintf(output, " (%sat 0x%lx).%s\n",
                // cppcheck-suppress knownConditionTrueFalse ; FP
                (type==-1)? "" :
                (type==0) ? "reading " : "writing ",
                (unsigned long)info->si_addr,
                (isAddressOnStack)?" Stackoverflow?":""
               );
        break;
    case SIGUSR1:
        fputs("cppcheck received signal ", output);
        fputs(signame, output);
        fputs(".\n", output);
        terminate=false;
        break;
    default:
        fputs("Internal error: cppcheck received signal ", output);
        fputs(signame, output);
        fputs(".\n", output);
        break;
    }
    if (printCallstack) {
        print_stacktrace(output, true, -1, lowMem);
    }
    if (unexpectedSignal) {
        fputs("\nPlease report this to the cppcheck developers!\n", output);
    }
    fflush(output);

    if (terminate) {
        // now let things proceed, shutdown and hopefully dump core for post-mortem analysis
        struct sigaction act;
        memset(&act, 0, sizeof(act));
        act.sa_handler=SIG_DFL;
        sigaction(signo, &act, nullptr);
        kill(killid, signo);
    }
}
#endif

#ifdef USE_WINDOWS_SEH
namespace {
    const ULONG maxnamelength = 512;
    struct IMAGEHLP_SYMBOL64_EXT : public IMAGEHLP_SYMBOL64 {
        TCHAR nameExt[maxnamelength]; // actually no need to worry about character encoding here
    };
    typedef BOOL (WINAPI *fpStackWalk64)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID, PREAD_PROCESS_MEMORY_ROUTINE64, PFUNCTION_TABLE_ACCESS_ROUTINE64, PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64);
    fpStackWalk64 pStackWalk64;
    typedef DWORD64(WINAPI *fpSymGetModuleBase64)(HANDLE, DWORD64);
    fpSymGetModuleBase64 pSymGetModuleBase64;
    typedef BOOL (WINAPI *fpSymGetSymFromAddr64)(HANDLE, DWORD64, PDWORD64, PIMAGEHLP_SYMBOL64);
    fpSymGetSymFromAddr64 pSymGetSymFromAddr64;
    typedef BOOL (WINAPI *fpSymGetLineFromAddr64)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
    fpSymGetLineFromAddr64 pSymGetLineFromAddr64;
    typedef DWORD (WINAPI *fpUnDecorateSymbolName)(const TCHAR*, PTSTR, DWORD, DWORD) ;
    fpUnDecorateSymbolName pUnDecorateSymbolName;
    typedef PVOID(WINAPI *fpSymFunctionTableAccess64)(HANDLE, DWORD64);
    fpSymFunctionTableAccess64 pSymFunctionTableAccess64;
    typedef BOOL (WINAPI *fpSymInitialize)(HANDLE, PCSTR, BOOL);
    fpSymInitialize pSymInitialize;

    HMODULE hLibDbgHelp;
// avoid explicit dependency on Dbghelp.dll
    bool loadDbgHelp()
    {
        hLibDbgHelp = ::LoadLibraryW(L"Dbghelp.dll");
        if (!hLibDbgHelp)
            return false;
        pStackWalk64 = (fpStackWalk64) ::GetProcAddress(hLibDbgHelp, "StackWalk64");
        pSymGetModuleBase64 = (fpSymGetModuleBase64) ::GetProcAddress(hLibDbgHelp, "SymGetModuleBase64");
        pSymGetSymFromAddr64 = (fpSymGetSymFromAddr64) ::GetProcAddress(hLibDbgHelp, "SymGetSymFromAddr64");
        pSymGetLineFromAddr64 = (fpSymGetLineFromAddr64)::GetProcAddress(hLibDbgHelp, "SymGetLineFromAddr64");
        pSymFunctionTableAccess64 = (fpSymFunctionTableAccess64)::GetProcAddress(hLibDbgHelp, "SymFunctionTableAccess64");
        pSymInitialize = (fpSymInitialize) ::GetProcAddress(hLibDbgHelp, "SymInitialize");
        pUnDecorateSymbolName = (fpUnDecorateSymbolName)::GetProcAddress(hLibDbgHelp, "UnDecorateSymbolName");
        return true;
    }


    void printCallstack(FILE* outputFile, PEXCEPTION_POINTERS ex)
    {
        if (!loadDbgHelp())
            return;
        const HANDLE hProcess   = GetCurrentProcess();
        const HANDLE hThread    = GetCurrentThread();
        pSymInitialize(
            hProcess,
            nullptr,
            TRUE
        );
        CONTEXT             context = *(ex->ContextRecord);
        STACKFRAME64        stack= {0};
#ifdef _M_IX86
        stack.AddrPC.Offset    = context.Eip;
        stack.AddrPC.Mode      = AddrModeFlat;
        stack.AddrStack.Offset = context.Esp;
        stack.AddrStack.Mode   = AddrModeFlat;
        stack.AddrFrame.Offset = context.Ebp;
        stack.AddrFrame.Mode   = AddrModeFlat;
#else
        stack.AddrPC.Offset    = context.Rip;
        stack.AddrPC.Mode      = AddrModeFlat;
        stack.AddrStack.Offset = context.Rsp;
        stack.AddrStack.Mode   = AddrModeFlat;
        stack.AddrFrame.Offset = context.Rsp;
        stack.AddrFrame.Mode   = AddrModeFlat;
#endif
        IMAGEHLP_SYMBOL64_EXT symbol;
        symbol.SizeOfStruct  = sizeof(IMAGEHLP_SYMBOL64);
        symbol.MaxNameLength = maxnamelength;
        DWORD64 displacement   = 0;
        int beyond_main=-1; // emergency exit, see below
        for (ULONG frame = 0; ; frame++) {
            BOOL result = pStackWalk64
                          (
#ifdef _M_IX86
                              IMAGE_FILE_MACHINE_I386,
#else
                              IMAGE_FILE_MACHINE_AMD64,
#endif
                              hProcess,
                              hThread,
                              &stack,
                              &context,
                              nullptr,
                              pSymFunctionTableAccess64,
                              pSymGetModuleBase64,
                              nullptr
                          );
            if (!result)  // official end...
                break;
            pSymGetSymFromAddr64(hProcess, (ULONG64)stack.AddrPC.Offset, &displacement, &symbol);
            TCHAR undname[maxnamelength]= {0};
            pUnDecorateSymbolName((const TCHAR*)symbol.Name, (PTSTR)undname, (DWORD)getArrayLength(undname), UNDNAME_COMPLETE);
            if (beyond_main>=0)
                ++beyond_main;
            if (_tcscmp(undname, _T("main"))==0)
                beyond_main=0;
            fprintf(outputFile,
                    "%lu. 0x%08I64X in ",
                    frame, (ULONG64)stack.AddrPC.Offset);
            fputs((const char *)undname, outputFile);
            fputc('\n', outputFile);
            if (0==stack.AddrReturn.Offset || beyond_main>2) // StackWalk64() sometimes doesn't reach any end...
                break;
        }

        FreeLibrary(hLibDbgHelp);
        hLibDbgHelp=nullptr;
    }

    void writeMemoryErrorDetails(FILE* outputFile, PEXCEPTION_POINTERS ex, const char* description)
    {
        fputs(description, outputFile);
        fprintf(outputFile, " (instruction: 0x%p) ", ex->ExceptionRecord->ExceptionAddress);
        // Using %p for ULONG_PTR later on, so it must have size identical to size of pointer
        // This is not the universally portable solution but good enough for Win32/64
        C_ASSERT(sizeof(void*) == sizeof(ex->ExceptionRecord->ExceptionInformation[1]));
        switch (ex->ExceptionRecord->ExceptionInformation[0]) {
        case 0:
            fprintf(outputFile, "reading from 0x%p",
                    reinterpret_cast<void*>(ex->ExceptionRecord->ExceptionInformation[1]));
            break;
        case 1:
            fprintf(outputFile, "writing to 0x%p",
                    reinterpret_cast<void*>(ex->ExceptionRecord->ExceptionInformation[1]));
            break;
        case 8:
            fprintf(outputFile, "data execution prevention at 0x%p",
                    reinterpret_cast<void*>(ex->ExceptionRecord->ExceptionInformation[1]));
            break;
        default:
            break;
        }
    }

    /*
     * Any evaluation of the exception needs to be done here!
     */
    int filterException(int code, PEXCEPTION_POINTERS ex)
    {
        FILE *outputFile = stdout;
        fputs("Internal error: ", outputFile);
        switch (ex->ExceptionRecord->ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION:
            writeMemoryErrorDetails(outputFile, ex, "Access violation");
            break;
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            fputs("Out of array bounds", outputFile);
            break;
        case EXCEPTION_BREAKPOINT:
            fputs("Breakpoint", outputFile);
            break;
        case EXCEPTION_DATATYPE_MISALIGNMENT:
            fputs("Misaligned data", outputFile);
            break;
        case EXCEPTION_FLT_DENORMAL_OPERAND:
            fputs("Denormalized floating-point value", outputFile);
            break;
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
            fputs("Floating-point divide-by-zero", outputFile);
            break;
        case EXCEPTION_FLT_INEXACT_RESULT:
            fputs("Inexact floating-point value", outputFile);
            break;
        case EXCEPTION_FLT_INVALID_OPERATION:
            fputs("Invalid floating-point operation", outputFile);
            break;
        case EXCEPTION_FLT_OVERFLOW:
            fputs("Floating-point overflow", outputFile);
            break;
        case EXCEPTION_FLT_STACK_CHECK:
            fputs("Floating-point stack overflow", outputFile);
            break;
        case EXCEPTION_FLT_UNDERFLOW:
            fputs("Floating-point underflow", outputFile);
            break;
        case EXCEPTION_GUARD_PAGE:
            fputs("Page-guard access", outputFile);
            break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            fputs("Illegal instruction", outputFile);
            break;
        case EXCEPTION_IN_PAGE_ERROR:
            writeMemoryErrorDetails(outputFile, ex, "Invalid page access");
            break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            fputs("Integer divide-by-zero", outputFile);
            break;
        case EXCEPTION_INT_OVERFLOW:
            fputs("Integer overflow", outputFile);
            break;
        case EXCEPTION_INVALID_DISPOSITION:
            fputs("Invalid exception dispatcher", outputFile);
            break;
        case EXCEPTION_INVALID_HANDLE:
            fputs("Invalid handle", outputFile);
            break;
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
            fputs("Non-continuable exception", outputFile);
            break;
        case EXCEPTION_PRIV_INSTRUCTION:
            fputs("Invalid instruction", outputFile);
            break;
        case EXCEPTION_SINGLE_STEP:
            fputs("Single instruction step", outputFile);
            break;
        case EXCEPTION_STACK_OVERFLOW:
            fputs("Stack overflow", outputFile);
            break;
        default:
            fprintf(outputFile, "Unknown exception (%d)\n",
                    code);
            break;
        }
        fputc('\n', outputFile);
        printCallstack(outputFile, ex);
        fflush(outputFile);
        return EXCEPTION_EXECUTE_HANDLER;
    }
}
#endif

/**
 * Signal/SEH handling
 * Has to be clean for using with SEH on windows, i.e. no construction of C++ object instances is allowed!
 * TODO Check for multi-threading issues!
 *
 */
int CppCheckExecutor::check_wrapper(CppCheck& cppcheck, int argc, const char* const argv[])
{
#ifdef USE_WINDOWS_SEH
    FILE *outputFile = stdout;
    __try {
        return check_internal(cppcheck, argc, argv);
    } __except (filterException(GetExceptionCode(), GetExceptionInformation())) {
        // reporting to stdout may not be helpful within a GUI application...
        fputs("Please report this to the cppcheck developers!\n", outputFile);
        return -1;
    }
#elif defined(USE_UNIX_SIGNAL_HANDLING)
    // determine stack vs. heap
    char stackVariable;
    char *heapVariable=(char*)malloc(1);
    bStackBelowHeap = &stackVariable < heapVariable;
    free(heapVariable);

    // set up alternative stack for signal handler
    stack_t segv_stack;
    segv_stack.ss_sp = mytstack;
    segv_stack.ss_flags = 0;
    segv_stack.ss_size = MYSTACKSIZE;
    sigaltstack(&segv_stack, nullptr);

    // install signal handler
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_flags=SA_SIGINFO|SA_ONSTACK;
    act.sa_sigaction=CppcheckSignalHandler;
    for (std::map<int, std::string>::const_iterator sig=listofsignals.begin(); sig!=listofsignals.end(); ++sig) {
        sigaction(sig->first, &act, nullptr);
    }
    return check_internal(cppcheck, argc, argv);
#else
    return check_internal(cppcheck, argc, argv);
#endif
}

/*
 * That is a method which gets called from check_wrapper
 * */
int CppCheckExecutor::check_internal(CppCheck& cppcheck, int /*argc*/, const char* const argv[])
{
    mProject.libraries.emplace("std");
    if (mProject.isWindowsPlatform())
        mProject.libraries.emplace("windows");

    for (const std::string &lib : mProject.libraries) {
        if (!tryLoadLibrary(mProject.library, argv[0], lib.c_str())) {
            std::string msg, details;
            if (lib == "std" || lib == "windows") {
                msg = "Failed to load '" + lib + ".cfg'. Your Cppcheck installation is broken, please re-install. ";
#ifdef FILESDIR
                details = "The " PROGRAMNAME " binary was compiled with FILESDIR set to \""
                          FILESDIR "\" and will therefore search for "
                          "std.cfg in " FILESDIR "/cfg.";
#else
                const std::string cfgfolder(Path::fromNativeSeparators(Path::getPathFromFilename(argv[0])) + "cfg");
                details = "The " PROGRAMNAME " binary was compiled without FILESDIR set. Either the "
                          "std.cfg should be available in " + cfgfolder + " or the FILESDIR "
                          "should be configured.";
#endif
            } else {
                msg = "Failed to load '" + lib + ".cfg'.";
            }
            const std::list<ErrorMessage::FileLocation> callstack;
            ErrorMessage errmsg(callstack, emptyString, Severity::information, msg + " " + details, "failedToLoadCfg", Certainty::safe);
            reportErr(errmsg);
            return EXIT_FAILURE;
        }
    }

    if (mSettings.output.isEnabled(Output::progress))
        mLatestProgressOutputTime = std::time(nullptr);

    if (!mProject.outputFile.empty()) {
        mErrorOutput = new std::ofstream(mProject.outputFile);
    }

    if (mSettings.xml) {
        reportErr(ErrorMessage::getXMLHeader());
    }

    // Check, possibly using multiple processes
    ThreadExecutor executor(mAnalyzerInformation.getCTUs(), mSettings, mProject, *this);
    unsigned int returnValue = executor.checkSync();

    if (cppcheck.analyseWholeProgram(mAnalyzerInformation))
        returnValue++;

    if (mProject.severity.isEnabled(Severity::information) || mSettings.checkConfiguration) {
        for (auto i = mAnalyzerInformation.getCTUs().begin(); i != mAnalyzerInformation.getCTUs().end(); ++i) {
            const bool err = reportUnmatchedSuppressions(mProject.nomsg.getUnmatchedLocalSuppressions(i->sourcefile));
            if (err && returnValue == 0)
                returnValue = mSettings.exitCode;
        }

        const bool err = reportUnmatchedSuppressions(mProject.nomsg.getUnmatchedGlobalSuppressions());
        if (err && returnValue == 0)
            returnValue = mSettings.exitCode;
    }

    if (!mSettings.checkConfiguration) {
        cppcheck.tooManyConfigsError("",0U);

        if (mProject.checks.isEnabled("MissingInclude") && (Preprocessor::missingIncludeFlag || Preprocessor::missingSystemIncludeFlag)) {
            const std::list<ErrorMessage::FileLocation> callStack;
            ErrorMessage msg(callStack,
                             emptyString,
                             Severity::information,
                             "Cppcheck cannot find all the include files (use --check-config for details)\n"
                             "Cppcheck cannot find all the include files. Cppcheck can check the code without the "
                             "include files found. But the results will probably be more accurate if all the include "
                             "files are found. Please check your project's include directories and add all of them "
                             "as include directories for Cppcheck. To see what files Cppcheck cannot find use "
                             "--check-config.",
                             Preprocessor::missingIncludeFlag ? "missingInclude" : "missingIncludeSystem",
                             Certainty::safe);
            reportErr(msg);
        }
    }

    if (mSettings.xml) {
        reportErr(ErrorMessage::getXMLFooter());
    }

    if (returnValue)
        return mSettings.exitCode;
    return 0;
}

#ifdef _WIN32
// fix trac ticket #439 'Cppcheck reports wrong filename for filenames containing 8-bit ASCII'
static inline std::string ansiToOEM(const std::string &msg, bool doConvert)
{
    if (doConvert) {
        const int msglength = static_cast<int>(msg.length());
        // convert ANSI strings to OEM strings in two steps
        std::vector<WCHAR> wcContainer(msglength);
        std::string result(msglength, '\0');

        // ansi code page characters to wide characters
        MultiByteToWideChar(CP_ACP, 0, msg.data(), msglength, wcContainer.data(), msglength);
        // wide characters to oem codepage characters
        WideCharToMultiByte(CP_OEMCP, 0, wcContainer.data(), msglength, const_cast<char *>(result.data()), msglength, nullptr, nullptr);

        return result; // hope for return value optimization
    }
    return msg;
}
#else
// no performance regression on non-windows systems
#define ansiToOEM(msg, doConvert) (msg)
#endif

void CppCheckExecutor::reportErr(const std::string &errmsg)
{
    // Alert only about unique errors
    if (mShownErrors.find(errmsg) != mShownErrors.end())
        return;

    mShownErrors.insert(errmsg);
    if (mErrorOutput)
        *mErrorOutput << errmsg << std::endl;
    else {
        std::cerr << ansiToOEM(errmsg, !mSettings.xml) << std::endl;
    }
}

void CppCheckExecutor::reportOut(const std::string &outmsg)
{
    std::cout << ansiToOEM(outmsg, true) << std::endl;
}

void CppCheckExecutor::reportProgress(const std::string &filename, const char stage[], const std::size_t value)
{
    (void)filename;

    if (!mLatestProgressOutputTime)
        return;

    // Report progress messages every 10 seconds
    const std::time_t currentTime = std::time(nullptr);
    if (currentTime >= (mLatestProgressOutputTime + 10)) {
        mLatestProgressOutputTime = currentTime;

        // format a progress message
        std::ostringstream ostr;
        ostr << "progress: "
             << stage
             << ' ' << value << '%';

        // Report progress message
        reportOut(ostr.str());
    }
}

void CppCheckExecutor::reportStatus(std::size_t fileindex, std::size_t filecount, std::size_t sizedone, std::size_t sizetotal)
{
    if (filecount > 1) {
        std::ostringstream oss;
        const long percentDone = (sizetotal > 0) ? static_cast<long>(static_cast<long double>(sizedone) / sizetotal * 100) : 0;
        oss << fileindex << '/' << filecount
            << " files checked " << percentDone
            << "% done";
        std::cout << oss.str() << std::endl;
    }
}

void CppCheckExecutor::reportErr(const ErrorMessage &msg)
{
    if (mShowAllErrors) {
        reportOut(msg.toXML());
    } else if (mSettings.xml) {
        reportErr(msg.toXML());
    } else {
        reportErr(msg.toString(mSettings.verbose, mSettings.templateFormat, mSettings.templateLocation));
    }
}

void CppCheckExecutor::setExceptionOutput(FILE* exceptionOutput)
{
    mExceptionOutput = exceptionOutput;
}

FILE* CppCheckExecutor::getExceptionOutput()
{
    return mExceptionOutput;
}

bool CppCheckExecutor::tryLoadLibrary(Library& destination, const char* basepath, const char* filename)
{
    const Library::Error err = destination.load(basepath, filename);

    if (err.errorcode == Library::UNKNOWN_ELEMENT)
        std::cout << "cppcheck: Found unknown elements in configuration file '" << filename << "': " << err.reason << std::endl;
    else if (err.errorcode != Library::OK) {
        std::cout << "cppcheck: Failed to load library configuration file '" << filename << "'. ";
        switch (err.errorcode) {
        case Library::OK:
            break;
        case Library::FILE_NOT_FOUND:
            std::cout << "File not found";
            break;
        case Library::BAD_XML:
            std::cout << "Bad XML";
            break;
        case Library::UNKNOWN_ELEMENT:
            std::cout << "Unexpected element";
            break;
        case Library::MISSING_ATTRIBUTE:
            std::cout << "Missing attribute";
            break;
        case Library::BAD_ATTRIBUTE_VALUE:
            std::cout << "Bad attribute value";
            break;
        case Library::UNSUPPORTED_FORMAT:
            std::cout << "File is of unsupported format version";
            break;
        case Library::DUPLICATE_PLATFORM_TYPE:
            std::cout << "Duplicate platform type";
            break;
        case Library::PLATFORM_TYPE_REDEFINED:
            std::cout << "Platform type redefined";
            break;
        }
        if (!err.reason.empty())
            std::cout << " '" << err.reason << "'";
        std::cout << std::endl;
        return false;
    }
    return true;
}
