/* Native stack dump of the main thread, used by the hang watchdog. */
#include "log.h"
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>

static HANDLE main_thread;

void stackdump_init(void)
{
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
}

void stackdump_main_thread(void)
{
    if (!main_thread) return;
    HANDLE proc = GetCurrentProcess();
    static int sym_ready;
    if (!sym_ready) { SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME); SymInitialize(proc, NULL, TRUE); sym_ready = 1; }
    if (SuspendThread(main_thread) == (DWORD)-1) return;
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(main_thread, &ctx)) {
        STACKFRAME64 f;
        memset(&f, 0, sizeof f);
#if defined(_M_X64)
        DWORD machine = IMAGE_FILE_MACHINE_AMD64;
        f.AddrPC.Offset = ctx.Rip; f.AddrFrame.Offset = ctx.Rbp; f.AddrStack.Offset = ctx.Rsp;
#elif defined(_M_ARM64)
        DWORD machine = IMAGE_FILE_MACHINE_ARM64;
        f.AddrPC.Offset = ctx.Pc; f.AddrFrame.Offset = ctx.Fp; f.AddrStack.Offset = ctx.Sp;
#else
        DWORD machine = IMAGE_FILE_MACHINE_I386;
        f.AddrPC.Offset = ctx.Eip; f.AddrFrame.Offset = ctx.Ebp; f.AddrStack.Offset = ctx.Esp;
#endif
        f.AddrPC.Mode = f.AddrFrame.Mode = f.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < 24; i++) {
            if (!StackWalk64(machine, proc, main_thread, &f, &ctx, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)) break;
            if (!f.AddrPC.Offset) break;
            char buf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
            sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 255;
            DWORD64 disp = 0;
            IMAGEHLP_LINE64 line; DWORD ldisp = 0;
            memset(&line, 0, sizeof line); line.SizeOfStruct = sizeof line;
            const char *name = SymFromAddr(proc, f.AddrPC.Offset, &disp, sym) ? sym->Name : "?";
            if (SymGetLineFromAddr64(proc, f.AddrPC.Offset, &ldisp, &line))
                log_warn("  #%d %s  %s:%lu", i, name, line.FileName, (unsigned long)line.LineNumber);
            else
                log_warn("  #%d %s", i, name);
        }
    }
    ResumeThread(main_thread);
}
#else
void stackdump_init(void) { }
void stackdump_main_thread(void) { }
#endif
