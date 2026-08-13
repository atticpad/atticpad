package net.atticpad

import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.graphics.Matrix
import android.graphics.RectF
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraAccessException
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.media.ImageReader
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.util.Log
import android.util.Size
import android.view.Surface
import android.view.TextureView
import android.view.WindowManager
import kotlin.math.max

/**
 * §10.3 in-app QR scanning: Camera2 in, `AtticPadNative.qrDecodeFrame` out.
 *
 * WHAT THIS CLASS DOES NOT DO: parse a URI, or understand a QR bitstream.
 * Every frame's Y plane crosses the JNI boundary as bytes and comes back as
 * either `{ip, port, secret}` or "nothing readable in this frame" — see
 * `cpp/apad_qr.c`. This class is camera plumbing, nothing else.
 *
 * NOT the only door: a deep link (`atticpad://…`, handled in
 * [MainActivity]) reaches the exact same native parser without ever asking
 * for the CAMERA permission, and manual entry (§7 tier 3) always remains.
 * This is the third way in, requested only when the user taps "Scan QR
 * code" — docs/CONVENTIONS.md's "requested only when the user chooses to scan."
 */
class QrScanner(private val context: Context) {

    companion object {
        private const val TAG = "AtticPadQrScanner"

        fun hasCamera(context: Context): Boolean =
            context.packageManager.hasSystemFeature(PackageManager.FEATURE_CAMERA_ANY)
    }

    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var imageReader: ImageReader? = null
    private var bgThread: HandlerThread? = null
    private var bgHandler: Handler? = null
    private var qrHandle = 0L

    /**
     * What [updatePreviewTransform] needs to know to fill the `TextureView`
     * correctly, captured once in [start] from the SAME `CameraCharacteristics`
     * [pickSize] already reads — nothing here touches a second query the
     * decode path depends on. [bufferSize] is in the SENSOR's own coordinate
     * frame (typically landscape, width > height, regardless of how the
     * phone is held); [sensorOrientationDegrees] is how far that frame is
     * mounted clockwise from the device's natural orientation.
     */
    private var bufferSize: Size? = null
    private var sensorOrientationDegrees = 0
    private var facingFront = false

    /** Whether the CURRENTLY OPEN camera has a flash unit at all — set once
     *  per [start], from the same `CameraCharacteristics` query [pickSize]
     *  already makes. The connect screen's torch toggle reads this to hide
     *  itself entirely on a device/camera with no flash, rather than
     *  offering a control that can only ever fail. */
    var hasFlash = false
        private set

    /** The repeating request's own builder, kept so [setTorch] can flip
     *  `FLASH_MODE` and resubmit without tearing down the session — torch
     *  control on an ALREADY-OPEN camera is a capture-request property, not
     *  `CameraManager.setTorchMode()` (that path is for a closed camera,
     *  e.g. a standalone flashlight toggle, and can throw
     *  `CameraAccessException` against a device this class already has
     *  open). */
    private var requestBuilder: CaptureRequest.Builder? = null
    private var captureHandler: Handler? = null

    /** Guards against a frame that lands after a result has already been
     *  delivered (or after [stop]) — `qrHandle` would otherwise be reused. */
    @Volatile private var delivered = false

    /**
     * Opens the camera and starts feeding frames to the native decoder.
     * `surfaceTexture` backs the on-screen preview; decoding runs against a
     * second, smaller target Camera2 fills concurrently, so the preview is
     * never held back waiting for `quirc`.
     *
     * `onResult` and `onError` are called on the MAIN thread, exactly once
     * between a `start()`/`stop()` pair — this class stops itself the
     * moment either fires.
     *
     * §10.3: the secret [onResult] carries is exactly as sensitive as a
     * typed PIN. The caller's job is to hand it straight to
     * `AtticPadNative.clientSetSecret()` and hold it nowhere else.
     */
    @SuppressLint("MissingPermission") // caller has already checked CAMERA
    fun start(
        surfaceTexture: SurfaceTexture,
        onResult: (ip: String, port: Int, secret: String) -> Unit,
        onError: (String) -> Unit,
    ) {
        stop()
        delivered = false

        val manager = context.getSystemService(CameraManager::class.java)
        if (manager == null) {
            onError("no camera service on this device")
            return
        }
        val cameraId = pickCamera(manager)
        if (cameraId == null) {
            onError("no usable camera")
            return
        }
        qrHandle = AtticPadNative.qrCreate()
        if (qrHandle == 0L) {
            onError("could not start the QR decoder")
            return
        }

        val thread = HandlerThread("atticpad-qr").also { it.start() }
        bgThread = thread
        val handler = Handler(thread.looper)
        bgHandler = handler

        val characteristics = manager.getCameraCharacteristics(cameraId)
        val map = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        val size = pickSize(map) ?: Size(640, 480)
        // For updatePreviewTransform() — the HUMAN-facing preview only.
        // apad_qr.c / quirc still read image.width/height/rowStride straight
        // off the ImageReader frame in onFrame() below, completely
        // unaffected by any of this.
        bufferSize = size
        sensorOrientationDegrees = characteristics.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0
        facingFront = characteristics.get(CameraCharacteristics.LENS_FACING) ==
            CameraCharacteristics.LENS_FACING_FRONT
        hasFlash = characteristics.get(CameraCharacteristics.FLASH_INFO_AVAILABLE) ?: false

        val reader = ImageReader.newInstance(size.width, size.height, ImageFormat.YUV_420_888, 2)
        imageReader = reader
        reader.setOnImageAvailableListener(
            { r -> onFrame(r, onResult) },
            handler,
        )

        surfaceTexture.setDefaultBufferSize(size.width, size.height)
        val previewSurface = Surface(surfaceTexture)

        try {
            manager.openCamera(cameraId, object : CameraDevice.StateCallback() {
                override fun onOpened(device: CameraDevice) {
                    cameraDevice = device
                    openSession(device, previewSurface, reader.surface, handler, onError)
                }

                override fun onDisconnected(device: CameraDevice) {
                    device.close()
                    cameraDevice = null
                }

                override fun onError(device: CameraDevice, error: Int) {
                    device.close()
                    cameraDevice = null
                    Log.w(TAG, "camera onError: $error")
                    postError(onError, "camera error ($error)")
                }
            }, handler)
        } catch (e: CameraAccessException) {
            onError(e.message ?: "camera access denied")
        }
    }

    private fun openSession(
        device: CameraDevice,
        previewSurface: Surface,
        readerSurface: Surface,
        handler: Handler,
        onError: (String) -> Unit,
    ) {
        try {
            val builder = device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW)
            builder.addTarget(previewSurface)
            builder.addTarget(readerSurface)
            requestBuilder = builder
            captureHandler = handler

            val callback = object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    captureSession = session
                    try {
                        session.setRepeatingRequest(builder.build(), null, handler)
                    } catch (e: CameraAccessException) {
                        postError(onError, e.message ?: "could not start the camera preview")
                    }
                }

                override fun onConfigureFailed(session: CameraCaptureSession) {
                    postError(onError, "could not configure the camera")
                }
            }

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                // The (List<Surface>, StateCallback, Handler) overload was
                // deprecated at API 28 in favour of SessionConfiguration.
                // Behaviourally identical for this simple a session (two
                // fixed OutputConfigurations, SESSION_REGULAR), so this is
                // purely "stop calling the deprecated form", the same reason
                // AtticPadNsd.kt version-gates NsdManager.resolveService.
                val outputs = listOf(
                    OutputConfiguration(previewSurface),
                    OutputConfiguration(readerSurface),
                )
                device.createCaptureSession(
                    SessionConfiguration(
                        SessionConfiguration.SESSION_REGULAR,
                        outputs,
                        { r -> handler.post(r) },
                        callback,
                    ),
                )
            } else {
                @Suppress("DEPRECATION")
                device.createCaptureSession(
                    listOf(previewSurface, readerSurface), callback, handler,
                )
            }
        } catch (e: CameraAccessException) {
            postError(onError, e.message ?: "camera access denied")
        }
    }

    /** One Y plane in, at most one result out — see cpp/apad_qr.c for what
     *  the three non-success outcomes mean. Only APAD_OK is worth acting on
     *  here: the other three are the ordinary "nothing usable in THIS
     *  frame" case at up to the camera's frame rate and would otherwise
     *  spam the UI many times a second while the user aims the camera. */
    private fun onFrame(
        reader: ImageReader,
        onResult: (String, Int, String) -> Unit,
    ) {
        val image = try {
            reader.acquireLatestImage()
        } catch (e: IllegalStateException) {
            null
        } ?: return
        try {
            if (delivered) return
            val plane = image.planes[0]
            val buffer = plane.buffer
            val bytes = ByteArray(buffer.remaining())
            buffer.get(bytes)

            val rc = IntArray(1)
            val result = AtticPadNative.qrDecodeFrame(
                qrHandle, bytes, image.width, image.height, plane.rowStride, rc,
            )
            if (result != null && rc[0] == 0 && !delivered) {
                delivered = true
                val ip = result[0]
                val port = result[1].toIntOrNull() ?: AtticPadNative.defaultPort()
                val secret = result[2]
                Handler(Looper.getMainLooper()).post { onResult(ip, port, secret) }
            }
        } finally {
            image.close()
        }
    }

    private fun postError(onError: (String) -> Unit, message: String) {
        Handler(Looper.getMainLooper()).post { onError(message) }
    }

    /**
     * Flips the torch on the currently open camera on/off, resubmitting the
     * SAME repeating request with `FLASH_MODE` changed — this is what the
     * connect screen's floating torch toggle calls. No-op (returns `false`)
     * before [start] has opened a session, or on a camera [hasFlash] says
     * has none: the caller is expected to have hidden the control in that
     * case, but this stays defensive rather than throwing.
     */
    fun setTorch(on: Boolean): Boolean {
        val builder = requestBuilder ?: return false
        val session = captureSession ?: return false
        val handler = captureHandler ?: return false
        if (!hasFlash) return false
        return try {
            builder.set(
                CaptureRequest.FLASH_MODE,
                if (on) CaptureRequest.FLASH_MODE_TORCH else CaptureRequest.FLASH_MODE_OFF,
            )
            session.setRepeatingRequest(builder.build(), null, handler)
            true
        } catch (_: CameraAccessException) {
            false
        } catch (_: IllegalStateException) {
            false
        }
    }

    /** Closes the camera and frees the native decoder. Safe to call more
     *  than once, and safe to call when nothing was ever started. */
    fun stop() {
        delivered = true // stop acting on any frame still in flight
        try {
            captureSession?.stopRepeating()
        } catch (_: CameraAccessException) {
        } catch (_: IllegalStateException) {
        }
        captureSession?.close()
        captureSession = null
        cameraDevice?.close()
        cameraDevice = null
        imageReader?.close()
        imageReader = null
        if (qrHandle != 0L) {
            AtticPadNative.qrDestroy(qrHandle)
            qrHandle = 0L
        }
        bgThread?.quitSafely()
        bgThread = null
        bgHandler = null
        bufferSize = null
        requestBuilder = null
        captureHandler = null
        hasFlash = false
    }

    /**
     * Fills [view] with the live preview: correct aspect ratio (centre-crop,
     * never stretched — a distorted preview both misleads whoever is aiming
     * it and hands `quirc` a distorted image) and correct upright
     * orientation, given the sensor's mounting angle and the CURRENT display
     * rotation. A no-op before [start] has run (nothing to size against yet)
     * — safe to call from a layout listener that fires before that.
     *
     * Call this whenever [view]'s own bounds change, not just once: a
     * rotation while the scanner is open does not reopen the camera (the
     * Activity survives it — `configChanges` — so [bufferSize] never
     * changes), but the TextureView's on-screen size DOES change, and the
     * matrix computed for the old size is wrong for the new one. The
     * caller wires this to `View.addOnLayoutChangeListener`, which is what
     * actually catches that — `onSurfaceTextureSizeChanged` does NOT fire
     * for it, only for a buffer-size change this class never makes.
     */
    fun updatePreviewTransform(view: TextureView) {
        val buffer = bufferSize ?: return
        val viewWidth = view.width
        val viewHeight = view.height
        if (viewWidth == 0 || viewHeight == 0) return

        val displayDegrees = when (displayRotation()) {
            Surface.ROTATION_90 -> 90
            Surface.ROTATION_180 -> 180
            Surface.ROTATION_270 -> 270
            else -> 0
        }

        // ---- DERIVATION (read this before touching any of the numbers
        //      below — this is the THIRD bug on this exact transform, and
        //      the first two both "passed" on the AVD; see
        //      hud_qr_fixes.md / icon_and_selftest_fixes.md in agent memory) ----
        //
        // Two Android-documented facts, both confirmed verbatim against
        // Google's own current docs, not recalled from memory:
        //
        //   1. developer.android.com/media/camera/camera2/camera-preview:
        //      "TextureView rotates the sensor image buffer based on sensor
        //      orientation but does not handle device rotation or preview
        //      scaling."
        //   2. developer.android.com/codelabs/android-camera2-preview §6:
        //      "By default, the TextureView already compensates for the
        //      camera orientation, but it does not handle the display
        //      rotation... This can be solved by simply rotating the
        //      target SurfaceTexture" — and the fix shown is
        //      `matrix.postRotate(-surfaceRotationDegrees)`, with NO
        //      `sensorOrientationDegrees` term anywhere in the angle passed
        //      to postRotate. `sensorOrientationDegrees` is used ONLY to
        //      decide whether width/height need swapping for the SCALE
        //      step (see below), never as part of the rotation itself.
        //
        // In other words: for a Camera2 preview stream whose target is a
        // TextureView's SurfaceTexture (this class's case, `previewSurface`
        // in start()) — as opposed to an ImageReader target, which is what
        // onFrame()/quirc reads and is NOT auto-rotated, hence that path's
        // untouched raw image.width/height/rowStride — the platform ALREADY
        // rotates the buffer by sensorOrientationDegrees before TextureView
        // ever sees it, so it is already upright for the device's NATURAL
        // orientation. The only rotation left for US to apply is how far
        // the CURRENT display orientation differs from natural — i.e.
        // -displayDegrees. Applying sensorOrientationDegrees again on top,
        // which the previous version of this function did, double-counts
        // the platform's own compensation.
        //
        // Why this "passed" on the emulator and only failed on a real device
        // Pro: NOT because of a difference in SENSOR_ORIENTATION value — the
        // `atticpad-test` AVD's back camera measured SENSOR_ORIENTATION=90
        // via the log line below (confirmed on this rig, not assumed), the
        // same as the overwhelming majority of real phones, so the OLD
        // (buggy) formula was adding the same wrong extra 90 degrees on the
        // AVD as it would on a real device. It went undetected there for a
        // more mundane reason: every prior verification pass on this
        // function (hud_qr_fixes.md, icon_and_selftest_fixes.md) checked
        // for LETTERBOXING — dark bands at the edges, a SCALE bug — by
        // confirming the AVD's synthetic colour-bar/checkerboard test
        // pattern filled the view edge to edge. A wrong ROTATION does not
        // cause letterboxing (the scale math still covers the view either
        // way) and that synthetic pattern has no "this edge is up" cue a
        // screenshot reviewer can check against — a 90-degrees-off frame
        // and a correct one both just look like "an abstract pattern that
        // fills the square." This is the real substance of "the emulator's
        // virtual camera is not representative": not a specific wrong
        // number, but the absence of ground truth to catch a wrong one
        // against. Only a real QR code (which fails to DECODE at all if
        // held sideways relative to how quirc scans it) or a human looking
        // at a live preview of themselves/their room can actually catch
        // this class of bug — which is exactly why this is reasoned and
        // logged, not claimed as AVD-verified.
        //
        // Front-camera mirroring: neither doc above claims TextureView
        // auto-mirrors a front camera's buffer (unlike the legacy Camera1
        // `setDisplayOrientation` path, which did) — Camera2 never mirrors
        // preview output for you. AtticPad always prefers the back camera
        // (pickCamera) and only falls back to front on a back-camera-less
        // device, but for that fallback to look like an ordinary "selfie"
        // preview rather than a text-reads-backwards mirror image, this
        // applies its own horizontal flip, as the LAST op in the chain so
        // it mirrors the final, already-upright-and-scaled image rather
        // than interacting with the rotation math above. This has no
        // bearing on decode correctness either way — onFrame() reads the
        // ImageReader target directly, never this TextureView.
        val rotation = (360 - displayDegrees) % 360

        // sensorOrientationDegrees is ONLY used here, to know whether the
        // buffer's APPARENT (already-platform-rotated-to-natural-
        // orientation) aspect ratio has width and height swapped relative
        // to the RAW buffer.width x buffer.height this class queried from
        // the StreamConfigurationMap (see start()) — which is the same raw,
        // sensor-native size ImageReader receives, unrotated. On the
        // ordinary phone case (sensorOrientationDegrees 90 or 270) that
        // raw size is landscape (width > height) even though the phone is
        // held in natural portrait, and the platform's own rotation makes
        // it APPEAR portrait by the time TextureView's default (identity
        // transform) content reaches us — so the aspect-ratio rectangle
        // this function reasons about from here on must be the SWAPPED
        // (apparent) one, not the raw one.
        val sensorSwapped = sensorOrientationDegrees == 90 || sensorOrientationDegrees == 270
        val apparentBufferWidth = if (sensorSwapped) buffer.height else buffer.width
        val apparentBufferHeight = if (sensorSwapped) buffer.width else buffer.height

        val matrix = Matrix()
        val viewRect = RectF(0f, 0f, viewWidth.toFloat(), viewHeight.toFloat())
        // The buffer's APPARENT (natural-orientation, platform-rotated)
        // width/height, NOT swapped again for `rotation` here — the
        // postRotate() below is what reorients this footprint for the
        // CURRENT display orientation, and it happens LAST. Swapping here
        // TOO would double-count that rotation's effect on which axis ends
        // up filled, the exact "double-rotation trap" this function was
        // fixed for once before (see hud_qr_fixes.md) — that fix is still
        // correct, it is just operating on the apparent size now instead of
        // the raw one.
        val bufferRect = RectF(0f, 0f, apparentBufferWidth.toFloat(), apparentBufferHeight.toFloat())
        val centerX = viewRect.centerX()
        val centerY = viewRect.centerY()
        bufferRect.offset(centerX - bufferRect.centerX(), centerY - bufferRect.centerY())
        // Re-expresses the view's own bounds in the buffer's (apparent)
        // aspect ratio, centred — undoes TextureView's default non-uniform
        // stretch-to-fill and leaves a correctly-proportioned but
        // not-yet-cropped image.
        matrix.setRectToRect(viewRect, bufferRect, Matrix.ScaleToFit.FILL)
        // Centre-crop: scale by whichever axis needs MORE enlargement so the
        // image covers the view on both axes, cropping the excess on the
        // other — never letterboxed, never squashed. Works for ANY view
        // aspect, including a square one (see icon_and_selftest_fixes.md —
        // the axis-swap bug on this same step only showed up for a view
        // aspect nobody had screenshotted yet; a square view is a NEW such
        // aspect and is exercised by the same max()-of-two-ratios formula
        // with no special case needed).
        //
        // The two axis ratios have to be paired against the buffer's
        // dimensions AS THEY WILL LAND ON SCREEN, i.e. swapped again
        // whenever `rotation` (our OWN remaining postRotate, now that the
        // sensor part is already baked into `apparentBufferWidth/Height`
        // above) is 90 or 270 — postRotate() below performs that swap.
        val displayRotationSwapped = rotation == 90 || rotation == 270
        val coverWidth = if (displayRotationSwapped) bufferRect.height() else bufferRect.width()
        val coverHeight = if (displayRotationSwapped) bufferRect.width() else bufferRect.height()
        val scale = max(
            viewWidth.toFloat() / coverWidth,
            viewHeight.toFloat() / coverHeight,
        )
        matrix.postScale(scale, scale, centerX, centerY)
        matrix.postRotate(rotation.toFloat(), centerX, centerY)
        // Front camera only: Camera2 never auto-mirrors preview output (see
        // derivation above) — flip horizontally about the view's own centre
        // so a front-facing fallback reads as an ordinary mirror, applied
        // last so it mirrors the final upright image rather than the raw
        // sensor-space one.
        if (facingFront) {
            matrix.postScale(-1f, 1f, centerX, centerY)
        }
        // Once per transform computation, at INFO under a stable tag — every
        // input the derivation above depends on, plus the two outputs
        // (`rotation`, `scale`), so a real-device report is `adb logcat -s
        // AtticPadQrScanner` away from being a data-driven fix rather than a
        // fourth guess if this is STILL wrong on a given handset.
        Log.i(
            TAG,
            "transform sensorOrientation=$sensorOrientationDegrees facingFront=$facingFront " +
                "displayDegrees=$displayDegrees buffer=${buffer.width}x${buffer.height} " +
                "view=${viewWidth}x$viewHeight rotation=$rotation scale=$scale",
        )
        view.setTransform(matrix)
    }

    private fun displayRotation(): Int {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            context.display?.let { return it.rotation }
        }
        @Suppress("DEPRECATION")
        val wm = context.getSystemService(WindowManager::class.java)
        @Suppress("DEPRECATION")
        return wm?.defaultDisplay?.rotation ?: Surface.ROTATION_0
    }

    private fun pickCamera(manager: CameraManager): String? {
        val ids = try {
            manager.cameraIdList
        } catch (e: CameraAccessException) {
            return null
        }
        // Prefer a back-facing camera — the ordinary way to scan something
        // in front of you — but any camera at all beats none, on a device
        // whose only camera is unusual (e.g. front-only).
        val back = ids.firstOrNull {
            manager.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) ==
                CameraCharacteristics.LENS_FACING_BACK
        }
        return back ?: ids.firstOrNull()
    }

    /** A modest resolution: `quirc` decodes a whole frame on the CPU per
     *  callback, and a QR code carrying a §10.3 URI (well under 128 bytes)
     *  needs nowhere near the sensor's native resolution to read cleanly.
     *  Picks the smallest available size that is at least 640 wide, or the
     *  smallest available size at all if none reaches that. */
    private fun pickSize(map: android.hardware.camera2.params.StreamConfigurationMap?): Size? {
        val sizes = map?.getOutputSizes(ImageFormat.YUV_420_888)?.toList() ?: return null
        if (sizes.isEmpty()) return null
        return sizes.filter { it.width >= 640 }.minByOrNull { it.width.toLong() * it.height }
            ?: sizes.minByOrNull { it.width.toLong() * it.height }
    }
}
