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
                    MelonDSAndroidInterface.mirrorStart(0)
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
                    MelonDSAndroidInterface.mirrorStart(0)
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

    fun onDestroy() {
        polling = false
        handler.removeCallbacks(poll)
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
            "waiting" -> "Hosting: waiting for the other player to join"
            "connecting" -> "Connecting to the host..."
            "syncing" -> "Connected, starting the session..."
            "running" -> if (status.desyncs == 0L) "Playing, in sync" else "Playing, ${status.desyncs} desync(s) detected"
            "error" -> status.message
            else -> if (status.message.isNotEmpty()) "Not connected (last session: ${status.message})" else "Not connected"
        }
    }

    private fun announceChanges(old: Status?, new: Status) {
        if (old == null) return
        if (old.phase != new.phase) {
            when (new.phase) {
                "syncing" -> toast("Player connected, starting...")
                "running" -> toast("Local multiplayer started")
                "error" -> toast(new.message)
                "off" -> if (old.phase != "error") toast(if (new.message.isNotEmpty()) "Local multiplayer ended: ${new.message}" else "Local multiplayer ended")
            }
        }
        if (new.phase == "running" && new.desyncs > old.desyncs)
            toast("The two devices are out of sync")
        if (new.phase == "off" || new.phase == "error") {
            // Keep polling only while the dialog is open
            if (dialogStatusView == null)
                polling = false
        }
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
