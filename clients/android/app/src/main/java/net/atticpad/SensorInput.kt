package net.atticpad

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager

/**
 * Accelerometer and gyroscope into the §5 units.
 *
 * docs/DESIGN.md §7.2: "Gyro via SensorManager at SENSOR_DELAY_GAME." That is ~20 ms,
 * which is slower than the 60 Hz input rate — the newest sample is repeated in
 * the frames between, which is correct: the wire carries a state, not a
 * stream of deltas.
 */
class SensorInput(context: Context, private val snapshot: InputSnapshot) :
    SensorEventListener {

    companion object {
        /** m/s² to milli-g (§5: accel is milli-g, X Y Z). */
        private const val MILLI_G_PER_MS2 = 1000.0f / SensorManager.GRAVITY_EARTH

        /** rad/s to deci-degrees/second (§5: gyro is deci-deg/s). */
        private const val DECI_DEG_PER_RAD = 572.9578f   // (180/pi) * 10

        fun hasAccelerometer(context: Context): Boolean =
            context.getSystemService(SensorManager::class.java)
                ?.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) != null

        fun hasGyroscope(context: Context): Boolean =
            context.getSystemService(SensorManager::class.java)
                ?.getDefaultSensor(Sensor.TYPE_GYROSCOPE) != null
    }

    private val manager = context.getSystemService(SensorManager::class.java)
    private val accelerometer = manager?.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
    private val gyroscope = manager?.getDefaultSensor(Sensor.TYPE_GYROSCOPE)

    fun start() {
        val m = manager ?: return
        accelerometer?.let { m.registerListener(this, it, SensorManager.SENSOR_DELAY_GAME) }
        gyroscope?.let { m.registerListener(this, it, SensorManager.SENSOR_DELAY_GAME) }
    }

    fun stop() {
        manager?.unregisterListener(this)
        snapshot.setAccel(0, 0, 0)
        snapshot.setGyro(0, 0, 0)
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit

    override fun onSensorChanged(event: SensorEvent) {
        when (event.sensor.type) {
            Sensor.TYPE_ACCELEROMETER -> snapshot.setAccel(
                clamp(event.values[0] * MILLI_G_PER_MS2),
                clamp(event.values[1] * MILLI_G_PER_MS2),
                clamp(event.values[2] * MILLI_G_PER_MS2),
            )

            Sensor.TYPE_GYROSCOPE -> {
                // AXIS MAPPING — UNVERIFIED, see the report for M3.
                //
                // Android's sensor frame is defined against the device in its
                // NATURAL orientation: +X right, +Y up the screen, +Z out of
                // it, with values[0..2] the rotation rate about those axes.
                // §5 asks for pitch, roll, yaw.
                //
                // This app runs landscape, and for a phone held in landscape
                // the natural-portrait X axis points up-screen and Y points
                // left. So the rotation the player experiences as pitch
                // (nose up/down) is rotation about the axis running through
                // the long edge — values[1] in landscape, not values[0].
                //
                // That reasoning has NOT been checked against a device. The
                // 3DS client's gyro constants are flagged the same way --
                // 600 °/s and 1.5 are invented numbers -- and this
                // is the same class of guess: it needs someone aiming at
                // something to settle.
                snapshot.setGyro(
                    clamp(event.values[1] * DECI_DEG_PER_RAD),   // pitch
                    clamp(event.values[0] * DECI_DEG_PER_RAD),   // roll
                    clamp(event.values[2] * DECI_DEG_PER_RAD),   // yaw
                )
            }
        }
    }

    private fun clamp(v: Float): Int = v.toInt().coerceIn(-32768, 32767)
}
