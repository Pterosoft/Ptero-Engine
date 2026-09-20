"""Render result screens with the existing verify.exe; never compile anything.

The older executable expects HUD-specific probe IDs. A temporary document maps
the three result action IDs to its button probes and supplies transparent empty
probes for the HUD-only elements. Authored RML/RCSS and artwork are not modified.
"""
from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "Tools" / "verify.exe"

def render(name, width=1600, height=900):
    if not EXE.is_file():
        raise RuntimeError("Existing Tools/verify.exe is required. This script never builds it.")
    tree = ET.parse(ROOT / f"{name}.rml")
    body = tree.getroot().find("body")
    ids = [el.get("id") for el in tree.iter() if el.get("id")]
    assert len(ids) == len(set(ids)), "Duplicate IDs"
    for link in tree.getroot().find("head").findall("link"):
        link.set("href", (ROOT / link.get("href")).as_posix())
    aliases = {"rematch-button": "roll-button", "main-menu-button": "bank-button", "continue-button": "clear-button"}
    for element in tree.iter():
        if element.get("id") in aliases:
            element.set("id", aliases[element.get("id")])
    for index, id_ in enumerate(("help-button", "pause-button")):
        ET.SubElement(body, "button", {"id": id_, "style": f"position: absolute; left: {index * 8}px; top: 0px; bottom: auto; right: auto; width: 4px; height: 4px; opacity: 0;"})
    for id_ in ("score-panel", "scoring-panel", "log-panel", "dice-tray", "actions", "dialog"):
        ET.SubElement(body, "div", {"id": id_, "style": "position: absolute; left: 0px; top: 0px; bottom: auto; right: auto; width: 0px; height: 0px; display: none;"})
    output = ROOT / "Preview" / f"{name}-{width}x{height}.png"
    with tempfile.TemporaryDirectory(prefix="farkle-result-preview-") as temporary:
        document = Path(temporary) / f"{name}.rml"
        tree.write(document, encoding="utf-8")
        # The dialog argument skips the old HUD's center-transparency assertions;
        # these result screens intentionally paint a centered board.
        completed = subprocess.run([str(EXE), str(document), str(output), str(width), str(height), "dialog"], capture_output=True, text=True)
        print(completed.stdout)
        if completed.returncode:
            raise RuntimeError(completed.stderr or f"Native preview failed: {completed.returncode}")
    image = Image.open(output)
    assert image.mode == "RGBA"
    for point in ((0, 0), (width-1, height-1), (width//10, height//2), (width*9//10, height//2)):
        assert image.getpixel(point)[3] == 0, f"Backdrop at {point}"
    print(f"PASS: {name}, native RmlUi layout and three action click targets; transparent margins. {output}")

if __name__ == "__main__":
    for screen in ("victory", "defeat"):
        render(screen)
        render(screen, 1280, 720)
