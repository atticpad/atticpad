package net.atticpad

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.wifi.WifiManager
import android.os.Binder
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.util.Log

/**
 * The app's spine.
 *
 * docs/DESIGN.md §7.2 is explicit that this is a foreground `Service` and not an
 * `Activity`, and gives three reasons, all of which bite in practice:
 *
 *  1. It is where the `WifiLock` belongs. Android powers Wi-Fi down
 *     aggressively and the resulting latency looks exactly like a protocol
 *     bug — and a known risk on Android specifically.
 *  2. Input must not die when the overlay loses focus. A controller whose
 *     input stops because a notification shade opened is not a controller.
 *  3. It is the difference between adding a server role later (docs/DESIGN.md §6.4)
 *     and rewriting the app's spine. A server must survive the user switching
 *     to a game; an Activity-hosted loop cannot.
 *
 * Nothing about this class is a server stub. It hosts a client session and
 * only a client session. What it gets right is the *lifecycle*, which is the
 * part that would be expensive to change later, and which is the better
 * choice for the client on its own merits — the test docs/DESIGN.md §7.2 sets for
 * whether "designing with X in mind" is foresight or speculation.
 */
class AtticPadService : Service() {

    companion object {
        private const val TAG = "AtticPadService"
        private const val CHANNEL_ID = "atticpad_session"
        private const val NOTIFICATION_ID = 1

        const val ACTION_STOP = "net.atticpad.action.STOP"

        /** §5.9 / §11: the server may ask for up to 125 Hz. */
        const val DEFAULT_RATE_HZ = 60

        /**
         * How long to wait for an ANNOUNCE before giving up and sending the
         * HELLO anyway (§7 tier 3 has no ANNOUNCE at all).
         *
         * Short on purpose. On a LAN the answer is back in a millisecond or
         * two and the wait ends the moment it arrives, so this budget is only
         * ever spent in full when nothing is going to answer — and then the
         * WELCOME tells us what we wanted to know a moment later.
         */
        private const val PROBE_TIMEOUT_MS = 400

        /** §10/§11: five failures rotate the PIN, so say the number. */
        const val WRONG_PIN =
            "wrong PIN — the server allows 5 attempts before it generates a new one"
    }

    /** What the UI is allowed to see. Delivered on the main thread. */
    data class Status(
        val state: Int = AtticPadNative.STATE_IDLE,
        val sessionId: Int = 0,
        val padSlot: Int = 0,
        val rateHz: Int = 0,
        val rttMs: Int = -1,
        val txPackets: Int = 0,
        val rxPackets: Int = 0,
        val closeReason: Int = 0,
        val lastError: Int = 0,
        val message: String = "",
        val target: String = "",
        /** §6.2: the server said a pairing window is open. */
        val pairingRequired: Boolean = false,
        /** §6.4: the WELCOME carried AUTH_REQUIRED. */
        val authRequired: Boolean = false,
        /** One of AtticPadNative.AUTH_*. */
        val authState: Int = AtticPadNative.AUTH_NONE,
        /** §6.11 ERROR code, 0 if none. */
        val errorCode: Int = 0,
    ) {
        /** The UI must ask for a PIN before this address will accept us. */
        val needsSecret: Boolean
            get() = authState == AtticPadNative.AUTH_NEED_SECRET ||
                authState == AtticPadNative.AUTH_FAILED ||
                (pairingRequired && authState == AtticPadNative.AUTH_NONE)
    }

    inner class LocalBinder : Binder() {
        val service: AtticPadService get() = this@AtticPadService
    }

    /** Every input source writes here; the session thread reads it. */
    val input = InputSnapshot()

    private val binder = LocalBinder()
    private val main = Handler(Looper.getMainLooper())

    @Volatile private var listener: ((Status) -> Unit)? = null
    @Volatile var status: Status = Status(); private set

    private var thread: Thread? = null
    @Volatile private var running = false

    private var wifiLock: WifiManager.WifiLock? = null
    private var multicastLock: WifiManager.MulticastLock? = null

    // ---- lifecycle ------------------------------------------------------

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onCreate() {
        super.onCreate()
        createChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSession()
            stopSelf()
            return START_NOT_STICKY
        }
        startInForeground()
        // START_NOT_STICKY: if the system kills us, silently resurrecting a
        // controller session pointed at a machine that has since gone away is
        // worse than making the user press Connect.
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        stopSession()
        super.onDestroy()
    }

    private fun createChannel() {
        val nm = getSystemService(NotificationManager::class.java)
        val channel = NotificationChannel(
            CHANNEL_ID,
            "AtticPad session",
            // LOW: no sound, no vibration. A rumble packet must be the only
            // thing that ever makes this phone buzz.
            NotificationManager.IMPORTANCE_LOW,
        )
        channel.description = "Shown while AtticPad is acting as a controller."
        nm.createNotificationChannel(channel)
    }

    private fun buildNotification(): Notification {
        val open = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java)
                .setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE,
        )
        val stop = PendingIntent.getService(
            this, 1,
            Intent(this, AtticPadService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE,
        )
        val s = status
        val text = when (s.state) {
            AtticPadNative.STATE_ACTIVE ->
                "Pad ${s.padSlot} on ${s.target} — ${if (s.rttMs >= 0) "${s.rttMs} ms" else "…"}"
            AtticPadNative.STATE_HANDSHAKING -> "Connecting to ${s.target}…"
            else -> "Not connected"
        }
        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("AtticPad")
            .setContentText(text)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentIntent(open)
            .addAction(
                Notification.Action.Builder(null as android.graphics.drawable.Icon?, "Stop", stop)
                    .build()
            )
            .setOngoing(true)
            .build()
    }

    private fun startInForeground() {
        val n = buildNotification()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            // Android 14+ requires a declared type. `connectedDevice` is the
            // documented fit: "interactions with external devices that require
            // a ... network connection". Its manifest prerequisite is
            // satisfied by CHANGE_WIFI_STATE / CHANGE_WIFI_MULTICAST_STATE,
            // both of which this app needs anyway for the WifiLock and for
            // mDNS.
            startForeground(
                NOTIFICATION_ID, n,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE,
            )
        } else {
            startForeground(NOTIFICATION_ID, n)
        }
    }

    private fun updateNotification() {
        getSystemService(NotificationManager::class.java)
            .notify(NOTIFICATION_ID, buildNotification())
    }

    // ---- Wi-Fi power management ----------------------------------------

    /**
     * docs/DESIGN.md §7.2, first bullet of the "most likely to cost a day" list.
     * Android's Wi-Fi power save wakes the radio on a beacon interval; a 60 Hz
     * UDP stream that gets held for tens of milliseconds looks identical to
     * packet loss or a protocol stall, and there is nothing in a capture to
     * suggest the radio rather than the code.
     */
    private fun acquireLocks() {
        val wm = applicationContext.getSystemService(WifiManager::class.java) ?: return
        if (wifiLock == null) {
            val mode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                // WIFI_MODE_FULL_HIGH_PERF was deprecated at API 29 in favour
                // of LOW_LATENCY, which disables power save AND asks the
                // firmware for a low-latency mode while the app is in the
                // foreground. HIGH_PERF still works but is the weaker request
                // on a modern device.
                WifiManager.WIFI_MODE_FULL_LOW_LATENCY
            } else {
                @Suppress("DEPRECATION")
                WifiManager.WIFI_MODE_FULL_HIGH_PERF
            }
            wifiLock = wm.createWifiLock(mode, "atticpad:session").apply {
                setReferenceCounted(false)
                acquire()
            }
        }
        if (multicastLock == null) {
            // Only mDNS discovery needs this, but it is cheap and holding it
            // for the session means tier-1 rediscovery after a roam works.
            multicastLock = wm.createMulticastLock("atticpad:mdns").apply {
                setReferenceCounted(false)
                acquire()
            }
        }
    }

    private fun releaseLocks() {
        wifiLock?.let { if (it.isHeld) it.release() }
        wifiLock = null
        multicastLock?.let { if (it.isHeld) it.release() }
        multicastLock = null
    }

    // ---- the session thread --------------------------------------------

    fun setListener(l: ((Status) -> Unit)?) {
        listener = l
        l?.invoke(status)
    }

    private fun publish(s: Status) {
        status = s
        main.post { listener?.invoke(s) }
    }

    /**
     * Starts a session to [ip]:[port]. Returns immediately; progress arrives
     * through the listener.
     *
     * [secret] is §10's pairing secret if the user has one. It is passed
     * straight to the native engine and held nowhere else — not in a field,
     * not in SharedPreferences. §10 says it never appears on the wire, and
     * persisting it would be the same mistake one layer up: the whole value
     * of a 120-second window is that nothing outlives it.
     */
    fun startSession(
        ip: String,
        port: Int,
        rateHz: Int = DEFAULT_RATE_HZ,
        secret: String? = null,
    ) {
        stopSession()
        startInForeground()
        acquireLocks()
        running = true
        thread = Thread({ sessionLoop(ip, port, rateHz, secret) }, "atticpad-session").apply {
            // Above default, below audio. Input latency is the product.
            priority = Thread.MAX_PRIORITY - 1
            start()
        }
    }

    fun stopSession() {
        running = false
        thread?.let {
            it.interrupt()
            it.join(1500)
        }
        thread = null
        releaseLocks()
    }

    private fun capabilities(): Int {
        var caps = AtticPadNative.CAP_DPAD or
            AtticPadNative.CAP_FACE4 or
            AtticPadNative.CAP_SHOULDER or
            AtticPadNative.CAP_SHOULDER2 or
            AtticPadNative.CAP_TRIGGERS or
            AtticPadNative.CAP_STICK_L or
            AtticPadNative.CAP_STICK_R or
            AtticPadNative.CAP_TOUCH or
            AtticPadNative.CAP_BATTERY
        // §5.5 / §6.3: the capability mask is the ONLY way to say "this
        // hardware does not exist". A phone with no gyro must leave the bit
        // clear rather than send zeroes and hope.
        if (SensorInput.hasAccelerometer(this)) caps = caps or AtticPadNative.CAP_ACCEL
        if (SensorInput.hasGyroscope(this)) caps = caps or AtticPadNative.CAP_GYRO
        if (hasVibrator()) caps = caps or AtticPadNative.CAP_RUMBLE
        return caps
    }

    private fun sessionLoop(ip: String, port: Int, rateHz: Int, secret: String?) {
        val deviceName = "${Build.MANUFACTURER} ${Build.MODEL}".take(31)
        val handle = AtticPadNative.clientCreate(deviceName, capabilities())
        if (handle == 0L) {
            publish(Status(state = AtticPadNative.STATE_CLOSED, target = ip,
                message = "could not open a UDP socket"))
            return
        }

        val inArr = IntArray(AtticPadNative.IN_LEN)
        val outArr = IntArray(AtticPadNative.OUT_LEN)
        var lastRumbleSerial = 0
        var lastStatusSerial = 0
        var lastPublish = 0L

        try {
            if (!secret.isNullOrEmpty()) {
                val src = AtticPadNative.clientSetSecret(handle, secret)
                if (src != 0) {
                    publish(Status(state = AtticPadNative.STATE_CLOSED, target = ip,
                        lastError = src, message = "that PIN is too long (64 bytes max)"))
                    return
                }
            }

            publish(Status(state = AtticPadNative.STATE_HANDSHAKING, target = ip))

            // §8: "Pairing happens BEFORE this sequence, not inside it ...
            // ANNOUNCE.pairing_required is the signal that tells a client to
            // obtain a secret first." So ask, before spending a HELLO. When
            // the server answers that it wants a PIN and we have none, stop
            // here — a HELLO would take a pad slot for three seconds to learn
            // what the ANNOUNCE just said.
            AtticPadNative.clientProbe(handle, ip, port, PROBE_TIMEOUT_MS)
            AtticPadNative.clientStats(handle, outArr)
            val pairingRequired = outArr[AtticPadNative.OUT_PAIRING_REQUIRED] == 1
            if (pairingRequired && secret.isNullOrEmpty()) {
                publish(
                    Status(
                        state = AtticPadNative.STATE_CLOSED, target = ip,
                        pairingRequired = true,
                        authState = AtticPadNative.AUTH_NEED_SECRET,
                        message = "this server is waiting to pair — enter its PIN",
                    )
                )
                return
            }

            val rc = AtticPadNative.clientConnect(handle, ip, port, rateHz, 5000)
            if (rc != 0) {
                AtticPadNative.clientStats(handle, outArr)
                publish(
                    Status(
                        state = AtticPadNative.STATE_CLOSED, target = ip, lastError = rc,
                        pairingRequired = pairingRequired,
                        authRequired = outArr[AtticPadNative.OUT_AUTH_REQUIRED] == 1,
                        authState = outArr[AtticPadNative.OUT_AUTH_STATE],
                        errorCode = outArr[AtticPadNative.OUT_ERROR_CODE],
                        message = describeConnectFailure(
                            rc,
                            outArr[AtticPadNative.OUT_AUTH_STATE],
                            outArr[AtticPadNative.OUT_ERROR_CODE],
                        ),
                    )
                )
                return
            }

            while (running && !Thread.currentThread().isInterrupted) {
                input.readInto(inArr)
                // 20 ms cap so a server that goes quiet still lets this loop
                // notice `running` went false within one frame of the user
                // pressing Disconnect.
                val state = AtticPadNative.clientPump(handle, inArr, 20)
                AtticPadNative.clientStats(handle, outArr)

                val rs = outArr[AtticPadNative.OUT_RUMBLE_SERIAL]
                if (rs != lastRumbleSerial) {
                    lastRumbleSerial = rs
                    vibrate(
                        outArr[AtticPadNative.OUT_RUMBLE_LOW],
                        outArr[AtticPadNative.OUT_RUMBLE_HIGH],
                        outArr[AtticPadNative.OUT_RUMBLE_MS],
                    )
                }

                val now = System.currentTimeMillis()
                val ss = outArr[AtticPadNative.OUT_STATUS_SERIAL]
                if (now - lastPublish >= 250 || ss != lastStatusSerial || state != status.state) {
                    lastStatusSerial = ss
                    lastPublish = now
                    val s = Status(
                        state = state,
                        sessionId = outArr[AtticPadNative.OUT_SESSION_ID],
                        padSlot = outArr[AtticPadNative.OUT_PAD_SLOT],
                        rateHz = outArr[AtticPadNative.OUT_RATE_HZ],
                        rttMs = outArr[AtticPadNative.OUT_RTT_MS],
                        txPackets = outArr[AtticPadNative.OUT_TX],
                        rxPackets = outArr[AtticPadNative.OUT_RX],
                        closeReason = outArr[AtticPadNative.OUT_CLOSE_REASON],
                        lastError = outArr[AtticPadNative.OUT_LAST_ERROR],
                        message = AtticPadNative.clientMessage(handle),
                        target = ip,
                        pairingRequired = outArr[AtticPadNative.OUT_PAIRING_REQUIRED] == 1,
                        authRequired = outArr[AtticPadNative.OUT_AUTH_REQUIRED] == 1,
                        authState = outArr[AtticPadNative.OUT_AUTH_STATE],
                        errorCode = outArr[AtticPadNative.OUT_ERROR_CODE],
                    )
                    publish(s)
                    main.post { updateNotification() }
                }

                if (state != AtticPadNative.STATE_ACTIVE) break
            }
        } catch (t: Throwable) {
            Log.e(TAG, "session thread died", t)
            publish(Status(state = AtticPadNative.STATE_CLOSED, target = ip,
                message = t.message ?: t.javaClass.simpleName))
        } finally {
            AtticPadNative.clientStats(handle, outArr)
            val closeReason = outArr[AtticPadNative.OUT_CLOSE_REASON]
            val authState = outArr[AtticPadNative.OUT_AUTH_STATE]
            val errorCode = outArr[AtticPadNative.OUT_ERROR_CODE]
            AtticPadNative.clientDestroy(handle)
            releaseLocks()
            if (status.state != AtticPadNative.STATE_CLOSED) {
                publish(
                    status.copy(
                        state = AtticPadNative.STATE_CLOSED,
                        closeReason = closeReason,
                        authState = authState,
                        errorCode = errorCode,
                        // A session that was authenticated and then died
                        // mid-flight is a dropped PIN as far as the user is
                        // concerned, and the engine has already worked out
                        // which of those two it was.
                        message = if (authState == AtticPadNative.AUTH_FAILED)
                            WRONG_PIN else status.message,
                    )
                )
            }
            main.post { updateNotification() }
        }
    }

    private fun describeConnectFailure(rc: Int, authState: Int, errorCode: Int): String = when {
        // §10/§11: five failed attempts invalidate the PIN and the server
        // generates a new one, so "try again" is only true up to a point and
        // the number is worth saying out loud.
        authState == AtticPadNative.AUTH_FAILED ||
            errorCode == AtticPadNative.ERRC_AUTH_FAILED -> WRONG_PIN
        authState == AtticPadNative.AUTH_NEED_SECRET ->
            "this server is waiting to pair — enter its PIN"
        errorCode == AtticPadNative.ERRC_TOO_MANY_TRIES ->
            "too many attempts — the server has generated a new PIN"
        errorCode == AtticPadNative.ERRC_PAIRING_CLOSED ->
            "the pairing window closed — open a new one on the server"
        errorCode == AtticPadNative.ERRC_NO_FREE_SLOT -> "the server has no free pad slot"
        rc == -1 -> "that is not a valid IPv4 address"
        // The engine reports a handshake that ran out of §9 retransmits as
        // APAD_ERR_STATE. On this LAN that nearly always means nothing is
        // listening, not that the protocol went wrong.
        rc == -9 -> "no WELCOME — is the server running on this address and port?"
        else -> "handshake failed (code $rc)"
    }

    // ---- rumble ---------------------------------------------------------

    private fun vibratorOrNull(): Vibrator? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            getSystemService(VibratorManager::class.java)?.defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            getSystemService(Vibrator::class.java)
        }

    private fun hasVibrator(): Boolean = vibratorOrNull()?.hasVibrator() == true

    /**
     * §6.7 RUMBLE carries two 16-bit motor magnitudes. A phone has one
     * actuator and no notion of low/high frequency motors, so the two collapse
     * to a single amplitude. Taking the max rather than the mean keeps a
     * single-motor effect from arriving at half strength.
     */
    private fun vibrate(low: Int, high: Int, durationMs: Int) {
        val v = vibratorOrNull() ?: return
        if (!v.hasVibrator()) return
        val magnitude = maxOf(low, high)
        if (magnitude <= 0 || durationMs <= 0) {
            v.cancel()
            return
        }
        val amplitude = (magnitude * 255 / 65535).coerceIn(1, 255)
        // §6.7 allows duration 0 for "until superseded"; a phone cannot buzz
        // indefinitely without draining, so an unbounded effect is capped.
        val ms = if (durationMs == 0) 250L else durationMs.toLong().coerceAtMost(5000L)
        v.vibrate(VibrationEffect.createOneShot(ms, amplitude))
    }
}
