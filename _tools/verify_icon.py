"""从 exe 抽取图标资源并用 Windows API 渲染出来，验证图标确实生效。
（直接搜二进制容易命中假阳性，走 ExtractIconEx 才是系统真实看到的东西。）"""
import ctypes, ctypes.wintypes as wt, os, struct, sys, zlib

u32 = ctypes.windll.user32
g32 = ctypes.windll.gdi32
shell32 = ctypes.windll.shell32

# ExtractIconEx 在 shell32，不在 user32
shell32.ExtractIconExW.argtypes = [wt.LPCWSTR, ctypes.c_int, ctypes.POINTER(wt.HICON),
                                   ctypes.POINTER(wt.HICON), wt.UINT]
shell32.ExtractIconExW.restype = wt.UINT
u32.GetIconInfo.argtypes = [wt.HICON, ctypes.c_void_p]
u32.DrawIconEx.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int, wt.HICON,
                           ctypes.c_int, ctypes.c_int, wt.UINT, wt.HANDLE, wt.UINT]
u32.GetDC.argtypes = [wt.HWND]; u32.GetDC.restype = wt.HDC
u32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
u32.DestroyIcon.argtypes = [wt.HICON]

g32.CreateCompatibleDC.argtypes = [wt.HDC]; g32.CreateCompatibleDC.restype = wt.HDC
g32.CreateDIBSection.argtypes = [wt.HDC, ctypes.c_void_p, wt.UINT,
                                 ctypes.POINTER(ctypes.c_void_p), wt.HANDLE, wt.DWORD]
g32.CreateDIBSection.restype = wt.HBITMAP
g32.SelectObject.argtypes = [wt.HDC, wt.HANDLE]; g32.SelectObject.restype = wt.HANDLE
g32.DeleteObject.argtypes = [wt.HANDLE]
g32.DeleteDC.argtypes = [wt.HDC]
g32.PatBlt.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.DWORD]


class BMIH(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]


class BMI(ctypes.Structure):
    _fields_ = [("bmiHeader", BMIH), ("bmiColors", wt.DWORD * 3)]


def render_icon(hicon, size, path, bg=(240, 240, 240)):
    hdc = u32.GetDC(None)
    mem = g32.CreateCompatibleDC(hdc)
    bmi = BMI()
    bmi.bmiHeader.biSize = ctypes.sizeof(BMIH)
    bmi.bmiHeader.biWidth = size
    bmi.bmiHeader.biHeight = -size
    bmi.bmiHeader.biPlanes = 1
    bmi.bmiHeader.biBitCount = 32
    bmi.bmiHeader.biCompression = 0
    bits = ctypes.c_void_p()
    bmp = g32.CreateDIBSection(hdc, ctypes.byref(bmi), 0, ctypes.byref(bits), None, 0)
    old = g32.SelectObject(mem, bmp)
    # 先铺底色，便于观察透明边缘
    g32.PatBlt(mem, 0, 0, size, size, 0x00F00021)
    u32.DrawIconEx(mem, 0, 0, hicon, size, size, 0, None, 3)  # DI_NORMAL
    buf = ctypes.string_at(bits, size * size * 4)
    g32.SelectObject(mem, old); g32.DeleteObject(bmp); g32.DeleteDC(mem)
    u32.ReleaseDC(None, hdc)

    rows = bytearray()
    for y in range(size):
        rows.append(0)
        for x in range(size):
            b, g, r = buf[(y * size + x) * 4], buf[(y * size + x) * 4 + 1], buf[(y * size + x) * 4 + 2]
            rows += bytes((r, g, b))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n"
                + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(bytes(rows), 6))
                + chunk(b"IEND", b""))
    return size


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else r"D:\Bandicam\LiteCapCpp\LiteCap.exe"
    out = os.path.dirname(os.path.abspath(__file__))
    large = wt.HICON(); small = wt.HICON()
    n = shell32.ExtractIconExW(exe, -1, None, None, 0)
    print("exe 中图标总数 =", n)
    n2 = shell32.ExtractIconExW(exe, 0, ctypes.byref(large), ctypes.byref(small), 1)
    print("ExtractIconEx(0) 返回 =", n2, " large=%s small=%s" % (hex(large.value or 0), hex(small.value or 0)))
    if large:
        print("渲染 128px ->", render_icon(large, 128, os.path.join(out, "extracted_128.png")))
        u32.DestroyIcon(large)
    if small:
        print("渲染 32px  ->", render_icon(small, 32, os.path.join(out, "extracted_32.png")))
        u32.DestroyIcon(small)
    return 0


sys.exit(main())
