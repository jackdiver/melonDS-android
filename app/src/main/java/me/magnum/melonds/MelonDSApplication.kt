package me.magnum.melonds

import android.app.Application
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.app.NotificationChannelCompat
import androidx.core.app.NotificationManagerCompat
import androidx.hilt.work.HiltWorkerFactory
import androidx.work.Configuration
import dagger.hilt.android.HiltAndroidApp
import kotlinx.coroutines.DelicateCoroutinesApi
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.GlobalScope
import kotlinx.coroutines.launch
import me.magnum.melonds.common.UriFileHandler
import me.magnum.melonds.common.uridelegates.UriHandler
import me.magnum.melonds.domain.repositories.SettingsRepository
import me.magnum.melonds.migrations.Migrator
import javax.inject.Inject

@HiltAndroidApp
class MelonDSApplication : Application(), Configuration.Provider {
    companion object {
        const val NOTIFICATION_CHANNEL_ID_BACKGROUND_TASKS = "channel_cheat_importing"

        init {
            System.loadLibrary("melonDS-android-frontend")
        }
    }

    @Inject lateinit var workerFactory: HiltWorkerFactory
    @Inject lateinit var settingsRepository: SettingsRepository
    @Inject lateinit var migrator: Migrator
    @Inject lateinit var uriHandler: UriHandler

    override fun onCreate() {
        super.onCreate()
        createNotificationChannels()
        applyTheme()
        performMigrations()
        MelonDSAndroidInterface.setup(UriFileHandler(this, uriHandler))
        registerDeterminismTestReceiver()
    }

    /**
     * Debug builds only: drive the netplay determinism harness from a PC (see docs/netplay-plan.md).
     *   adb shell am broadcast -p <package> -a me.magnum.melonds.NETPLAY_TEST --es cmd record
     *   adb shell am broadcast -p <package> -a me.magnum.melonds.NETPLAY_TEST --es cmd replay --es tag s23
     *   adb shell am broadcast -p <package> -a me.magnum.melonds.NETPLAY_TEST --es cmd stop
     * Files live in <filesDir>/netplay-test (readable with `adb shell run-as <package>`).
     */
    private fun registerDeterminismTestReceiver() {
        if (applicationInfo.flags and android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE == 0) return

        val receiver = object : android.content.BroadcastReceiver() {
            override fun onReceive(context: android.content.Context, intent: android.content.Intent) {
                val dir = java.io.File(filesDir, "netplay-test").apply { mkdirs() }.absolutePath
                when (intent.getStringExtra("cmd")) {
                    "record" -> MelonDSAndroidInterface.determinismRecord(dir)
                    "replay" -> MelonDSAndroidInterface.determinismReplay(dir, intent.getStringExtra("tag") ?: "replay")
                    "stop" -> MelonDSAndroidInterface.determinismStop()
                    "selftest" -> MelonDSAndroidInterface.determinismSelfTest(intent.getIntExtra("frames", 300))
                    "mirror-start" -> MelonDSAndroidInterface.mirrorStart(intent.getIntExtra("debug", 0))
                    "mirror-stop" -> MelonDSAndroidInterface.mirrorStop()
                    "focus" -> MelonDSAndroidInterface.mirrorFocus(intent.getIntExtra("index", 0))
                    "mirror-record" -> MelonDSAndroidInterface.mirrorRecord(dir)
                    "mirror-replay" -> MelonDSAndroidInterface.mirrorReplay(dir, intent.getStringExtra("tag") ?: "replay")
                    "mirror-recstop" -> MelonDSAndroidInterface.mirrorRecordStop()
                    "net-host" -> MelonDSAndroidInterface.netHost(intent.getIntExtra("port", 7064), intent.getIntExtra("delay", 4))
                    "net-join" -> MelonDSAndroidInterface.netJoin(intent.getStringExtra("ip") ?: "", intent.getIntExtra("port", 7064), intent.getIntExtra("delay", 4))
                    "net-stop" -> MelonDSAndroidInterface.netStop()
                }
            }
        }
        androidx.core.content.ContextCompat.registerReceiver(
            this,
            receiver,
            android.content.IntentFilter("me.magnum.melonds.NETPLAY_TEST"),
            androidx.core.content.ContextCompat.RECEIVER_EXPORTED,
        )
    }

    private fun createNotificationChannels() {
        val defaultChannel = NotificationChannelCompat.Builder(NOTIFICATION_CHANNEL_ID_BACKGROUND_TASKS, NotificationManagerCompat.IMPORTANCE_LOW)
            .setName(getString(R.string.notification_channel_background_tasks))
            .build()

        val notificationManager = NotificationManagerCompat.from(this)
        notificationManager.createNotificationChannel(defaultChannel)
    }

    @OptIn(DelicateCoroutinesApi::class)
    private fun applyTheme() {
        GlobalScope.launch(Dispatchers.Main) {
            settingsRepository.observeTheme().collect {
                AppCompatDelegate.setDefaultNightMode(it.nightMode)
            }
        }
    }

    private fun performMigrations() {
        migrator.performMigrations()
    }

    override fun onTerminate() {
        super.onTerminate()
        MelonDSAndroidInterface.cleanup()
    }

    override val workManagerConfiguration: Configuration get() {
        return Configuration.Builder()
            .setWorkerFactory(workerFactory)
            .build()
    }
}