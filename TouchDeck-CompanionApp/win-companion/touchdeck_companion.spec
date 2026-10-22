# -*- mode: python ; coding: utf-8 -*-
from PyInstaller.utils.hooks import collect_all

datas = [('..\\companion-common\\webui', 'webui')]
binaries = []
pc_state_imports = ['pc_state', 'art_server', 'winrt.windows.storage.streams', 'pycaw.pycaw', 'comtypes.stream', 'comtypes.gen',
                    'winrt.windows.media.control', 'winrt.windows.foundation',
                    'winrt.windows.foundation.collections', 'winrt.runtime']
hiddenimports = ['server', 'walls', 'app_window', 'app_state', 'config_model', 'pc_control', 'serial', 'serial.tools.list_ports', 'serial_link', 'win32security', 'clr', 'wmi', 'pythoncom', 'pywintypes', 'win32com', 'win32com.client', 'win32api', 'win32con', 'win32timezone']
hiddenimports += pc_state_imports
tmp_ret = collect_all('webview')
datas += tmp_ret[0]; binaries += tmp_ret[1]; hiddenimports += tmp_ret[2]
tmp_ret = collect_all('pythonnet')
datas += tmp_ret[0]; binaries += tmp_ret[1]; hiddenimports += tmp_ret[2]
tmp_ret = collect_all('pystray')
datas += tmp_ret[0]; binaries += tmp_ret[1]; hiddenimports += tmp_ret[2]
tmp_ret = collect_all('PIL')
datas += tmp_ret[0]; binaries += tmp_ret[1]; hiddenimports += tmp_ret[2]


a = Analysis(
    ['touchdeck_companion.py'],
    pathex=['..\\companion-common'],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)
exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='TouchDeckCompanion',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
