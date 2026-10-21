#include "core/crash.hh"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <dbghelp.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>

namespace oanim {

namespace {

wchar_t g_dir[MAX_PATH] = L"";
volatile LONG g_writing = 0;

const char *exception_name(DWORD code)
{
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
      return "access violation (bad memory read or write)";
    case EXCEPTION_STACK_OVERFLOW:
      return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
      return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
      return "integer divide by zero";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
      return "array bounds exceeded";
    case EXCEPTION_IN_PAGE_ERROR:
      return "in-page error";
    case 0xE06D7363:
      return "C++ exception not caught";
    default:
      return "exception";
  }
}

/* The report and the minidump, from `context` (where it stopped) and `record` (the exception, if
 * there is one). */
void write_report(const char *reason, CONTEXT *context, EXCEPTION_POINTERS *pointers)
{
  if (InterlockedExchange(&g_writing, 1) != 0 || g_dir[0] == L'\0') {
    return;
  }
  CreateDirectoryW(g_dir, nullptr);
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t base[MAX_PATH];
  swprintf(base, MAX_PATH, L"%ls\\crash_%04d%02d%02d-%02d%02d%02d", g_dir, t.wYear, t.wMonth, t.wDay, t.wHour,
           t.wMinute, t.wSecond);
  wchar_t dump_path[MAX_PATH], text_path[MAX_PATH], marker[MAX_PATH];
  swprintf(dump_path, MAX_PATH, L"%ls.dmp", base);
  swprintf(text_path, MAX_PATH, L"%ls.txt", base);
  swprintf(marker, MAX_PATH, L"%ls\\unseen.txt", g_dir);

  const HANDLE process = GetCurrentProcess();
  /* The minidump: every thread, with the memory the stacks point at. */
  const HANDLE dump = CreateFileW(dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (dump != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), pointers, FALSE};
    MiniDumpWriteDump(process, GetCurrentProcessId(), dump,
                      MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo),
                      pointers ? &info : nullptr, nullptr, nullptr);
    CloseHandle(dump);
  }

  FILE *out = nullptr;
  if (_wfopen_s(&out, text_path, L"w") != 0 || !out) {
    return;
  }
  wchar_t exe[MAX_PATH] = L"";
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::fprintf(out, "Open Animate crashed on %04d-%02d-%02d at %02d:%02d:%02d\n", t.wYear, t.wMonth, t.wDay, t.wHour,
               t.wMinute, t.wSecond);
  std::fprintf(out, "Program: %ls\n", exe);
  std::fprintf(out, "Reason: %s\n", reason);
  if (pointers && pointers->ExceptionRecord) {
    const EXCEPTION_RECORD &r = *pointers->ExceptionRecord;
    std::fprintf(out, "Exception 0x%08lX: %s", r.ExceptionCode, exception_name(r.ExceptionCode));
    if (r.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r.NumberParameters >= 2) {
      const ULONG_PTR kind = r.ExceptionInformation[0];
      std::fprintf(out, ", %s address 0x%llX", kind == 0 ? "reading" : kind == 1 ? "writing" : "executing",
                   static_cast<unsigned long long>(r.ExceptionInformation[1]));
    }
    std::fprintf(out, "\n");
  }
  std::fprintf(out, "Minidump: %ls\n\nCall stack (most recent first):\n", dump_path);

  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
  SymInitialize(process, nullptr, TRUE);
  CONTEXT ctx = *context;
  STACKFRAME64 frame{};
  frame.AddrPC.Offset = ctx.Rip;
  frame.AddrFrame.Offset = ctx.Rbp;
  frame.AddrStack.Offset = ctx.Rsp;
  frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
  alignas(SYMBOL_INFO) char symbol_buffer[sizeof(SYMBOL_INFO) + 512];
  for (int i = 0; i < 64; i++) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &ctx, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
        frame.AddrPC.Offset == 0)
    {
      break;
    }
    const DWORD64 address = frame.AddrPC.Offset;
    IMAGEHLP_MODULE64 module{};
    module.SizeOfStruct = sizeof(module);
    const bool has_module = SymGetModuleInfo64(process, address, &module) != FALSE;
    SYMBOL_INFO *symbol = reinterpret_cast<SYMBOL_INFO *>(symbol_buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    std::fprintf(out, "  %2d  %s!", i, has_module ? module.ModuleName : "?");
    if (SymFromAddr(process, address, &displacement, symbol)) {
      std::fprintf(out, "%s+0x%llX", symbol->Name, static_cast<unsigned long long>(displacement));
    }
    else {
      std::fprintf(out, "0x%llX", static_cast<unsigned long long>(address - (has_module ? module.BaseOfImage : 0)));
    }
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD line_displacement = 0;
    if (SymGetLineFromAddr64(process, address, &line_displacement, &line)) {
      const char *file = std::strrchr(line.FileName, '\\');
      std::fprintf(out, "   %s:%lu", file ? file + 1 : line.FileName, line.LineNumber);
    }
    std::fprintf(out, "\n");
  }
  SymCleanup(process);
  std::fclose(out);

  /* Told about on the next start. */
  FILE *m = nullptr;
  if (_wfopen_s(&m, marker, L"w") == 0 && m) {
    std::fprintf(m, "%ls", text_path);
    std::fclose(m);
  }
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS *pointers)
{
  write_report("unhandled exception", pointers->ContextRecord, pointers);
  /* Windows' own error reporting runs after (its dump and the event log stay as they were). */
  return EXCEPTION_CONTINUE_SEARCH;
}

/* No exception to go by: from where we are now. */
void report_here(const char *reason)
{
  CONTEXT context{};
  RtlCaptureContext(&context);
  write_report(reason, &context, nullptr);
}

void on_terminate()
{
  report_here("std::terminate (an exception nothing caught, or a failed noexcept)");
  std::abort();
}

void on_abort(int)
{
  report_here("abort");
  std::_Exit(3);
}

void on_purecall()
{
  report_here("pure virtual function called");
  std::_Exit(3);
}

void on_invalid_parameter(const wchar_t *, const wchar_t *, const wchar_t *, unsigned, uintptr_t)
{
  report_here("invalid parameter passed to the C runtime");
  std::_Exit(3);
}

}  // namespace

void crash_handler_install(const std::string &dir)
{
  const std::wstring wide = std::filesystem::path(reinterpret_cast<const char8_t *>(dir.c_str())).wstring();
  wcsncpy_s(g_dir, wide.c_str(), _TRUNCATE);
  SetUnhandledExceptionFilter(on_unhandled);
  std::set_terminate(on_terminate);
  std::signal(SIGABRT, on_abort);
  _set_purecall_handler(on_purecall);
  _set_invalid_parameter_handler(on_invalid_parameter);
}

std::string crash_report_unseen(const std::string &dir)
{
  const std::filesystem::path marker =
      std::filesystem::path(reinterpret_cast<const char8_t *>(dir.c_str())) / "unseen.txt";
  std::error_code ec;
  if (!std::filesystem::exists(marker, ec)) {
    return "";
  }
  std::wifstream in(marker);
  std::wstring path;
  std::getline(in, path);
  in.close();
  std::filesystem::remove(marker, ec);
  const std::u8string utf8 = std::filesystem::path(path).u8string();
  return std::string(reinterpret_cast<const char *>(utf8.c_str()), utf8.size());
}

void crash_now()
{
  volatile int *nowhere = nullptr;
  *nowhere = 1;
  std::abort();
}

}  // namespace oanim
