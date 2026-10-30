"""A sampling profiler for a running Open Animate (any Windows process with PDBs beside it): pauses its main
thread every few ms, walks its stack with dbghelp (StackWalk64) and names the frames from the PDBs, then lists
where the time went, inclusive (the function and what it calls) and self. 64-bit Python only. Measure before
guessing: on the 360 Twilight rig the costs were rarely where they looked to be.

    python tools/profile/sample_profile.py PID [SECONDS] [--callers NAME ...]

From a script (a private copy driven over remote control, as tools/render_check does):

    s = Sampler(pid); s.start(); ...drive it...; s.stop(); print(s.report()); print(s.callers("armature_pose"))

Samples under remote_capture (a remote screenshot stepping frames) are left out of the report."""
import ctypes
import ctypes.wintypes as wt
import threading
import time
from collections import Counter, defaultdict

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
dbg = ctypes.WinDLL("dbghelp", use_last_error=True)

PROCESS_ALL_ACCESS = 0x1F0FFF
THREAD_ALL_ACCESS = 0x1F03FF
TH32CS_SNAPTHREAD = 0x4
CONTEXT_FULL = 0x10000B
IMAGE_FILE_MACHINE_AMD64 = 0x8664


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", ctypes.c_long), ("tpDeltaPri", ctypes.c_long),
                ("dwFlags", wt.DWORD)]


class ADDRESS64(ctypes.Structure):
    _fields_ = [("Offset", ctypes.c_uint64), ("Segment", ctypes.c_uint16), ("Mode", ctypes.c_int)]


class STACKFRAME64(ctypes.Structure):
    _fields_ = [("AddrPC", ADDRESS64), ("AddrReturn", ADDRESS64), ("AddrFrame", ADDRESS64),
                ("AddrStack", ADDRESS64), ("AddrBStore", ADDRESS64), ("FuncTableEntry", ctypes.c_void_p),
                ("Params", ctypes.c_uint64 * 4), ("Far", wt.BOOL), ("Virtual", wt.BOOL),
                ("Reserved", ctypes.c_uint64 * 3), ("KdHelp", ctypes.c_byte * 256)]


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wt.ULONG), ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG),
                ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", wt.ULONG),
                ("Scope", wt.ULONG), ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG),
                ("Name", ctypes.c_char * 1024)]


k32.OpenProcess.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
dbg.SymFunctionTableAccess64.restype = ctypes.c_void_p
dbg.SymGetModuleBase64.restype = ctypes.c_uint64
dbg.StackWalk64.argtypes = [wt.DWORD, wt.HANDLE, wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
dbg.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.c_void_p]
k32.SuspendThread.argtypes = [wt.HANDLE]
k32.ResumeThread.argtypes = [wt.HANDLE]
k32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.GetThreadTimes.argtypes = [wt.HANDLE] + [ctypes.POINTER(wt.FILETIME)] * 4


def threads_of(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    out = []
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            out.append(te.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    return out


def main_thread(pid):
    """The thread created first."""
    best = None
    for tid in threads_of(pid):
        h = k32.OpenThread(THREAD_ALL_ACCESS, False, tid)
        if not h:
            continue
        c, e, kt, ut = wt.FILETIME(), wt.FILETIME(), wt.FILETIME(), wt.FILETIME()
        k32.GetThreadTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut))
        t = (c.dwHighDateTime << 32) | c.dwLowDateTime
        if best is None or t < best[0]:
            if best:
                k32.CloseHandle(best[1])
            best = (t, h)
        else:
            k32.CloseHandle(h)
    return best[1]


class Sampler:
    def __init__(self, pid, interval=0.002, depth=80):
        self.pid, self.interval, self.depth = pid, interval, depth
        self.proc = k32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
        dbg.SymSetOptions(0x2 | 0x4 | 0x10)  # UNDNAME, DEFERRED_LOADS, LOAD_LINES
        if not dbg.SymInitialize(self.proc, None, True):
            raise OSError("SymInitialize", ctypes.get_last_error())
        self.thread = main_thread(pid)
        buf = ctypes.create_string_buffer(1232 + 16)
        off = (-ctypes.addressof(buf)) % 16
        self._ctxbuf, self.ctx = buf, ctypes.addressof(buf) + off
        self.samples = []
        self._stop = threading.Event()
        self._names = {}

    def _sample(self):
        if k32.SuspendThread(self.thread) == 0xFFFFFFFF:
            return
        try:
            ctypes.memset(self.ctx, 0, 1232)
            ctypes.c_uint32.from_address(self.ctx + 0x30).value = CONTEXT_FULL
            if not k32.GetThreadContext(self.thread, self.ctx):
                return
            rip = ctypes.c_uint64.from_address(self.ctx + 0xF8).value
            rsp = ctypes.c_uint64.from_address(self.ctx + 0x98).value
            rbp = ctypes.c_uint64.from_address(self.ctx + 0xA0).value
            f = STACKFRAME64()
            f.AddrPC.Offset, f.AddrPC.Mode = rip, 3
            f.AddrFrame.Offset, f.AddrFrame.Mode = rbp, 3
            f.AddrStack.Offset, f.AddrStack.Mode = rsp, 3
            stack = []
            fta = ctypes.cast(dbg.SymFunctionTableAccess64, ctypes.c_void_p)
            gmb = ctypes.cast(dbg.SymGetModuleBase64, ctypes.c_void_p)
            for _ in range(self.depth):
                if not dbg.StackWalk64(IMAGE_FILE_MACHINE_AMD64, self.proc, self.thread, ctypes.byref(f),
                                       self.ctx, None, fta, gmb, None):
                    break
                if f.AddrPC.Offset == 0:
                    break
                stack.append(f.AddrPC.Offset)
            self.samples.append(stack)
        finally:
            k32.ResumeThread(self.thread)

    def _run(self):
        while not self._stop.is_set():
            self._sample()
            time.sleep(self.interval)

    def start(self):
        self.samples = []
        self._stop.clear()
        self._t = threading.Thread(target=self._run, daemon=True)
        self._t.start()

    def stop(self):
        self._stop.set()
        self._t.join()
        return self.samples

    def name(self, addr):
        if addr in self._names:
            return self._names[addr]
        s = SYMBOL_INFO()
        s.SizeOfStruct = 88
        s.MaxNameLen = 1000
        disp = ctypes.c_uint64()
        n = s.Name.decode(errors="replace") if dbg.SymFromAddr(self.proc, addr, ctypes.byref(disp), ctypes.byref(s)) \
            else "0x%x" % addr
        self._names[addr] = n
        return n

    def report(self, samples=None, top=40, skip=("NtDelayExecution", "ZwWaitFor", "NtWaitFor")):
        samples = self.samples if samples is None else samples
        incl, excl = Counter(), Counter()
        harness = 0
        kept = []
        for st in samples:
            names = [self.name(a) for a in st]
            if not names:
                continue
            if any("remote_capture" in n for n in names):
                harness += 1
                continue
            kept.append(names)
        samples = kept
        for names in kept:
            excl[names[0]] += 1
            for n in set(names):
                incl[n] += 1
        total = max(len(samples), 1)
        idle = sum(1 for names in kept if any("SwapWindow" in n or "SDL_Delay" in n for n in names))
        lines = ["%d samples (left out %d of the harness's own screenshots); waiting in SwapWindow %.0f%%" % (total, harness, 100.0 * idle / total), "-- inclusive --"]
        for n, c in incl.most_common(top):
            lines.append("%5.1f%%  %s" % (100.0 * c / total, n[:150]))
        lines.append("-- self --")
        for n, c in excl.most_common(top // 2):
            lines.append("%5.1f%%  %s" % (100.0 * c / total, n[:150]))
        return "\n".join(lines)

    def callers(self, target, samples=None, top=15):
        """Who calls `target` (substring), by samples."""
        samples = self.samples if samples is None else samples
        c = Counter()
        for st in samples:
            names = [self.name(a) for a in st]
            for i, n in enumerate(names):
                if target in n and i + 1 < len(names):
                    c[names[i + 1]] += 1
                    break
        return "\n".join("%5d  %s" % (v, k[:150]) for k, v in c.most_common(top))


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description="Sample a running process's main thread")
    ap.add_argument("pid", type=int)
    ap.add_argument("seconds", type=float, nargs="?", default=5.0)
    ap.add_argument("--callers", nargs="*", default=[], help="also list who calls these (substrings of names)")
    ap.add_argument("--top", type=int, default=40)
    a = ap.parse_args()
    sampler = Sampler(a.pid)
    sampler.start()
    time.sleep(a.seconds)
    sampler.stop()
    print(sampler.report(top=a.top))
    for name in a.callers:
        print("-- callers of", name)
        print(sampler.callers(name))
