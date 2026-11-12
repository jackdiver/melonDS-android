package me.magnum.melonds

import me.magnum.melonds.common.UriFileHandler

object MelonDSAndroidInterface {
    external fun setup(uriFileHandler: UriFileHandler)
    external fun getEmulatorGlContext(): Long
    external fun cleanup()

    // Local multiplayer over Wi-Fi (see docs/netplay-plan.md). Sessions start from the game's own wireless menus
    // (create group / join list); these calls back the Local multiplayer screen.
    external fun netStop()

    // "<phase> <frame> <desyncs> <message>"; phase is idle|hosting|searching|joining|running|stalled
    external fun netStatus(): String
}
