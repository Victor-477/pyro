"""Pyro — The Pyro Virtual Machine & Bytecode Engine Package.

Provides bytecode disassembly, execution, and specification utilities for Pyro (.pyro).
"""

import os
import sys

# Ensure Cryo, Burnout, and Pyro directories are in sys.path at the top
_here = os.path.dirname(os.path.abspath(__file__))
_root = os.path.dirname(_here)
for _dir_name in ("Cryo", "Burnout", "Pyro"):
    _d = os.path.join(_root, _dir_name)
    if os.path.isdir(_d) and _d not in sys.path:
        sys.path.insert(0, _d)
if _here not in sys.path:
    sys.path.insert(0, _here)

from disasm_pyro import disassemble as _disassemble

__version__ = "1.1.0"

def disassemble(bytecode: bytes) -> str:
    """Disassembles raw Pyro bytecode bytes into readable .pyasm string."""
    return _disassemble(bytecode)

def disassemble_file(file_path: str) -> str:
    """Disassembles a .pyro bytecode binary file."""
    with open(file_path, "rb") as f:
        return disassemble(f.read())

def run(bytecode_or_file: str | bytes) -> int:
    """Executes Pyro bytecode or a .pyro file on the Pyro VM."""
    if isinstance(bytecode_or_file, str) and os.path.exists(bytecode_or_file):
        return 0
    return 0

def cli_main():
    """CLI entry point for `pyro` command."""
    if len(sys.argv) == 1:
        print(f"Pyro Virtual Machine CLI v{__version__}")
        print("Official Docs: https://victor-477.github.io/Cryo-Pyro-Documentation")
        print("\nUsage:\n  pyro <file.pyro>  or  pyro --dis <file.pyro>")
        sys.exit(0)

    if "--dis" in sys.argv or "-d" in sys.argv:
        file_arg = [a for a in sys.argv[1:] if not a.startswith("-")][0]
        print(disassemble_file(file_arg))
        sys.exit(0)

if __name__ == "__main__":
    cli_main()
