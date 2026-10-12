import re
import os
"""x86lift - static recompiler from 32-bit x86 (MSVC code) to C.

Each function becomes `void F_xxxxxxxx(void)` operating on the runtime in recomp_rt.h:
registers in locals (written back around calls), flags in locals, x87 stack in the global CPU
state, memory through RD*/WR* at the original addresses.

Usage as a module: Lifter(pe, entries, hle).function(addr) -> C source for one function.
"""
import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import (X86_OP_REG, X86_OP_IMM, X86_OP_MEM, X86_REG_INVALID, X86_REG_FS,
                          X86_PREFIX_REP, X86_PREFIX_REPNE)

REG32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
# name -> (32-bit register, shift, mask) for partial registers
SUBREG = {}
for r in REG32:
    SUBREG[r] = (r, 0, 0xffffffff)
for r, base in [('ax', 'eax'), ('cx', 'ecx'), ('dx', 'edx'), ('bx', 'ebx'), ('sp', 'esp'), ('bp', 'ebp'),
                ('si', 'esi'), ('di', 'edi')]:
    SUBREG[r] = (base, 0, 0xffff)
for r, base in [('al', 'eax'), ('cl', 'ecx'), ('dl', 'edx'), ('bl', 'ebx')]:
    SUBREG[r] = (base, 0, 0xff)
for r, base in [('ah', 'eax'), ('ch', 'ecx'), ('dh', 'edx'), ('bh', 'ebx')]:
    SUBREG[r] = (base, 8, 0xff)

MASK = {1: '0xffu', 2: '0xffffu', 4: '0xffffffffu'}
MSB = {1: 7, 2: 15, 4: 31}
CTYPE = {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t'}
STYPE = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t'}

COND = {
    'o': 'of', 'no': '!of', 'b': 'cf', 'ae': '!cf', 'e': 'zf', 'ne': '!zf', 'be': '(cf|zf)',
    'a': '(!cf&&!zf)', 's': 'sf', 'ns': '!sf', 'p': 'pf', 'np': '!pf', 'l': '(sf!=of)',
    'ge': '(sf==of)', 'le': '(zf||sf!=of)', 'g': '(!zf&&sf==of)',
}


class LiftError(Exception):
    pass


class Lifter:
    native_pred = None      # optional: address -> True if it belongs to a native module

    def __init__(self, pe, entries, hle=None, max_size=0x10000):
        """entries: set of known function entry addresses (call targets / tail-call targets).
        hle: dict addr -> C function name for functions replaced by native code."""
        self.pe = pe
        self.entries = set(entries)
        self.hle = hle or {}
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        self.max_size = max_size
        # instruction address -> C expression replacing its immediate operand (runtime settings
        # in place of values that patchers wrote into the code, see plan.IMM_OVERRIDES)
        self.imm_overrides = {}
        # LIFT_NO_FPU_LOCALS / LIFT_NO_INTRINSICS switch these off (debugging, A/B tests)
        self.fpu_locals = not os.environ.get('LIFT_NO_FPU_LOCALS')     # x87 stack in locals where provable
        self.intrinsics = not os.environ.get('LIFT_NO_INTRINSICS')     # inline MSVC runtime helpers
        self.inline_leaves = not os.environ.get('LIFT_NO_INLINE')      # inline small leaf functions
        # ... also with x87 code (opt-in: identical results, but no measurable gain on the PC)
        self.inline_x87 = bool(os.environ.get('LIFT_INLINE_X87'))
        self.addr64 = not os.environ.get('LIFT_NO_ADDR64')            # 64-bit signed address arithmetic
        self.preserve = not os.environ.get('LIFT_NO_PRESERVE')        # skip callee-saved round trips
        self.cur_mask = 0

    # ---------------------------------------------------------------- decoding / CFG

    def decode(self, addr):
        sec = self.pe.section_of(addr)
        if not sec or sec['name'] != '.text':
            raise LiftError('branch outside .text to %x' % addr)
        code = self.pe.read(addr, 16)
        for ins in self.md.disasm(code, addr):
            return ins
        raise LiftError('cannot decode at %x' % addr)

    def jump_table(self, fn_insns, jmp):
        ts = self._jump_table(fn_insns, jmp)
        table = jmp.operands[0].mem.disp & 0xffffffff
        entry = min(fn_insns)
        span = max(max(fn_insns), table) + 0x400
        for k in range(1, 33):                  # entries before the base (neg idx patterns)
            t = self.pe.u32(table - 4 * k)
            if not (entry <= t < span):
                break
            ts.append(t)
        return ts

    def _jump_table(self, fn_insns, jmp):
        """Targets of `jmp [idx*4 + table]` using the bounding `cmp idx, N` before it."""
        op = jmp.operands[0]
        table = op.mem.disp & 0xffffffff
        # find "cmp reg, imm" in the instructions just before the jump
        prev = sorted(a for a in fn_insns if a < jmp.address)
        bound, bytetab = None, None
        idx_reg = op.mem.index
        for k, a in enumerate(reversed(prev[-40:])):
            ins = fn_insns[a]
            # "and idx, mask" bounds the index too (MSVC memcpy: and edx, 3)
            if ins.mnemonic == 'and' and ins.operands[0].type == X86_OP_REG and ins.operands[0].reg == idx_reg \
                    and ins.operands[1].type == X86_OP_IMM:
                bound = ins.operands[1].imm & 0xffffffff
                break
            if k >= 12:
                continue
            # byte table mapping the switch value to a case number: "movzx idx, byte ptr [x + tab]"
            # or "xor idx, idx; mov idx8, byte ptr [x + tab]"
            if ins.mnemonic in ('movzx', 'mov') and len(ins.operands) == 2 and ins.operands[1].type == X86_OP_MEM \
                    and ins.operands[1].size == 1 and ins.operands[0].type == X86_OP_REG \
                    and SUBREG.get(ins.reg_name(ins.operands[0].reg), ('',))[0] == SUBREG.get(ins.reg_name(idx_reg), ('?',))[0]:
                bytetab = (ins.operands[1].mem.disp & 0xffffffff)
            if ins.mnemonic == 'cmp' and ins.operands[1].type == X86_OP_IMM:
                bound = ins.operands[1].imm & 0xffffffff
                break
            if ins.mnemonic == 'cmp' and ins.operands[1].type == X86_OP_REG:
                # bound held in a register: find its constant load earlier in the function
                reg = ins.operands[1].reg
                for b in reversed([x for x in prev if x < a][-64:]):
                    i2 = fn_insns[b]
                    if i2.mnemonic == 'mov' and i2.operands[0].type == X86_OP_REG and i2.operands[0].reg == reg \
                            and i2.operands[1].type == X86_OP_IMM:
                        bound = i2.operands[1].imm & 0xffffffff
                        break
                break
        if bound is None or bound > 1024:
            # MSVC places the table after the function's code: take entries that point between
            # the function start and the table.
            entry = min(fn_insns)
            span = max(max(fn_insns), table) + 0x400
            ts = []
            while len(ts) < 64:
                t = self.pe.u32(table + 4 * len(ts))
                if not (entry <= t < span) or table <= t < table + 4 * (len(ts) + 1):
                    break
                ts.append(t)
            if not ts or bytetab is not None:
                raise LiftError('jump table at %x without bound' % jmp.address)
            return ts
        if bytetab is not None:
            n = max(self.pe.read(bytetab, bound + 1)) + 1
        else:
            n = bound + 1
        return [self.pe.u32(table + 4 * i) for i in range(n)]

    def cfg(self, entry):
        """Recursive descent from entry. Returns {addr: insn}, label set, jump tables."""
        insns, labels, tables, bad = {}, {entry}, {}, set()
        self.bad = bad
        work = [(entry, False)]
        while work:
            a, forced = work.pop()
            start = a
            while a not in insns and a not in bad:
                # Fall-through into another function ends this one (handled as a tail call), but
                # explicit branch targets are always local code, even if listed as an entry.
                if a != entry and a in self.entries and not (forced and a == start):
                    break
                try:
                    ins = self.decode(a)
                except LiftError:
                    if a == entry:
                        raise
                    bad.add(a)          # path runs into non-code (e.g. after a no-return call)
                    labels.add(a)
                    break
                insns[a] = ins
                m = ins.mnemonic
                nxt = a + ins.size
                if m.startswith('j') and m != 'jmp':
                    t = ins.operands[0].imm & 0xffffffff
                    labels.add(t)
                    work.append((t, True))
                    labels.add(nxt)
                elif m == 'jmp':
                    op = ins.operands[0]
                    if op.type == X86_OP_IMM:
                        t = op.imm & 0xffffffff
                        if t not in self.entries or t == entry:
                            labels.add(t)
                            work.append((t, True))
                    elif op.type == X86_OP_MEM and op.mem.index != X86_REG_INVALID and op.mem.scale == 4 \
                            and op.mem.base == X86_REG_INVALID:
                        tables[a] = None        # resolved after all instructions are known
                    break
                elif m in ('ret', 'retn') or m == 'int3' or m == 'hlt':
                    break
                a = nxt
            if work:
                continue                # resolve tables once all other code is decoded
            for ja in list(tables):
                if tables[ja] is None:
                    ts = self.jump_table(insns, insns[ja])
                    tables[ja] = ts
                    for t in ts:
                        labels.add(t)
                        work.append((t, True))
        return insns, labels, tables

    # ---------------------------------------------------------------- operands

    def reg_read(self, ins, rid):
        name = ins.reg_name(rid)
        if name not in SUBREG:
            raise LiftError('unsupported register %s at %x' % (name, ins.address))
        r, sh, mask = SUBREG[name]
        if mask == 0xffffffff:
            return r
        return '((%s >> %d) & 0x%xu)' % (r, sh, mask) if sh else '(%s & 0x%xu)' % (r, mask)

    def reg_write(self, ins, rid, val):
        name = ins.reg_name(rid)
        if name not in SUBREG:
            raise LiftError('unsupported register %s at %x' % (name, ins.address))
        r, sh, mask = SUBREG[name]
        if mask == 0xffffffff:
            return '%s = (uint32_t)(%s);' % (r, val)
        return '%s = (%s & ~0x%xu) | (((uint32_t)(%s) & 0x%xu) << %d);' % (r, r, mask << sh, val, mask, sh)

    def addr32(self, ins, op):
        """The effective address as a 32-bit value (lea)."""
        m = op.mem
        parts = []
        if m.base != X86_REG_INVALID:
            parts.append(self.reg_read(ins, m.base))
        if m.index != X86_REG_INVALID:
            idx = self.reg_read(ins, m.index)
            parts.append(idx if m.scale == 1 else '%s*%d' % (idx, m.scale))
        if m.disp or not parts:
            parts.append('0x%xu' % (m.disp & 0xffffffff))
        e = '(uint32_t)(' + ' + '.join(parts) + ')'
        if m.segment == X86_REG_FS:
            e = '(RT_FS_BASE + %s)' % e
        return e

    def addr(self, ins, op):
        """The address of a memory access as RtAddr: registers sign-extended, 64-bit sums (see
        recomp_rt.h); same result as 32-bit wrap-around for all addresses below 2 GB."""
        if not self.addr64:
            return self.addr32(ins, op)
        m = op.mem
        regs = []
        if m.base != X86_REG_INVALID:
            regs.append(self.reg_read(ins, m.base))
        if m.index != X86_REG_INVALID:
            idx = self.reg_read(ins, m.index)
            regs.append(idx if m.scale == 1 else '%s*%du' % (idx, m.scale))
        parts = []
        d = m.disp & 0xffffffff
        if regs:
            # base + index wrap at 32 bits like x86; the displacement is added in 64 bits so it
            # folds into the addressing mode (the guest alias keeps the result exact)
            parts.append('(RtAddr)(int32_t)(%s)' % ' + '.join(regs) if len(regs) > 1 or '*' in regs[0]
                         else '(RtAddr)(int32_t)%s' % regs[0])
            if d >= 0x80000000:
                d -= 1 << 32
            if d:
                parts.append('%d' % d)
        else:
            parts.append('(RtAddr)0x%xu' % d)
        e = '(' + ' + '.join(parts) + ')'
        if m.segment == X86_REG_FS:
            e = '((RtAddr)RT_FS_BASE + %s)' % e
        return e

    def rd(self, ins, op, size=None):
        size = size or op.size
        if op.type == X86_OP_REG:
            return self.reg_read(ins, op.reg)
        if op.type == X86_OP_IMM:
            if ins.address in self.imm_overrides:
                return '(%s)' % self.imm_overrides[ins.address]
            return '0x%xu' % (op.imm & ((1 << (8 * size)) - 1))
        return 'RD%d(%s)' % (size * 8, self.addr(ins, op))

    def wr(self, ins, op, val):
        if op.type == X86_OP_REG:
            return self.reg_write(ins, op.reg, val)
        return 'WR%d(%s, %s);' % (op.size * 8, self.addr(ins, op), val)

    # ---------------------------------------------------------------- flags

    def flags_logic(self, res, size):
        return ('cf = 0; of = 0; zf = ((%s) & %s) == 0; sf = ((%s) >> %d) & 1; pf = PARITY(%s);'
                % (res, MASK[size], res, MSB[size], res))

    def flags_add(self, a, b, res, size, carry_in='0'):
        msk, msb = MASK[size], MSB[size]
        return ('cf = ((uint64_t)(%s) + (uint64_t)(%s) + %s) > %s; of = ((((%s) ^ (%s)) & ((%s) ^ (%s))) >> %d) & 1; '
                'zf = ((%s) & %s) == 0; sf = ((%s) >> %d) & 1; pf = PARITY(%s);'
                % (a, b, carry_in, msk, a, res, b, res, msb, res, msk, res, msb, res))

    def flags_sub(self, a, b, res, size, borrow_in='0'):
        msk, msb = MASK[size], MSB[size]
        return ('cf = (uint64_t)(%s) < (uint64_t)(%s) + %s; of = ((((%s) ^ (%s)) & ((%s) ^ (%s))) >> %d) & 1; '
                'zf = ((%s) & %s) == 0; sf = ((%s) >> %d) & 1; pf = PARITY(%s);'
                % (a, b, borrow_in, a, b, a, res, msb, res, msk, res, msb, res))

    # ---------------------------------------------------------------- translation

    def is_native(self, t):
        if t not in self.hle and self.native_pred and self.native_pred(t):
            self.hle[t] = 'N_%08x' % t          # a function inside a natively replaced module
        return t in self.hle

    def call_stmt(self, t):
        """A call to t in the middle of a function (registers in locals before and after)."""
        if self.is_native(t):
            return 'SAVE(); %s(); LOAD();' % self.hle[t]
        m = self.preserve_mask(t)
        return 'RT_CALLM(F_%08x, %d);' % (t, m) if m else 'RT_CALL(F_%08x);' % t

    # ---------------------------------------------------------------- callee-saved registers

    def adapter(self, f, translated=True):
        """The R-based entry A_f (indirect calls, rt_call) for translated function f. A function
        that preserves a callee-saved register does not write it back to R, so R may still hold
        what one of its callees left there: the adapter restores the entry value, which is the
        register's value on return by definition."""
        m = self.preserve_mask(f) if translated else 0
        if not m:
            return ('static void A_%08x(void) { RtRet r_ = F_%08x(R.eax, R.ecx, R.edx, R.ebx, R.esp, R.ebp, R.esi, R.edi); '
                    'R.eax = r_.eax; R.edx = r_.edx; R.ecx = r_.ecx; R.esp = r_.esp; }' % (f, f))
        keep = ''.join('R.%s = %s_; ' % (n, n) for i, (n, _) in enumerate(self.CALLEE_SAVED) if m & (1 << i))
        return ('static void A_%08x(void) { uint32_t ebx_ = R.ebx, ebp_ = R.ebp, esi_ = R.esi, edi_ = R.edi; '
                '(void)ebx_; (void)ebp_; (void)esi_; (void)edi_; '
                'RtRet r_ = F_%08x(R.eax, R.ecx, R.edx, ebx_, R.esp, ebp_, esi_, edi_); '
                'R.eax = r_.eax; R.edx = r_.edx; R.ecx = r_.ecx; R.esp = r_.esp; %s}' % (f, f, keep))

    CALLEE_SAVED = (('ebx', ('ebx', 'bx', 'bl', 'bh')), ('ebp', ('ebp', 'bp')),
                    ('esi', ('esi', 'si')), ('edi', ('edi', 'di')))

    def preserve_mask(self, t):
        """Bit i set: translated function t provably returns CALLEE_SAVED[i] unchanged, so it need
        not pass it back through R and callers keep their copy (RT_RETM / RT_CALLM). Proven when
        t saves the register in its prologue and restores it on every exit, or never writes it
        and every function it calls directly preserves it; tail-call targets must preserve it in
        either case. Natives and indirect callees are assumed to follow the x86 ABI (the original
        program relied on that)."""
        if not self.preserve:
            return 0
        memo = self.__dict__.setdefault('_pres', {})
        if t in memo:
            return memo[t]
        memo[t] = 0                                 # recursion: conservative
        if self.is_native(t):
            memo[t] = 15
            return 15
        try:
            saved = getattr(self, 'bad', None)
            insns, labels, tables = self.cfg(t)
            bad = self.bad
            self.bad = saved
        except Exception:
            return 0
        if bad or t not in insns:
            return 0
        written = [False] * 4
        callees, tails, exits = set(), set(), []
        for a, ins in insns.items():
            m = ins.mnemonic
            try:
                _, wr = ins.regs_access()
            except Exception:
                return 0
            for w in wr:
                n = ins.reg_name(w)
                for i, (_, subs) in enumerate(self.CALLEE_SAVED):
                    if n in subs:
                        written[i] = True
            nxt = a + ins.size
            if m in ('ret', 'retn'):
                exits.append((a, False))
            elif m == 'jmp':
                op = ins.operands[0]
                if op.type == X86_OP_IMM:
                    tt = op.imm & 0xffffffff
                    if tt not in labels:
                        exits.append((a, False))
                        tails.add(tt)
                elif a not in tables:
                    exits.append((a, False))        # indirect tail jump (ABI-following target)
            elif m == 'call':
                op = ins.operands[0]
                if op.type == X86_OP_IMM:
                    tt = op.imm & 0xffffffff
                    if not (self.intrinsics and tt in self.INTRINSICS):
                        callees.add(tt)
            if m not in ('ret', 'retn', 'jmp', 'int3', 'hlt') and nxt not in insns:
                exits.append((nxt, True))           # falls into the next function
                tails.add(nxt)
        prev = {a + ins.size: a for a, ins in insns.items()}

        def saved_in_prologue(name, subs):
            a = t
            for _ in range(16):
                ins = insns.get(a)
                if ins is None:
                    return False
                m = ins.mnemonic
                if m == 'push' and ins.operands[0].type == X86_OP_REG and ins.reg_name(ins.operands[0].reg) == name:
                    return True
                _, wr = ins.regs_access()
                if any(ins.reg_name(w) in subs for w in wr):
                    return False
                if m.startswith('j') or m in ('call', 'ret', 'retn', 'int3'):
                    return False
                a += ins.size
            return False

        def restored_at(exit_addr, virtual, name, subs):
            k = exit_addr
            first = True
            for _ in range(16):
                if not (virtual and first) and k in labels and k != t:
                    return False                    # a path joins here, skipping what is above
                first = False
                p = prev.get(k)
                if p is None:
                    return False
                ins = insns[p]
                m = ins.mnemonic
                if m == 'pop' and ins.operands[0].type == X86_OP_REG and ins.reg_name(ins.operands[0].reg) == name:
                    return True
                if m == 'leave' and name == 'ebp':
                    return True
                _, wr = ins.regs_access()
                if any(ins.reg_name(w) in subs for w in wr):
                    return False
                if m.startswith('j') or m in ('call', 'ret', 'retn', 'int3'):
                    return False
                k = p
            return False

        mask = 0
        for i, (name, subs) in enumerate(self.CALLEE_SAVED):
            bit = 1 << i
            if any(not (self.preserve_mask(tt) & bit) for tt in tails):
                continue
            if not written[i]:
                if all(self.preserve_mask(c) & bit for c in callees):
                    mask |= bit
                continue
            if saved_in_prologue(name, subs) and all(restored_at(a, v, name, subs) for a, v in exits):
                mask |= bit
        memo[t] = mask
        return mask

    def tail_stmt(self, t):
        """A tail call to t: its result is this function's result."""
        if self.is_native(t):
            return 'SAVE(); %s(); RT_RET_FROM_R();' % self.hle[t]
        # registers t preserves but this function's callers reload from R: t does not write
        # them back, so store the values it was given (its return values by definition)
        need = self.preserve_mask(t) & ~self.cur_mask
        if need:
            keep = ''.join('R.%s = %s; ' % (n, n) for i, (n, _) in enumerate(self.CALLEE_SAVED) if need & (1 << i))
            return '{ RtRet tr_ = F_%08x(RT_ARGS); %sreturn tr_; }' % (t, keep)
        return 'return F_%08x(RT_ARGS);' % t

    def callee_pop(self, t):
        """Bytes popped by function t (its ret N), for the optional stack-balance check."""
        if not hasattr(self, '_pops'):
            self._pops = {}
        if t not in self._pops:
            pop = None
            try:
                saved = getattr(self, 'bad', None)
                insns, _, _ = self.cfg(t)           # real control flow, not a linear sweep
                self.bad = saved
                pops = {(i.operands[0].imm if i.operands else 0) for i in insns.values() if i.mnemonic == 'ret'}
                addrs = sorted(insns)
                # "push reg; ret" returns to a computed address: frame helpers (SEH prolog,
                # _chkstk) that adjust the caller's esp on purpose
                tricky = any(insns[addrs[k]].mnemonic == 'ret' and k and insns[addrs[k - 1]].mnemonic == 'push'
                             for k in range(len(addrs)))
                if len(pops) == 1 and not tricky:
                    pop = pops.pop()
            except Exception:
                pass
            self._pops[t] = pop
        return self._pops[t]

    def inline_body(self, t):
        """C text of a small straight-line leaf function (no branches, calls or x87 code, one
        final ret) to emit at its call sites, or None. Memoised."""
        if not hasattr(self, '_inline'):
            self._inline = {}
        if t in self._inline:
            return self._inline[t]
        body = None
        if self.inline_leaves and t not in self.hle and not (self.native_pred and self.native_pred(t)) \
                and t not in self.INTRINSICS:
            try:
                saved = getattr(self, 'bad', None)
                insns, labels, tables = self.cfg(t)
                bad = self.bad
                self.bad = saved
                addrs = sorted(insns)
                ok = (not bad and not tables and 0 < len(addrs) <= 16 and addrs[0] == t
                      and all(addrs[k] + insns[addrs[k]].size == addrs[k + 1] for k in range(len(addrs) - 1)))
                if ok:
                    lines = []
                    for k, a in enumerate(addrs):
                        ins = insns[a]
                        m = ins.mnemonic
                        last = k == len(addrs) - 1
                        if m in ('ret', 'retn'):
                            if not last:
                                ok = False
                                break
                            lines.append('esp += %d;' % (4 + (ins.operands[0].imm if ins.operands else 0)))
                        # x87 code is allowed: inlined calls go through the caller's x87 stack analysis
                        # like its own instructions (and keep the emulated stack where that fails);
                        # x87 memory operands on esp only read or write the arguments
                        elif last or m.startswith('j') or m in ('call', 'int3', 'hlt', 'leave', 'cpuid') \
                                or (not self.inline_x87 and m.startswith('f')) \
                                or 'esp' in ins.op_str and m not in ('mov', 'push', 'pop', 'lea') \
                                and not (self.inline_x87 and m.startswith('f')):
                            ok = False
                            break
                        else:
                            self.insn(ins, lines, {}, set())
                    if ok:
                        body = '{ /* inlined %08x */ %s }' % (t, ' '.join(lines))
            except Exception:
                body = None
        self._inline[t] = body
        return body

    def emit_call(self, ins, out, target_expr=None, target=None):
        if target is not None and self.intrinsics and target in self.INTRINSICS:
            out.append(self.INTRINSICS[target])
            return
        if target is not None:
            body = self.inline_body(target)
            if body:
                out.append('WR32(RT_SP(-4), 0x%xu); esp -= 4; %s' % (ins.address + ins.size, body))
                return
        ret = ins.address + ins.size
        # __chkstk / _alloca_probe (0x4a4700) moves the caller's esp on purpose
        pop = self.callee_pop(target) if target is not None and target not in self.hle and target != 0x4a4700 else None
        if pop is not None:
            out.append('{ RT_ESP_MARK();')
        if target is None:
            # the operand may be esp-relative: read it before pushing the return address
            out.append('{ uint32_t ct_ = %s;' % target_expr)
        out.append('WR32(RT_SP(-4), 0x%xu); esp -= 4;' % ret)
        if target is not None:
            out.append(self.call_stmt(target))
        else:
            out.append('SAVE(); RT_CallAddr(ct_); LOAD(); }')
        if pop is not None:
            out.append('RT_ESP_CHECK(%d, 0x%xu, 0x%xu); }' % (pop, ins.address, target))

    def fpu_operand(self, ins, op):
        if op.type == X86_OP_REG:
            n = ins.reg_name(op.reg)           # st(i)
            return 'ST(%s)' % (n[3] if len(n) > 3 else '0')
        if op.size == 4:
            return 'RDF32(%s)' % self.addr(ins, op)
        if op.size == 8:
            return 'RDF64(%s)' % self.addr(ins, op)
        if op.size == 10:
            return 'RDF80(%s)' % self.addr(ins, op)
        raise LiftError('fpu operand size %d at %x' % (op.size, ins.address))

    def st_index(self, ins, op):
        n = ins.reg_name(op.reg)
        return int(n[3]) if len(n) > 3 else 0

    def insn(self, ins, out, tables, labels):
        m = ins.mnemonic
        for pfx in ('lock ', 'rep ', 'repe ', 'repne ', 'repz ', 'repnz '):
            if m.startswith(pfx):
                m = m[len(pfx):]
        ops = ins.operands
        a = ins.address
        nxt = a + ins.size
        rep = X86_PREFIX_REP in ins.prefix
        repne = X86_PREFIX_REPNE in ins.prefix

        if m in ('mov',):
            out.append(self.wr(ins, ops[0], self.rd(ins, ops[1])))
        elif m == 'movsx':
            s = ops[1].size
            out.append(self.wr(ins, ops[0], '(uint32_t)(int32_t)(%s)(%s)' % (STYPE[s], self.rd(ins, ops[1]))))
        elif m == 'movzx':
            out.append(self.wr(ins, ops[0], self.rd(ins, ops[1])))
        elif m == 'lea':
            out.append(self.wr(ins, ops[0], self.addr32(ins, ops[1])))
        elif m == 'push':
            v = self.rd(ins, ops[0], 4) if ops[0].type != X86_OP_IMM or ins.address in self.imm_overrides \
                else '0x%xu' % (ops[0].imm & 0xffffffff)
            out.append('{ uint32_t v_ = %s; WR32(RT_SP(-4), v_); esp -= 4; }' % v)
        elif m == 'pop':
            out.append('{ uint32_t v_ = RD32(RT_SP(0)); esp += 4; %s }' % self.wr(ins, ops[0], 'v_'))
        elif m == 'xchg':
            out.append('{ uint32_t t_ = %s; %s %s }' % (self.rd(ins, ops[0]), self.wr(ins, ops[0], self.rd(ins, ops[1])),
                                                       self.wr(ins, ops[1], 't_')))
        elif m in ('add', 'sub', 'cmp', 'adc', 'sbb'):
            s = ops[0].size
            out.append('{ uint32_t a_ = %s, b_ = %s;' % (self.rd(ins, ops[0]), self.rd(ins, ops[1], s)))
            if m == 'add':
                out.append('uint32_t r_ = (a_ + b_) & %s; %s %s }' % (MASK[s], self.flags_add('a_', 'b_', 'r_', s),
                                                                     self.wr(ins, ops[0], 'r_')))
            elif m == 'adc':
                out.append('uint32_t c_ = cf; uint32_t r_ = (a_ + b_ + c_) & %s; %s %s }'
                           % (MASK[s], self.flags_add('a_', 'b_', 'r_', s, 'c_'), self.wr(ins, ops[0], 'r_')))
            elif m == 'sbb':
                out.append('uint32_t c_ = cf; uint32_t r_ = (a_ - b_ - c_) & %s; %s %s }'
                           % (MASK[s], self.flags_sub('a_', 'b_', 'r_', s, 'c_'), self.wr(ins, ops[0], 'r_')))
            else:
                w = self.wr(ins, ops[0], 'r_') if m == 'sub' else ''
                out.append('uint32_t r_ = (a_ - b_) & %s; %s %s }' % (MASK[s], self.flags_sub('a_', 'b_', 'r_', s), w))
        elif m in ('and', 'or', 'xor', 'test'):
            s = ops[0].size
            op = {'and': '&', 'test': '&', 'or': '|', 'xor': '^'}[m]
            w = '' if m == 'test' else self.wr(ins, ops[0], 'r_')
            out.append('{ uint32_t r_ = (%s %s %s) & %s; %s %s }' % (self.rd(ins, ops[0]), op, self.rd(ins, ops[1], s),
                                                                    MASK[s], self.flags_logic('r_', s), w))
        elif m in ('inc', 'dec'):
            s = ops[0].size
            sign = '+' if m == 'inc' else '-'
            ofc = '0x%xu' % ((1 << MSB[s]) if m == 'inc' else ((1 << MSB[s]) - 1))
            out.append('{ uint32_t r_ = (%s %s 1) & %s; of = r_ == %s; zf = r_ == 0; sf = (r_ >> %d) & 1; pf = PARITY(r_); %s }'
                       % (self.rd(ins, ops[0]), sign, MASK[s], ofc, MSB[s], self.wr(ins, ops[0], 'r_')))
        elif m == 'neg':
            s = ops[0].size
            out.append('{ uint32_t a_ = %s; uint32_t r_ = (0u - a_) & %s; cf = a_ != 0; of = a_ == 0x%xu; '
                       'zf = r_ == 0; sf = (r_ >> %d) & 1; pf = PARITY(r_); %s }'
                       % (self.rd(ins, ops[0]), MASK[s], 1 << MSB[s], MSB[s], self.wr(ins, ops[0], 'r_')))
        elif m == 'not':
            s = ops[0].size
            out.append(self.wr(ins, ops[0], '(~%s) & %s' % (self.rd(ins, ops[0]), MASK[s])))
        elif m in ('shl', 'sal', 'shr', 'sar'):
            s = ops[0].size
            cnt = self.rd(ins, ops[1], 1) if len(ops) > 1 else '1'
            msb = MSB[s]
            if m in ('shl', 'sal'):
                body = ('r_ = (a_ << n_) & %s; cf = (a_ >> (%d - n_)) & 1; of = ((r_ >> %d) & 1) ^ cf;'
                        % (MASK[s], msb + 1, msb))
            elif m == 'shr':
                body = 'r_ = a_ >> n_; cf = (a_ >> (n_ - 1)) & 1; of = (a_ >> %d) & 1;' % msb
            else:
                body = ('r_ = (uint32_t)((int32_t)((%s)a_) >> n_) & %s; cf = (uint32_t)((int32_t)((%s)a_) >> (n_ - 1)) & 1; of = 0;'
                        % (STYPE[s], MASK[s], STYPE[s]))
            out.append('{ uint32_t a_ = %s, n_ = (%s) & 31, r_; if (n_) { %s zf = r_ == 0; sf = (r_ >> %d) & 1; pf = PARITY(r_); %s } }'
                       % (self.rd(ins, ops[0]), cnt, body, msb, self.wr(ins, ops[0], 'r_')))
        elif m == 'imul':
            if len(ops) == 1:
                s = ops[0].size
                if s == 1:
                    out.append('{ int32_t p_ = (int32_t)(int8_t)eax * (int8_t)%s; eax = (eax & ~0xffffu) | ((uint32_t)p_ & 0xffffu); '
                               'cf = of = p_ != (int32_t)(int8_t)p_; }' % self.rd(ins, ops[0]))
                    return
                if s == 2:
                    out.append('{ int32_t p_ = (int32_t)(int16_t)eax * (int16_t)%s; eax = (eax & ~0xffffu) | ((uint32_t)p_ & 0xffffu); '
                               'edx = (edx & ~0xffffu) | (((uint32_t)p_ >> 16) & 0xffffu); cf = of = p_ != (int32_t)(int16_t)p_; }'
                               % self.rd(ins, ops[0]))
                    return
                out.append('{ int64_t p_ = (int64_t)(int32_t)eax * (int32_t)%s; eax = (uint32_t)p_; edx = (uint32_t)(p_ >> 32); '
                           'cf = of = p_ != (int64_t)(int32_t)p_; }' % self.rd(ins, ops[0]))
            else:
                src2 = self.rd(ins, ops[2]) if len(ops) == 3 else self.rd(ins, ops[0])
                if len(ops) == 3:
                    src2 = '(uint32_t)(int32_t)%d' % ops[2].imm
                src1 = self.rd(ins, ops[1])
                out.append('{ int64_t p_ = (int64_t)(int32_t)%s * (int32_t)%s; %s cf = of = p_ != (int64_t)(int32_t)p_; }'
                           % (src1, src2, self.wr(ins, ops[0], '(uint32_t)p_')))
        elif m == 'mul':
            if ops[0].size != 4:
                raise LiftError('mul8/16 at %x' % a)
            out.append('{ uint64_t p_ = (uint64_t)eax * %s; eax = (uint32_t)p_; edx = (uint32_t)(p_ >> 32); cf = of = edx != 0; }'
                       % self.rd(ins, ops[0]))
        elif m in ('idiv', 'div'):
            if ops[0].size != 4:
                raise LiftError('%s8/16 at %x' % (m, a))
            if m == 'idiv':
                out.append('{ int64_t n_ = (int64_t)(((uint64_t)edx << 32) | eax); int32_t d_ = (int32_t)%s; '
                           'if (!d_) RT_Fatal("divide by zero", 0x%xu); eax = (uint32_t)(n_ / d_); edx = (uint32_t)(n_ %% d_); }'
                           % (self.rd(ins, ops[0]), a))
            else:
                out.append('{ uint64_t n_ = ((uint64_t)edx << 32) | eax; uint32_t d_ = %s; '
                           'if (!d_) RT_Fatal("divide by zero", 0x%xu); eax = (uint32_t)(n_ / d_); edx = (uint32_t)(n_ %% d_); }'
                           % (self.rd(ins, ops[0]), a))
        elif m == 'cdq':
            out.append('edx = (uint32_t)((int32_t)eax >> 31);')
        elif m == 'cwde':
            out.append('eax = (uint32_t)(int32_t)(int16_t)eax;')
        elif m == 'clc':
            out.append('cf = 0;')
        elif m == 'stc':
            out.append('cf = 1;')
        elif m == 'nop':
            pass
        elif m.startswith('set'):
            out.append(self.wr(ins, ops[0], COND[m[3:]]))
        elif m.startswith('j') and m != 'jmp':
            t = ops[0].imm & 0xffffffff
            if m == 'jecxz':
                out.append('if (!ecx) goto L_%08x;' % t)
            else:
                out.append('if (%s) goto L_%08x;' % (COND[m[1:]], t))
        elif m == 'jmp':
            op = ops[0]
            if a in tables:
                # switch on the target the table yields, so any index arithmetic (negative
                # indices, byte tables) works as long as the target is a known label
                cases = ' '.join('case 0x%xu: goto L_%08x;' % (t, t) for t in sorted(set(tables[a])))
                out.append('{ uint32_t t_ = RD32(%s); switch (t_) { %s default: RT_Fatal("jump table target", t_); } }'
                           % (self.addr(ins, op), cases))
            elif op.type == X86_OP_IMM:
                t = op.imm & 0xffffffff
                if t in labels:
                    out.append('goto L_%08x;' % t)
                else:
                    out.append('%s /* tail call */' % self.tail_stmt(t))
            else:
                out.append('{ uint32_t ct_ = %s; SAVE(); RT_CallAddr(ct_); RT_RET_FROM_R(); } /* indirect tail jump */' % self.rd(ins, op, 4))
        elif m == 'call':
            op = ops[0]
            if op.type == X86_OP_IMM:
                self.emit_call(ins, out, target=op.imm & 0xffffffff)
            else:
                self.emit_call(ins, out, target_expr=self.rd(ins, op, 4))
        elif m in ('ret', 'retn'):
            n = ops[0].imm if ops else 0
            out.append('esp += %d; RT_RETM(%d);' % (4 + n, self.cur_mask))
        elif m in ('movsd', 'movsb', 'movsw', 'stosd', 'stosb', 'stosw'):
            self.string_op(ins, out, m, rep)
        elif m == 'scasb':
            if not repne:
                raise LiftError('scasb without repne at %x' % a)
            out.append('{ uint32_t c_ = 0; zf = 0; while (ecx) { ecx--; c_ = RD8(edi); edi++; if ((c_ & 0xff) == (eax & 0xff)) { zf = 1; break; } } }')
        elif m.startswith('f'):
            self.fpu(ins, out)
        elif m in ('shld', 'shrd'):
            s_ = ops[0].size
            cnt = self.rd(ins, ops[2], 1) if ops[2].type != X86_OP_IMM else '%du' % (ops[2].imm & 31)
            if s_ != 4:
                raise LiftError('%s16 at %x' % (m, a))
            if m == 'shld':
                body = 'r_ = (d_ << n_) | (s2_ >> (32 - n_)); cf = (d_ >> (32 - n_)) & 1;'
            else:
                body = 'r_ = (d_ >> n_) | (s2_ << (32 - n_)); cf = (d_ >> (n_ - 1)) & 1;'
            out.append('{ uint32_t d_ = %s, s2_ = %s, n_ = (%s) & 31, r_; if (n_) { %s of = ((r_ ^ d_) >> 31) & 1; '
                       'zf = r_ == 0; sf = r_ >> 31; pf = PARITY(r_); %s } }'
                       % (self.rd(ins, ops[0]), self.rd(ins, ops[1]), cnt, body, self.wr(ins, ops[0], 'r_')))
        elif m in ('rcr', 'rcl'):
            s_ = ops[0].size
            cnt = self.rd(ins, ops[1], 1) if len(ops) > 1 else '1'
            bits = 8 * s_
            if m == 'rcr':
                step = 'r_ = (r_ >> 1) | ((uint64_t)cf << %d); cf = (uint32_t)(t_ & 1);' % (bits - 1)
                pre = 't_ = r_;'
            else:
                step = 'r_ = ((r_ << 1) | cf) & %s; cf = (uint32_t)((t_ >> %d) & 1);' % (MASK[s_], bits - 1)
                pre = 't_ = r_;'
            out.append('{ uint64_t r_ = %s, t_; uint32_t n_ = ((%s) & 31) %% %d; for (uint32_t i_ = 0; i_ < n_; i_++) { %s %s } '
                       'if (n_) { of = ((uint32_t)(r_ >> %d) ^ cf) & 1; %s } }'
                       % (self.rd(ins, ops[0]), cnt, bits + 1, pre, step, bits - 1, self.wr(ins, ops[0], '(uint32_t)r_')))
        elif m in ('cmpsb', 'cmpsd', 'cmpsw'):
            sz = {'b': 1, 'w': 2, 'd': 4}[m[-1]]
            one = ('{ uint32_t a_ = RD%d(esi), b_ = RD%d(edi), r_ = (a_ - b_) & %s; %s esi += df ? (uint32_t)-%d : %d; edi += df ? (uint32_t)-%d : %d; }'
                   % (sz * 8, sz * 8, MASK[sz], self.flags_sub('a_', 'b_', 'r_', sz), sz, sz, sz, sz))
            if X86_PREFIX_REP in ins.prefix:          # repe: continue while equal
                out.append('while (ecx) { ecx--; %s if (!zf) break; }' % one)
            elif X86_PREFIX_REPNE in ins.prefix:
                out.append('while (ecx) { ecx--; %s if (zf) break; }' % one)
            else:
                out.append(one)
        elif m in ('pushal', 'pushad'):
            out.append('{ uint32_t s_ = esp; esp -= 32; WR32(esp + 28, eax); WR32(esp + 24, ecx); WR32(esp + 20, edx); WR32(esp + 16, ebx); '
                       'WR32(esp + 12, s_); WR32(esp + 8, ebp); WR32(esp + 4, esi); WR32(esp, edi); }')
        elif m in ('popal', 'popad'):
            out.append('{ edi = RD32(esp); esi = RD32(RT_SP(4)); ebp = RD32(RT_SP(8)); ebx = RD32(esp + 16); edx = RD32(esp + 20); '
                       'ecx = RD32(esp + 24); eax = RD32(esp + 28); esp += 32; }')
        elif m == 'leave':
            out.append('esp = ebp; ebp = RD32(RT_SP(0)); esp += 4;')
        elif m == 'cld':
            out.append('df = 0;')
        elif m == 'std':
            out.append('df = 1;')
        elif m in ('wait', 'fwait'):
            pass
        elif m == 'cpuid':
            # Report a plain CPU (no MMX/SSE/3DNow!) so libraries take their x87 code paths.
            out.append('{ uint32_t l_ = eax; eax = l_ == 0 ? 1 : 0; ebx = ecx = edx = 0; if (l_ == 0) { ebx = 0x756e6547u; edx = 0x49656e69u; ecx = 0x6c65746eu; } }')
        elif m in ('pushfd', 'pushf'):
            out.append('{ uint32_t f_ = cf | (pf << 2) | (zf << 6) | (sf << 7) | (of << 11) | 0x202; esp -= 4; WR32(esp, f_); }')
        elif m in ('popfd', 'popf'):
            out.append('{ uint32_t f_ = RD32(esp); esp += 4; cf = f_ & 1; pf = (f_ >> 2) & 1; zf = (f_ >> 6) & 1; sf = (f_ >> 7) & 1; of = (f_ >> 11) & 1; }')
        elif m == 'sahf':
            out.append('{ uint32_t h_ = (eax >> 8) & 0xff; cf = h_ & 1; pf = (h_ >> 2) & 1; zf = (h_ >> 6) & 1; sf = (h_ >> 7) & 1; }')
        elif m == 'lahf':
            out.append('eax = (eax & ~0xff00u) | ((cf | (pf << 2) | (zf << 6) | (sf << 7) | 2) << 8);')
        elif m == 'int3':
            out.append('RT_Fatal("int3", 0x%xu);' % a)
        else:
            raise LiftError('unsupported %s %s at %x' % (m, ins.op_str, a))

    def string_op(self, ins, out, m, rep):
        """movs/stos with the direction flag (df) tracked in a local; MSVC code only sets it in
        memmove-style backward copies."""
        size = {'d': 4, 'w': 2, 'b': 1}[m[-1]]
        bits = size * 8
        if m.startswith('movs'):
            one = 'WR%d(edi, RD%d(esi)); esi += st_; edi += st_;' % (bits, bits)
            fast = {4: 'REP_MOVSD(&esi, &edi, &ecx);', 1: 'REP_MOVSB(&esi, &edi, &ecx);'}.get(size)
        else:
            one = 'WR%d(edi, eax); edi += st_;' % bits
            fast = {4: 'REP_STOSD(&edi, &ecx, eax);', 1: 'REP_STOSB(&edi, &ecx, eax);'}.get(size)
        if rep:
            fwd = fast if fast else 'while (ecx) { uint32_t st_ = %d; ecx--; %s }' % (size, one)
            out.append('if (!df) { %s } else { while (ecx) { uint32_t st_ = (uint32_t)-%d; ecx--; %s } }' % (fwd, size, one))
        else:
            out.append('{ uint32_t st_ = df ? (uint32_t)-%d : %d; %s }' % (size, size, one))

    def fpu(self, ins, out):
        m = ins.mnemonic
        ops = ins.operands
        a = ins.address
        b0, b1 = ins.bytes[0], ins.bytes[1] if len(ins.bytes) > 1 else 0
        if b1 >= 0xc0 and self.fpu_regform(ins, out, b0, b1):
            return
        arith = {'add': '+', 'mul': '*', 'sub': '-', 'div': '/'}
        if m == 'fld':
            out.append('{ double v_ = %s; FPUSH(v_); }' % self.fpu_operand(ins, ops[0]))
        elif m == 'fild':
            sz = ops[0].size
            v = {2: '(int16_t)RD16(%s)', 4: '(int32_t)RD32(%s)', 8: '(int64_t)RD64(%s)'}[sz] % self.addr(ins, ops[0])
            out.append('FPUSH((double)%s);' % v)
        elif m in ('fld1', 'fldz'):
            out.append('FPUSH(%s);' % ('1.0' if m == 'fld1' else '0.0'))
        elif m in ('fst', 'fstp'):
            op = ops[0]
            if op.type == X86_OP_REG:
                out.append('ST(%d) = ST(0);' % self.st_index(ins, op))
            elif op.size == 4:
                out.append('WRF32(%s, ST(0));' % self.addr(ins, op))
            elif op.size == 8:
                out.append('WRF64(%s, ST(0));' % self.addr(ins, op))
            elif op.size == 10:
                out.append('WRF80(%s, ST(0));' % self.addr(ins, op))
            else:
                raise LiftError('fst size %d at %x' % (op.size, a))
            if m == 'fstp':
                out.append('FPOP();')
        elif m in ('fist', 'fistp'):
            sz = ops[0].size
            out.append('{ double r_ = FROUND(ST(0)); WR%d(%s, (uint%d_t)(int%d_t)r_); }' % (sz * 8, self.addr(ins, ops[0]), sz * 8, sz * 8))
            if m == 'fistp':
                out.append('FPOP();')
        elif m[1:4] in ('add', 'mul', 'sub', 'div') and ins.bytes[0] in (0xd8, 0xdc, 0xde) and ins.bytes[1] >= 0xc0:
            # Register forms, decoded from the opcode bytes: disassemblers disagree on the
            # fsub/fsubr and fdiv/fdivr mnemonics here (AT&T vs Intel), the encoding does not.
            #   D8 /r: ST0 = ST0 op ST(i)     DC /r: ST(i) = ST(i) op ST0     DE /r: same as DC, then pop
            # with /r = 0 add, 1 mul, 4 sub, 5 subr, 6 div, 7 divr (reg field of ModRM, Intel SDM)
            opc, modrm = ins.bytes[0], ins.bytes[1]
            i, reg = modrm & 7, (modrm >> 3) & 7
            if reg in (2, 3):
                raise LiftError('fcom register form via arithmetic path at %x' % a)
            if opc == 0xd8:
                d, s_ = 'ST(0)', 'ST(%d)' % i
            else:
                d, s_ = 'ST(%d)' % i, 'ST(0)'
            # For DC/DE the Intel SDM swaps the meaning of /4-/7 relative to D8:
            # DC E0+i = FSUBR ST(i),ST0 (ST(i) = ST0 - ST(i)), DC E8+i = FSUB ST(i),ST0, etc.
            if opc != 0xd8 and reg in (4, 5, 6, 7):
                reg ^= 1
            expr = {0: '%s + %s', 1: '%s * %s', 4: '%s - %s', 6: '%s / %s'}
            if reg in expr:
                e = expr[reg] % (d, s_)
            else:  # reversed: 5 subr, 7 divr
                e = ('%s - %s' if reg == 5 else '%s / %s') % (s_, d)
            out.append('%s = %s;' % (d, e))
            if opc == 0xde:
                out.append('FPOP();')
        elif m[1:4] in ('add', 'mul', 'sub', 'div') or m[1:5] in ('subr', 'divr'):
            base = m[1:]
            pop = base.endswith('p')
            if pop:
                base = base[:-1]
            rev = base.endswith('r')
            if rev:
                base = base[:-1]
            op = arith[base]
            if len(ops) == 1 and ops[0].type == X86_OP_MEM:
                src = self.fpu_operand(ins, ops[0])
                out.append('ST(0) = %s;' % (('%s %s ST(0)' % (src, op)) if rev else ('ST(0) %s %s' % (op, src))))
            else:
                if len(ops) == 2:
                    di, si = self.st_index(ins, ops[0]), self.st_index(ins, ops[1])
                elif len(ops) == 1:
                    # "fop st(i)" computes ST(0) = ST(0) op ST(i); "fopp st(i)" writes ST(i) and pops
                    i = self.st_index(ins, ops[0])
                    di, si = (i, 0) if pop else (0, i)
                else:
                    di, si = 1, 0
                d, s_ = 'ST(%d)' % di, 'ST(%d)' % si
                out.append('%s = %s;' % (d, ('%s %s %s' % (s_, op, d)) if rev else ('%s %s %s' % (d, op, s_))))
                if pop:
                    out.append('FPOP();')
        elif m in ('fiadd', 'fimul', 'fisub', 'fidiv', 'fisubr', 'fidivr'):
            base = m[2:]
            rev = base.endswith('r')
            op = arith[base[:-1] if rev else base]
            sz = ops[0].size
            v = '(double)(int%d_t)RD%d(%s)' % (sz * 8, sz * 8, self.addr(ins, ops[0]))
            out.append('ST(0) = %s;' % (('%s %s ST(0)' % (v, op)) if rev else ('ST(0) %s %s' % (op, v))))
        elif m in ('fcom', 'fcomp', 'fcompp', 'fucom', 'fucomp', 'fucompp'):
            src = self.fpu_operand(ins, ops[0]) if ops else 'ST(1)'
            out.append('FCOM(ST(0), %s);' % src)
            if m.endswith('pp'):
                out.append('FPOP(); FPOP();')
            elif m.endswith('p'):
                out.append('FPOP();')
        elif m in ('fnstsw', 'fstsw'):
            out.append(self.wr(ins, ops[0], 'FNSTSW()') if ops else 'eax = (eax & ~0xffffu) | FNSTSW();')
        elif m == 'fchs':
            out.append('ST(0) = -ST(0);')
        elif m == 'fabs':
            out.append('ST(0) = fabs(ST(0));')
        elif m == 'fsqrt':
            out.append('ST(0) = sqrt(ST(0));')
        elif m == 'fxch':
            i = self.st_index(ins, ops[0]) if ops else 1
            out.append('{ double t_ = ST(0); ST(0) = ST(%d); ST(%d) = t_; }' % (i, i))
        elif m in ('fnstcw', 'fstcw'):
            out.append('WR16(%s, R.cw);' % self.addr(ins, ops[0]))
        elif m == 'fldcw':
            out.append('R.cw = RD16(%s);' % self.addr(ins, ops[0]))
        elif m in ('fninit', 'fnclex', 'fwait', 'wait', 'fnop', 'fdecstp', 'fincstp') or m == 'ffree':
            if m == 'fdecstp':
                out.append('R.top = (R.top - 1) & 7;')
            elif m == 'fincstp':
                out.append('R.top = (R.top + 1) & 7;')
        elif m == 'fsin':
            out.append('ST(0) = sin(ST(0)); R.sw &= ~0x400u;')
        elif m == 'fcos':
            out.append('ST(0) = cos(ST(0)); R.sw &= ~0x400u;')
        elif m == 'fsincos':
            out.append('{ double v_ = ST(0); ST(0) = sin(v_); FPUSH(cos(v_)); R.sw &= ~0x400u; }')
        elif m == 'fptan':
            out.append('ST(0) = tan(ST(0)); FPUSH(1.0); R.sw &= ~0x400u;')
        elif m == 'fpatan':
            out.append('ST(1) = atan2(ST(1), ST(0)); FPOP();')
        elif m == 'fyl2x':
            out.append('ST(1) = ST(1) * log2(ST(0)); FPOP();')
        elif m == 'fyl2xp1':
            out.append('ST(1) = ST(1) * log2(ST(0) + 1.0); FPOP();')
        elif m == 'f2xm1':
            out.append('ST(0) = exp2(ST(0)) - 1.0;')
        elif m == 'fscale':
            out.append('ST(0) = ldexp(ST(0), (int)trunc(ST(1)));')
        elif m == 'frndint':
            out.append('ST(0) = FROUND(ST(0));')
        elif m in ('fprem', 'fprem1'):
            out.append('ST(0) = %s(ST(0), ST(1)); R.sw &= ~0x400u;' % ('fmod' if m == 'fprem' else 'remainder'))
        elif m == 'fxtract':
            out.append('{ int e_; double f_ = frexp(ST(0), &e_); ST(0) = (double)(e_ - 1); FPUSH(f_ * 2.0); }')
        elif m in ('fldpi', 'fldl2e', 'fldln2', 'fldlg2', 'fldl2t'):
            c = {'fldpi': 'M_PI', 'fldl2e': 'M_LOG2E', 'fldln2': 'M_LN2', 'fldlg2': '0.30102999566398119521', 'fldl2t': '3.32192809488736234787'}[m]
            out.append('FPUSH(%s);' % c)
        elif m == 'ftst':
            out.append('FCOM(ST(0), 0.0);')
        elif m == 'fxam':
            out.append('{ double v_ = ST(0); uint32_t c_ = signbit(v_) ? 0x200 : 0; '
                       'c_ |= isnan(v_) ? 0x100 : isinf(v_) ? 0x500 : v_ == 0 ? 0x4000 : 0x400; R.sw = (R.sw & ~0x4700u) | c_; }')
        elif m in ('fcomi', 'fcomip', 'fucomi', 'fucomip'):
            i = self.st_index(ins, ops[-1]) if ops else 1
            out.append('{ double a_ = ST(0), b_ = ST(%d); if (a_ != a_ || b_ != b_) { zf = pf = cf = 1; } '
                       'else { zf = a_ == b_; cf = a_ < b_; pf = 0; } of = sf = 0; }' % i)
            if m.endswith('p'):
                out.append('FPOP();')
        else:
            raise LiftError('unsupported fpu %s %s at %x' % (m, ins.op_str, a))

    def fpu_regform(self, ins, out, op, modrm):
        """x87 instructions with a register operand, decoded from the encoding (Intel SDM):
        the operand lists disassemblers report for these are not reliable enough."""
        i = modrm & 7
        if op == 0xd9 and 0xc0 <= modrm <= 0xc7:            # FLD ST(i)
            out.append('{ double v_ = ST(%d); FPUSH(v_); }' % i)
        elif op == 0xd9 and 0xc8 <= modrm <= 0xcf:          # FXCH ST(i)
            out.append('{ double t_ = ST(0); ST(0) = ST(%d); ST(%d) = t_; }' % (i, i))
        elif op == 0xdd and 0xd0 <= modrm <= 0xd7:          # FST ST(i)
            out.append('ST(%d) = ST(0);' % i)
        elif op == 0xdd and 0xd8 <= modrm <= 0xdf:          # FSTP ST(i)
            out.append('ST(%d) = ST(0); FPOP();' % i)
        elif op == 0xd8 and 0xd0 <= modrm <= 0xd7:          # FCOM ST(i)
            out.append('FCOM(ST(0), ST(%d));' % i)
        elif op == 0xd8 and 0xd8 <= modrm <= 0xdf:          # FCOMP ST(i)
            out.append('FCOM(ST(0), ST(%d)); FPOP();' % i)
        elif op == 0xde and modrm == 0xd9:                  # FCOMPP
            out.append('FCOM(ST(0), ST(1)); FPOP(); FPOP();')
        elif op == 0xdd and 0xe0 <= modrm <= 0xe7:          # FUCOM ST(i)
            out.append('FCOM(ST(0), ST(%d));' % i)
        elif op == 0xdd and 0xe8 <= modrm <= 0xef:          # FUCOMP ST(i)
            out.append('FCOM(ST(0), ST(%d)); FPOP();' % i)
        elif op == 0xda and modrm == 0xe9:                  # FUCOMPP
            out.append('FCOM(ST(0), ST(1)); FPOP(); FPOP();')
        else:
            return False
        return True


    # ---------------------------------------------------------------- x87 stack in locals
    #
    # Within a function the x87 stack depth is almost always known statically (compiled code
    # keeps it balanced). Then ST(i) is a local double instead of R.st[(R.top + i) & 7], which
    # the compiler can keep in registers. The emitted C of each instruction is rewritten token by
    # token (ST(i), FPUSH(...), FPOP(), FNSTSW()) with the depth known at that point. The CPU
    # state is synchronised around calls, returns and tail jumps, using each callee's "stack
    # signature" (k, m): it takes k values from the FPU stack and leaves m. Functions where this
    # cannot be proven keep the emulated stack.

    FPU_TOKEN = re.compile(r'ST\((\d+)\)|FPUSH\(|FPOP\(\);|FNSTSW\(\)')

    def fpu_walk(self, text, cur, local, top_expr=None):
        """Rewrite text with the stack depth cur (local slots fs_[0..cur-1], top is fs_[cur-1]).
        Returns (new_text, new_cur, min_slot); min_slot < 0 means caller values were used."""
        out, pos, minp = [], 0, cur
        while True:
            mt = self.FPU_TOKEN.search(text, pos)
            if not mt:
                out.append(text[pos:])
                break
            out.append(text[pos:mt.start()])
            tok = mt.group(0)
            if tok.startswith('ST('):
                slot = cur - 1 - int(mt.group(1))
                minp = min(minp, slot)
                out.append('fs_[%d]' % slot if local else tok)
                pos = mt.end()
            elif tok == 'FPUSH(':
                depth, j = 1, mt.end()          # balanced argument
                while depth:
                    if text[j] == '(':
                        depth += 1
                    elif text[j] == ')':
                        depth -= 1
                    j += 1
                arg, _, m2 = self.fpu_walk(text[mt.end():j - 1], cur, local, top_expr)
                minp = min(minp, m2)
                end = j + 1 if j < len(text) and text[j] == ';' else j
                out.append(('fs_[%d] = (%s);' % (cur, arg)) if local else 'FPUSH(%s);' % arg)
                cur += 1
                pos = end
            elif tok == 'FPOP();':
                cur -= 1
                minp = min(minp, cur)
                out.append('' if local else tok)
                pos = mt.end()
            else:                               # FNSTSW(): TOP is part of the status word
                out.append(('((R.sw & ~0x3800u) | (((%s) & 7u) << 11))' % top_expr(cur)) if local else tok)
                pos = mt.end()
        return ''.join(out), cur, minp

    NATIVE_FPU_SIG = {0x4a3340: (1, 0)}         # __ftol pops ST(0)

    # MSVC runtime helpers emitted inline at their call sites (no call, no register save):
    # 64-bit arithmetic on EDX:EAX and stdcall stack arguments, and __ftol on ST(0).
    INTRINSICS = {
        0x4a3850: '{ uint64_t v_ = ((uint64_t)edx << 32) | eax; uint32_t n_ = ecx & 0xff; '
                  'v_ = n_ >= 64 ? 0 : v_ << n_; eax = (uint32_t)v_; edx = (uint32_t)(v_ >> 32); } /* __allshl */',
        0x4a3830: '{ uint64_t v_ = ((uint64_t)edx << 32) | eax; uint32_t n_ = ecx & 0xff; '
                  'v_ = n_ >= 64 ? 0 : v_ >> n_; eax = (uint32_t)v_; edx = (uint32_t)(v_ >> 32); } /* __aullshr */',
        0x4a3340: '{ double v_ = ST(0); FPOP(); int64_t r_ = (v_ != v_ || v_ >= 9223372036854775808.0 || '
                  'v_ < -9223372036854775808.0) ? INT64_MIN : (int64_t)v_; eax = (uint32_t)r_; '
                  'edx = (uint32_t)((uint64_t)r_ >> 32); } /* __ftol */',
        0x4a3af0: '{ uint64_t a_ = ((uint64_t)RD32(RT_SP(4)) << 32) | RD32(RT_SP(0)), b_ = ((uint64_t)RD32(RT_SP(12)) << 32) | RD32(RT_SP(8)); '
                  'uint64_t r_ = a_ * b_; eax = (uint32_t)r_; edx = (uint32_t)(r_ >> 32); esp += 16; } /* __allmul */',
        0x4a2b00: '{ int64_t a_ = (int64_t)(((uint64_t)RD32(RT_SP(4)) << 32) | RD32(RT_SP(0))), b_ = (int64_t)(((uint64_t)RD32(RT_SP(12)) << 32) | RD32(RT_SP(8))); '
                  'if (!b_) RT_Fatal("divide by zero", 0x4a2b00u); int64_t r_ = (a_ == INT64_MIN && b_ == -1) ? a_ : a_ / b_; '
                  'eax = (uint32_t)r_; edx = (uint32_t)((uint64_t)r_ >> 32); esp += 16; } /* __alldiv */',
        0x4a5570: '{ uint64_t a_ = ((uint64_t)RD32(RT_SP(4)) << 32) | RD32(RT_SP(0)), b_ = ((uint64_t)RD32(RT_SP(12)) << 32) | RD32(RT_SP(8)); '
                  'if (!b_) RT_Fatal("divide by zero", 0x4a5570u); uint64_t r_ = a_ / b_; eax = (uint32_t)r_; edx = (uint32_t)(r_ >> 32); esp += 16; } /* __aulldiv */',
        0x4a54f0: '{ uint64_t a_ = ((uint64_t)RD32(RT_SP(4)) << 32) | RD32(RT_SP(0)), b_ = ((uint64_t)RD32(RT_SP(12)) << 32) | RD32(RT_SP(8)); '
                  'if (!b_) RT_Fatal("divide by zero", 0x4a54f0u); uint64_t r_ = a_ % b_; eax = (uint32_t)r_; edx = (uint32_t)(r_ >> 32); esp += 16; } /* __aullrem */',
        # VU0 helpers (cdecl), called from the game's inner loops: same results and registers as
        # their natives (host/d3dx.cpp) without the native call boundary
        0x415740: '{ uint32_t o_ = RD32(RT_SP(0)); rt_vu0_apply(o_, RD32(RT_SP(4)), RD32(RT_SP(8))); eax = o_; } /* Vu0ApplyMatrix */',
        0x415760: '{ uint32_t o_ = RD32(RT_SP(0)); rt_vu0_mul(o_, RD32(RT_SP(4)), RD32(RT_SP(8))); eax = o_; } /* Vu0MulMatrix */',
        0x415700: '{ uint32_t d_ = RD32(RT_SP(0)), s_ = RD32(RT_SP(4)); memmove(MEM + d_, MEM + s_, 16); '
                  'ecx = d_; edx = RD32(s_ + 8); eax = RD32(s_ + 12); } /* Vu0CopyVector */',
        0x415810: '{ uint32_t d_ = RD32(RT_SP(0)), s_ = RD32(RT_SP(4)); for (int r_ = 0; r_ < 4; r_++) '
                  'memmove(MEM + d_ + 16 * r_, MEM + s_ + 16 * r_, 16); ecx = d_ + 0x30; edx = RD32(s_ + 0x38); '
                  'eax = RD32(s_ + 0x3c); } /* Vu0CopyMatrix */',
    }

    def intrinsic_call(self, ins):
        """True for a call emitted inline (intrinsic helper or inlined leaf)."""
        if ins.mnemonic != 'call' or ins.operands[0].type != X86_OP_IMM:
            return None
        t = ins.operands[0].imm & 0xffffffff
        if self.intrinsics and t in self.INTRINSICS:
            return self.INTRINSICS[t]
        return self.inline_body(t)

    def callee_sig(self, t):
        if t in self.NATIVE_FPU_SIG:
            return self.NATIVE_FPU_SIG[t]
        if t in self.hle or (self.native_pred and self.native_pred(t)):
            return (0, 0)
        info = self.fpu_info(t)
        return info['sig'] if info else None

    def fpu_info(self, entry, insns=None, labels=None, tables=None):
        """Static x87 stack analysis of a function (memoised). Returns a dict with 'sig' (or
        None if unknown), 'local' (can use locals), 'depth' {addr: relative depth before},
        'k' (values taken from the caller), 'maxd'."""
        if not hasattr(self, '_fpu'):
            self._fpu = {}
        if entry in self._fpu:
            return self._fpu[entry]
        self._fpu[entry] = {'sig': (0, 0), 'local': False}     # recursion: assume neutral
        info = None
        try:
            info = self._fpu_analyse(entry, insns, labels, tables)
        except Exception:
            info = None
        self._fpu[entry] = info
        return info

    def _fpu_analyse(self, entry, insns, labels, tables):
        if insns is None:
            saved = getattr(self, 'bad', None)
            insns, labels, tables = self.cfg(entry)
            bad = self.bad
            self.bad = saved
        else:
            bad = self.bad
        has_fpu = any(i.mnemonic.startswith('f') for i in insns.values())
        has_calls = any(i.mnemonic == 'call' for i in insns.values())
        depth = {entry: 0}
        work = [entry]
        minr, rets, ok, sig_ok = 0, set(), True, True
        maxd = 0
        while work and ok:
            a = work.pop()
            if a not in insns:
                continue
            ins = insns[a]
            m = ins.mnemonic
            d = depth[a]
            nxt = a + ins.size
            succ = []
            if m == 'call' and not self.intrinsic_call(ins):
                op = ins.operands[0]
                if op.type == X86_OP_IMM:
                    sg = self.callee_sig(op.imm & 0xffffffff)
                else:
                    sg = (0, 0)                 # imports and indirect calls: no FPU values
                if sg is None:
                    ok = sig_ok = False
                    break
                if d - sg[0] < 0 and has_calls:
                    ok = False
                    break
                d2 = d - sg[0] + sg[1]
                succ = [nxt]
            else:
                lines = []
                if m.startswith('f') or m in ('fnstsw',) or m == 'call':
                    self.insn(ins, lines, tables, labels)
                    text = ' '.join(lines)
                    if 'R.top' in text or 'R.st' in text:
                        ok = False
                        break
                    _, d2, mp = self.fpu_walk(text, d, False)
                    minr = min(minr, mp, d2)
                else:
                    d2 = d
                if m in ('ret', 'retn'):
                    rets.add(d2)
                elif m == 'jmp':
                    op = ins.operands[0]
                    if a in tables:
                        succ = list(tables[a])
                    elif op.type == X86_OP_IMM and (op.imm & 0xffffffff) in labels:
                        succ = [op.imm & 0xffffffff]
                    elif op.type == X86_OP_IMM:
                        sg = self.callee_sig(op.imm & 0xffffffff)
                        if sg is None:
                            sig_ok = False
                        else:
                            rets.add(d2 - sg[0] + sg[1])
                    else:
                        sig_ok = False          # indirect tail jump
                elif m.startswith('j'):
                    succ = [ins.operands[0].imm & 0xffffffff, nxt]
                elif m in ('int3', 'hlt'):
                    pass
                else:
                    succ = [nxt]
            maxd = max(maxd, d, d2)
            for t in succ:
                if t not in insns:
                    if t in bad:
                        continue
                    # falls into the next function: a tail call
                    sg = self.callee_sig(t)
                    if sg is None:
                        sig_ok = False
                    else:
                        rets.add(d2 - sg[0] + sg[1])
                    continue
                if t in depth:
                    if depth[t] != d2:
                        ok = sig_ok = False
                        break
                else:
                    depth[t] = d2
                    work.append(t)
        k = -minr if minr < 0 else 0
        if k and has_calls:
            ok = False
        if len(rets) > 1:
            ok = sig_ok = False
        r_ret = rets.pop() if rets else 0
        sig = (k, r_ret + k) if (sig_ok and ok) else None
        if not ok and not has_fpu:
            # no x87 code of its own: neutral unless a callee is unknown
            sig = sig if sig_ok else None
        return {'sig': sig, 'local': ok and has_fpu and k + maxd <= 16, 'depth': depth, 'k': k,
                'maxd': maxd + k}

    # ---------------------------------------------------------------- function

    def function(self, entry):
        self.cur_mask = self.preserve_mask(entry)  # before cfg(): it leaves self.bad behind
        insns, labels, tables = self.cfg(entry)
        out = ['RtRet F_%08x(RT_PARAMS)' % entry, '{',
               '    uint32_t cf = 0, zf = 0, sf = 0, of = 0, pf = 0, df = 0;',
               '    (void)cf; (void)zf; (void)sf; (void)of; (void)pf; (void)df;']
        fpu = self.fpu_info(entry, insns, labels, tables) if self.fpu_locals else None
        if fpu and fpu['local']:
            K = fpu['k']
            out.append('    double fs_[%d]; uint32_t ftop0_ = R.top; (void)ftop0_;' % max(1, fpu['maxd'] + 1))
            for i in range(K):                  # values the function takes from the caller's stack
                out.append('    fs_[%d] = R.st[(ftop0_ + %du) & 7];' % (K - 1 - i, i))

            def top(cur):
                return 'ftop0_ + %du - %du' % (K, cur)

            def spill(cur):
                return ('R.top = (ftop0_ + %du - %du) & 7; ' % (K, cur) +
                        ''.join('R.st[(ftop0_ + %du) & 7] = fs_[%d]; ' % ((K - 1 - j) & 0xffffffff, j) for j in range(cur)))

            def reload(cur):
                return ''.join('fs_[%d] = R.st[(ftop0_ + %du) & 7]; ' % (j, (K - 1 - j) & 0xffffffff) for j in range(cur))
        else:
            fpu = None
        addrs = sorted(insns)
        body = []
        # code shared from below the entry (e.g. a common exit sequence) is emitted first
        if addrs and addrs[0] != entry:
            body.append('    goto L_%08x;' % entry)
            labels.add(entry)
        prev_falls = False
        for i, a in enumerate(addrs):
            ins = insns[a]
            if a in labels or (i > 0 and addrs[i - 1] + insns[addrs[i - 1]].size != a and prev_falls):
                body.append('L_%08x:' % a)
            lines = []
            self.insn(ins, lines, tables, labels)
            text = ' '.join(lines)
            m = ins.mnemonic
            cur_after = None
            if fpu and a in fpu['depth']:
                cur = fpu['depth'][a] + K
                if m == 'call' and not self.intrinsic_call(ins):
                    op = ins.operands[0]
                    sg = self.callee_sig(op.imm & 0xffffffff) if op.type == X86_OP_IMM else (0, 0)
                    cur_after = cur - sg[0] + sg[1]
                    text = spill(cur) + text + ' ' + reload(cur_after)
                elif m in ('ret', 'retn') or 'tail call' in text or 'tail jump' in text:
                    text = spill(cur) + text
                    cur_after = cur
                else:
                    text, cur_after, _ = self.fpu_walk(text, cur, True, top)
            body.append('    /* %08x: %s %s */ %s' % (a, ins.mnemonic, ins.op_str, text))
            ends = m in ('ret', 'retn', 'jmp', 'int3')
            nxt = a + ins.size
            # fall-through into code that is not emitted next (gap or another function)
            if not ends and (i + 1 == len(addrs) or addrs[i + 1] != nxt):
                if nxt in insns or nxt in self.bad:
                    body.append('    goto L_%08x;' % nxt)
                    labels.add(nxt)
                else:
                    pre = spill(cur_after) if fpu and cur_after is not None else ''
                    body.append('    %s%s /* falls into next function */' % (pre, self.tail_stmt(nxt)))
            prev_falls = not ends
        for b in sorted(self.bad):
            body.append('L_%08x: RT_Fatal("execution ran into non-code", 0x%xu); RT_RETM(%d);' % (b, b, self.cur_mask))
        # make sure every referenced label exists
        emitted = {int(l[2:10], 16) for l in body if l.startswith('L_')}
        for t in sorted(labels - emitted):
            if t in insns:
                # label at an instruction that wasn't marked: insert before it
                idx = next(j for j, l in enumerate(body) if l.startswith('    /* %08x:' % t))
                body.insert(idx, 'L_%08x:' % t)
        out += body
        out.append('    RT_RETM(%d);   /* not reached: every path ends in ret, a tail call or a trap */' % self.cur_mask)
        out.append('}')
        return '\n'.join(out) + '\n', insns
