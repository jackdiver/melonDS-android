package me.magnum.melonds.ui.emulator.netplay

import android.content.pm.PackageManager
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import me.magnum.melonds.MelonDSAndroidInterface

/**
 * Wi-Fi multiplayer status (netplay, see docs/netplay-plan.md).
 *
 * Sessions start from the game's own wireless menus: one device creates a group, the other opens the group list.
 * Both devices then run both consoles and only inputs travel over the network. This class reports progress as short
 * messages, asks for the local network permission when the game first uses wireless, and backs the pause menu's
 * "Local multiplayer" screen (status and a Stop button).
 */
class LocalMultiplayerController(
    private val activity: AppCompatActivity,
    private val onDialogShown: () -> Unit,
    private val onDialogClosed: (resume: Boolean) -> Unit,
) {
    private companion object {
        const val LOCAL_NETWORK_PERMISSION = "android.permission.ACCESS_LOCAL_NETWORK"
        const val PERMISSION_REQUEST = 7064
        const val POLL_MS = 500L
    }

    private data class Status(val phase: String, val frame: Long, val desyncs: Long, val message: String)

    private val handler = Handler(Looper.getMainLooper())
    private var lastStatus: Status? = null
    private var polling = false
    private var dialogStatusView: TextView? = null
    private var multicastLock: WifiManager.MulticastLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private var permissionRequested = false
    private var lastStalledToastMs = 0L

    private val poll = object : Runnable {
        override fun run() {
            val status = readStatus()
            dialogStatusView?.text = describe(status)
            announceChanges(lastStatus, status)
            lastStatus = status
            repeatStalledToast(status)
            updateWifiLock(status.phase)
            if (polling)
                handler.postDelayed(this, POLL_MS)
        }
    }

    fun showDialog() {
        val padding = (20 * activity.resources.displayMetrics.density).toInt()
        val statusView = TextView(activity).apply {
            gravity = Gravity.START
            setPadding(padding, padding / 2, padding, 0)
            text = describe(readStatus())
        }
        val layout = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            addView(statusView)
        }
        dialogStatusView = statusView

        onDialogShown()
        AlertDialog.Builder(activity)
            .setTitle("Local multiplayer")
            .setView(layout)
            .setMessage("To play: create a group in the game's multiplayer menu on one device and open the group list on the other.")
            .setPositiveButton("Close", null)
            .setNegativeButton("Stop session") { _, _ -> MelonDSAndroidInterface.netStop() }
            .setOnDismissListener {
                dialogStatusView = null
                onDialogClosed(true)
            }
            .show()
        startPolling()
    }

    /**
     * Always-on watcher: sessions start from the game's own wireless menus, so status changes are reported even when
     * the screen was never opened. Also keeps Wi-Fi broadcasts flowing for host discovery.
     */
    fun start() {
        acquireMulticastLock()
        startPolling()
    }

    fun onDestroy() {
        polling = false
        handler.removeCallbacks(poll)
        multicastLock?.let { if (it.isHeld) it.release() }
        multicastLock = null
        wifiLock?.let { if (it.isHeld) it.release() }
        wifiLock = null
    }

    private fun acquireMulticastLock() {
        if (multicastLock != null) return
        val wifi = activity.applicationContext.getSystemService(WifiManager::class.java) ?: return
        multicastLock = wifi.createMulticastLock("melonDS-netplay").apply {
            setReferenceCounted(false)
            try { acquire() } catch (_: SecurityException) { }
        }
    }

    // Low-latency mode stops Wi-Fi power saving, which otherwise delays packets by tens of ms in bursts (Android 10+).
    // Only held while multiplayer is in use (it costs battery).
    private fun updateWifiLock(phase: String) {
        val wanted = phase == "hosting" || phase == "searching" || phase == "joining" || phase == "running" || phase == "stalled"
        if (wanted && wifiLock == null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val wifi = activity.applicationContext.getSystemService(WifiManager::class.java) ?: return
            wifiLock = wifi.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "melonDS-netplay").apply {
                setReferenceCounted(false)
                try { acquire() } catch (_: SecurityException) { }
            }
        } else if (!wanted) {
            wifiLock?.let { if (it.isHeld) it.release() }
            wifiLock = null
        }
    }

    private fun startPolling() {
        if (polling) return
        polling = true
        handler.post(poll)
    }

    private fun readStatus(): Status {
        val parts = MelonDSAndroidInterface.netStatus().split(" ", limit = 4)
        return Status(
            phase = parts.getOrElse(0) { "off" },
            frame = parts.getOrNull(1)?.toLongOrNull() ?: 0,
            desyncs = parts.getOrNull(2)?.toLongOrNull() ?: 0,
            message = parts.getOrElse(3) { "" },
        )
    }

    private fun describe(status: Status): String {
        return when (status.phase) {
            "hosting" -> "Your game created a group: waiting for another device to look for groups"
            "searching" -> "Your game is looking for groups: searching the Wi-Fi network for a host"
            "joining" -> "Another device is joining..."
            "running" -> if (status.desyncs == 0L) "Playing, in sync" else "Playing, ${status.desyncs} desync(s) detected"
            "stalled" -> "Waiting for the other player (their game may be paused)"
            else -> if (status.message.isNotEmpty()) status.message else "Not connected"
        }
    }

    private fun announceChanges(old: Status?, new: Status) {
        if (new.phase == "hosting" || new.phase == "searching")
            ensureLocalNetworkPermissionOnce()
        if ((new.phase == "hosting" || new.phase == "searching") && old?.phase != new.phase)
            warnAboutIncompatibleSettings()
        if (old == null) return
        if (old.phase != new.phase) {
            when (new.phase) {
                "hosting" -> toast("Wi-Fi multiplayer: group open to other devices")
                "joining" -> toast("A player is joining over Wi-Fi...")
                "running" -> if (old.phase != "stalled") toast("Wi-Fi multiplayer connected")
                else -> if (old.phase == "running" || old.phase == "stalled")
                    toast(if (new.message.isNotEmpty()) "Wi-Fi multiplayer ended: ${new.message}" else "Wi-Fi multiplayer ended")
            }
        }
        if (new.message.isNotEmpty() && new.message != old.message && new.phase != "running" && new.phase != "stalled" && old.phase != "running" && old.phase != "stalled")
            toast(new.message)
        if ((new.phase == "running" || new.phase == "stalled") && old.desyncs == 0L && new.desyncs > 0)
            toast("The two devices are out of sync") // once per session
    }

    // Different BIOS or DSi mode on the two devices breaks sync and cannot be switched while a game runs: tell the player.
    private fun warnAboutIncompatibleSettings() {
        val prefs = androidx.preference.PreferenceManager.getDefaultSharedPreferences(activity)
        val customBios = prefs.getBoolean("use_custom_bios", false)
        val dsi = prefs.getString("console_type", "ds") != "ds"
        if (customBios || dsi)
            toast("Wi-Fi multiplayer needs the built-in BIOS and DS mode on both devices. Change this in Settings, then restart the game.")
    }

    private fun ensureLocalNetworkPermissionOnce() {
        if (permissionRequested || Build.VERSION.SDK_INT < 37)
            return
        if (ContextCompat.checkSelfPermission(activity, LOCAL_NETWORK_PERMISSION) == PackageManager.PERMISSION_GRANTED)
            return
        permissionRequested = true
        ActivityCompat.requestPermissions(activity, arrayOf(LOCAL_NETWORK_PERMISSION), PERMISSION_REQUEST)
    }

    // A single toast is easy to miss (you are usually looking at the device that paused): repeat it while stalled.
    private fun repeatStalledToast(status: Status) {
        if (status.phase != "stalled") return
        val now = android.os.SystemClock.elapsedRealtime()
        if (now - lastStalledToastMs < 3500) return
        lastStalledToastMs = now
        toast("Waiting for the other player (their game may be paused)...")
    }

    private fun toast(text: String) {
        Toast.makeText(activity, text, Toast.LENGTH_LONG).show()
    }
}
