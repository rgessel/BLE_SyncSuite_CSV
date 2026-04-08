package com.example.ble_sync_suite_app

import androidx.compose.runtime.mutableStateMapOf
import java.util.UUID

// =============================================================================
// BLE models and constants
// =============================================================================

// ----- Data classes -----
/** One ESP32 BLE packet: sequence number, beacon time in μs, and phone receive time in ns (set when received). */
data class EspPacket(
    val seq: Long,
    val tUs: Long,
    val receivedAtNs: Long,
    /** BLE MAC of the board that sent this packet (normalized uppercase). */
    val deviceAddress: String = "",
    val deviceName: String = "",
    /**
     * Beacon time mapped to the phone monotonic timeline (ns) via CheepSync: Tr ≈ α + β×t_beacon_ns.
     * Snapshot taken immediately after this sample updates the per-device fit.
     */
    val syncedAtNs: Long = 0L,
    /** CheepSync α (offset, ns) after processing this sample. */
    val cheepSyncAlphaNs: Double = 0.0,
    /** CheepSync β (skew) after processing this sample. */
    val cheepSyncBeta: Double = 1.0
)

/** A board currently connected over BLE (name + MAC). */
data class ConnectedBoard(val name: String, val address: String)

/** BLE characteristic metadata for UI (service/char UUID, name, properties string). */
data class CharacteristicInfo(
    val serviceUuid: UUID,
    val serviceName: String,
    val charUuid: UUID,
    val charName: String,
    val properties: String,
    val deviceAddress: String = "",
    val deviceName: String = ""
)

// ----- Android permissions (Android 12+) -----
const val PERMISSION_BLUETOOTH_SCAN = "android.permission.BLUETOOTH_SCAN"
const val PERMISSION_BLUETOOTH_CONNECT = "android.permission.BLUETOOTH_CONNECT"

// ----- BLE UUIDs -----
/** Standard CCCD descriptor UUID (required to enable/disable notifications). */
val CLIENT_CONFIG_DESCRIPTOR_UUID = UUID.fromString("00002902-0000-1000-8000-00805F9B34FB")
/** Custom service/characteristic used by the ESP32 for streaming (seq + tUs). */
val ESP32_SERVICE_UUID = UUID.fromString("0000181a-0000-1000-8000-00805f9b34fb")
val ESP32_CHAR_UUID = UUID.fromString("0015a1a1-1212-efde-1523-785feabcd123")

val standardServiceNames = mapOf(
    UUID.fromString("00001800-0000-1000-8000-00805f9b34fb") to "Generic Access",
    UUID.fromString("00001801-0000-1000-8000-00805f9b34fb") to "Generic Attribute",
    UUID.fromString("0000180D-0000-1000-8000-00805f9b34fb") to "Heart Rate",
    UUID.fromString("0000180F-0000-1000-8000-00805f9b34fb") to "Battery Service"
)

val standardCharacteristicNames = mapOf(
    UUID.fromString("00002A00-0000-1000-8000-00805f9b34fb") to "Device Name",
    UUID.fromString("00002A01-0000-1000-8000-00805f9b34fb") to "Appearance",
    UUID.fromString("00002A37-0000-1000-8000-00805f9b34fb") to "Heart Rate Measurement",
    UUID.fromString("00002A38-0000-1000-8000-00805f9b34fb") to "Body Sensor Location",
    UUID.fromString("00002A19-0000-1000-8000-00805f9b34fb") to "Battery Level"
)

/** Last read value per device + characteristic (for UI "Read" button). Key: [readValueKey]. */
val readValues = mutableStateMapOf<String, String>()

fun readValueKey(deviceAddress: String, charUuid: UUID): String =
    "${deviceAddress.uppercase()}|${charUuid}"

// ----- Byte parsing (little-endian) -----
/** Read 4 bytes as unsigned 32-bit little-endian. */
fun u32LE(bytes: ByteArray, offset: Int): Long =
    ((bytes[offset].toUByte().toLong() and 0xFF) shl 0) or
            ((bytes[offset + 1].toUByte().toLong() and 0xFF) shl 8) or
            ((bytes[offset + 2].toUByte().toLong() and 0xFF) shl 16) or
            ((bytes[offset + 3].toUByte().toLong() and 0xFF) shl 24)

/** Read 8 bytes as unsigned 64-bit little-endian. */
fun u64LE(bytes: ByteArray, offset: Int): Long =
    ((bytes[offset].toUByte().toLong() and 0xFF) shl 0) or
            ((bytes[offset + 1].toUByte().toLong() and 0xFF) shl 8) or
            ((bytes[offset + 2].toUByte().toLong() and 0xFF) shl 16) or
            ((bytes[offset + 3].toUByte().toLong() and 0xFF) shl 24) or
            ((bytes[offset + 4].toUByte().toLong() and 0xFF) shl 32) or
            ((bytes[offset + 5].toUByte().toLong() and 0xFF) shl 40) or
            ((bytes[offset + 6].toUByte().toLong() and 0xFF) shl 48) or
            ((bytes[offset + 7].toUByte().toLong() and 0xFF) shl 56)
