"""Translate a set of functions into one C file (plus stubs for referenced but untranslated
targets) and the dispatch table used by rt_test.c."""
import re
from x86lift import Lifter, LiftError

def generate(pe, funcs, entries, out_path, hle=None):
    lifter = Lifter(pe, entries, hle)
    parts, ok, failed = [], [], {}
    for f in sorted(funcs):
        try:
            src, _ = lifter.function(f)
            parts.append(src)
            ok.append(f)
        except LiftError as e:
            failed[f] = str(e)
        except Exception as e:          # translator bug: report per function, keep going
            failed[f] = 'internal: %s: %s' % (type(e).__name__, e)
    text = '\n'.join(parts)
    called = set(int(x, 16) for x in re.findall(r'\bF_([0-9a-f]{8})\b', text))
    stubs = sorted(called - set(ok))
    head = ['#include "recomp_rt.h"', '']
    head += ['RtRet F_%08x(RT_PARAMS);' % f for f in sorted(set(ok) | set(stubs))]
    head.append('')
    tail = ['RtRet F_%08x(RT_PARAMS) { RT_Fatal("not translated", 0x%08xu); RT_RET(); }' % (s, s) for s in stubs]
    tail += [lifter.adapter(f, f in ok) for f in sorted(set(ok) | set(stubs))]
    tail.append('typedef struct { uint32_t addr; RecompFn fn; } Entry;')
    tail.append('const Entry RT_TABLE[] = {')
    tail += ['    {0x%08xu, A_%08x},' % (f, f) for f in sorted(set(ok) | set(stubs))]
    tail.append('};')
    tail.append('const int RT_TABLE_N = %d;' % len(set(ok) | set(stubs)))
    open(out_path, 'w').write('\n'.join(head) + '\n' + text + '\n' + '\n'.join(tail) + '\n')
    return ok, failed, stubs
