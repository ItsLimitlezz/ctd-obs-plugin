#!/usr/bin/env python3
"""Tells whether a macOS crash report (.ips) is OBS's own crash on GitHub's arm64 macOS runners (issue #30).

On those runner VMs (macos-15 and macos-26 arm64 images from November 2026), OBS 32.2.2 crashes on startup with no
plugin installed: loading the scene collection sets Qt::WaitCursor, Qt builds that cursor from an image, and
QImage::toCGImage() hands CGImageCreate an invalid colour space (SIGTRAP in CGColorSpaceGetType). Exits 0 when the
report is that crash and no CTD Live code is on the crashing thread, 1 otherwise.
  macos-known-crash.py <report.ips>
"""

import json
import sys


def main(path):
    with open(path, encoding="utf-8") as f:
        _header, body = f.read().split("\n", 1)
    report = json.loads(body)
    images = report.get("usedImages", [])
    thread = next((t for t in report.get("threads", []) if t.get("triggered")), None)
    if thread is None:
        return 1
    symbols, ours = set(), False
    for frame in thread.get("frames", []):
        index = frame.get("imageIndex", -1)
        image = images[index].get("name", "") if 0 <= index < len(images) else ""
        if image.startswith("ctd-live"):
            ours = True
        symbols.add(frame.get("symbol", ""))
    wanted = {"CGImageCreate", "QImage::toCGImage() const", "QWidget::setCursor(QCursor const&)"}
    return 0 if wanted <= symbols and not ours else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
