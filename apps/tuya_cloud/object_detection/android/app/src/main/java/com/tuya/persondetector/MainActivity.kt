package com.tuya.persondetector

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.SurfaceTexture
import android.media.MediaPlayer
import android.os.Bundle
import android.util.Base64
import android.util.Log
import android.view.Surface
import android.view.TextureView
import android.view.View
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.*
import java.io.ByteArrayOutputStream

class MainActivity : AppCompatActivity() {
    private lateinit var textureView: TextureView
    private lateinit var statusText: TextView
    private lateinit var logText: TextView
    private lateinit var startBtn: Button
    private lateinit var rtspInput: EditText
    private lateinit var accessIdInput: EditText
    private lateinit var accessSecretInput: EditText
    private lateinit var deviceIdInput: EditText
    
    private var mediaPlayer: MediaPlayer? = null
    private var detectionJob: Job? = null
    private var isRunning = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        
        textureView = findViewById(R.id.textureView)
        statusText = findViewById(R.id.statusText)
        logText = findViewById(R.id.logText)
        startBtn = findViewById(R.id.startBtn)
        rtspInput = findViewById(R.id.rtspInput)
        accessIdInput = findViewById(R.id.accessIdInput)
        accessSecretInput = findViewById(R.id.accessSecretInput)
        deviceIdInput = findViewById(R.id.deviceIdInput)
        
        // Load defaults
        rtspInput.setText(Config.rtspUrl)
        accessIdInput.setText(Config.accessId)
        accessSecretInput.setText(Config.accessSecret)
        deviceIdInput.setText(Config.deviceId)
        
        if (ActivityCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, arrayOf(Manifest.permission.POST_NOTIFICATIONS), 1)
        }
        
        textureView.surfaceTextureListener = object : TextureView.SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
                log("Surface ready")
            }
            override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) {}
            override fun onSurfaceTextureDestroyed(st: SurfaceTexture) = true
            override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}
        }
        
        startBtn.setOnClickListener {
            if (isRunning) stop() else start()
        }
    }

    private fun start() {
        // Save config
        Config.rtspUrl = rtspInput.text.toString()
        Config.accessId = accessIdInput.text.toString()
        Config.accessSecret = accessSecretInput.text.toString()
        Config.deviceId = deviceIdInput.text.toString()
        
        if (Config.accessId == "your_access_id") {
            log("⚠️ Please enter your Tuya credentials")
            return
        }
        
        isRunning = true
        startBtn.text = "Stop"
        statusText.text = "🟢 Running"
        log("Starting RTSP: ${Config.rtspUrl}")
        
        startRtsp()
        startDetection()
    }

    private fun stop() {
        isRunning = false
        startBtn.text = "Start"
        statusText.text = "🔴 Stopped"
        detectionJob?.cancel()
        mediaPlayer?.release()
        mediaPlayer = null
        log("Stopped")
    }

    private fun startRtsp() {
        try {
            mediaPlayer?.release()
            mediaPlayer = MediaPlayer().apply {
                setSurface(Surface(textureView.surfaceTexture))
                setDataSource(Config.rtspUrl)
                setOnPreparedListener { 
                    start()
                    log("✅ RTSP connected")
                }
                setOnErrorListener { _, what, extra ->
                    log("❌ RTSP error: $what/$extra")
                    true
                }
                prepareAsync()
            }
        } catch (e: Exception) {
            log("❌ RTSP failed: ${e.message}")
        }
    }

    private fun startDetection() {
        detectionJob = lifecycleScope.launch {
            while (isRunning) {
                delay(Config.DETECTION_INTERVAL_MS)
                if (!isRunning) break
                
                val bitmap = textureView.bitmap ?: continue
                val base64 = bitmapToBase64(bitmap)
                
                log("📸 Analyzing frame...")
                val (detected, desc) = TuyaApi.detectPerson(base64)
                
                if (detected) {
                    log("👤 Person detected! Triggering alert...")
                    val success = TuyaApi.triggerDevice(true)
                    if (success) {
                        log("✅ Alert triggered on devkit")
                    } else {
                        log("❌ Failed to trigger devkit")
                    }
                    // Reset switch after delay
                    delay(2000)
                    TuyaApi.triggerDevice(false)
                } else {
                    log("No person detected")
                }
            }
        }
    }

    private fun bitmapToBase64(bitmap: Bitmap): String {
        val scaled = Bitmap.createScaledBitmap(bitmap, 640, 480, true)
        val stream = ByteArrayOutputStream()
        scaled.compress(Bitmap.CompressFormat.JPEG, 80, stream)
        return Base64.encodeToString(stream.toByteArray(), Base64.NO_WRAP)
    }

    private fun log(msg: String) {
        Log.d("PersonDetector", msg)
        runOnUiThread {
            val time = java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.getDefault()).format(java.util.Date())
            logText.append("[$time] $msg\n")
            val scrollView = logText.parent as? ScrollView
            scrollView?.fullScroll(View.FOCUS_DOWN)
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        stop()
    }
}
