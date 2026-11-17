# tmc_backtrace_gdb.py
#
# GDB frame filter + pretty printer that reconstructs async (coroutine) stacks
# for TooManyCooks. Load it with `source tmc_backtrace_gdb.py`; `bt` then
# shows the awaiting coroutines as `[async]` frames below the running one.
# Also provides `p *$get_coro_frame(handle)` and `p *$get_coro_promise(handle)`.
#
# Where each piece of information comes from:
#   running coroutine      : `__coro_frame` (Clang) / `frame_ptr` (GCC) in a
#                            physical frame; both survive optimization
#   coroutine function     : the destroy pointer in the frame header -> the
#                            out-of-line function that starts there
#   coroutine frame type   : type of `__coro_frame` (Clang) / `*frame_ptr` (GCC)
#   promise                : ABI offset: 2 pointers, rounded up to alignof(promise)
#   awaiting coroutine     : promise.customizer.continuation (TooManyCooks)
#   suspend index          : `__coro_index` (Clang) / `_Coro_resume_index` (GCC)
#   suspend source location:
#     Clang >= 21          : DW_TAG_label `__coro_resume_N` (carries file:line)
#     otherwise            : the `mov $N, index(%reg)` store in the resume
#                            function (x86 only), see _scan_index_stores()
#     GCC >= 16, no store  : address of DW_TAG_label `resume.N`
import gdb
import gdb.printing
from gdb.FrameDecorator import FrameDecorator

import contextlib
import re
import typing

# Upper bound on synthetic frames per chain; guards against corrupted memory.
MAX_ASYNC_DEPTH = 512

# How far past the index store to look for the await_suspend call (GCC >= 14).
_GCC_FORWARD_WINDOW = 48
_GCC_BACKWARD_WINDOW = 48
# The awaiter's member functions: in an inlined function's name, and in the
# (possibly mangled) target of a call instruction.
_AWAITER_FN_RE = re.compile(r"\bawait_(ready|suspend)\b")
_AWAITER_CALL_RE = re.compile(r"^call.*await_(ready|suspend)")

_CLANG_CLONE_SUFFIXES = (".resume", ".destroy", ".cleanup")
_CLANG_LABEL_PREFIX = "__coro_resume_"
_CLANG_CLONE_RE = re.compile(r"(\.|\[clone \.)(resume|destroy|cleanup)\]?$")
_CLONE_SUFFIX_RE = re.compile(r"(?: \[clone \.[A-Za-z0-9_.$]+\])+$")
_COLD_SUFFIX = " [clone .cold]"
_INFO_SYMBOL_RE = re.compile(r"^(?P<name>.+?)(?: \+ \d+)? in section \S+(?: of .+)?$")
# mov{b,w} $imm, disp(%reg)
_INDEX_STORE_RE = re.compile(
    r"^mov[bw]\s+\$(0x[0-9a-fA-F]+|\d+),\s*(0x[0-9a-fA-F]+|\d+)?\(%([a-z0-9]+)\)$"
)
# The same store in two instructions: mov $imm, %ecx ... mov %cx, disp(%reg).
# See _split_store_value().
_REG_STORE_RE = re.compile(
    r"^movw?\s+%([a-z0-9]+),\s*(0x[0-9a-fA-F]+|\d+)?\(%([a-z0-9]+)\)$"
)
_IMM_LOAD_RE = re.compile(r"^mov[lq]?\s+\$(0x[0-9a-fA-F]+|\d+),\s*%[a-z0-9]+$")
_LAST_REG_RE = re.compile(r"%([a-z0-9]+)$")
_REG16_RE = re.compile(r"^([abcd]x|[sd]i|bp|r\d+w)$")
_REG_FAMILY_RE = re.compile(r"^(?:[er]?([abcd])[xlh]|[er]?([sd]i|bp)l?|(r\d+)[dwb]?)$")
_SPLIT_STORE_WINDOW = 64
# Registers that keep their value across a call (System V x86-64).
_CALLEE_SAVED = ("b", "bp", "r12", "r13", "r14", "r15")
# movzwl disp(%reg), %reg: how GCC's actor loads the suspend index.
_INDEX_LOAD_RE = re.compile(r"^movzwl\s+(0x[0-9a-fA-F]+|\d+)?\(%([a-z0-9]+)\),\s*%[a-z0-9]+$")


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

def _void_ptr():
    return gdb.lookup_type("void").pointer()


def _load_pointer_at(addr: int) -> int:
    return int(gdb.Value(addr).cast(_void_ptr().pointer()).dereference())


def _field(struct_type, name):
    for f in struct_type.fields():
        if f.name == name:
            return f
    return None


def _function_blocks(pc: int):
    """
    Blocks that have a function symbol and contain pc, innermost first. These
    are inlined callees followed by the out-of-line function - and, for a GCC
    lambda, the function that lexically encloses it.
    """
    result = []
    try:
        block = gdb.block_for_pc(pc)
    except RuntimeError:
        block = None
    while block is not None and not block.is_static and not block.is_global:
        if block.function is not None:
            result.append(block)
        block = block.superblock
    return result


def _block_key(block):
    # The range alone does not identify a block: start..end is a bounding range,
    # so with GCC's hot/cold splitting an inlined callee can report the same
    # range as the function it was inlined into.
    fn = block.function
    return (block.start, block.end, fn.name if fn is not None else None, fn.line if fn is not None else None)


def _same_block(a, b) -> bool:
    return _block_key(a) == _block_key(b)


def _function_entry(block) -> typing.Optional[int]:
    try:
        return int(block.function.value().address)
    except Exception:
        return None


def _function_block_at_entry(pc: int):
    """
    The block of the out-of-line function whose entry point is pc, or None if
    pc is not a function entry.

    gdb.block_for_pc() returns the innermost block, which in optimized builds is
    often an inlined callee (the first instruction of a Clang `.destroy` clone
    typically belongs to an inlined `operator delete`), and name lookup does not
    cross an inlined-function boundary. Taking the outermost function block is
    wrong too: GCC nests a lambda's actor inside the block of the function that
    defines the lambda. So take the outermost block that starts exactly here.
    """
    match = None
    for block in _function_blocks(pc):
        if _function_entry(block) == pc:
            match = block
    return match


def _location_in_function(pc: int, func_block):
    """
    (symtab, line) of pc as seen from func_block. If pc is inside code inlined
    into func_block, this is the call site rather than a line in some header.
    (For an inlined function's block symbol, GDB stores DW_AT_call_file /
    DW_AT_call_line as the symbol's symtab / line.)
    """
    sal = gdb.find_pc_line(pc)
    loc = (sal.symtab, sal.line) if sal.symtab is not None and sal.line > 0 else (None, None)
    candidates = [loc]
    for block in _function_blocks(pc):
        if _same_block(block, func_block):
            return loc
        # Leaving an inlined callee: continue from its call site.
        callee = block.function
        if callee.line > 0 and callee.symtab is not None:
            loc = (callee.symtab, callee.line)
            candidates.append(loc)
    # func_block is not an ancestor. GCC -O2 produces overlapping inline ranges
    # that make GDB file parts of an actor under a different function; settle
    # for the innermost location that is in the coroutine's own source file.
    own = func_block.function.symtab
    if own is not None:
        for symtab, line in candidates:
            if symtab is not None and symtab.filename == own.filename:
                return symtab, line
    return candidates[0]


def _elf_symbol_at(addr: int) -> typing.Optional[str]:
    """
    Name of the ELF symbol covering addr, ignoring GCC's hot/cold split.

    The Python API has no minimal-symbol lookup, and blocks cannot stand in for
    it: a block only exposes its bounding range, which for a hot/cold split
    function spans unrelated functions.
    """
    try:
        out = gdb.execute("info symbol %#x" % addr, to_string=True).strip()
    except gdb.error:
        return None
    m = _INFO_SYMBOL_RE.match(out)
    if m is None:
        return None
    name = m.group("name")
    if name.endswith(_COLD_SUFFIX):
        name = name[: -len(_COLD_SUFFIX)]
    return name


def _strip_params(name: str) -> str:
    """
    'ns::f(int) const [clone .resume]' -> 'ns::f', as GDB prints real frames.
    Returns the name minus clone suffix if it does not end in a parameter list.
    """
    name = _CLONE_SUFFIX_RE.sub("", name)
    base = name
    for qual in (" const", " volatile", " &&", " &", " noexcept"):
        while base.endswith(qual):
            base = base[: -len(qual)]
    if not base.endswith(")"):
        return name
    depth = 0
    for i in range(len(base) - 1, -1, -1):
        c = base[i]
        if c == ")":
            depth += 1
        elif c == "(":
            depth -= 1
            if depth == 0:
                return base[:i] if i > 0 else name
    return name


@contextlib.contextmanager
def _att_syntax():
    """The store scanner parses AT&T operands; the user may prefer Intel."""
    try:
        old = gdb.parameter("disassembly-flavor")
    except gdb.error:
        old = None  # not an x86 target
    if old in (None, "att"):
        yield
        return
    gdb.execute("set disassembly-flavor att", to_string=True)
    try:
        yield
    finally:
        gdb.execute("set disassembly-flavor " + old, to_string=True)


def _gcc_index_offset_from_code(entry_pc: int) -> typing.Optional[int]:
    """
    Offset of `_Coro_resume_index` in the coroutine frame, read from the code
    of GCC's actor: it begins by dispatching on the index, so its first 16-bit
    load through a register is `movzwl index(%reg), %eax`. x86 only.
    """
    try:
        arch = gdb.selected_inferior().architecture()
        if not arch.name().startswith("i386"):
            return None
        with _att_syntax():
            insns = arch.disassemble(entry_pc, count=48)
    except Exception:
        return None
    for insn in insns:
        m = _INDEX_LOAD_RE.match(insn["asm"])
        if m is not None and m.group(2) not in ("rsp", "rbp", "rip"):
            return int(m.group(1), 0) if m.group(1) else 0
    return None


# ---------------------------------------------------------------------------
# Per-function debug info (cached)
# ---------------------------------------------------------------------------

class _CoroFunction:
    """What the debug info says about one resume/destroy function."""

    def __init__(self, entry_pc: int):
        self.entry_pc = entry_pc
        self.block = _function_block_at_entry(entry_pc)
        self.frame_type = None
        self.is_gcc = False
        self.promise_type = None
        self.index_offset = None  # of the suspend index in the coroutine frame
        self.index_size = None    # as the debug info declares it
        self.symbols = {}
        self.max_label_index = None  # highest N of __coro_resume_N, if any
        self.max_store_index = None  # highest N found by the store scan
        self._stores = None
        if self.block is None:
            return
        # Everything we need is declared in the function's outermost block.
        # Reading the block directly (instead of gdb.lookup_symbol) cannot be
        # satisfied by an unrelated global, and does not pay for a failed
        # program-wide lookup.
        self.symbols = {sym.name: sym for sym in self.block}
        label_indices = [
            int(name[len(_CLANG_LABEL_PREFIX):])
            for name in self.symbols
            if name.startswith(_CLANG_LABEL_PREFIX) and name[len(_CLANG_LABEL_PREFIX):].isdigit()
        ]
        if label_indices:
            self.max_label_index = max(label_indices)

        # Clang: `__coro_frame` is the frame itself. GCC: `frame_ptr` points to it.
        sym = self.symbols.get("__coro_frame")
        if sym is not None:
            t = sym.type.strip_typedefs()
            if t.code == gdb.TYPE_CODE_STRUCT and _field(t, "__resume_fn") is not None:
                self.frame_type = t
        else:
            sym = self.symbols.get("frame_ptr")
            if sym is not None:
                t = sym.type.strip_typedefs()
                if t.code == gdb.TYPE_CODE_PTR:
                    t = t.target().strip_typedefs()
                    # `frame_ptr` is an ordinary identifier; check it is GCC's.
                    if t.code == gdb.TYPE_CODE_STRUCT and _field(t, "_Coro_resume_fn") is not None:
                        self.frame_type = t
                        self.is_gcc = True
                    elif t.code == gdb.TYPE_CODE_STRUCT and not t.fields() and "_Coro_resume_index" in self.symbols:
                        # GCC <= 14 declares the frame struct of a member function
                        # inside that function's *declaration*, and GDB does not
                        # read the members of a type declared there. The actor
                        # still has a variable for every member.
                        self.frame_type = t
                        self.is_gcc = True

        if self.frame_type is not None:
            f = _field(self.frame_type, "_Coro_resume_index" if self.is_gcc else "__coro_index")
            if f is not None:
                self.index_offset = f.bitpos // 8
                self.index_size = f.type.sizeof
            elif self.is_gcc and "_Coro_resume_index" in self.symbols:
                # The variable's location is an expression on a live frame_ptr,
                # which a suspended coroutine does not have. Ask the code.
                self.index_offset = _gcc_index_offset_from_code(entry_pc)
                self.index_size = self.symbols["_Coro_resume_index"].type.sizeof
            f = _field(self.frame_type, "_Coro_promise" if self.is_gcc else "__promise")
            if f is not None:
                self.promise_type = f.type.strip_typedefs()
        if self.promise_type is None:
            # Clang at -O1+ names the frame member after its type, but keeps the
            # (optimized-out) `__promise` variable, whose type is all we need.
            sym = self.symbols.get("__promise") or self.symbols.get("_Coro_promise")
            if sym is not None:
                self.promise_type = sym.type.strip_typedefs()

    @property
    def name(self) -> typing.Optional[str]:
        return _strip_params(self.block.function.name) if self.block is not None else None

    def label(self, name: str):
        sym = self.symbols.get(name)
        if sym is not None and sym.addr_class == gdb.SYMBOL_LOC_LABEL:
            return sym
        return None

    def index_stores(self) -> dict:
        if self._stores is None:
            self._stores = _scan_index_stores(self)
        return self._stores


_function_cache: typing.Dict[int, _CoroFunction] = {}


def _coro_function(entry_pc: int) -> _CoroFunction:
    fn = _function_cache.get(entry_pc)
    if fn is None:
        fn = _function_cache[entry_pc] = _CoroFunction(entry_pc)
    return fn


def _clear_caches(*_):
    _function_cache.clear()


# The cache is keyed by address; drop it whenever symbols may have changed
# (rebuild + re-run, another inferior, dlopen/dlclose).
for _ev in ("new_objfile", "clear_objfiles", "free_objfile"):
    if hasattr(gdb.events, _ev):
        getattr(gdb.events, _ev).connect(_clear_caches)


def _reg_family(reg: str) -> typing.Optional[str]:
    """cx / ecx / rcx / cl -> 'c'; r8w / r8d / r8 -> 'r8'; None if not a general register."""
    m = _REG_FAMILY_RE.match(reg)
    return (m.group(1) or m.group(2) or m.group(3)) if m is not None else None


def _split_store_value(insns, i: int, reg: str) -> typing.Optional[int]:
    """
    The constant that `mov %reg, mem` at insns[i] stores, if a `mov $imm, %reg`
    shortly before it in the same basic block loaded it.

    GCC emits the index store this way when it tunes for CPUs where a 16-bit
    immediate stalls the decoder. That includes the default (generic) tuning,
    so most optimized GCC builds look like this; -march=native on an AMD CPU
    gives the single `movw $N, index(%reg)`.
    """
    family = _reg_family(reg)
    for j in range(i - 1, max(-1, i - 1 - _SPLIT_STORE_WINDOW), -1):
        asm = insns[j]["asm"].split("#")[0].rstrip()
        if asm.startswith("call"):
            if family in _CALLEE_SAVED:
                continue  # GCC keeps the constant in such a register across the callee's ramp
            return None
        if asm.startswith(("j", "ret")):
            return None
        m = _LAST_REG_RE.search(asm)
        if m is None or _reg_family(m.group(1)) != family:
            continue
        # The nearest instruction whose destination is this register: either
        # the load, or something that makes the value unknown.
        m = _IMM_LOAD_RE.match(asm)
        return int(m.group(1), 0) if m is not None else None
    return None


def _awaiter_call_site(pc: int, func_block):
    """
    If pc is in code inlined from an await_ready / await_suspend that is called
    from func_block's source file, the (symtab, line) of that call. Else None.
    """
    own = func_block.function.symtab
    if own is None:
        return None
    for block in _function_blocks(pc):
        if _same_block(block, func_block):
            return None
        callee = block.function
        if (callee.line > 0 and callee.symtab is not None and callee.symtab.filename == own.filename
                and _AWAITER_FN_RE.search(callee.name or "")):
            return callee.symtab, callee.line
    return None


def _gcc_await_location(insns, i: int, store_at, block, own_file, brace):
    """
    GCC: (symtab, line) of the co_await that the index store at insns[i]
    belongs to, or None.

    GCC >= 14 attributes the store itself to the function's closing brace
    (`brace`, None if unknown), and in optimized code of any version the
    store's own line is unreliable. What carries the co_await's location is
    the awaiter's await_ready / await_suspend around it: a real call at -O0,
    the call site of inlined code at -O2. That code normally follows the
    store, but the -O2 scheduler also leaves parts of it just before the
    store. Taking simply the next line that is not the brace finds the
    previous statement, or an inlined callee, whenever the optimizer has moved
    one of those in between.
    """
    def awaiter(j):
        insn = insns[j]
        site = _awaiter_call_site(insn["addr"], block)
        if site is None and _AWAITER_CALL_RE.match(insn["asm"]):
            site = _location_in_function(insn["addr"], block)
        if site is None or site[0] is None or not site[1]:
            return None
        if site[0].filename != own_file or (site[0].filename, site[1]) == brace:
            return None
        return site

    def ends_block(j):
        return j in store_at or insns[j]["asm"].startswith(("jmp", "ret"))

    # After the store, to the end of the suspend path.
    for j in range(i + 1, min(len(insns), i + 1 + _GCC_FORWARD_WINDOW)):
        if j in store_at:
            break
        site = awaiter(j)
        if site is not None:
            return site
        if ends_block(j):
            break
    # Before the store, back to the start of its basic block.
    for j in range(i - 1, max(-1, i - 1 - _GCC_BACKWARD_WINDOW), -1):
        if ends_block(j):
            break
        site = awaiter(j)
        if site is not None:
            return site
    if brace is None:
        return None
    # No awaiter code is left (e.g. std::suspend_always, whose members fold away
    # completely): the next line that is not the brace.
    for j in range(i + 1, min(len(insns), i + 1 + _GCC_FORWARD_WINDOW)):
        st, ln = _location_in_function(insns[j]["addr"], block)
        if ln and st.filename == own_file and (st.filename, ln) != brace:
            return st, ln
    return None


def _scan_index_stores(fn: _CoroFunction) -> dict:
    """
    {suspend index: (pc, symtab, line)} for every `mov $N, index(%reg)` in the
    resume function. x86 only.

    Clang attributes the store to the co_await itself, and so does GCC <= 13
    at -O0. GCC >= 14 attributes it to the function's closing brace. For GCC
    see _gcc_await_location().
    """
    result = {}
    block = fn.block
    index_offset = fn.index_offset
    try:
        arch = gdb.selected_inferior().architecture()
        if not arch.name().startswith("i386"):
            return result
        with _att_syntax():
            insns = arch.disassemble(block.start, block.end - 1)
    except Exception:
        return result

    # block.start..end is a bounding range: with hot/cold splitting it spans
    # other functions, whose index stores must not be mistaken for ours.
    owner = _elf_symbol_at(fn.entry_pc)
    # GCC's index is an unsigned short. Clang sizes it by the number of suspend
    # points: a byte up to 256 of them, two bytes beyond that.
    mnemonics = ("movw",) if fn.is_gcc else ("movb", "movw")
    stores = []  # (instruction index, suspend index, symtab, line)
    wide = []    # parallel to stores: was it a 2-byte store?
    nesting = []  # parallel to stores: the function blocks around it, innermost first
    for i, insn in enumerate(insns):
        asm = insn["asm"]
        if not asm.startswith("mov"):
            continue
        m = _INDEX_STORE_RE.match(asm) if asm.startswith(mnemonics) else None
        split = m is None and fn.is_gcc
        if split:
            m = _REG_STORE_RE.match(asm)
            if m is None or _REG16_RE.match(m.group(1)) is None:
                continue
        if m is None or m.group(3) in ("rsp", "rip"):
            continue
        disp = int(m.group(2), 0) if m.group(2) else 0
        if disp != index_offset:
            continue
        if owner is not None and _elf_symbol_at(insn["addr"]) != owner:
            continue
        value = _split_store_value(insns, i, m.group(1)) if split else int(m.group(1), 0)
        if value is None:
            continue
        if fn.is_gcc and (value == 0 or value & 1):
            continue  # GCC's actor only ever stores 2, 4, 6, ...: this writes something else
        symtab, line = _location_in_function(insn["addr"], block)
        stores.append((i, value, symtab, line))
        wide.append(split or asm.startswith("movw"))
        nesting.append([_block_key(b) for b in _function_blocks(insn["addr"])])

    # The coroutine's own index stores are never in inlined code. A store in
    # inlined code writes another coroutine's frame: when the ramp of an awaited
    # child is inlined here, it sets the child's index (or stores an argument),
    # and the child's frame often has the same layout. "Inlined" cannot simply
    # mean "not directly in `block`": for a lambda, GCC describes parts of the
    # actor as code inlined into the enclosing function, and GDB then files them
    # under that function. So compare the stores with each other: one that is
    # nested inside the function block of another one is in inlined code.
    homes = {_block_key(block)} | {chain[0] for chain in nesting if chain}
    own = [not any(key in homes for key in chain[1:]) for chain in nesting]
    stores = [s for s, keep in zip(stores, own) if keep]
    wide = [w for w, keep in zip(wide, own) if keep]

    if not fn.is_gcc:
        if fn.max_label_index is not None:
            is_wide = fn.max_label_index > 0xFF
        else:
            is_wide = any(w and s[1] > 0xFF for s, w in zip(stores, wide))
        stores = [s for s, w in zip(stores, wide) if w == is_wide]
    fn.max_store_index = max((s[1] for s in stores), default=None)

    own_file = block.function.symtab.filename if block.function.symtab else None
    brace = None
    if fn.is_gcc:
        # Every coroutine has at least an initial and a final suspend. GCC >= 14
        # attributes their index stores to the closing brace, GCC <= 13 to the
        # opening line. So if the last line that any store is attributed to is
        # shared by several stores, it is the closing brace.
        locs = [
            (st.filename, ln)
            for _, idx, st, ln in stores
            if idx != 0 and st is not None and st.filename == own_file
        ]
        if locs:
            last = max(locs, key=lambda loc: loc[1])
            if locs.count(last) >= 2:
                brace = last

    store_at = {s[0] for s in stores}
    for i, index, symtab, line in stores:
        if fn.is_gcc:
            found = _gcc_await_location(insns, i, store_at, block, own_file, brace)
            if found is not None:
                symtab, line = found
            elif brace is not None:
                continue  # all we have is the store's own line: the brace
        if line:
            result[index] = (insns[i]["addr"], symtab, line)
    return result


# ---------------------------------------------------------------------------
# One coroutine frame
# ---------------------------------------------------------------------------

class DevirtualizedCoroFrame:
    """
    A coroutine frame whose frame/promise types were recovered from the debug
    info of its destroy (or resume) function.

    Implements `to_string` and `children` for gdb.printing.
    """

    def __init__(self, frame_ptr_raw: int, handle_type: typing.Optional[gdb.Type] = None):
        void = gdb.lookup_type("void")
        self.frame_ptr_raw = frame_ptr_raw
        self.resume_ptr = None
        self.destroy_ptr = None
        self.resume_fn = None   # _CoroFunction
        self.destroy_fn = None  # _CoroFunction
        self.is_gcc = False
        self.suspension_point_index = None
        self.suspend_pc = None
        self.suspend_symtab = None
        self.suspend_line = None
        self.promise_ptr = None
        self.frame_ptr = gdb.Value(frame_ptr_raw).cast(void.pointer())
        if frame_ptr_raw == 0:
            return

        psize = void.pointer().sizeof
        self.resume_ptr = _load_pointer_at(frame_ptr_raw)
        self.destroy_ptr = _load_pointer_at(frame_ptr_raw + psize)
        if self.resume_ptr:
            self.resume_fn = _coro_function(self.resume_ptr)
        if self.destroy_ptr:
            self.destroy_fn = _coro_function(self.destroy_ptr)

        # Prefer the destroy function: it stays valid at final suspend, where
        # Clang nulls the resume pointer.
        typed = None
        for fn in (self.destroy_fn, self.resume_fn):
            if fn is not None and fn.block is not None and (fn.frame_type is not None or fn.promise_type is not None):
                typed = fn
                break

        promise_type = void
        if handle_type is not None:
            try:
                arg = handle_type.template_argument(0)
                if arg.code != gdb.TYPE_CODE_VOID:
                    promise_type = arg.strip_typedefs()
            except Exception:
                pass
        if promise_type.code == gdb.TYPE_CODE_VOID and typed is not None and typed.promise_type is not None:
            promise_type = typed.promise_type

        # [coroutine.handle]: the promise follows the two function pointers,
        # padded to its own alignment. Both compilers must agree with
        # coroutine_handle::from_promise(), so this is ABI, not a guess.
        offset = 2 * psize
        try:
            align = promise_type.alignof
            if align > 1:
                offset = (offset + align - 1) & ~(align - 1)
        except Exception:
            pass
        self.promise_ptr = gdb.Value(frame_ptr_raw + offset).cast(promise_type.pointer())

        if typed is None or typed.frame_type is None:
            return
        self.is_gcc = typed.is_gcc
        self.frame_ptr = gdb.Value(frame_ptr_raw).cast(typed.frame_type.pointer())
        if typed.index_offset is None:
            return
        try:
            self.suspension_point_index = self._read_index(typed)
        except gdb.error:
            return
        if self.resume_fn is not None and self.resume_fn.block is not None and self.resume_fn.index_offset is not None:
            self._resolve_suspend_location()

    def _read_index(self, typed: _CoroFunction) -> int:
        size = typed.index_size
        if not typed.is_gcc:
            # Clang always describes `__coro_index` as a one-byte type, even
            # when a coroutine has more than 256 suspend points and the member
            # is really wider. The labels (or, without them, the stores) tell.
            top = typed.max_label_index
            if top is None:
                fn = self.resume_fn
                if fn is not None and fn.block is not None and fn.index_offset is not None:
                    fn.index_stores()
                    top = fn.max_store_index
            while top is not None and size < 8 and top >= (1 << (8 * size)):
                size *= 2
        address = self.frame_ptr_raw + typed.index_offset
        raw = bytes(gdb.selected_inferior().read_memory(address, size))
        return int.from_bytes(raw, "big" if "big" in gdb.execute("show endian", to_string=True) else "little")

    def _resolve_suspend_location(self):
        fn = self.resume_fn
        index = self.suspension_point_index
        if self.is_gcc:
            # The low bit selects the destroy path; resume.N is the even value.
            index &= ~1
            label = fn.label("resume.%d" % index)
        else:
            label = fn.label("__coro_resume_%d" % index)

        # 1. The label the compiler emits for this resume point.
        label_loc = None
        if label is not None:
            try:
                pc = int(label.value())
            except Exception:
                pc = 0  # no address: optimized away, or GCC < 16
            if pc:
                self.suspend_pc = pc
                if label.line > 0 and label.symtab is not None:
                    # Clang records the co_await's own location on the label.
                    self.suspend_symtab, self.suspend_line = label.symtab, label.line
                    return
                # GCC: only an address. At -O0 it is the await_resume call,
                # which has the co_await's line; in optimized code it is
                # whatever runs next, so prefer the store scan when it works.
                label_loc = _location_in_function(pc, fn.block)

        # 2. Where the suspend index is written.
        hit = fn.index_stores().get(index)
        if hit is not None:
            if self.suspend_pc is None:
                self.suspend_pc = hit[0]
            self.suspend_symtab, self.suspend_line = hit[1], hit[2]
        elif label_loc is not None and label_loc[1]:
            self.suspend_symtab, self.suspend_line = label_loc

    def _function(self) -> typing.Optional[_CoroFunction]:
        for fn in (self.destroy_fn, self.resume_fn):
            if fn is not None and fn.block is not None:
                return fn
        return None

    def looks_valid(self) -> bool:
        """
        True if the frame header points at the entry of a function for which we
        could recover the promise type. (The frame type itself is not required:
        Clang <= 19 omits it in optimized builds, which only costs the line.)
        """
        if self._function() is None or self.promise_ptr is None:
            return False
        return self.promise_ptr.type.target().strip_typedefs().code == gdb.TYPE_CODE_STRUCT

    def continuation(self):
        """
        (address of the awaiting coroutine's frame, is_fork), or (0, False).

        TMC keeps the awaiter in promise.customizer.continuation. When
        done_count is set (fork / spawn_many / spawn_tuple) it is a pointer to
        the handle instead of the handle itself.
        """
        try:
            customizer = self.promise_ptr.dereference()["customizer"]
            cont = int(customizer["continuation"])
            is_fork = int(customizer["done_count"]) != 0
            if cont != 0 and is_fork:
                cont = _load_pointer_at(cont)
            return cont, (is_fork and cont != 0)
        except Exception:
            return 0, False

    def get_function_name(self):
        fn = self._function()
        return fn.name if fn is not None else None

    def function_symtab(self):
        fn = self._function()
        return fn.block.function.symtab if fn is not None else None

    def to_string(self):
        if self.frame_ptr_raw == 0:
            return "coro(nullptr)"
        result = "coro(0x%x)" % self.frame_ptr_raw
        name = self.get_function_name()
        if name is not None:
            result += ": " + name
        if self.suspend_line:
            result += " at %s:%d" % (self.suspend_symtab.filename, self.suspend_line)
        if self.suspension_point_index is not None:
            result += ", suspension point " + str(self.suspension_point_index)
        return result

    def children(self):
        if self.resume_ptr is None:
            return [("coro_frame", self.frame_ptr)]
        return [
            ("resume", gdb.Value(self.resume_ptr).cast(_void_ptr())),
            ("destroy", gdb.Value(self.destroy_ptr).cast(_void_ptr())),
            ("promise", self.promise_ptr),
            ("coro_frame", self.frame_ptr),
        ]


# ---------------------------------------------------------------------------
# Pretty printer and convenience functions
# ---------------------------------------------------------------------------

_corohdl_regex = re.compile(r"^std::(__[A-Za-z0-9]+::)?coroutine_handle<.*>$")


def _handle_to_coro_frame(val: gdb.Value) -> DevirtualizedCoroFrame:
    if val.type.code in (gdb.TYPE_CODE_REF, gdb.TYPE_CODE_RVALUE_REF):
        val = val.referenced_value()
    handle_type = val.type.strip_typedefs()
    if handle_type.code == gdb.TYPE_CODE_PTR:
        val = val.dereference()
        handle_type = val.type.strip_typedefs()
    if handle_type.tag is None or _corohdl_regex.match(handle_type.tag) is None:
        raise ValueError("Expected a std::coroutine_handle, got %s" % val.type)
    # Both standard libraries store exactly one member: the frame pointer.
    fields = handle_type.fields()
    if len(fields) != 1:
        raise ValueError("Expected 1 field, got %d" % len(fields))
    return DevirtualizedCoroFrame(int(val[fields[0]]), handle_type)


class CoroutineHandlePrettyPrinter(gdb.printing.PrettyPrinter):
    """Pretty printer for std::coroutine_handle<T> (libc++ and libstdc++)."""

    def __init__(self):
        super().__init__("tmc_coroutine_handle")

    def __call__(self, val: gdb.Value):
        # Never raise from here: an exception is printed once per value, which
        # floods `bt` in optimized builds where handles are <optimized out>.
        try:
            t = val.type
            if t.code in (gdb.TYPE_CODE_REF, gdb.TYPE_CODE_RVALUE_REF):
                t = t.target()
            tag = t.strip_typedefs().tag
            if tag is None or _corohdl_regex.match(tag) is None or val.is_optimized_out:
                return None
            return _handle_to_coro_frame(val)
        except Exception:
            return None


# replace=True so that sourcing the script twice does not abort half way.
gdb.printing.register_pretty_printer(gdb.current_objfile(), CoroutineHandlePrettyPrinter(), replace=True)


class GetCoroFrame(gdb.Function):
    """p *$get_coro_frame(handle)"""

    def __init__(self):
        super().__init__("get_coro_frame")

    def invoke(self, handle):
        return _handle_to_coro_frame(handle).frame_ptr


class GetCoroPromise(gdb.Function):
    """p *$get_coro_promise(handle)"""

    def __init__(self):
        super().__init__("get_coro_promise")

    def invoke(self, handle):
        return _handle_to_coro_frame(handle).promise_ptr


GetCoroFrame()
GetCoroPromise()


# ---------------------------------------------------------------------------
# Frame filter
# ---------------------------------------------------------------------------

class CoroutineFrameDecorator(FrameDecorator):
    """A synthetic frame for a suspended coroutine in the continuation chain."""

    def __init__(self, coro_frame: DevirtualizedCoroFrame, inferior_frame: gdb.Frame, is_fork: bool):
        super().__init__(inferior_frame)
        self.coro_frame = coro_frame
        self.is_fork = is_fork

    def function(self):
        prefix = "[fork] [async] " if self.is_fork else "[async] "
        name = self.coro_frame.get_function_name()
        if name is None:
            name = "coroutine (coro_frame=0x%x)" % self.coro_frame.frame_ptr_raw
        return prefix + name

    def address(self):
        # Where the coroutine will continue, if known; else its resume function.
        return self.coro_frame.suspend_pc or self.coro_frame.resume_ptr

    def filename(self):
        # File and line must come from the same place: the suspend point may
        # be in a different file than the function's declaration.
        symtab = self.coro_frame.suspend_symtab or self.coro_frame.function_symtab()
        return symtab.filename if symtab is not None else None

    def line(self):
        return self.coro_frame.suspend_line

    def frame_args(self):
        return []

    def frame_locals(self):
        return []


def _frame_local(frame: gdb.Frame, names):
    """A variable/argument declared in frame's own function (never a global)."""
    try:
        block = frame.block()
    except RuntimeError:
        return None
    while block is not None and not block.is_static and not block.is_global:
        for sym in block:
            if sym.name in names:
                return sym
        if block.function is not None:
            break
        block = block.superblock
    return None


def _is_clang_coro_clone(frame: gdb.Frame) -> bool:
    """
    True for the .resume/.destroy/.cleanup clones, False for the ramp.

    The ramp declares `__coro_frame` and `__promise` too, but until it has run
    the promise constructor they hold whatever the allocator returned - usually
    a recycled frame with a stale continuation. Its caller is on the real stack
    anyway. Clang >= 21 gives each clone its own DW_AT_linkage_name; before
    that the clones reuse the ramp's, so fall back to the ELF symbol.
    """
    function = frame.function()
    if function is not None and (function.linkage_name or "").endswith(_CLANG_CLONE_SUFFIXES):
        return True
    elf_name = _elf_symbol_at(frame.pc())
    if elf_name is None:
        return True  # no symbol table to consult; accept
    return _CLANG_CLONE_RE.search(elf_name) is not None


def _physical_coro_frame_address(frame: gdb.Frame) -> typing.Optional[int]:
    """
    If `frame` is executing a coroutine body, return the address of its
    coroutine frame.

    Uses the frame variable rather than the promise variable: the promise is
    <optimized out> in optimized builds of both compilers, the frame is not.
    """
    try:
        sym = _frame_local(frame, ("__coro_frame", "frame_ptr"))
        if sym is None:
            return None
        t = sym.type.strip_typedefs()
        if sym.name == "__coro_frame":
            # Clang: the variable is the frame itself.
            if t.code != gdb.TYPE_CODE_STRUCT or _field(t, "__resume_fn") is None:
                return None
            address = frame.read_var(sym).address
            if address is None or not _is_clang_coro_clone(frame):
                return None
            return int(address)
        # GCC: the actor (and destroy stub) take the frame as `frame_ptr`. The
        # ramp has no such variable.
        if t.code != gdb.TYPE_CODE_PTR:
            return None
        t = t.target().strip_typedefs()
        if t.code != gdb.TYPE_CODE_STRUCT or _field(t, "_Coro_resume_fn") is None:
            return None
        value = frame.read_var(sym)
        return None if value.is_optimized_out else int(value)
    except Exception:
        return None


class CppCoroutineFrameFilter:
    """Adds [async] frames to `bt` / -stack-list-frames."""

    def __init__(self):
        self.name = "CppCoroutineFrameFilter"
        self.priority = 50
        self.enabled = True
        gdb.frame_filters[self.name] = self

    def filter(self, frame_iter):
        # Coroutine frames already shown, as physical or synthetic frames.
        # Without tail calls (GCC -O0) symmetric transfer nests resumptions, so
        # the same coroutine - or an already destroyed one - shows up again
        # further down the real stack; its chain must not be repeated.
        seen = set()
        for frame in frame_iter:
            yield frame
            try:
                inferior_frame = frame.inferior_frame()
                if inferior_frame is None or not inferior_frame.is_valid():
                    continue
                addr = _physical_coro_frame_address(inferior_frame)
                if not addr or addr in seen:
                    continue
                seen.add(addr)
                chain = self._chain(addr, seen)
            except Exception:
                continue
            for coro_frame, is_fork in chain:
                yield CoroutineFrameDecorator(coro_frame, inferior_frame, is_fork)

    @staticmethod
    def _chain(root_addr: int, seen: set):
        chain = []
        root = DevirtualizedCoroFrame(root_addr)
        if not root.looks_valid():
            return chain
        addr, is_fork = root.continuation()
        while addr != 0 and addr not in seen and len(chain) < MAX_ASYNC_DEPTH:
            seen.add(addr)
            try:
                coro_frame = DevirtualizedCoroFrame(addr)
            except Exception:
                break
            if not coro_frame.looks_valid():
                break
            chain.append((coro_frame, is_fork))
            addr, is_fork = coro_frame.continuation()
        return chain


CppCoroutineFrameFilter()
