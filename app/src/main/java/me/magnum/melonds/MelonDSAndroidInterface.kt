package me.magnum.melonds

import me.magnum.melonds.common.UriFileHandler

object MelonDSAndroidInterface {
    external fun setup(uriFileHandler: UriFileHandler)
    external fun getEmulatorGlContext(): Long
    external fun cleanup()

    // Local multiplayer over Wi-Fi (see docs/netplay-plan.md). Sessions normally start from the game's own
    // wireless menus; these calls back the manual Host / Join buttons of the Local multiplayer screen.
    external fun mirrorStart()
    external fun mirrorStop()
    external fun netHost(port: Int, inputDelay: Int)
    external fun netJoin(address: String, port: Int, inputDelay: Int)
    external fun netStop()

    // "<phase> <frame> <desyncs> <message>"; phase is idle|hosting|searching|joining|waiting|connecting|syncing|running|error
    external fun netStatus(): String
}
