"""The release's game code tools in one program, frozen by PyInstaller (tools/armsgen.spec), so a player needs no Python:

    armsgen extract <game.xci|nsp> <prod.keys> <outdir> [--update <update.nsp>]   = tools/nsp_extract.py
    armsgen gen <name> <nso> <base> <outdir> [--hle ...] [--simd c]                = tools/gen.py

The prepare step (launcher/launcher.cpp, `armslauncher --prepare`) runs both on the player's own game files.
"""
import sys


def run():
    cmds = ('extract', 'gen')
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        print('usage: armsgen extract|gen <arguments>  (as tools/nsp_extract.py / tools/gen.py)', file=sys.stderr)
        return 2
    cmd = sys.argv.pop(1)
    sys.argv[0] = 'armsgen ' + cmd
    if cmd == 'extract':
        import nsp_extract
        return nsp_extract.main()
    import gen
    return gen.main()


if __name__ == '__main__':
    sys.exit(run())
