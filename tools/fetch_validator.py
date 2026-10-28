#!/usr/bin/env python3

import hashlib
import io
import os
from pathlib import Path, PurePosixPath
import shutil
import tarfile
import urllib.request
import zipfile

URL = "https://github.com/free-audio/clap-validator/releases/download/0.4.1/clap-validator-0.4.1-127-g152b982-macos-15-aarch64.zip"
SHA256 = "719a0248ea431718bb7c92c8a0b9a78afa0bb23d692cf452e80973fe8b89282d"


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    destination = root / "build" / "tools" / "clap-validator-0.4.1"
    destination.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(URL) as response:
        payload = response.read()
    if hashlib.sha256(payload).hexdigest() != SHA256:
        raise SystemExit("Validator archive SHA-256 mismatch")

    with zipfile.ZipFile(io.BytesIO(payload)) as archive:
        names = [name for name in archive.namelist() if name.endswith(".tar.gz")]
        if len(names) != 1:
            raise SystemExit("Expected one validator tarball")
        tarball = io.BytesIO(archive.read(names[0]))

    with tarfile.open(fileobj=tarball, mode="r:gz") as archive:
        for member in archive.getmembers():
            path = PurePosixPath(member.name)
            if not member.isfile() or path.is_absolute() or ".." in path.parts:
                continue
            source = archive.extractfile(member)
            if source is None:
                continue
            target = destination.joinpath(*path.parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            with source, target.open("wb") as output:
                shutil.copyfileobj(source, output)
            target.chmod(member.mode | 0o100)

    binary = destination / "clap-validator"
    if not binary.is_file():
        raise SystemExit("Validator binary was not present in the release")
    os.chmod(binary, 0o755)
    print(binary)


if __name__ == "__main__":
    main()
