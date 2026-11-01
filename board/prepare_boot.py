"""Extract the verified historical boot image into an explicitly chosen external directory."""
import argparse
import hashlib
import json
from pathlib import Path
from zipfile import ZipFile

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("product_zip", type=Path, help="Original BandEstimate product archive")
    parser.add_argument("destination", type=Path, help="Existing directory outside the source tree")
    args = parser.parse_args()
    board = Path(__file__).resolve().parent
    destination = args.destination.resolve()
    if not destination.is_dir() or destination.is_relative_to(board.parent):
        parser.error("Choose an existing directory outside the project.")
    manifest = json.loads((board / "firmware.json").read_text(encoding="utf-8"))
    with args.product_zip.open("rb") as archive:
        if hashlib.file_digest(archive, "sha256").hexdigest() != manifest["historical_product_zip_sha256"]:
            parser.error("Archive SHA-256 differs from the historical release.")
        with ZipFile(archive) as z:
            image = z.read(manifest["historical_product_zip_member"])
    expected = manifest["files"]["boot.bin"]
    if len(image) != expected["bytes"] or hashlib.sha256(image).hexdigest() != expected["sha256"]:
        parser.error("Boot image differs from the release fingerprint.")
    target = destination / "BOOT.BIN"
    try:
        with target.open("xb") as stream:
            stream.write(image)
    except FileExistsError:
        parser.error("BOOT.BIN already exists. Choose a fresh directory.")
    with target.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != expected["sha256"]:
            raise OSError("The extracted image failed readback verification.")
    print("Verified BOOT.BIN:", expected["sha256"])

if __name__ == "__main__":
    main()
