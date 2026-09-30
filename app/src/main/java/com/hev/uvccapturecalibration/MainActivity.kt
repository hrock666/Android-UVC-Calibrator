package com.hev.uvccapturecalibration

import android.app.PendingIntent
import android.Manifest
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.res.ColorStateList
import android.graphics.Bitmap
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.content.pm.PackageManager
import android.view.View
import android.widget.ArrayAdapter
import androidx.activity.result.contract.ActivityResultContracts
import androidx.annotation.Keep
import androidx.core.content.ContextCompat
import androidx.appcompat.app.AppCompatActivity
import com.hev.uvccapturecalibration.databinding.ActivityMainBinding
import org.json.JSONObject
import java.io.FileOutputStream
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var usbManager: UsbManager
    private var pendingDevice: UsbDevice? = null
    private var usbConnection: UsbDeviceConnection? = null
    private var sessionDeviceId: Int? = null
    private var pendingProfileJson: String? = null
    private var pendingOneTapCalibration = false
    @Volatile private var progressStageBase = 0
    @Volatile private var progressStageSpan = 100
    @Volatile private var progressStageLabel = ""
    private var calibrationStatus = CalibrationStatus.IDLE
    private val previewHandler = Handler(Looper.getMainLooper())
    private val previewExecutor = Executors.newSingleThreadExecutor()
    private val previewRequestInFlight = AtomicBoolean(false)
    private var previewUpdatesActive = false
    @Volatile private var previewSuspendedForOperation = false
    private var previewBitmap: Bitmap? = null

    private val previewRunnable = object : Runnable {
        override fun run() {
            if (!previewUpdatesActive) return
            if (previewRequestInFlight.compareAndSet(false, true)) {
                previewExecutor.execute {
                    val pixels = try {
                        nativeCopyPreviewArgb(PREVIEW_WIDTH, PREVIEW_HEIGHT)
                    } catch (_: Exception) {
                        null
                    }
                    runOnUiThread {
                        if (previewUpdatesActive && pixels != null &&
                            pixels.size == PREVIEW_WIDTH * PREVIEW_HEIGHT
                        ) {
                            val bitmap = previewBitmap ?: Bitmap.createBitmap(
                                PREVIEW_WIDTH, PREVIEW_HEIGHT, Bitmap.Config.ARGB_8888
                            ).also { previewBitmap = it }
                            bitmap.setPixels(
                                pixels, 0, PREVIEW_WIDTH, 0, 0,
                                PREVIEW_WIDTH, PREVIEW_HEIGHT
                            )
                            binding.previewImage.setImageBitmap(bitmap)
                            binding.previewStatus.text = if (previewSuspendedForOperation) {
                                "MEASUREMENT SNAPSHOT · 480×270"
                            } else {
                                "LIVE · 1280×720 · 60 fps"
                            }
                        }
                        previewRequestInFlight.set(false)
                    }
                }
            }
            previewHandler.postDelayed(this, PREVIEW_INTERVAL_MS)
        }
    }

    private val profileExportLauncher =
        registerForActivityResult(ActivityResultContracts.CreateDocument("application/json")) { uri ->
            val json = pendingProfileJson
            pendingProfileJson = null
            if (uri != null && json != null) {
                try {
                    contentResolver.openOutputStream(uri)?.bufferedWriter()?.use { it.write(json) }
                    showResult(binding.resultText.text.toString() + "\n\nProfile export: OK")
                } catch (error: Exception) {
                    showResult("Profile export failed: ${error.message}")
                }
            }
        }

    private val cameraPermissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) {
                if (pendingOneTapCalibration) {
                    startOneTapCalibration()
                } else {
                    startUsbInspection()
                }
            } else {
                pendingOneTapCalibration = false
                showResult("Camera permission is required to access a UVC device.")
            }
        }

    private val permissionReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == UsbManager.ACTION_USB_DEVICE_DETACHED) {
                val device = intent.usbDeviceExtra()
                if (device != null && device.deviceId == sessionDeviceId) {
                    nativeDetachUsb()
                    usbConnection?.close()
                    usbConnection = null
                    sessionDeviceId = null
                    binding.previewImage.setImageDrawable(null)
                    binding.previewStatus.text = "USB device disconnected"
                    setOperationEnabled(true)
                    showResult("USB device detached. Session closed.")
                }
                return
            }
            if (intent.action != ACTION_USB_PERMISSION) return
            val device = intent.usbDeviceExtra()
            if (device == null || device.deviceId != pendingDevice?.deviceId) return

            pendingDevice = null
            val resumeOneTap = pendingOneTapCalibration
            pendingOneTapCalibration = false
            if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                if (resumeOneTap) runOneTapCalibration(device) else inspect(device)
            } else {
                showResult("USB permission was denied.")
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        usbManager = getSystemService(USB_SERVICE) as UsbManager

        val filter = IntentFilter(ACTION_USB_PERMISSION)
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(permissionReceiver, filter, RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("DEPRECATION")
            registerReceiver(permissionReceiver, filter)
        }

        binding.inspectButton.setOnClickListener { startInspection() }
        binding.controlSpinner.adapter = ArrayAdapter(
            this,
            android.R.layout.simple_spinner_dropdown_item,
            CONTROL_NAMES
        )
        binding.setButton.setOnClickListener { startSetAndVerify() }
        binding.measureButton.setOnClickListener { startMeasurement() }
        binding.autoCalButton.setOnClickListener { startAutoCalibration() }
        binding.solveButton.setOnClickListener { startMatrixSolve() }
        binding.validateButton.setOnClickListener { startValidation() }
        binding.commitButton.setOnClickListener { commitAndExportProfile() }
        binding.oneTapButton.setOnClickListener { startOneTapCalibration() }
        binding.advancedButton.setOnClickListener {
            val show = binding.advancedContainer.visibility != View.VISIBLE
            binding.advancedContainer.visibility = if (show) View.VISIBLE else View.GONE
            binding.advancedButton.text = if (show) {
                "Hide manual operations"
            } else {
                "Manual operations"
            }
        }
        binding.encodingSpinner.adapter = stringAdapter(INPUT_ENCODINGS)
        binding.rangeSpinner.adapter = stringAdapter(INPUT_RANGES)
        binding.colorimetrySpinner.adapter = stringAdapter(COLORIMETRIES)
        binding.transferSpinner.adapter = stringAdapter(TRANSFER_CHARACTERISTICS)
        binding.resultText.text = savedProfileSummary()
    }

    override fun onStart() {
        super.onStart()
        previewUpdatesActive = true
        previewHandler.post(previewRunnable)
    }

    override fun onStop() {
        previewUpdatesActive = false
        previewHandler.removeCallbacks(previewRunnable)
        super.onStop()
    }

    override fun onDestroy() {
        previewUpdatesActive = false
        previewHandler.removeCallbacks(previewRunnable)
        previewExecutor.shutdownNow()
        previewBitmap?.recycle()
        previewBitmap = null
        nativeDetachUsb()
        usbConnection?.close()
        usbConnection = null
        sessionDeviceId = null
        unregisterReceiver(permissionReceiver)
        super.onDestroy()
    }

    private fun startInspection() {
        pendingOneTapCalibration = false
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            cameraPermissionLauncher.launch(Manifest.permission.CAMERA)
            return
        }
        startUsbInspection()
    }

    private fun startUsbInspection() {
        val device = usbManager.deviceList.values.firstOrNull(::isUvcDevice)
        if (device == null) {
            showResult("No UVC device found.")
            return
        }

        binding.resultText.text = deviceLabel(device) + "\n\nWaiting for USB permission..."
        if (usbManager.hasPermission(device)) {
            inspect(device)
            return
        }

        pendingDevice = device
        val intent = PendingIntent.getBroadcast(
            this,
            0,
            Intent(ACTION_USB_PERMISSION).setPackage(packageName),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE
        )
        usbManager.requestPermission(device, intent)
    }

    private fun inspect(device: UsbDevice) {
        setOperationEnabled(false)
        binding.resultText.text = deviceLabel(device) + "\n\nReading Processing Unit..."

        Thread {
            val report = try {
                if (!ensureUsbSession(device)) {
                    "ERROR: USB session attach failed"
                } else {
                    nativeInspectUsb(usbConnection!!.fileDescriptor)
                }
            } catch (error: Exception) {
                "ERROR: ${error.javaClass.simpleName}: ${error.message}"
            }

            runOnUiThread {
                setOperationEnabled(true)
                showResult(deviceLabel(device) + "\n\n" + report)
            }
        }.start()
    }

    @Synchronized
    private fun ensureUsbSession(device: UsbDevice): Boolean {
        if (usbConnection != null && sessionDeviceId == device.deviceId) return true
        nativeDetachUsb()
        usbConnection?.close()
        usbConnection = null
        sessionDeviceId = null

        val connection = usbManager.openDevice(device) ?: return false
        claimUvcInterfaces(connection, device)
        if (!nativeAttachUsb(connection.fileDescriptor)) {
            connection.close()
            return false
        }
        usbConnection = connection
        sessionDeviceId = device.deviceId
        runOnUiThread {
            binding.previewStatus.text = if (previewSuspendedForOperation) {
                "Preview paused during measurement"
            } else {
                "Starting preview · 1280×720 · 60 fps"
            }
        }
        return true
    }

    private fun startSetAndVerify() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            showResult("Grant Camera permission with INSPECT PU before writing a control.")
            return
        }

        val device = usbManager.deviceList.values.firstOrNull(::isUvcDevice)
        if (device == null) {
            showResult("No UVC device found.")
            return
        }
        if (!usbManager.hasPermission(device)) {
            showResult("Grant USB permission with INSPECT PU before writing a control.")
            return
        }

        val requested = binding.requestedValue.text.toString().trim().toLongOrNull()
        if (requested == null) {
            binding.requestedValue.error = "Enter an integer value"
            return
        }
        val position = binding.controlSpinner.selectedItemPosition
        val selector = CONTROL_SELECTORS[position]

        setOperationEnabled(false)
        binding.resultText.text = deviceLabel(device) + "\n\nWriting ${CONTROL_NAMES[position]}..."
        Thread {
            val report = try {
                if (!ensureUsbSession(device)) "ERROR: USB session attach failed"
                else nativeSetAndVerifyUsb(usbConnection!!.fileDescriptor, selector, requested)
            } catch (error: Exception) {
                "ERROR: ${error.javaClass.simpleName}: ${error.message}"
            }
            runOnUiThread {
                setOperationEnabled(true)
                showResult(deviceLabel(device) + "\n\n" + report)
            }
        }.start()
    }

    private fun setOperationEnabled(enabled: Boolean) {
        binding.inspectButton.isEnabled = enabled
        binding.setButton.isEnabled = enabled
        binding.measureButton.isEnabled = enabled
        binding.autoCalButton.isEnabled = enabled
        binding.solveButton.isEnabled = enabled
        binding.validateButton.isEnabled = enabled
        binding.commitButton.isEnabled = enabled
        binding.oneTapButton.isEnabled = enabled
        binding.advancedButton.isEnabled = enabled
        binding.controlSpinner.isEnabled = enabled
        binding.requestedValue.isEnabled = enabled
        binding.frameCount.isEnabled = enabled
        binding.encodingSpinner.isEnabled = enabled
        binding.rangeSpinner.isEnabled = enabled
        binding.colorimetrySpinner.isEnabled = enabled
        binding.transferSpinner.isEnabled = enabled
    }

    private fun startMeasurement() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            showResult("Grant Camera permission with INSPECT PU before measuring.")
            return
        }
        val device = usbManager.deviceList.values.firstOrNull(::isUvcDevice)
        if (device == null) {
            showResult("No UVC device found.")
            return
        }
        if (!usbManager.hasPermission(device)) {
            showResult("Grant USB permission with INSPECT PU before measuring.")
            return
        }
        val frameCount = binding.frameCount.text.toString().toIntOrNull()
        if (frameCount == null || frameCount !in 1..32) {
            binding.frameCount.error = "Enter 1–32 frames"
            return
        }

        setOperationEnabled(false)
        beginCalibrationProgress("Preparing measurement")
        binding.resultText.text = deviceLabel(device) +
            "\n\nStarting UVC stream and measuring $frameCount frames..."
        Thread {
            val report = try {
                if (!ensureUsbSession(device)) "ERROR: USB session attach failed"
                else nativeMeasurePatches(usbConnection!!.fileDescriptor, frameCount)
            } catch (error: Exception) {
                "ERROR: ${error.javaClass.simpleName}: ${error.message}"
            }
            runOnUiThread {
                setOperationEnabled(true)
                finishCalibrationProgress(report.contains("RESULT   : OK"))
                showResult(deviceLabel(device) + "\n\n" + report)
            }
        }.start()
    }

    private fun startOneTapCalibration() {
        if (selectedInputContractOrNull() == null) return
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            pendingOneTapCalibration = true
            cameraPermissionLauncher.launch(Manifest.permission.CAMERA)
            return
        }
        val device = usbManager.deviceList.values.firstOrNull(::isUvcDevice)
        if (device == null) {
            pendingOneTapCalibration = false
            showResult("No UVC device found.")
            return
        }
        if (!usbManager.hasPermission(device)) {
            pendingOneTapCalibration = true
            pendingDevice = device
            binding.previewStatus.text = "Waiting for USB permission"
            val intent = PendingIntent.getBroadcast(
                this,
                0,
                Intent(ACTION_USB_PERMISSION).setPackage(packageName),
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE
            )
            usbManager.requestPermission(device, intent)
            return
        }
        pendingOneTapCalibration = false
        runOneTapCalibration(device)
    }

    private fun runOneTapCalibration(device: UsbDevice) {
        setOperationEnabled(false)
        beginCalibrationProgress("Preparing one-tap calibration")
        binding.resultText.text = deviceLabel(device) +
            "\n\nStarting PU calibration, matrix solve, and validation..."
        Thread {
            val reports = mutableListOf<String>()
            var eligibleForCommit = false
            var finalStatus = CalibrationStatus.WARNING
            try {
                if (!ensureUsbSession(device)) {
                    reports += "ERROR: USB session attach failed"
                } else {
                    configureProgressStage(0, 80, "PU")
                    val autoReport = nativeAutoCalibratePu(usbConnection!!.fileDescriptor)
                    reports += autoReport
                    if (autoReport.contains("RESULT: AUTO_CAL_OK")) {
                        configureProgressStage(80, 10, "Matrix")
                        val solveReport = nativeSolveCaptureMatrix(usbConnection!!.fileDescriptor)
                        reports += solveReport
                        if (solveReport.contains("RESULT: SOLVE_OK")) {
                            configureProgressStage(90, 10, "Validation")
                            val validationReport = nativeValidateCalibration()
                            reports += validationReport
                            eligibleForCommit =
                                validationReport.contains("RESULT: CALIBRATION_VALID") ||
                                validationReport.contains("RESULT: CALIBRATION_POOR_FIT")
                            finalStatus = statusFromValidationReport(validationReport)
                        }
                    }
                }
            } catch (error: Exception) {
                reports += "ERROR: ${error.javaClass.simpleName}: ${error.message}"
            }
            val report = reports.joinToString("\n\n----------------\n\n")
            runOnUiThread {
                setOperationEnabled(true)
                finishCalibrationProgress(eligibleForCommit)
                updateCalibrationStatus(finalStatus)
                showResult(deviceLabel(device) + "\n\n" + report)
            }
        }.start()
    }

    private fun startAutoCalibration() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            showResult("Grant Camera permission with INSPECT PU before calibration.")
            return
        }
        val device = usbManager.deviceList.values.firstOrNull(::isUvcDevice)
        if (device == null) {
            showResult("No UVC device found.")
            return
        }
        if (!usbManager.hasPermission(device)) {
            showResult("Grant USB permission with INSPECT PU before calibration.")
            return
        }

        setOperationEnabled(false)
        beginCalibrationProgress("Preparing PU auto calibration")
        binding.resultText.text = deviceLabel(device) +
            "\n\nRunning two-pass PU auto calibration..."
        Thread {
            val report = try {
                if (!ensureUsbSession(device)) "ERROR: USB session attach failed"
                else nativeAutoCalibratePu(usbConnection!!.fileDescriptor)
            } catch (error: Exception) {
                "ERROR: ${error.javaClass.simpleName}: ${error.message}"
            }
            runOnUiThread {
                setOperationEnabled(true)
                finishCalibrationProgress(report.contains("RESULT: AUTO_CAL_OK"))
                showResult(deviceLabel(device) + "\n\n" + report)
            }
        }.start()
    }

    private fun startMatrixSolve() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            showResult("Grant Camera permission with INSPECT PU before solving.")
            return
        }
        val device = usbManager.deviceList.values.firstOrNull(::isUvcDevice)
        if (device == null) {
            showResult("No UVC device found.")
            return
        }
        if (!usbManager.hasPermission(device)) {
            showResult("Grant USB permission with INSPECT PU before solving.")
            return
        }

        setOperationEnabled(false)
        beginCalibrationProgress("Preparing matrix solve")
        binding.resultText.text = deviceLabel(device) +
            "\n\nMeasuring 24 patches and solving Offset + 3x3 Matrix..."
        Thread {
            val report = try {
                if (!ensureUsbSession(device)) "ERROR: USB session attach failed"
                else nativeSolveCaptureMatrix(usbConnection!!.fileDescriptor)
            } catch (error: Exception) {
                "ERROR: ${error.javaClass.simpleName}: ${error.message}"
            }
            runOnUiThread {
                setOperationEnabled(true)
                finishCalibrationProgress(report.contains("RESULT: SOLVE_OK"))
                showResult(deviceLabel(device) + "\n\n" + report)
            }
        }.start()
    }

    private fun startValidation() {
        val device = usbManager.deviceList.values.firstOrNull(::isUvcDevice)
        if (device == null || usbConnection == null || sessionDeviceId != device.deviceId) {
            showResult("Attach the USB session and run Phase 5 Solve first.")
            return
        }
        setOperationEnabled(false)
        beginCalibrationProgress("Preparing validation")
        binding.resultText.text = deviceLabel(device) +
            "\n\nMeasuring independent validation frames..."
        Thread {
            val report = try {
                nativeValidateCalibration()
            } catch (error: Exception) {
                "ERROR: ${error.javaClass.simpleName}: ${error.message}"
            }
            runOnUiThread {
                setOperationEnabled(true)
                finishCalibrationProgress(report.contains("RESULT: CALIBRATION_"))
                updateCalibrationStatus(statusFromValidationReport(report))
                showResult(deviceLabel(device) + "\n\n" + report)
            }
        }.start()
    }

    private fun commitAndExportProfile() {
        val device = usbManager.deviceList.values.firstOrNull(::isUvcDevice)
        if (device == null || usbConnection == null || sessionDeviceId != device.deviceId) {
            showResult("USB session is not attached.")
            return
        }
        val inputContract = selectedInputContractOrNull() ?: return
        val nativePayload = nativeBuildProfilePayload()
        if (nativePayload.startsWith("ERROR:")) {
            showResult(
                nativePayload +
                    "\nRun Solve and validation before commit. " +
                    "VALID and POOR_FIT results can be committed."
            )
            return
        }

        try {
            val now = Date()
            val timestamp = utcTimestamp(now)
            val fileTimestamp = utcFileTimestamp(now)
            val serial = try { usbConnection?.serial } catch (_: Exception) { null }
            val payload = JSONObject(nativePayload)
            val profile = JSONObject()
            profile.put("profileFormatVersion", 1)
            profile.put("device", JSONObject().apply {
                put("vid", device.vendorId)
                put("pid", device.productId)
                if (!serial.isNullOrBlank()) put("serial", serial)
                put("product", device.productName)
            })
            profile.put("captureMode", JSONObject().apply {
                put("pixelFormat", "MJPEG")
                put("width", CAPTURE_WIDTH)
                put("height", CAPTURE_HEIGHT)
                put("frameRate", CAPTURE_FRAME_RATE)
            })
            profile.put("inputContract", JSONObject().apply {
                put("inputEncoding", inputContract.inputEncoding)
                put("range", inputContract.range)
                put("colorimetry", inputContract.colorimetry)
                put("transferCharacteristics", inputContract.transferCharacteristics)
            })
            profile.put("pu", payload.getJSONObject("pu"))
            profile.put("correction", payload.getJSONObject("correction"))
            profile.put("validation", payload.getJSONObject("validation"))
            profile.put("metadata", JSONObject().apply {
                put("calibrationModelVersion", "offset-matrix-v1")
                put("patternVersion", "24patch-v1")
                put("solverVersion", "least-squares-v1")
                put("transportDecode", "BT.601_LIMITED")
                put("timestampUtc", timestamp)
            })

            val formatted = profile.toString(2) + "\n"
            val directory = filesDir.resolve("calibration_profiles")
            directory.mkdirs()
            val finalFile = directory.resolve(
                ProfileFileNaming.build(
                    vendorId = device.vendorId,
                    productId = device.productId,
                    serial = serial,
                    width = CAPTURE_WIDTH,
                    height = CAPTURE_HEIGHT,
                    frameRate = CAPTURE_FRAME_RATE,
                    colorimetry = inputContract.colorimetry,
                    transferCharacteristics = inputContract.transferCharacteristics,
                    inputEncoding = inputContract.inputEncoding,
                    range = inputContract.range
                )
            )
            val temporaryFile = directory.resolve(finalFile.name + ".tmp")
            FileOutputStream(temporaryFile).use { stream ->
                stream.write(formatted.toByteArray(Charsets.UTF_8))
                stream.fd.sync()
            }
            try {
                Files.move(
                    temporaryFile.toPath(),
                    finalFile.toPath(),
                    StandardCopyOption.ATOMIC_MOVE,
                    StandardCopyOption.REPLACE_EXISTING
                )
            } catch (_: AtomicMoveNotSupportedException) {
                Files.move(
                    temporaryFile.toPath(),
                    finalFile.toPath(),
                    StandardCopyOption.REPLACE_EXISTING
                )
            }

            pendingProfileJson = formatted
            val validationResult = payload.getJSONObject("validation").getString("result")
            val warning = if (validationResult == "CALIBRATION_POOR_FIT") {
                "\nWARNING: Committed with CALIBRATION_POOR_FIT."
            } else {
                ""
            }
            showResult(
                "Profile commit: OK$warning\n${finalFile.absolutePath}" +
                    "\n\nSelect export destination."
            )
            profileExportLauncher.launch(
                ProfileFileNaming.build(
                    vendorId = device.vendorId,
                    productId = device.productId,
                    serial = serial,
                    width = CAPTURE_WIDTH,
                    height = CAPTURE_HEIGHT,
                    frameRate = CAPTURE_FRAME_RATE,
                    colorimetry = inputContract.colorimetry,
                    transferCharacteristics = inputContract.transferCharacteristics,
                    inputEncoding = inputContract.inputEncoding,
                    range = inputContract.range,
                    timestampUtc = fileTimestamp
                )
            )
        } catch (error: Exception) {
            showResult("Profile commit failed: ${error.javaClass.simpleName}: ${error.message}")
        }
    }

    private fun stringAdapter(values: Array<String>) = ArrayAdapter(
        this,
        android.R.layout.simple_spinner_dropdown_item,
        values
    )

    private fun selectedInputContractOrNull(): InputContractSelection? {
        val missing = buildList {
            if (binding.encodingSpinner.selectedItemPosition == 0) add("Input Encoding")
            if (binding.rangeSpinner.selectedItemPosition == 0) add("Range")
            if (binding.colorimetrySpinner.selectedItemPosition == 0) add("Colorimetry")
            if (binding.transferSpinner.selectedItemPosition == 0) {
                add("Transfer Characteristics")
            }
        }
        if (missing.isNotEmpty()) {
            showResult(
                "Select the Input Contract explicitly before commit:\n" +
                    missing.joinToString(separator = "\n") { "- $it" }
            )
            return null
        }
        return InputContractSelection(
            inputEncoding = binding.encodingSpinner.selectedItem.toString(),
            range = binding.rangeSpinner.selectedItem.toString(),
            colorimetry = binding.colorimetrySpinner.selectedItem.toString(),
            transferCharacteristics = binding.transferSpinner.selectedItem.toString()
        )
    }

    private fun utcTimestamp(date: Date): String = SimpleDateFormat(
        "yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US
    ).apply { timeZone = TimeZone.getTimeZone("UTC") }.format(date)

    private fun utcFileTimestamp(date: Date): String = SimpleDateFormat(
        "yyyyMMdd'T'HHmmss'Z'", Locale.US
    ).apply { timeZone = TimeZone.getTimeZone("UTC") }.format(date)

    private fun savedProfileSummary(): String {
        val directory = filesDir.resolve("calibration_profiles")
        val profiles = directory.listFiles { file ->
            file.isFile && file.extension.equals("json", ignoreCase = true)
        }?.sortedByDescending { it.lastModified() }.orEmpty()
        if (profiles.isEmpty()) {
            return "Connect a UVC device, then tap INSPECT PU.\n\nSaved profiles: none"
        }

        val summary = profiles.mapNotNull { file ->
            try {
                val profile = JSONObject(file.readText(Charsets.UTF_8))
                val device = profile.getJSONObject("device")
                val mode = profile.getJSONObject("captureMode")
                val input = profile.getJSONObject("inputContract")
                val metadata = profile.getJSONObject("metadata")
                "%04X:%04X  %s %dx%d@%d  %s/%s/%s/%s  %s".format(
                    device.getInt("vid"),
                    device.getInt("pid"),
                    mode.getString("pixelFormat"),
                    mode.getInt("width"),
                    mode.getInt("height"),
                    mode.getInt("frameRate"),
                    input.getString("inputEncoding"),
                    input.getString("range"),
                    input.getString("colorimetry"),
                    input.getString("transferCharacteristics"),
                    metadata.getString("timestampUtc")
                )
            } catch (_: Exception) {
                null
            }
        }
        return buildString {
            append("Connect a UVC device, then tap INSPECT PU.\n\n")
            append("Saved profiles loaded: ${summary.size}/${profiles.size}")
            summary.forEach { append("\n").append(it) }
        }
    }

    private fun claimUvcInterfaces(connection: UsbDeviceConnection, device: UsbDevice) {
        for (index in 0 until device.interfaceCount) {
            val intf = device.getInterface(index)
            if (intf.interfaceClass == UsbConstants.USB_CLASS_VIDEO &&
                intf.alternateSetting == 0
            ) {
                // Keep VC and VS alt 0 claimed on the Android-owned fd while
                // libusb performs control requests and streaming.
                connection.claimInterface(intf, true)
            }
        }
    }

    private fun isUvcDevice(device: UsbDevice): Boolean {
        for (index in 0 until device.interfaceCount) {
            if (device.getInterface(index).interfaceClass == UsbConstants.USB_CLASS_VIDEO) {
                return true
            }
        }
        return false
    }

    private fun deviceLabel(device: UsbDevice): String =
        "Device: %04X:%04X  %s".format(
            device.vendorId,
            device.productId,
            device.productName ?: device.deviceName
        )

    private fun showResult(text: String) {
        binding.resultText.text = text
    }

    private fun beginCalibrationProgress(message: String) {
        previewSuspendedForOperation = true
        nativeSetPreviewEnabled(false)
        if (usbConnection != null) {
            binding.previewStatus.text = "Preview paused during measurement"
        }
        configureProgressStage(0, 100, "")
        binding.calibrationProgress.visibility = View.VISIBLE
        binding.calibrationProgressStatus.visibility = View.VISIBLE
        binding.calibrationProgress.progress = 0
        binding.calibrationProgressStatus.text = "0%  $message"
        updateCalibrationStatus(CalibrationStatus.RUNNING)
    }

    @Keep
    private fun onNativeCalibrationProgress(percent: Int, message: String) {
        runOnUiThread {
            val mappedPercent = (
                progressStageBase + percent.coerceIn(0, 100) * progressStageSpan / 100
            ).coerceIn(0, 100)
            val stageMessage = if (progressStageLabel.isBlank()) {
                message
            } else {
                "$progressStageLabel — $message"
            }
            binding.calibrationProgress.progress = mappedPercent
            binding.calibrationProgressStatus.text =
                "$mappedPercent%  $stageMessage"
        }
    }

    private fun configureProgressStage(base: Int, span: Int, label: String) {
        progressStageBase = base.coerceIn(0, 100)
        progressStageSpan = span.coerceIn(0, 100 - progressStageBase)
        progressStageLabel = label
    }

    private fun finishCalibrationProgress(success: Boolean) {
        previewSuspendedForOperation = false
        nativeSetPreviewEnabled(true)
        if (usbConnection != null) {
            binding.previewStatus.text = "LIVE · 1280×720 · 60 fps"
        }
        binding.calibrationProgress.progress = 100
        binding.calibrationProgressStatus.text = if (success) {
            "100%  Completed"
        } else {
            "Stopped — see result"
        }
        if (!success) {
            updateCalibrationStatus(CalibrationStatus.WARNING)
        } else if (calibrationStatus == CalibrationStatus.RUNNING) {
            updateCalibrationStatus(CalibrationStatus.IDLE)
        }
    }

    private fun statusFromValidationReport(report: String): CalibrationStatus = when {
        report.contains("RESULT: CALIBRATION_VALID") -> CalibrationStatus.VALID
        report.contains("RESULT: CALIBRATION_POOR_FIT") -> CalibrationStatus.POOR_FIT
        else -> CalibrationStatus.WARNING
    }

    private fun updateCalibrationStatus(status: CalibrationStatus) {
        calibrationStatus = status
        val (label, color) = when (status) {
            CalibrationStatus.IDLE -> "NOT CALIBRATED" to R.color.calibration_status_idle
            CalibrationStatus.RUNNING -> "CALIBRATING" to R.color.calibration_status_running
            CalibrationStatus.VALID -> "VALID" to R.color.calibration_status_valid
            CalibrationStatus.POOR_FIT -> "POOR FIT" to R.color.calibration_status_poor
            CalibrationStatus.WARNING -> "WARNING" to R.color.calibration_status_warning
        }
        binding.calibrationStatus.text = label
        binding.calibrationStatus.backgroundTintList = ColorStateList.valueOf(
            ContextCompat.getColor(this, color)
        )
    }

    @Suppress("DEPRECATION")
    private fun Intent.usbDeviceExtra(): UsbDevice? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
        } else {
            getParcelableExtra(UsbManager.EXTRA_DEVICE)
        }

    private external fun nativeInspectUsb(fd: Int): String
    private external fun nativeAttachUsb(fd: Int): Boolean
    private external fun nativeDetachUsb()
    private external fun nativeSetAndVerifyUsb(fd: Int, selector: Int, requested: Long): String
    private external fun nativeMeasurePatches(fd: Int, frameCount: Int): String
    private external fun nativeAutoCalibratePu(fd: Int): String
    private external fun nativeSolveCaptureMatrix(fd: Int): String
    private external fun nativeValidateCalibration(): String
    private external fun nativeBuildProfilePayload(): String
    private external fun nativeCopyPreviewArgb(width: Int, height: Int): IntArray?
    private external fun nativeSetPreviewEnabled(enabled: Boolean)

    private data class InputContractSelection(
        val inputEncoding: String,
        val range: String,
        val colorimetry: String,
        val transferCharacteristics: String
    )

    private enum class CalibrationStatus {
        IDLE,
        RUNNING,
        VALID,
        POOR_FIT,
        WARNING
    }

    companion object {
        private const val ACTION_USB_PERMISSION =
            "com.hev.uvccapturecalibration.USB_PERMISSION"
        private val CONTROL_NAMES = arrayOf(
            "Brightness", "Contrast", "Saturation", "Hue"
        )
        private val CONTROL_SELECTORS = intArrayOf(0x02, 0x03, 0x07, 0x06)
        private val INPUT_ENCODINGS = arrayOf("", "RGB", "YCBCR_444", "YCBCR_422")
        private val INPUT_RANGES = arrayOf("", "FULL", "LIMITED")
        private val COLORIMETRIES = arrayOf("", "BT601", "BT709", "BT2020")
        private val TRANSFER_CHARACTERISTICS = arrayOf("", "SDR", "PQ")
        private const val CAPTURE_WIDTH = 1280
        private const val CAPTURE_HEIGHT = 720
        private const val CAPTURE_FRAME_RATE = 60
        private const val PREVIEW_WIDTH = 480
        private const val PREVIEW_HEIGHT = 270
        private const val PREVIEW_INTERVAL_MS = 100L

        init {
            System.loadLibrary("uvccapturecalibration")
        }
    }
}
