"""Read the standard Windows file clipboard independently of the app's writer."""
import ctypes as c
import ctypes.wintypes as w


def clipboard_files():
    user = c.windll.user32
    shell = c.windll.shell32
    user.GetClipboardData.argtypes = [w.UINT]
    user.GetClipboardData.restype = w.HANDLE
    shell.DragQueryFileW.argtypes = [w.HANDLE, w.UINT, w.LPWSTR, w.UINT]
    result = []
    assert user.OpenClipboard(None)
    try:
        drop = user.GetClipboardData(15)
        if drop:
            for index in range(shell.DragQueryFileW(drop, 0xFFFFFFFF, None, 0)):
                size = shell.DragQueryFileW(drop, index, None, 0)
                text = c.create_unicode_buffer(size + 1)
                shell.DragQueryFileW(drop, index, text, len(text))
                result.append(text.value)
    finally:
        user.CloseClipboard()
    return result
