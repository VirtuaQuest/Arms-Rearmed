# PyInstaller spec: tools/armsgen.py (the extractor and the game code generator) as a standalone onedir program, so the
# release's prepare step builds the game code on the player's machine without Python (launcher/launcher.cpp).
# `make gen-helper` -> build/helper/armsgen/ (armsgen.exe + _internal/). Holds no game code or game files.
import glob
import importlib.util
import os
import sys

from PyInstaller.utils.hooks import collect_dynamic_libs, collect_submodules

TOOLS = os.path.normpath(SPECPATH)

# unicorn's library has a versioned name that collect_dynamic_libs misses: added by hand where unicorn looks for it
_uc_lib_dir = os.path.join(os.path.dirname(importlib.util.find_spec('unicorn').origin), 'lib')
_uc_libs = glob.glob(os.path.join(_uc_lib_dir, 'unicorn.dll')) + glob.glob(os.path.join(_uc_lib_dir, 'libunicorn.so*'))

a = Analysis(
    [os.path.join(TOOLS, 'armsgen.py')],
    pathex=[TOOLS],
    binaries=collect_dynamic_libs('capstone') + collect_dynamic_libs('unicorn') + [(p, 'unicorn/lib') for p in _uc_libs],
    datas=[],
    hiddenimports=collect_submodules('unicorn') + ['gen', 'nsp_extract', 'lz4.block', 'Crypto.Cipher.AES'],
    excludes=['tkinter', 'pydoc', 'doctest', 'unittest', 'pdb', 'IPython', 'numpy', 'setuptools', '_distutils_hack',
              'pkg_resources', 'ssl', '_ssl', '_hashlib', '_decimal', 'PIL'],
    noarchive=False,
)
pyz = PYZ(a.pure)
exe = EXE(
    pyz,
    a.scripts,
    [('hash_seed=0', None, 'OPTION')],  # (the same output every run)
    exclude_binaries=True,
    name='armsgen',
    console=True,  # (its output goes to the prepare step's log; started without a window)
    upx=False,
)
coll = COLLECT(exe, a.binaries, a.datas, upx=False, name='armsgen')
