# Lista símbolos do chrome.dll.pdb que batem com os padrões (dbghelp do Windows SDK)
import ctypes, sys, os
from ctypes import wintypes as w

DBG = r'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\dbghelp.dll'
dh = ctypes.WinDLL(DBG)
here = os.path.dirname(os.path.abspath(__file__))
pdb = os.path.join(here, 'chrome.dll.pdb')
BASE = 0x10000000


class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [('SizeOfStruct', w.ULONG), ('TypeIndex', w.ULONG), ('Reserved', ctypes.c_uint64 * 2), ('Index', w.ULONG),
                ('Size', w.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', w.ULONG), ('Value', ctypes.c_uint64),
                ('Address', ctypes.c_uint64), ('Register', w.ULONG), ('Scope', w.ULONG), ('Tag', w.ULONG),
                ('NameLen', w.ULONG), ('MaxNameLen', w.ULONG), ('Name', w.WCHAR * 1)]


CB = ctypes.WINFUNCTYPE(w.BOOL, ctypes.POINTER(SYMBOL_INFOW), w.ULONG, ctypes.c_void_p)
dh.SymSetOptions(0x2 | 0x4 | 0x200)  # UNDNAME | DEFERRED_LOADS | ...
proc = ctypes.c_void_p(0x1234)
assert dh.SymInitializeW(proc, None, False)
dh.SymLoadModuleExW.restype = ctypes.c_uint64
base = dh.SymLoadModuleExW(proc, None, ctypes.c_wchar_p(pdb), None, ctypes.c_uint64(BASE), w.DWORD(0x13000000), None, 0)
print('base', hex(base))
out = []


def cb(p, size, ctx):
    s = p.contents
    name = ctypes.wstring_at(ctypes.addressof(s) + SYMBOL_INFOW.Name.offset, s.NameLen)
    out.append((s.Address - BASE, s.Size, s.Tag, name))
    return True


fn = CB(cb)
for pat in sys.argv[1:]:
    out.clear()
    dh.SymEnumSymbolsW(proc, ctypes.c_uint64(base), ctypes.c_wchar_p(pat), fn, None)
    for rva, size, tag, name in sorted(out, key=lambda x: x[3])[:80]:
        print('%-10s %6d tag%-3d %s' % (hex(rva), size, tag, name))
    print('--', pat, len(out))
