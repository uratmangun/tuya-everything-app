package com.tuya.persondetector

object Config {
    // Tuya Cloud API credentials (get from iot.tuya.com → Cloud → Development)
    var accessId = "your_access_id"
    var accessSecret = "your_access_secret"
    
    // API endpoint (US/EU/CN)
    var apiEndpoint = "https://openapi.tuyaus.com"
    
    // Your T5AI DevKit device ID
    var deviceId = "your_device_id"
    
    // RTSP camera URL
    var rtspUrl = "rtsp://admin:Admin1234@192.168.18.34:554/live/ch00_0"
    
    // Detection interval (ms)
    const val DETECTION_INTERVAL_MS = 3000L
    
    // DP ID for switch
    const val DP_SWITCH_ID = "1"
}
