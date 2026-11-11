package me.magnum.melonds

import me.magnum.melonds.common.UriFileHandler

object MelonDSAndroidInterface {
    external fun setup(uriFileHandler: UriFileHandler)
    external fun getEmulatorGlContext(): Long
    external fun cleanup()

    // Netplay phase 1: determinism test harness (see docs/netplay-plan.md)
    external fun determinismRecord(directory: String)
    external fun determinismReplay(directory: String, tag: String)
    external fun determinismStop()
    external fun determinismSelfTest(frames: Int)
}
