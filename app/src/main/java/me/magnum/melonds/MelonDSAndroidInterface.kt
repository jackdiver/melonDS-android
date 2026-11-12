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

    // Netplay phase 3 (debug): two devices over the network
    external fun netHost(port: Int, inputDelay: Int)
    external fun netJoin(address: String, port: Int, inputDelay: Int)
    external fun netStop()
    // "<phase> <frame> <desyncs> <message>"; phase is off|waiting|connecting|syncing|running|error
    external fun netStatus(): String
}
