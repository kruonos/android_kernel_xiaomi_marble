#!/usr/bin/env python3
import json
from pathlib import Path

root = Path(__file__).resolve().parent / "manifests"
reference = json.loads((root / "csi.json").read_text())
for label in ("baseline", "mglru", "wifi"):
    other = json.loads((root / (label + ".json")).read_text())
    content = sorted(name for name in reference.keys() & other.keys()
                     if (reference[name].get("sha256"), reference[name].get("target")) !=
                        (other[name].get("sha256"), other[name].get("target")))
    (root / (label + "-content-differences.json")).write_text(json.dumps(content, indent=2) + "\n")
    print(label, len(content), "content differences:")
    print("\n".join(content))
