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

    // Netplay phase 2 (debug): hidden second console in the same process
    external fun mirrorStart(debugFlags: Int)
    external fun mirrorStop()
    external fun mirrorFocus(index: Int)
    external fun mirrorRecord(directory: String)
    external fun mirrorReplay(directory: String, tag: String)
    external fun mirrorRecordStop()
}
