package net.atticpad

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Build
import android.util.Log
import java.net.Inet4Address

/**
 * Tier-1 discovery (docs/PROTOCOL.md §7): mDNS browsing for
 * `_atticpad._udp.local`.
 *
 * DELIBERATELY ROLE-NEUTRAL, per docs/DESIGN.md §7.2: this wraps *NSD*, not "finding
 * servers". It owns the NsdManager and the service-type constant, and browsing
 * is one operation on it. If docs/DESIGN.md §6.4's Android-server role ever clears
 * its spike, advertising is an added method here — not an unwrapping of a
 * class whose shape assumed one direction. There is no advertise() today
 * because nothing calls one, and an interface stubbed out for a caller that
 * does not exist yet is a liability, not preparation.
 *
 * Tier 1 is a convenience, never the contract. §7: "A client implements
 * whichever it can, and MUST implement tier 3." On this project's own
 * development LAN the AP isolates clients and broadcast discovery does not
 * work at all, and over a VPN there is no multicast whatsoever —
 * so manual IP entry is the path that is always there, and the UI treats it
 * that way.
 */
class AtticPadNsd(context: Context) {

    companion object {
        private const val TAG = "AtticPadNsd"

        /** §7. NsdManager wants the trailing dot-less form with the domain. */
        const val SERVICE_TYPE = "_atticpad._udp."
    }

    data class Found(val name: String, val host: String, val port: Int)

    private val nsd = context.applicationContext.getSystemService(NsdManager::class.java)
    private var discoveryListener: NsdManager.DiscoveryListener? = null
    private val seen = LinkedHashMap<String, Found>()

    val isAvailable: Boolean get() = nsd != null

    fun startBrowse(onChanged: (List<Found>) -> Unit) {
        val manager = nsd ?: return
        stopBrowse()
        seen.clear()
        onChanged(emptyList())

        val listener = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String) = Unit

            override fun onServiceFound(info: NsdServiceInfo) {
                resolve(manager, info) { found ->
                    seen[found.name] = found
                    onChanged(seen.values.toList())
                }
            }

            override fun onServiceLost(info: NsdServiceInfo) {
                if (seen.remove(info.serviceName) != null) {
                    onChanged(seen.values.toList())
                }
            }

            override fun onDiscoveryStopped(serviceType: String) = Unit

            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) {
                // §7: "If the 5353 bind fails, tier 1 is disabled and the
                // server UI MUST say so." The same honesty applies here — the
                // UI must not sit on an empty list looking like it is still
                // searching.
                Log.w(TAG, "mDNS discovery unavailable, error $errorCode")
                onChanged(emptyList())
            }

            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) = Unit
        }

        discoveryListener = listener
        manager.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, listener)
    }

    fun stopBrowse() {
        val manager = nsd ?: return
        discoveryListener?.let {
            try {
                manager.stopServiceDiscovery(it)
            } catch (_: IllegalArgumentException) {
                // Already stopped; NsdManager throws rather than no-oping.
            }
        }
        discoveryListener = null
    }

    private fun resolve(
        manager: NsdManager,
        info: NsdServiceInfo,
        onResolved: (Found) -> Unit,
    ) {
        if (Build.VERSION.SDK_INT >= 34) {
            // resolveService was deprecated at API 34 in favour of a callback
            // that also delivers later attribute changes.
            val cb = object : NsdManager.ServiceInfoCallback {
                override fun onServiceInfoCallbackRegistrationFailed(errorCode: Int) = Unit
                override fun onServiceUpdated(updated: NsdServiceInfo) {
                    toFound(updated)?.let(onResolved)
                    try {
                        manager.unregisterServiceInfoCallback(this)
                    } catch (_: IllegalArgumentException) {
                    }
                }

                override fun onServiceLost() = Unit
                override fun onServiceInfoCallbackUnregistered() = Unit
            }
            manager.registerServiceInfoCallback(info, { it.run() }, cb)
        } else {
            @Suppress("DEPRECATION")
            manager.resolveService(info, object : NsdManager.ResolveListener {
                override fun onResolveFailed(si: NsdServiceInfo, errorCode: Int) {
                    Log.w(TAG, "resolve failed for ${si.serviceName}: $errorCode")
                }

                override fun onServiceResolved(si: NsdServiceInfo) {
                    toFound(si)?.let(onResolved)
                }
            })
        }
    }

    /**
     * IPv4 only, matching the shim: `apad_addr` is four octets and the DS has
     * no IPv6 stack at all (docs/DESIGN.md §3). A server that only publishes a v6
     * address is not reachable by this protocol, so it is dropped here rather
     * than offered and then failing at connect time.
     */
    private fun toFound(info: NsdServiceInfo): Found? {
        val port = info.port
        if (port <= 0) return null

        val v4: String? = if (Build.VERSION.SDK_INT >= 34) {
            info.hostAddresses.filterIsInstance<Inet4Address>()
                .firstOrNull()?.hostAddress
        } else {
            @Suppress("DEPRECATION")
            (info.host as? Inet4Address)?.hostAddress
        }
        return v4?.let { Found(info.serviceName ?: it, it, port) }
    }
}
