"""Packages the extension into a zip that can be unzipped and loaded unpacked in Chrome.

Usage (run from anywhere):
    python package.py [--out <path to the .zip>]

The archive contains a single top level folder, so unzipping it does not scatter files.
"""

import argparse
import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
NAME = os.path.basename(HERE)

# Build artefacts and tooling that is not part of the extension itself.
EXCLUDED_FILES = {"package.py", "make_icons.py", "panel.png"}
EXCLUDED_SUFFIXES = (".zip", ".crx", ".pem", ".log", ".pyc")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=os.path.join(os.path.dirname(HERE), NAME + ".zip"),
        help="where to write the archive")
    args = parser.parse_args()

    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as archive:
        for root, directories, files in os.walk(HERE):
            directories[:] = [d for d in directories if d != "__pycache__"]
            for name in sorted(files):
                if name in EXCLUDED_FILES or name.endswith(EXCLUDED_SUFFIXES):
                    continue
                path = os.path.join(root, name)
                relative = os.path.relpath(path, HERE).replace(os.sep, "/")
                archive.write(path, f"{NAME}/{relative}")

    size = os.path.getsize(args.out)
    print(f"wrote {args.out} ({size} bytes)")
    with zipfile.ZipFile(args.out) as archive:
        for info in archive.infolist():
            print(f"  {info.file_size:>9}  {info.filename}")


if __name__ == "__main__":
    main()
