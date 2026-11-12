package me.magnum.melonds.ui.emulator.netplay

import android.content.Context
import android.content.pm.PackageManager
import android.net.ConnectivityManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.ViewGroup
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.core.content.edit
import me.magnum.melonds.MelonDSAndroidInterface
import java.net.Inet4Address

/**
 * Host/Join screen for local multiplayer over Wi-Fi (netplay, see docs/netplay-plan.md).
 *
 * Both devices run both consoles; only inputs travel over the network. The host is player 1, the joiner
 * player 2. While a session is active, status changes are shown as short messages.
 */
class LocalMultiplayerController(
    private val activity: AppCompatActivity,
    private val onDialogShown: () -> Unit,
    private val onDialogClosed: (resume: Boolean) -> Unit,
) {
    private companion object {
        const val PORT = 7064
        const val INPUT_DELAY = 2
        const val PREFS = "local_multiplayer"
        const val PREF_LAST_HOST = "last_host"
        const val LOCAL_NETWORK_PERMISSION = "android.permission.ACCESS_LOCAL_NETWORK"
        const val PERMISSION_REQUEST = 7064
        const val POLL_MS = 500L
    }

    private data class Status(val phase: String, val frame: Long, val desyncs: Long, val message: String)

    private val handler = Handler(Looper.getMainLooper())
    private var lastStatus: Status? = null
    private var polling = false
    private var dialogStatusView: TextView? = null

    private val poll = object : Runnable {
        override fun run() {
            val status = readStatus()
            dialogStatusView?.text = describe(status)
            announceChanges(lastStatus, status)
            lastStatus = status
            if (polling)
                handler.postDelayed(this, POLL_MS)
        }
    }

    fun showDialog() {
        val padding = (20 * activity.resources.displayMetrics.density).toInt()
        val ownAddress = localIpAddress()
        val prefs = activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

        val statusView = TextView(activity)
        val addressView = TextView(activity).apply {
            text = if (ownAddress != null) "This device: $ownAddress" else "Not connected to Wi-Fi"
        }
        val hostInput = EditText(activity).apply {
            hint = "Host IP address (to join)"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI
            setText(prefs.getString(PREF_LAST_HOST, ""))
            setSingleLine()
        }
        val layout = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(padding, padding / 2, padding, 0)
            addView(addressView)
            addView(statusView, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).also { })
            addView(hostInput)
        }
        statusView.setPadding(0, padding / 2, 0, padding / 2)
        dialogStatusView = statusView
        statusView.text = describe(readStatus())

        onDialogShown()
        var resumeOnClose = true
        val dialog = AlertDialog.Builder(activity)
            .setTitle("Local multiplayer")
            .setView(layout)
            .setPositiveButton("Host", null)
            .setNeutralButton("Join", null)
            .setNegativeButton("Stop", null)
            .setOnDismissListener {
                dialogStatusView = null
                onDialogClosed(resumeOnClose)
            }
            .create()

        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                if (ensureLocalNetworkPermission()) {
                    MelonDSAndroidInterface.mirrorStart()
                    MelonDSAndroidInterface.netHost(PORT, INPUT_DELAY)
                    toast(if (ownAddress != null) "Hosting. On the other device, join $ownAddress" else "Hosting")
                    startPolling()
                    dialog.dismiss()
                }
            }
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener {
                val address = hostInput.text.toString().trim()
                if (!isIpv4(address)) {
                    hostInput.error = "Enter the host's IP address, e.g. 192.168.0.4"
                    return@setOnClickListener
                }
                if (ensureLocalNetworkPermission()) {
                    prefs.edit { putString(PREF_LAST_HOST, address) }
                    MelonDSAndroidInterface.mirrorStart()
                    MelonDSAndroidInterface.netJoin(address, PORT, INPUT_DELAY)
                    startPolling()
                    dialog.dismiss()
                }
            }
            dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setOnClickListener {
                stopSession()
                dialog.dismiss()
            }
        }
        dialog.show()
        startPolling()
    }

    fun stopSession() {
        MelonDSAndroidInterface.netStop()
        MelonDSAndroidInterface.mirrorStop()
    }

    /**
     * Always-on watcher: sessions can start from the game's own wireless menus (create group / join), so status
     * changes are reported even when this dialog was never opened. Also keeps Wi-Fi broadcasts flowing for host
     * discovery.
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
    }

    private var multicastLock: android.net.wifi.WifiManager.MulticastLock? = null
    private var permissionRequested = false

    private fun acquireMulticastLock() {
        if (multicastLock != null) return
        val wifi = activity.applicationContext.getSystemService(android.net.wifi.WifiManager::class.java) ?: return
        multicastLock = wifi.createMulticastLock("melonDS-netplay").apply {
            setReferenceCounted(false)
            try { acquire() } catch (_: SecurityException) { }
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
            "idle" -> if (status.message.isNotEmpty()) status.message else "Not connected. Create or look for a group in the game, or use Host / Join below."
            "waiting" -> "Hosting: waiting for the other player to join"
            "connecting" -> "Connecting to the host..."
            "syncing" -> "Connected, starting the session..."
            "running" -> if (status.desyncs == 0L) "Playing, in sync" else "Playing, ${status.desyncs} desync(s) detected"
            "error" -> status.message
            else -> if (status.message.isNotEmpty()) "Not connected (last session: ${status.message})" else "Not connected"
        }
    }

    private fun announceChanges(old: Status?, new: Status) {
        if (new.phase == "hosting" || new.phase == "searching")
            ensureLocalNetworkPermissionOnce()
        if (old == null) return
        if (old.phase != new.phase) {
            when (new.phase) {
                "hosting" -> toast("Wi-Fi multiplayer: group open to other devices")
                "joining" -> toast("A player is joining over Wi-Fi...")
                "syncing" -> toast("Player connected, starting...")
                "running" -> toast("Wi-Fi multiplayer connected")
                "error" -> toast(new.message)
                "off", "idle" -> if (old.phase == "running")
                    toast(if (new.message.isNotEmpty()) "Wi-Fi multiplayer ended: ${new.message}" else "Wi-Fi multiplayer ended")
            }
        }
        if (new.message.isNotEmpty() && new.message != old.message && new.phase != "running" && old.phase != "running")
            toast(new.message)
        if (new.phase == "running" && old.desyncs == 0L && new.desyncs > 0)
            toast("The two devices are out of sync") // once per session
    }

    private fun ensureLocalNetworkPermissionOnce() {
        if (permissionRequested || Build.VERSION.SDK_INT < 37)
            return
        if (ContextCompat.checkSelfPermission(activity, LOCAL_NETWORK_PERMISSION) == PackageManager.PERMISSION_GRANTED)
            return
        permissionRequested = true
        ActivityCompat.requestPermissions(activity, arrayOf(LOCAL_NETWORK_PERMISSION), PERMISSION_REQUEST)
    }

    private fun ensureLocalNetworkPermission(): Boolean {
        if (Build.VERSION.SDK_INT < 37)
            return true
        if (ContextCompat.checkSelfPermission(activity, LOCAL_NETWORK_PERMISSION) == PackageManager.PERMISSION_GRANTED)
            return true
        ActivityCompat.requestPermissions(activity, arrayOf(LOCAL_NETWORK_PERMISSION), PERMISSION_REQUEST)
        toast("Allow local network access, then try again")
        return false
    }

    private fun localIpAddress(): String? {
        val connectivity = activity.getSystemService(ConnectivityManager::class.java) ?: return null
        for (network in connectivity.allNetworks) {
            val properties = connectivity.getLinkProperties(network) ?: continue
            val address = properties.linkAddresses
                .map { it.address }
                .firstOrNull { it is Inet4Address && !it.isLoopbackAddress && it.isSiteLocalAddress }
            if (address != null)
                return address.hostAddress
        }
        return null
    }

    private fun isIpv4(text: String): Boolean {
        val parts = text.split(".")
        return parts.size == 4 && parts.all { part -> part.toIntOrNull()?.let { it in 0..255 } == true }
    }

    private fun toast(text: String) {
        Toast.makeText(activity, text, Toast.LENGTH_LONG).show()
    }
}
