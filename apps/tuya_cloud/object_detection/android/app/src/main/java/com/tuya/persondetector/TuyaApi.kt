package com.tuya.persondetector

import android.util.Base64
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

object TuyaApi {
    private const val TAG = "TuyaApi"
    private var accessToken: String? = null
    private var tokenExpiry: Long = 0

    private fun sha256(data: String): String {
        val digest = MessageDigest.getInstance("SHA-256")
        return digest.digest(data.toByteArray()).joinToString("") { "%02x".format(it) }
    }

    private fun hmacSha256(key: String, data: String): String {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(key.toByteArray(), "HmacSHA256"))
        return mac.doFinal(data.toByteArray()).joinToString("") { "%02X".format(it) }
    }

    private suspend fun getToken(): String? = withContext(Dispatchers.IO) {
        if (accessToken != null && System.currentTimeMillis() < tokenExpiry) return@withContext accessToken

        val t = System.currentTimeMillis().toString()
        val path = "/v1.0/token?grant_type=1"
        val stringToSign = "${Config.accessId}${t}GET\n${sha256("")}\n\n$path"
        val sign = hmacSha256(Config.accessSecret, stringToSign)

        val url = URL("${Config.apiEndpoint}$path")
        val conn = url.openConnection() as HttpURLConnection
        conn.apply {
            requestMethod = "GET"
            setRequestProperty("client_id", Config.accessId)
            setRequestProperty("t", t)
            setRequestProperty("sign", sign)
            setRequestProperty("sign_method", "HMAC-SHA256")
        }

        try {
            val response = conn.inputStream.bufferedReader().readText()
            val json = JSONObject(response)
            if (json.getBoolean("success")) {
                val result = json.getJSONObject("result")
                accessToken = result.getString("access_token")
                tokenExpiry = System.currentTimeMillis() + (result.getInt("expire_time") * 1000L) - 60000
                Log.d(TAG, "Token obtained")
                accessToken
            } else {
                Log.e(TAG, "Token error: ${json.getString("msg")}")
                null
            }
        } catch (e: Exception) {
            Log.e(TAG, "Token failed: ${e.message}")
            null
        } finally {
            conn.disconnect()
        }
    }

    suspend fun detectPerson(imageBase64: String): Pair<Boolean, String?> = withContext(Dispatchers.IO) {
        val token = getToken() ?: return@withContext Pair(false, null)
        
        val t = System.currentTimeMillis().toString()
        val path = "/v1.0/ai/image/detect"
        val body = JSONObject().apply {
            put("image", imageBase64)
            put("type", "person")
        }.toString()
        
        val bodyHash = sha256(body)
        val stringToSign = "${Config.accessId}${token}${t}POST\n$bodyHash\n\n$path"
        val sign = hmacSha256(Config.accessSecret, stringToSign)

        val url = URL("${Config.apiEndpoint}$path")
        val conn = url.openConnection() as HttpURLConnection
        conn.apply {
            requestMethod = "POST"
            doOutput = true
            setRequestProperty("client_id", Config.accessId)
            setRequestProperty("access_token", token)
            setRequestProperty("t", t)
            setRequestProperty("sign", sign)
            setRequestProperty("sign_method", "HMAC-SHA256")
            setRequestProperty("Content-Type", "application/json")
        }

        try {
            conn.outputStream.write(body.toByteArray())
            val response = conn.inputStream.bufferedReader().readText()
            val json = JSONObject(response)
            Log.d(TAG, "Detection response: $response")
            
            if (json.getBoolean("success")) {
                val result = json.optJSONObject("result")
                val detected = result?.optJSONArray("persons")?.length()?.let { it > 0 } ?: false
                val description = if (detected) result?.toString() else null
                Pair(detected, description)
            } else {
                Pair(false, null)
            }
        } catch (e: Exception) {
            Log.e(TAG, "Detection failed: ${e.message}")
            Pair(false, null)
        } finally {
            conn.disconnect()
        }
    }

    suspend fun triggerDevice(switchOn: Boolean): Boolean = withContext(Dispatchers.IO) {
        val token = getToken() ?: return@withContext false
        
        val t = System.currentTimeMillis().toString()
        val path = "/v1.0/devices/${Config.deviceId}/commands"
        val body = JSONObject().apply {
            put("commands", org.json.JSONArray().apply {
                put(JSONObject().apply {
                    put("code", "switch_${Config.DP_SWITCH_ID}")
                    put("value", switchOn)
                })
            })
        }.toString()
        
        val bodyHash = sha256(body)
        val stringToSign = "${Config.accessId}${token}${t}POST\n$bodyHash\n\n$path"
        val sign = hmacSha256(Config.accessSecret, stringToSign)

        val url = URL("${Config.apiEndpoint}$path")
        val conn = url.openConnection() as HttpURLConnection
        conn.apply {
            requestMethod = "POST"
            doOutput = true
            setRequestProperty("client_id", Config.accessId)
            setRequestProperty("access_token", token)
            setRequestProperty("t", t)
            setRequestProperty("sign", sign)
            setRequestProperty("sign_method", "HMAC-SHA256")
            setRequestProperty("Content-Type", "application/json")
        }

        try {
            conn.outputStream.write(body.toByteArray())
            val response = conn.inputStream.bufferedReader().readText()
            val json = JSONObject(response)
            Log.d(TAG, "Trigger response: $response")
            json.getBoolean("success")
        } catch (e: Exception) {
            Log.e(TAG, "Trigger failed: ${e.message}")
            false
        } finally {
            conn.disconnect()
        }
    }
}
