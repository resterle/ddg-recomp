"""Minimal PE32 loader: sections mapped at their virtual addresses."""
import struct

class PE:
    def __init__(self, path_or_data):
        d = path_or_data if isinstance(path_or_data, bytes) else open(path_or_data, 'rb').read()
        self.data = d
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        self.nsec = struct.unpack_from('<H', d, pe + 6)[0]
        opt = pe + 24
        self.base = struct.unpack_from('<I', d, opt + 28)[0]
        self.entry = self.base + struct.unpack_from('<I', d, opt + 16)[0]
        so = opt + struct.unpack_from('<H', d, pe + 20)[0]
        self.sections = []
        for i in range(self.nsec):
            name, vs, va, rs, ra = struct.unpack_from('<8sIIII', d, so + i * 40)
            chars = struct.unpack_from('<I', d, so + i * 40 + 36)[0]
            self.sections.append(dict(name=name.rstrip(b'\0').decode(), va=self.base + va, vsize=vs,
                                      raw=d[ra:ra + min(rs, vs) if vs else rs], rsize=rs, chars=chars))
        dirs = [struct.unpack_from('<II', d, opt + 96 + 8 * i) for i in range(16)]
        self.imports = self._imports(dirs[1][0])

    def section_of(self, va):
        for s in self.sections:
            if s['va'] <= va < s['va'] + max(s['vsize'], s['rsize']):
                return s
        return None

    def read(self, va, n):
        s = self.section_of(va)
        if not s:
            raise ValueError('unmapped %x' % va)
        o = va - s['va']
        b = s['raw'][o:o + n]
        return b + b'\0' * (n - len(b))

    def u32(self, va):
        return struct.unpack('<I', self.read(va, 4))[0]

    def _imports(self, rva):
        """IAT address -> 'DLL!name'."""
        out = {}
        if not rva:
            return out
        def cstr(va):
            b = self.read(va, 256)
            return b[:b.index(b'\0')].decode('latin1')
        p = self.base + rva
        while True:
            olt, ts, fc, name, ft = struct.unpack('<IIIII', self.read(p, 20))
            if not name:
                break
            dll = cstr(self.base + name)
            i = 0
            while True:
                thunk = self.u32(self.base + (olt or ft) + 4 * i)
                if not thunk:
                    break
                fn = ('#%d' % (thunk & 0xffff)) if thunk & 0x80000000 else cstr(self.base + thunk + 2)
                out[self.base + ft + 4 * i] = dll + '!' + fn
                i += 1
            p += 20
        return out
