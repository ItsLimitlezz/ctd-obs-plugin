// Prints the main display's colour space and, with --set, makes sRGB the display's profile for this user.
import AppKit
import ColorSync
import CoreGraphics

func describe(_ label: String) {
    let id = CGMainDisplayID()
    let cs = CGDisplayCopyColorSpace(id)
    let name = (cs.name as String?) ?? "unnamed (ICC-based)"
    print("\(label): display \(id) colour space: \(name), model \(cs.model.rawValue)")
    for s in NSScreen.screens {
        print("  NSScreen \(s.localizedName): \(s.colorSpace?.localizedName ?? "nil"), cg name \((s.colorSpace?.cgColorSpace?.name as String?) ?? "unnamed")")
    }
}

describe("before")
if CommandLine.arguments.contains("--set") {
    let id = CGMainDisplayID()
    guard let uuid = CGDisplayCreateUUIDFromDisplayID(id)?.takeRetainedValue() else { print("no display UUID"); exit(1) }
    let srgb = URL(fileURLWithPath: "/System/Library/ColorSync/Profiles/sRGB Profile.icc") as CFURL
    let profiles: [CFString: Any] = [
        kColorSyncDeviceDefaultProfileID.takeUnretainedValue(): srgb,
        kColorSyncProfileUserScope.takeUnretainedValue(): kCFPreferencesCurrentUser,
    ]
    let ok = ColorSyncDeviceSetCustomProfiles(kColorSyncDisplayDeviceClass.takeUnretainedValue(), uuid, profiles as CFDictionary)
    print("set sRGB: \(ok)")
    Thread.sleep(forTimeInterval: 2)
    describe("after")
}
