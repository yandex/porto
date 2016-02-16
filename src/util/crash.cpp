#include "common.hpp"

#include <atomic>

#include "util/log.hpp"
#include "util/unix.hpp"
#include "util/locks.hpp"

extern "C" {
#include <pthread.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <execinfo.h>
#include <stdlib.h>
#include <cxxabi.h>
}

// https://panthema.net/2008/0901-stacktrace-demangled/
// stacktrace.h (c) 2008, Timo Bingmann from http://idlebox.net/
// published under the WTFPL v2.0

void PrintTrace() {
    L_ERR() << "Backtrace:" << std::endl;

    unsigned int max_frames = 63;
    // storage array for stack trace address data
    void* addrlist[max_frames+1];

    // retrieve current stack addresses
    int addrlen = backtrace(addrlist, sizeof(addrlist) / sizeof(void*));

    if (addrlen == 0) {
	L_ERR() << "  <empty, possibly corrupt>\n" << std::endl;
	return;
    }

    // resolve addresses into strings containing "filename(function+address)",
    // this array must be free()-ed
    char** symbollist = backtrace_symbols(addrlist, addrlen);

    // allocate string which will be filled with the demangled function name
    size_t funcnamesize = 256;
    char* funcname = (char*)malloc(funcnamesize);

    // iterate over the returned symbol lines. skip the first, it is the
    // address of this function.
    for (int i = 1; i < addrlen; i++) {
	char *begin_name = 0, *begin_offset = 0, *end_offset = 0;
        char *begin_addr = 0;

	// find parentheses and +address offset surrounding the mangled name:
	// ./module(function+0x15c) [0x8048a6d]
	for (char *p = symbollist[i]; *p; ++p) {
	    if (*p == '(')
		begin_name = p;
	    else if (*p == '+')
		begin_offset = p;
	    else if (*p == ')' && begin_offset)
		end_offset = p;
	    else if (*p == '[') {
                begin_addr = p;
                break;
            }
	}

	if (begin_name && begin_offset && end_offset && begin_name < begin_offset) {
	    *begin_name++ = '\0';
	    *begin_offset++ = '\0';
	    *end_offset = '\0';

	    // mangled name is now in [begin_name, begin_offset) and caller
	    // offset in [begin_offset, end_offset). now apply
	    // __cxa_demangle():

	    int status;
	    char* ret = abi::__cxa_demangle(begin_name,
					    funcname, &funcnamesize, &status);
	    if (status == 0) {
		funcname = ret; // use possibly realloc()-ed string
		L_ERR() << symbollist[i] << ": " << funcname << " " << begin_addr << std::endl;
	    } else {
		// demangling failed. Output function name as a C function with
		// no arguments.
		L_ERR() << symbollist[i] << ": " << begin_name << "()+"
                    << begin_offset << " " << begin_addr << std::endl;
	    }
	} else {
	    // couldn't parse the line? print the whole line.
	    L_ERR() << symbollist[i] << std::endl;
	}
    }

    free(funcname);
    free(symbollist);
}

static std::mutex CrashLock;

void Crash() {
    std::lock_guard<std::mutex> guard(CrashLock);
    L_ERR() << "Crashed" << std::endl;
    PrintTrace();
    exit(-1);
}

void DumpStackAndDie(int sig) {
    L_ERR() << "Received fatal signal " << strsignal(sig) << std::endl;
    PrintTrace();
    RaiseSignal(sig);
}

void DumpStack(int sig) {
    L_ERR() << "Received " << strsignal(sig) << std::endl;
    PrintTrace();
}
