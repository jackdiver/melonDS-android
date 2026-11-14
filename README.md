# melonDS Android port
Android port of [melonDS](https://melonds.kuribo64.net/), a DS and DSi emulator.

[<img src="https://play.google.com/intl/en_us/badges/static/images/badges/en_badge_web_generic.png" alt="Get it on Google Play" height="80">](https://play.google.com/store/apps/details?id=me.magnum.melonds&pcampaignid=pcampaignidMKT-Other-global-all-co-prtnr-py-PartBadge-Mar2515-1)[<img src="https://raw.githubusercontent.com/Kunzisoft/Github-badge/main/get-it-on-github.png" alt="Get it on GitHub" height="80">](https://github.com/rafaelvcaetano/melonDS-android/releases/latest)

|Rom List|Dark Theme|Pocket Physics|Layout Editor|
|---|---|---|---|
|![Screenshot 1](./.github/images/screenshot_mobile0.png)|![Screenshot 2](./.github/images/screenshot_mobile1.png)|![Screenshot 3](./.github/images/screenshot_mobile2.png)|![Screenshot 4](./.github/images/screenshot_mobile3.png)|

# Missing Features
*  DSi SD card support
*  Customizable button skins
*  More display filters

# Local Multiplayer (Wi-Fi)
This fork can play the DS's own local-wireless modes (for example Mario Kart DS: Multiplayer) between 2 to 8 Android devices on the same Wi-Fi network. No hotspot or SIM is needed.

**How to use it**
1. Both devices: same game ROM, same BIOS setting (built-in BIOS recommended), and the **software** video renderer for the lobby to start.
2. On one device, open the game's multiplayer menu and create a group. On the others, open the same menu and wait for the group to appear, then join it.
3. The session starts a few seconds after the last player joins. The pause menu has a "Local multiplayer" screen with the status and a Stop button.

**How it works.** Every device runs every player's console, and only the controller inputs travel over the network. A deterministic in-process wireless link keeps all copies identical, and state checks every 60 frames detect any mismatch. Each extra player costs about 125 MB of memory and CPU time, so the player limit depends on the device's RAM.

**Limits**
* Only Mario Kart DS has been tested. Download Play only games (such as Mario Party DS) are not supported.
* 2 and 3 players are tested; 4 to 8 are not. Slower phones may not keep full speed with 3 or more players.
* Both devices need Wi-Fi and the local network permission. Rewind is disabled during a session.
* Loading a save state from another device overwrites the game's save, so back up your saves first.

More detail: [docs/netplay-plan.md](docs/netplay-plan.md).

# Performance
Performance is solid on 64 bit devices with thread rendering and JIT enabled, and should run at full speed on flagship devices. Performance on older devices, specially
32 bit devices, is very poor due to the lack of JIT support.

# Integration with third-party frontends
It's possible to launch melonDS from third part frontends. For that, you will need to have the ROMs you want to launch already scanned by melonDS. Then, you can configure your
third-party frontend with the following configuration:
*  Package name: `me.magnum.melonds`
*  Activity name: `me.magnum.melonds.ui.emulator.EmulatorActivity`
*  Parameters (choose one):
    * Intent data (preferred) - a URI of the NDS ROM (ZIP and 7z files are supported). Ensure [read permission is granted](https://developer.android.com/reference/android/content/Intent#FLAG_GRANT_READ_URI_PERMISSION)
    * `uri` (deprecated) - a string with the [SAF](https://developer.android.com/guide/topics/providers/create-document-provider) URI of the NDS ROM (ZIP and 7z files are supported)
    * `PATH` (deprecated) - a string with the absolute path to the NDS ROM (ZIP and 7z files are supported)

### Pegasus metadata files
* [melonds.metadata.txt](./.github/pegasus/melonds.metadata.txt) 
* [melonds-nightly.metadata.txt](./.github/pegasus/melonds-nightly.metadata.txt) 

### Info regarding save files
When launching ROMs from third-party frontends, if melonDS hasn't scanned that particular ROM previously, it won't be able to create the save file next to the ROM file if the
option "Save next to ROM file" is enabled in the settings or the save file directory is not set. Instead, melonDS will create a save file in
`Android/data/me.magnum.melonds/files/saves`

# Nightly Builds

To have access to the latest changes, you can install nightly builds that you can find [here](https://github.com/rafaelvcaetano/melonDS-android/releases/tag/nightly-release).

Be aware that these builds can contain more bugs than usual and you may need to clear your app data to get it to work properly after updates.

# Building
To build the project you will need Android SDK, NDK and CMake.

## Build steps:
1.  Clone the project, including submodules with:
    
    `git clone --recurse-submodules https://github.com/rafaelvcaetano/melonDS-android.git`
2.  Install the Android SDK, NDK and CMake
3.  Build with:
    1.  Unix: `./gradlew :app:assembleGitHubProdDebug`
    2.  Windows: `gradlew.bat :app:assembleGitHubProdDebug`
4.  The generated APK can be found at `app/gitHubProd/debug`

If you want to create a release build, you will need to modify your `local.properties` file to include the following fields:  
*  `MELONDS_KEYSTORE=<path_to_your_keystore>`
*  `MELONDS_KEYSTORE_PASSWORD=<keystore_password>`
*  `MELONDS_KEY_ALIAS=<name_of_your_key_alias>`
*  `MELONDS_KEY_PASSWORD=<key_alias_password>`
