# tmc_backtrace_lldb.py
#
# LLDB ScriptedFrameProvider that reconstructs async (coroutine) stacks for TooManyCooks.
# Requires LLDB 22+. Load it *after* the target exists (frame providers are per target):
#     (lldb) command script import ./tmc_backtrace_lldb.py
# `bt`, the SB API and lldb-dap then show the awaiting coroutines below the running one.
#
# Where each piece of information comes from:
#   running coroutine      : `__coro_frame` (Clang) / `frame_ptr` (GCC) in a physical frame whose
#                            symbol is a coroutine body (.resume/.destroy/.cleanup/.actor)
#   coroutine function     : the resume pointer in the frame header -> the function starting there
#   promise                : `__promise` / `_Coro_promise` member of the frame type, else the ABI
#                            offset (2 pointers)
#   awaiting coroutine     : promise.customizer.continuation (TooManyCooks)
#   suspend index          : `__coro_index` (Clang) / `_Coro_resume_index` (GCC)
#   suspend source location: the `mov $N, index(%reg)` store in the resume function (x86 only).
#                            LLDB does not expose DW_TAG_label, so the `__coro_resume_N` labels
#                            that the GDB script uses are not available here.
#
# Environment variables (read when the script is imported):
#   CORO_BT_FRAME_MODE=dict       see FRAME_MODE below
#   CORO_BT_DEBUG=1               print diagnostics
import os
import re

import lldb

try:
    from lldb.plugins.scripted_frame_provider import ScriptedFrameProvider
    from lldb.plugins.scripted_process import ScriptedFrame

    _HAVE_FRAME_PROVIDER = True
except ImportError:  # LLDB < 22: keep the module importable so we can print a real message
    ScriptedFrameProvider = object
    ScriptedFrame = object
    _HAVE_FRAME_PROVIDER = False

DEBUG = bool(os.environ.get("CORO_BT_DEBUG"))
# How synthetic frames are handed to LLDB:
# "scripted": ScriptedFrame objects (CoroFrame). Default. The script supplies the frame's name
#             ("[fork]" prefix, readable GCC names) and its exact line, and the frame is marked
#             artificial, which lldb-dap shows greyed out. LLDB 22 and 23 segfault when an
#             expression is evaluated with a ScriptedFrame selected that has no variable list
#             (fixed upstream by llvm/llvm-project#239795); CoroFrame.get_variables() works
#             around that.
# "dict":     plain {"idx", "pc"} frames; LLDB derives function, name and line from the pc.
#             Fallback that avoids LLDB's ScriptedFrame code entirely: no "[fork]", and the
#             line is only as good as the pc that can be found for it.
# Can also be changed after import: (lldb) script tmc_backtrace_lldb.FRAME_MODE = "dict"
FRAME_MODE = os.environ.get("CORO_BT_FRAME_MODE", "scripted")
# Upper bound on synthetic frames per chain; guards against corrupted memory.
MAX_ASYNC_DEPTH = 1000

# Clang: f.resume / f.destroy / f.cleanup.   GCC: f.actor / f.destroy.
_CORO_BODY_SUFFIXES = (".resume", ".destroy", ".cleanup", ".actor")
_AWAITER_FN_RE = re.compile(r"\bawait_(ready|suspend)\b")
# AT&T syntax: "$0x1, 0x90(%rax)"
_IMM_STORE_RE = re.compile(
    r"^\$(0x[0-9a-fA-F]+|\d+),\s*(0x[0-9a-fA-F]+|\d+)\((%[a-z0-9]+)\)$"
)
_STORE_WIDTH = {"movb": 1, "movw": 2, "movl": 4}
# The same store in two instructions: "movl $0x4, %ecx" ... "movw %cx, 0x44(%rdx)".
# See CoroFnInfo._split_store_value().
_REG_STORE_RE = re.compile(r"^%([a-z0-9]+),\s*(0x[0-9a-fA-F]+|\d+)\((%[a-z0-9]+)\)$")
_IMM_LOAD_RE = re.compile(r"^\$(0x[0-9a-fA-F]+|\d+),\s*%[a-z0-9]+$")
_LAST_REG_RE = re.compile(r"%([a-z0-9]+)$")
_REG_FAMILY_RE = re.compile(r"^(?:[er]?([abcd])[xlh]|[er]?([sd]i|bp)l?|(r\d+)[dwb]?)$")
_SPLIT_STORE_WINDOW = 64
# Registers that keep their value across a call (System V x86-64).
_CALLEE_SAVED = ("b", "bp", "r12", "r13", "r14", "r15")
# One variable of `image lookup -v -a <addr>`: `Variable: id = {..}, name = "x", ..., location = ..`
_LOOKUP_VARIABLE_RE = re.compile(r'^\s*Variable: .*?\bname = "(?P<name>[^"]+)".*$', re.M)
_CLONE_SUFFIX_RE = re.compile(r" \(\.[^()]+\)$")
_GCC_FRAME_PARAM_RE = re.compile(r"\.Frame\s*\*\)$")


def debug_log(msg):
    if DEBUG:
        print(f"[coro-provider] {msg}")


def _gcc_coroutine_name(symbol_name):
    """
    'ns::f(<frame type>*) (.actor)' -> 'ns::f'; None if this is not a GCC actor/destroy symbol.

    GCC's actor takes the coroutine frame as its only parameter, so its demangled symbol reads
    `fib(_ZL3fibm.Frame*) (.actor)`. The debug-info name is no better in optimized builds
    (`::top_fib(_ZL7top_fibm.Frame *)`, or no name at all).
    """
    base = _CLONE_SUFFIX_RE.sub("", symbol_name or "")
    if not base or base == symbol_name or not _GCC_FRAME_PARAM_RE.search(base):
        return None
    depth = 0
    for i in range(len(base) - 1, -1, -1):
        if base[i] == ")":
            depth += 1
        elif base[i] == "(":
            depth -= 1
            if depth == 0:
                return base[:i] or None
    return None


def _reg_family(reg):
    """cx / ecx / rcx / cl -> 'c'; r8w / r8d / r8 -> 'r8'; None if not a general register."""
    m = _REG_FAMILY_RE.match(reg)
    return (m.group(1) or m.group(2) or m.group(3)) if m else None


def _find_field(sbtype, name):
    """Return (SBTypeMember, offset) for a direct field or a field of a base class."""
    for i in range(sbtype.GetNumberOfFields()):
        f = sbtype.GetFieldAtIndex(i)
        if f.GetName() == name:
            return f, f.GetOffsetInBytes()
    for i in range(sbtype.GetNumberOfDirectBaseClasses()):
        b = sbtype.GetDirectBaseClassAtIndex(i)
        f, off = _find_field(b.GetType(), name)
        if f is not None:
            return f, b.GetOffsetInBytes() + off
    return None, None


def _body_location(addr):
    """
    Source location of `addr` *as seen from the enclosing function body*: if addr is inside
    inlined code, this is the call site of the outermost inlined call, not the callee's line.
    Returns (SBFileSpec, line, column, outermost_inlined_name_or_None).
    """
    blk = addr.GetBlock()
    site = None
    while blk.IsValid():
        ib = blk.GetContainingInlinedBlock()
        if not ib.IsValid():
            break
        site = (
            ib.GetInlinedCallSiteFile(),
            ib.GetInlinedCallSiteLine(),
            ib.GetInlinedCallSiteColumn(),
            ib.GetInlinedName() or "",
        )
        blk = ib.GetParent()
    if site is not None:
        return site
    le = addr.GetLineEntry()
    return (le.GetFileSpec(), le.GetLine(), le.GetColumn(), None)


def _same_file(a, b):
    return a.IsValid() and b.IsValid() and a.GetFilename() == b.GetFilename() and (
        not a.GetDirectory() or not b.GetDirectory() or a.GetDirectory() == b.GetDirectory()
    )


class _Insn:
    """One instruction of a resume function. See CoroFnInfo._disassemble()."""

    __slots__ = ("module", "file_addr", "mnemonic", "operands", "kind", "branches", "_addr")

    def __init__(self, module, file_addr, mnemonic, operands, kind, branches):
        self.module = module
        self.file_addr = file_addr
        self.mnemonic = mnemonic
        self.operands = operands
        self.kind = kind  # lldb.eInstructionControlFlowKind*
        self.branches = branches
        self._addr = None

    @property
    def addr(self):
        """The address with its section, which is what has a block and a line entry."""
        if self._addr is None:
            self._addr = self.module.ResolveFileAddress(self.file_addr)
        return self._addr


class CoroFnInfo:
    """
    Everything we know statically about one coroutine resume function.
    Cached per (module UUID, file address) so it survives re-runs, ASLR and multiple targets.
    """

    _cache = {}

    def __init__(self, target, func, symbol):
        self.func = func
        self.symbol = symbol
        self.is_gcc = False
        self.has_frame_type = False
        self.index_offset = None
        self.index_size = None
        self.promise_offset = None  # None -> use the ABI default (2 pointers)
        self.is_tmc = None  # True/False when debug info can tell, None if unknown
        self.cont_offset = 0  # offsets inside the promise
        self.done_offset = 2 * target.GetAddressByteSize()
        self._await_map = None  # resume index -> (file_addr_for_pc, (file, line, col) or None)
        self._store_width = None
        self._index_may_be_wider = False
        self._decl_file = func.GetStartAddress().GetLineEntry().GetFileSpec() if func.IsValid() else None
        if func.IsValid():
            self._read_types(target)

    @classmethod
    def for_resume_address(cls, target, load_addr):
        """Validate that load_addr is the *entry point* of a function and return its info."""
        addr = lldb.SBAddress(load_addr, target)
        if not addr.IsValid() or not addr.GetSection().IsValid():
            return None
        func = addr.GetFunction()
        sym = addr.GetSymbol()
        if func.IsValid():
            start = func.GetStartAddress()
        elif sym.IsValid():
            start = sym.GetStartAddress()
        else:
            return None
        if start.GetLoadAddress(target) != load_addr:
            return None  # a pointer into the middle of a function is not a resume_fn
        mod = addr.GetModule()
        key = (mod.GetUUIDString() or str(mod.GetFileSpec()), start.GetFileAddress())
        info = cls._cache.get(key)
        if info is None:
            info = cls(target, func, sym)
            if not info.has_frame_type:
                # No debug info for the frame: only accept it if the symbol name says coroutine.
                name = sym.GetMangledName() or sym.GetName() or ""
                if not name.endswith(_CORO_BODY_SUFFIXES):
                    info = False
            cls._cache[key] = info
        return info or None

    def _read_types(self, target):
        frame_type = None
        promise_type = None
        for v in self.func.GetBlock().GetVariables(target, True, True, True):
            name = v.GetName()
            if name == "__coro_frame":  # Clang: the frame struct itself
                frame_type = v.GetType()
            elif name == "frame_ptr":  # GCC: pointer to the frame struct
                t = v.GetType()
                frame_type = t.GetPointeeType() if t.IsPointerType() else t
                self.is_gcc = True
            elif name == "__promise":  # Clang; present (type only) even when optimized out
                promise_type = v.GetType()
        if frame_type is None or not frame_type.IsValid():
            return
        self.has_frame_type = True
        f, off = _find_field(frame_type, "_Coro_resume_index" if self.is_gcc else "__coro_index")
        if f is not None:
            self.index_offset = off
            self.index_size = f.GetType().GetByteSize()  # GCC: 2. Clang: says 1 (see index_width)
            # Is there another member right behind a 1-byte index? Then it really is 1 byte.
            self._index_may_be_wider = self.index_size == 1 and not any(
                frame_type.GetFieldAtIndex(i).GetOffsetInBytes() == off + 1
                for i in range(frame_type.GetNumberOfFields())
            )
        # Clang -O0 names the member __promise, GCC names it _Coro_promise. Clang -O2 uses a
        # synthesized name, so there we fall back to the ABI offset and the __promise variable type.
        f, off = _find_field(frame_type, "_Coro_promise" if self.is_gcc else "__promise")
        if f is not None:
            self.promise_offset = off
            promise_type = f.GetType()
        if promise_type is not None and promise_type.IsValid():
            pt = promise_type.GetCanonicalType()
            cust, cust_off = _find_field(pt, "customizer")
            if cust is None:
                self.is_tmc = False
                return
            ct = cust.GetType().GetCanonicalType()
            c, c_off = _find_field(ct, "continuation")
            d, d_off = _find_field(ct, "done_count")
            if c is None or d is None:
                self.is_tmc = False
                return
            self.is_tmc = True
            self.cont_offset = cust_off + c_off
            self.done_offset = cust_off + d_off

    # ---- resume index -> await location -------------------------------------------------

    def index_width(self, target):
        """
        Width of the resume index. Normally the DWARF member size, except that Clang describes
        __coro_index as `unsigned char` even when a coroutine has > 256 suspend points and the
        generated code stores 2 bytes.
        """
        if self._await_map is None:
            self._await_map = self._build_await_map(target)
        return self._store_width or self.index_size

    def await_point(self, target, index):
        """Returns (load_pc, (SBFileSpec, line, col) or None) for a resume index, or None."""
        if self.index_offset is None:
            return None
        if self._await_map is None:
            self._await_map = self._build_await_map(target)
        if self.is_gcc:
            index &= ~1  # odd = same suspend point on the destroy path
        ent = self._await_map.get(index)
        if ent is None:
            return None
        file_addr, loc = ent
        mod = self.func.GetStartAddress().GetModule()
        return (mod.ResolveFileAddress(file_addr).GetLoadAddress(target), loc)

    def _disassemble(self, target):
        """
        The instructions of the function as a list of _Insn, in AT&T syntax whatever
        target.x86-disassembly-flavor says.

        Not `self.func.GetInstructions(target, "att")`: for every instruction with an address
        operand LLDB looks up the symbol it prints in the comment, and in a running process
        that lookup (DisassemblerLLVMC::SymbolLookup -> Function::GetAddressRange) can take a
        second per instruction. Seen with LLDB 22.1.8 and GCC 13 -O2 -march=native: the first
        backtrace through a coroutine with 270 awaits (4,500 instructions) took more than five
        minutes. Disassembling the bytes at an address that has no section skips the lookup
        (a quarter of a second for the same function). The addresses are therefore file
        addresses; _Insn.addr resolves one again where a block or a line is needed.
        """
        module = self.func.GetStartAddress().GetModule()
        insns = []
        ranges = self.func.GetRanges()
        for k in range(ranges.GetSize()):
            rng = ranges.GetAddressRangeAtIndex(k)
            start = rng.GetBaseAddress()
            size = rng.GetByteSize()
            if size <= 0:
                continue  # GCC -O2 describes some functions with an empty range
            data = self._code_bytes(target, start, size)
            if not data:
                debug_log(f"cannot read the code of {self.symbol.GetName()} (range {k})")
                continue
            base = lldb.SBAddress()
            base.SetAddress(lldb.SBSection(), start.GetFileAddress())
            raw = target.GetInstructionsWithFlavor(base, "att", data)
            for i in range(raw.GetSize()):
                ins = raw.GetInstructionAtIndex(i)
                insns.append(_Insn(
                    module,
                    ins.GetAddress().GetOffset(),
                    ins.GetMnemonic(target) or "",
                    ins.GetOperands(target) or "",
                    ins.GetControlFlowKind(target),
                    ins.DoesBranch(),
                ))
        return insns

    @staticmethod
    def _code_bytes(target, start, size):
        """
        The bytes of a code range. From the object file first: that works without a process,
        and with a core file, which usually has no code pages. Memory is the fallback.
        """
        error = lldb.SBError()
        data = start.GetSection().GetSectionData(start.GetOffset(), size)
        if data.IsValid() and data.GetByteSize() == size:
            raw = data.ReadRawData(error, 0, size)
            if error.Success() and raw:
                return raw
        error = lldb.SBError()
        raw = target.ReadMemory(start, size, error)
        return raw if error.Success() else None

    def _build_await_map(self, target):
        result = {}
        self._store_width = None
        insns = self._disassemble(target)
        stores = []
        for i, ins in enumerate(insns):
            width = _STORE_WIDTH.get(ins.mnemonic)
            if width is None:
                continue
            ops = ins.operands
            m = _IMM_STORE_RE.match(ops)
            split = not m and self.is_gcc and width == 2
            if split:
                m = _REG_STORE_RE.match(ops)
            if not m or int(m.group(2), 0) != self.index_offset or m.group(3) in ("%rsp", "%rip"):
                continue
            if width < self.index_size:
                continue
            # The coroutine's own index stores are never in inlined code. A store in inlined
            # code writes another coroutine's frame: when the ramp of an awaited child is
            # inlined here, it sets the child's index, and the child's frame usually has the
            # index at the same offset.
            if ins.addr.GetBlock().GetContainingInlinedBlock().IsValid():
                continue
            value = self._split_store_value(insns, i, m.group(1)) if split else int(m.group(1), 0)
            if value is None:
                continue
            stores.append((i, value, width))
        # A store wider than the member is either Clang's mis-sized index (values > 0xff, nothing
        # packed behind the index) or store-merging with the neighbouring members (GCC -O2 emits
        # e.g. `movl $0x10000, idx(%reg)` = index 0 + refcount 1). Either way: mask to the width.
        self._store_width = self.index_size
        if self._index_may_be_wider and any(w >= 2 and v > 0xFF for _, v, w in stores):
            self._store_width = 2
        mask = (1 << (8 * self._store_width)) - 1
        stores = [(i, v & mask) for i, v, _ in stores]
        if self.is_gcc:
            # GCC's actor only ever stores 2, 4, 6, ...: anything else writes something else.
            stores = [(i, v) for i, v in stores if v and not v & 1]
        store_idx = {i for i, _ in stores}
        for i, value in stores:
            if value in result:
                continue  # duplicates (e.g. final suspend on normal + EH path) share a location
            if self.is_gcc:
                result[value] = self._gcc_await_location(insns, i, store_idx)
            else:
                # Clang attributes the index store to the co_await expression itself.
                f, line, col, _ = _body_location(insns[i].addr)
                result[value] = (insns[i].file_addr, (f, line, col))
        return result

    @staticmethod
    def _split_store_value(insns, i, reg):
        """
        The constant that `movw %reg, mem` at instruction i stores, if a `movl $imm, %reg`
        shortly before it in the same basic block loaded it.

        GCC emits the index store this way when it tunes for CPUs where a 16-bit immediate
        stalls the decoder. That includes the default (generic) tuning, so most optimized GCC
        builds look like this; -march=native on an AMD CPU gives the single
        `movw $N, index(%reg)`.
        """
        family = _reg_family(reg)
        for j in range(i - 1, max(-1, i - 1 - _SPLIT_STORE_WINDOW), -1):
            ins = insns[j]
            mnemonic = ins.mnemonic
            if mnemonic.startswith("call"):
                if family in _CALLEE_SAVED:
                    continue  # GCC keeps the constant in such a register across the callee's ramp
                return None
            if ins.branches or mnemonic.startswith(("j", "ret")):
                return None
            ops = ins.operands
            m = _LAST_REG_RE.search(ops)
            if m is None or _reg_family(m.group(1)) != family:
                continue
            # The nearest instruction whose destination is this register: either the load, or
            # something that makes the value unknown.
            m = _IMM_LOAD_RE.match(ops) if mnemonic.startswith("mov") else None
            return int(m.group(1), 0) if m else None
        return None

    def _gcc_await_location(self, insns, i, store_idx, window=80):
        """
        GCC attributes `_Coro_resume_index = N` to the function's closing brace. What carries
        the co_await's location is the awaiter's await_ready / await_suspend around the store:
        a real call at -O0, the call site of inlined code at -O2. Use that as the anchor instead
        of guessing which line is the closing brace.

        That code normally follows the store, but the -O2 scheduler also leaves parts of it just
        before the store, so look at the whole basic block: forward to the end of the suspend
        path first, then backward.
        """
        store = insns[i]
        sfile, sline, scol, _ = _body_location(store.addr)
        decl_file = self._decl_file
        n = len(insns)
        fallback = None
        anchor = None

        def ends_block(ins):
            return ins.kind in (lldb.eInstructionControlFlowKindJump, lldb.eInstructionControlFlowKindFarJump,
                                lldb.eInstructionControlFlowKindReturn, lldb.eInstructionControlFlowKindFarReturn)

        def awaiter_site(ins):
            f, line, col, inl_name = _body_location(ins.addr)
            if line == 0 or not _same_file(f, decl_file):
                return None
            if inl_name is not None:
                found = bool(_AWAITER_FN_RE.search(inl_name))
            else:
                found = ins.kind == lldb.eInstructionControlFlowKindCall and self._call_target_is_awaiter(ins)
            return (f, line, col) if found else None

        past_block = False
        for j in range(i + 1, min(n, i + 1 + window)):
            if j in store_idx:
                break  # ran into the next suspend point
            ins = insns[j]
            if not past_block:
                anchor = awaiter_site(ins)
                if anchor is not None:
                    break
                past_block = ends_block(ins)
            f, line, col, _ = _body_location(ins.addr)
            if fallback is None and line != 0 and line != sline and _same_file(f, decl_file):
                fallback = (f, line, col)
        if anchor is None:
            for j in range(i - 1, max(-1, i - 1 - window), -1):
                ins = insns[j]
                if j in store_idx or ends_block(ins):
                    break
                anchor = awaiter_site(ins)
                if anchor is not None:
                    break
        loc = anchor or fallback
        if loc is None:
            # Last resort: the original backward scan, but file-aware.
            for j in range(i - 1, max(-1, i - 50), -1):
                f, line, col, _ = _body_location(insns[j].addr)
                if line != 0 and line != sline and _same_file(f, decl_file):
                    loc = (f, line, col)
                    break
        if loc is None:
            return (store.file_addr, None)
        # For plain (dict) frames LLDB derives function+line from the pc alone, so find a pc in
        # this function that is not inside inlined code and whose own line entry is that line.
        pc = self._find_clean_pc(insns, i, loc)
        return (pc if pc is not None else store.file_addr, loc)

    @staticmethod
    def _call_target_is_awaiter(ins):
        # The instruction was disassembled at its file address, so the operand is one too.
        try:
            dest = int(ins.operands.split()[0], 0)
        except (ValueError, IndexError):
            return False
        symbol = ins.module.ResolveFileAddress(dest).GetSymbol()
        return bool(symbol.IsValid() and _AWAITER_FN_RE.search(symbol.GetName() or ""))

    @staticmethod
    def _find_clean_pc(insns, i, loc):
        f, line, _ = loc
        order = sorted(range(len(insns)), key=lambda j: (abs(j - i), j < i))  # nearest first, prefer after
        for j in order:
            a = insns[j].addr
            if a.GetBlock().GetContainingInlinedBlock().IsValid():
                continue
            le = a.GetLineEntry()
            if le.GetLine() == line and _same_file(le.GetFileSpec(), f):
                return insns[j].file_addr
        return None


if _HAVE_FRAME_PROVIDER:

    class CoroFrame(ScriptedFrame):
        """A synthetic frame for a suspended coroutine (used unless FRAME_MODE == 'dict')."""

        def __init__(self, target, frame_id, pc, loc, is_fork):
            # Deliberately not calling super().__init__: it assumes a ScriptedThread.
            self.target = target
            self._id = frame_id
            self._pc = pc
            self._loc = loc
            self._is_fork = is_fork
            self.register_info = {}
            self.register_ctx = {}
            self.variables = []

        def get_id(self):
            return self._id

        def get_pc(self):
            return self._pc

        def get_symbol_context(self):
            addr = lldb.SBAddress(self._pc, self.target)
            sc = addr.GetSymbolContext(
                lldb.eSymbolContextModule | lldb.eSymbolContextCompUnit | lldb.eSymbolContextFunction
                | lldb.eSymbolContextSymbol | lldb.eSymbolContextLineEntry
            )
            # Always hand LLDB the coroutine function's own block. Left unset, LLDB resolves the
            # block itself, at pc - 1, and in optimized code that is often a function inlined
            # into the coroutine: the frame then takes that function's identity (its name in
            # `bt`, its `this` for expressions).
            func = sc.GetFunction()
            if func.IsValid() and func.GetBlock().IsValid():
                sc.SetBlock(func.GetBlock())
            if self._loc is not None:
                le = lldb.SBLineEntry()
                le.SetFileSpec(self._loc[0])
                le.SetLine(self._loc[1])
                le.SetColumn(self._loc[2])
                sc.SetLineEntry(le)
            return sc

        def get_function_name(self):
            addr = lldb.SBAddress(self._pc, self.target)
            sym = addr.GetSymbol()
            # GCC: the coroutine's own name, without the actor's frame parameter and suffix.
            name = _gcc_coroutine_name(sym.GetName()) if sym.IsValid() else None
            if not name:
                # Otherwise whatever LLDB shows for a real frame in this function.
                func = addr.GetFunction()
                name = (func.GetDisplayName() or func.GetName()) if func.IsValid() else None
            if not name and sym.IsValid():
                name = sym.GetDisplayName() or sym.GetName()
            name = name or "<unknown>"
            return f"[fork] {name}" if self._is_fork else name

        def get_display_function_name(self):
            return self.get_function_name()

        def is_artificial(self):
            return True

        def get_register_info(self):
            return self.register_info

        def get_register_context(self):
            return None

        def get_variables(self, filters=None):
            # Must not be None (the base class default): before llvm/llvm-project#239795, LLDB
            # then dereferences a null variable list when an expression is evaluated with this
            # frame selected. LLDB calls this without arguments, hence the default.
            return lldb.SBValueList()

        def get_value_for_variable_expression(self, expr, options, error):
            # When the coroutine is a member function, an expression evaluated with this frame
            # selected makes LLDB look up `this` here, and LLDB 22 dereferences the answer
            # without a null check (ClangUserExpression::GetCppObjectPointer). So `this` must
            # get a value. With the error set, LLDB warns and carries on without an object.
            if expr != "this":
                return None
            error.SetErrorString("locals of an async coroutine frame are not available")
            size = self.target.GetAddressByteSize()
            data = lldb.SBData.CreateDataFromUInt64Array(self.target.GetByteOrder(), size, [0])
            void_ptr = self.target.GetBasicType(lldb.eBasicTypeVoid).GetPointerType()
            return self.target.CreateValueFromData("this", data, void_ptr)

    class CoroFrameProvider(ScriptedFrameProvider):
        def __init__(self, input_frames, args):
            super().__init__(input_frames, args)
            self._out = []  # entries: int (input frame index) or synthetic frame object/dict
            self._next_input = 0
            self._root_done = False
            self._input_exhausted = False

        @staticmethod
        def applies_to_thread(thread):
            return True

        @staticmethod
        def get_description():
            return "C++20 coroutine async stack unwinding (TooManyCooks)"

        # ---- lazy frame production ----------------------------------------------------------

        def get_frame_at_index(self, index):
            try:
                while len(self._out) <= index and not self._input_exhausted:
                    self._pull_one_input_frame()
                return self._out[index] if index < len(self._out) else None
            except Exception as e:  # never let an exception escape into LLDB
                debug_log(f"get_frame_at_index({index}): {type(e).__name__}: {e}")
                return None

        def _pull_one_input_frame(self):
            i = self._next_input
            frame = self.input_frames.GetFrameAtIndex(i)
            if not frame.IsValid():
                self._input_exhausted = True
                return
            self._next_input += 1
            self._out.append(i)
            if self._root_done:
                return
            try:
                coro_addr = self._running_coroutine(frame, i)
                if coro_addr is None:
                    return
                # Only the innermost *running* coroutine is a trustworthy root: deeper physical
                # coroutine frames may belong to already-destroyed coroutines (symmetric transfer
                # without tail calls, e.g. GCC -O0).
                self._root_done = True
                for pc, loc, is_fork in self._walk_chain(coro_addr):
                    idx = len(self._out)
                    if FRAME_MODE == "dict":
                        self._out.append({"idx": idx, "pc": pc})
                    else:
                        self._out.append(CoroFrame(self.target, idx, pc, loc, is_fork))
            except Exception as e:
                debug_log(f"frame {i}: {type(e).__name__}: {e}")

        # ---- root discovery -----------------------------------------------------------------

        # (module, file address, variable name) -> does its location there use an entry value?
        _entry_value_cache = {}

        def _reads_entry_value(self, frame, frame_idx, var_name):
            """
            True if reading `var_name` in this input frame would make LLDB evaluate a
            DW_OP_entry_value location. GCC -O1+ describes `frame_ptr` that way wherever the
            register that held it has been reused.

            LLDB resolves an entry value by asking the thread for the caller's frame. That
            re-enters the frame list, which is locked while this provider runs, and LLDB
            terminates with std::system_error "Resource deadlock avoided". The SB API cannot
            show a variable's location without evaluating it, so ask `image lookup`.
            """
            # LLDB picks the location list entry by the pc *register*. That differs from
            # frame.GetPC() when other functions are inlined into this one at this point: the
            # frame for the outer function then reports the call site, not the machine pc.
            reg = frame.FindRegister("pc")
            pc = reg.GetValueAsUnsigned(0) if reg.IsValid() else 0
            if pc == 0:
                return True
            # The innermost frame is looked up at pc, its callers at pc - 1.
            for load_addr in ((pc,) if frame_idx == 0 else (pc - 1, pc)):
                addr = lldb.SBAddress(load_addr, self.target)
                mod = addr.GetModule()
                key = (mod.GetUUIDString() or str(mod.GetFileSpec()), addr.GetFileAddress(), var_name)
                verdict = self._entry_value_cache.get(key)
                if verdict is None:
                    res = lldb.SBCommandReturnObject()
                    # Target-only context: with a thread in it the command looks at the selected
                    # frame, which re-enters the locked frame list just the same.
                    self.target.GetDebugger().GetCommandInterpreter().HandleCommand(
                        f"image lookup -v -a {load_addr:#x}", lldb.SBExecutionContext(self.target), res
                    )
                    out = res.GetOutput() or ""
                    if "Address:" not in out:
                        # Not the output this was written against: do not risk it.
                        debug_log(f"image lookup output not understood at {load_addr:#x}")
                        verdict = True
                    else:
                        # A variable without a location at this address is not listed, and
                        # reading it is then harmless ("variable not available").
                        verdict = any(
                            m.group("name") == var_name and "entry_value" in m.group(0)
                            for m in _LOOKUP_VARIABLE_RE.finditer(out)
                        )
                    self._entry_value_cache[key] = verdict
                if verdict:
                    return True
            return False

        def _running_coroutine(self, frame, frame_idx):
            """If `frame` is a coroutine body (not a ramp), return its coroutine frame address."""
            sym = frame.GetSymbol()
            name = (sym.GetMangledName() or sym.GetName() or "") if sym.IsValid() else ""
            if not name.endswith(_CORO_BODY_SUFFIXES) and not name.endswith("(.resume)"):
                return None
            # Clang: __coro_frame is the frame struct (stays available at -O2 when __promise is
            # optimized out). GCC: frame_ptr is the actor's parameter.
            v = frame.FindVariable("__coro_frame")
            if v.IsValid() and not self._reads_entry_value(frame, frame_idx, "__coro_frame"):
                a = v.GetLoadAddress()
                if a != lldb.LLDB_INVALID_ADDRESS and a != 0:
                    return a
            v = frame.FindVariable("frame_ptr")
            if v.IsValid() and not self._reads_entry_value(frame, frame_idx, "frame_ptr") and v.GetError().Success():
                a = v.GetValueAsUnsigned(0)
                if a != 0:
                    return a
            # Known gaps, GCC -O2: frame_ptr is only described as an entry value here (see
            # above), or it is kept in a caller-saved register across a call to a same-TU
            # function (-fipa-ra), which no unwinder can recover for a non-top frame.
            debug_log(f"{name}: coroutine body, but its frame pointer is not available")
            return None

        # ---- continuation chain -------------------------------------------------------------

        def _read_ptr(self, addr):
            err = lldb.SBError()
            v = self.process.ReadPointerFromMemory(addr, err)
            return v if err.Success() else None

        def _promise_fields(self, coro_addr, info):
            """Read (continuation, done_count) of the TMC promise in the frame at coro_addr."""
            ptr = self.target.GetAddressByteSize()
            poff = info.promise_offset if (info is not None and info.promise_offset is not None) else 2 * ptr
            coff = info.cont_offset if info is not None else 0
            doff = info.done_offset if info is not None else 2 * ptr
            return self._read_ptr(coro_addr + poff + coff), self._read_ptr(coro_addr + poff + doff)

        def _info_for_frame(self, coro_addr):
            """
            Identify the coroutine function of the frame at coro_addr from its resume pointer.
            Returns (CoroFnInfo or None, resume_fn). The resume pointer is null at final suspend,
            which a coroutine that is awaiting something never is.
            """
            resume_fn = self._read_ptr(coro_addr)
            if resume_fn:
                return CoroFnInfo.for_resume_address(self.target, resume_fn), resume_fn
            return None, resume_fn

        def _walk_chain(self, root_addr):
            """Yield (pc, loc, is_fork) for each coroutine awaiting the one at root_addr."""
            info, _ = self._info_for_frame(root_addr)
            if info is not None and info.is_tmc is False:
                return  # running coroutine is not a TMC task: its promise layout is unknown
            cont, done = self._promise_fields(root_addr, info)
            visited = {root_addr}
            depth = 0
            while cont and depth < MAX_ASYNC_DEPTH:
                is_fork = bool(done)
                if is_fork:
                    # continuation is a std::coroutine_handle<>* shared by the group / fork
                    cont = self._read_ptr(cont)
                    if not cont:
                        return
                if cont in visited:
                    return
                visited.add(cont)
                info, resume_fn = self._info_for_frame(cont)
                if info is None:
                    debug_log(f"{cont:#x} does not look like a coroutine frame (resume_fn={resume_fn})")
                    return
                pc, loc = resume_fn, None
                if info.index_offset is not None:
                    err = lldb.SBError()
                    index = self.process.ReadUnsignedFromMemory(cont + info.index_offset, info.index_width(self.target), err)
                    if err.Success():
                        ap = info.await_point(self.target, index)
                        if ap is not None:
                            pc, loc = ap
                yield pc, loc, is_fork
                depth += 1
                if info.is_tmc is False:
                    return  # e.g. a user-defined coroutine type awaiting a tmc::task
                cont, done = self._promise_fields(cont, info)


def __lldb_init_module(debugger, internal_dict):
    if not _HAVE_FRAME_PROVIDER:
        print(f"Coroutine frame provider requires LLDB 22+ (this is: {lldb.SBDebugger.GetVersionString()}). Not registered.")
        return
    if not debugger.GetSelectedTarget().IsValid():
        print(
            "Coroutine frame provider: no target yet. Frame providers are registered per target; "
            "import this script after the target is created (lldb-dap: preRunCommands, not initCommands)."
        )
        return
    res = lldb.SBCommandReturnObject()
    debugger.GetCommandInterpreter().HandleCommand(
        f"target frame-provider register -C {__name__}.CoroFrameProvider", res
    )
    if res.Succeeded():
        print("Coroutine frame provider registered. Async coroutine frames will appear in backtraces.")
    else:
        print(f"Coroutine frame provider NOT registered: {res.GetError()}")


if __name__ == "__main__":
    print("This script should be loaded from LLDB using `command script import <filename>`")
