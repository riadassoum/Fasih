import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from grammar import generate_cpp_table

if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.normpath(os.path.join(here, "..", "cpp", "include", "tashkeel", "particles_table.inc"))
    generate_cpp_table(out)
    print(f"Wrote {out}")
