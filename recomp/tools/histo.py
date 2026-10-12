# Instruction mnemonic histogram over reachable functions: histo.py <exe> <file of hex addresses>
import collections, sys
from capstone import *
from capstone.x86 import *
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from pe import PE
pe = PE(sys.argv[1])
reach = [int(x, 16) for x in open(sys.argv[2]).read().split()]
sizes = {}
for l in open(sys.argv[3]).read().splitlines()[1:]:
    p = l.split('\t')
    try: sizes[int(p[0], 16)] = int(p[1])
    except ValueError: pass
game = [e for e in reach if 0x41d000 <= e < 0x483800]
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
mn = collections.Counter(); forms = collections.Counter(); total = 0
for e in game:
    code = pe.read(e, sizes.get(e, 0))
    for ins in md.disasm(code, e):
        total += 1
        mn[ins.mnemonic] += 1
        ops = []
        for o in ins.operands:
            ops.append({X86_OP_REG: 'r', X86_OP_IMM: 'i', X86_OP_MEM: 'm'}[o.type] + str(o.size))
        forms[ins.mnemonic + ' ' + ','.join(ops)] += 1
print('instructions', total, 'distinct mnemonics', len(mn))
print(' '.join('%s:%d' % kv for kv in mn.most_common()))
