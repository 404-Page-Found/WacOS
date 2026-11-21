# Desmonta funções do chrome.dll por RVA, com nomes dos alvos de call (dbghelp)
import sys, struct, ctypes, os
import capstone
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
DLL = r'C:\Program Files\Google\Chrome\Application\157.0.8103.79\chrome.dll'
f = open(DLL, 'rb')
d = f.read(4096)
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
optsz = struct.unpack_from('<H', d, pe + 20)[0]
opt = pe + 24
secs = []
for i in range(nsec):
    o = opt + optsz + i * 40
    vs, va, rs, ra = struct.unpack_from('<IIII', d, o + 8)
    secs.append((va, vs, ra))


def read(rva, n):
    for va, vs, ra in secs:
        if va <= rva < va + vs:
            f.seek(rva - va + ra)
            return f.read(n)


# nomes por endereço (dbghelp)
DBG = r'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\dbghelp.dll'
dh = ctypes.WinDLL(DBG)
from ctypes import wintypes as w


class SYM(ctypes.Structure):
    _fields_ = [('SizeOfStruct', w.ULONG), ('TypeIndex', w.ULONG), ('Reserved', ctypes.c_uint64 * 2), ('Index', w.ULONG),
                ('Size', w.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', w.ULONG), ('Value', ctypes.c_uint64),
                ('Address', ctypes.c_uint64), ('Register', w.ULONG), ('Scope', w.ULONG), ('Tag', w.ULONG),
                ('NameLen', w.ULONG), ('MaxNameLen', w.ULONG), ('Name', w.WCHAR * 512)]


BASE = 0x10000000
proc = ctypes.c_void_p(0x1235)
dh.SymSetOptions(0x2 | 0x4)
dh.SymInitializeW(proc, None, False)
dh.SymLoadModuleExW.restype = ctypes.c_uint64
dh.SymLoadModuleExW(proc, None, ctypes.c_wchar_p(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'chrome.dll.pdb')), None, ctypes.c_uint64(BASE), w.DWORD(0x13000000), None, 0)


def name(rva):
    s = SYM(); s.SizeOfStruct = ctypes.sizeof(SYM) - 512 * 2 + 2; s.MaxNameLen = 511
    disp = ctypes.c_uint64()
    if dh.SymFromAddrW(proc, ctypes.c_uint64(BASE + rva), ctypes.byref(disp), ctypes.byref(s)):
        return s.Name + ('+0x%x' % disp.value if disp.value else '')
    return '?'


md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for arg in sys.argv[1:]:
    rva, n = arg.split(':') if ':' in arg else (arg, '400')
    rva = int(rva, 16); n = int(n)
    print('==', hex(rva), name(rva))
    for ins in md.disasm(read(rva, n), rva):
        extra = ''
        if ins.mnemonic in ('call', 'jmp') and ins.op_str.startswith('0x'):
            extra = '   ; ' + name(int(ins.op_str, 16))
        elif 'rip' in ins.op_str:
            import re
            m = re.search(r'\[rip ([+-]) (0x[0-9a-f]+)\]', ins.op_str)
            if m:
                t = ins.address + ins.size + (int(m.group(2), 16) * (1 if m.group(1) == '+' else -1))
                extra = '   ; ' + hex(t) + ' ' + name(t)
        print('%08x  %-8s %s%s' % (ins.address, ins.mnemonic, ins.op_str, extra))
        if ins.mnemonic == 'int3':
            break
