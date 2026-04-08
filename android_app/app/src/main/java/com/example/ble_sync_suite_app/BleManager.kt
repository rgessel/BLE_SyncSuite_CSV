package com.example.ble_sync_suite_app

// BLE Manager: BLE scan, connect, multi-GATT, and CheepSync. Sync math is in sync/CheepSync.kt.

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.annotation.RequiresPermission
import com.example.ble_sync_suite_app.sync.CheepSync
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.util.UUID

class BleManager(
    private val activity: ComponentActivity,
    private val hasScanPermission: () -> Boolean,
    private val hasConnectPermission: () -> Boolean,
    private val onDeviceFound: (String) -> Unit,
    private val onConnected: (deviceName: String, deviceAddress: String) -> Unit,
    private val onDisconnected: (deviceAddress: String) -> Unit,
    private val onCharacteristicsDiscovered: (List<CharacteristicInfo>) -> Unit,
    private val onPacketReceived: (EspPacket) -> Unit
) {
    private val bluetoothManager = activity.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
    private val bluetoothAdapter = bluetoothManager?.adapter
    private var bluetoothLeScanner: BluetoothLeScanner? = null

    private val gattByAddress = mutableMapOf<String, BluetoothGatt>()
    private val cheepSyncByAddress = mutableMapOf<String, CheepSync>()
    private val connectingAddresses = mutableSetOf<String>()
    private val mainHandler = Handler(Looper.getMainLooper())

    private val _cheepSyncAlphaByDevice = MutableStateFlow<Map<String, Double>>(emptyMap())
    val cheepSyncAlphaByDevice: StateFlow<Map<String, Double>> = _cheepSyncAlphaByDevice.asStateFlow()
    private val _cheepSyncBetaByDevice = MutableStateFlow<Map<String, Double>>(emptyMap())
    val cheepSyncBetaByDevice: StateFlow<Map<String, Double>> = _cheepSyncBetaByDevice.asStateFlow()
    private val _cheepSyncRmsResidualMsByDevice = MutableStateFlow<Map<String, Double>>(emptyMap())
    val cheepSyncRmsResidualMsByDevice: StateFlow<Map<String, Double>> = _cheepSyncRmsResidualMsByDevice.asStateFlow()

    private fun addrKey(address: String) = address.uppercase()

    private fun resetCheepSyncForDevice(address: String) {
        val key = addrKey(address)
        cheepSyncByAddress.remove(key)?.reset()
        _cheepSyncAlphaByDevice.value = _cheepSyncAlphaByDevice.value - key
        _cheepSyncBetaByDevice.value = _cheepSyncBetaByDevice.value - key
        _cheepSyncRmsResidualMsByDevice.value = _cheepSyncRmsResidualMsByDevice.value - key
    }

    private fun updateCheepSync(packet: EspPacket) {
        val key = addrKey(packet.deviceAddress)
        val cheepSync = cheepSyncByAddress.getOrPut(key) { CheepSync(windowSize = CheepSync.DEFAULT_WINDOW_SIZE) }
        cheepSync.addSample(packet.tUs, packet.receivedAtNs)
        _cheepSyncAlphaByDevice.value = _cheepSyncAlphaByDevice.value + (key to cheepSync.alpha)
        _cheepSyncBetaByDevice.value = _cheepSyncBetaByDevice.value + (key to cheepSync.beta)
        _cheepSyncRmsResidualMsByDevice.value = _cheepSyncRmsResidualMsByDevice.value + (key to cheepSync.rmsResidualMs)
    }

    /**
     * Map a beacon timestamp (μs since beacon boot) into the phone’s monotonic ns timeline for a given board.
     */
    fun mapBeaconToPhoneNs(beaconTimeUs: Long, deviceAddress: String): Long {
        val cs = cheepSyncByAddress[addrKey(deviceAddress)]
            ?: error("No CheepSync state for $deviceAddress")
        return cs.mapBeaconToReceiverNs(beaconTimeUs)
    }

    fun estimateLatestResidualMs(packet: EspPacket): Double {
        val cs = cheepSyncByAddress[addrKey(packet.deviceAddress)] ?: return 0.0
        return cs.residualMs(packet.tUs, packet.receivedAtNs)
    }

    fun connectionCount(): Int = gattByAddress.size

    // -----------------------------
    // BLE scanning
    // -----------------------------
    private val bleScanCallback = object : ScanCallback() {
        @SuppressLint("MissingPermission")
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            super.onScanResult(callbackType, result)
            if (!hasConnectPermission()) return
            val name = result.device.name ?: "Unnamed"
            val addr = result.device.address
            val display = "$name [$addr]"
            activity.runOnUiThread { onDeviceFound(display) }
        }

        override fun onScanFailed(errorCode: Int) {
            super.onScanFailed(errorCode)
            Log.e("BLE", "Scan failed: $errorCode")
        }
    }

    // ----- GATT callbacks: connection lifecycle and characteristic notifications -----
    private val gattCallback = object : BluetoothGattCallback() {

        @RequiresPermission(PERMISSION_BLUETOOTH_CONNECT)
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            val address = addrKey(gatt.device.address)
            if (status != BluetoothGatt.GATT_SUCCESS) {
                Log.e("BLE", "GATT failed status=$status addr=$address")
                connectingAddresses.remove(address)
                gattByAddress.remove(address)
                resetCheepSyncForDevice(address)
                activity.runOnUiThread {
                    Toast.makeText(activity, "Connection failed ($status)", Toast.LENGTH_SHORT).show()
                }
                gatt.close()
                return
            }

            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    connectingAddresses.remove(address)
                    gattByAddress[address] = gatt
                    cheepSyncByAddress.getOrPut(address) { CheepSync(windowSize = CheepSync.DEFAULT_WINDOW_SIZE) }

                    val name = gatt.device.name ?: "Unnamed"
                    activity.runOnUiThread {
                        Toast.makeText(activity, "Connected to $name", Toast.LENGTH_SHORT).show()
                        onConnected(name, address)
                    }
                    gatt.requestMtu(247)
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
                        @SuppressLint("MissingPermission")
                        gatt.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
                    }
                    gatt.discoverServices()
                }

                BluetoothProfile.STATE_DISCONNECTED -> {
                    resetCheepSyncForDevice(address)
                    gattByAddress.remove(address)
                    connectingAddresses.remove(address)
                    activity.runOnUiThread {
                        Toast.makeText(activity, "Disconnected $address", Toast.LENGTH_SHORT).show()
                        onDisconnected(address)
                    }
                    gatt.close()
                }

                else -> Log.w("BLE", "Unknown state: $newState")
            }
        }

        override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            if (characteristic.uuid != ESP32_CHAR_UUID) return

            @Suppress("DEPRECATION")
            val value = characteristic.value ?: return
            if (value.size < 12) return

            val address = addrKey(gatt.device.address)
            val devName = gatt.device.name ?: "Unnamed"

            try {
                val raw = EspPacket(
                    seq = u32LE(value, 0),
                    tUs = u64LE(value, 4),
                    receivedAtNs = SystemClock.elapsedRealtimeNanos(),
                    deviceAddress = address,
                    deviceName = devName
                )
                updateCheepSync(raw)
                val cs = cheepSyncByAddress[address]!!
                val packet = raw.copy(
                    syncedAtNs = cs.mapBeaconToReceiverNs(raw.tUs),
                    cheepSyncAlphaNs = cs.alpha,
                    cheepSyncBeta = cs.beta
                )
                activity.runOnUiThread { onPacketReceived(packet) }
            } catch (e: Exception) {
                Log.e("ESP32", "Decode error", e)
            }
        }

        override fun onDescriptorWrite(gatt: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) Log.e("BLE", "Descriptor write failed")
        }

        @RequiresPermission(PERMISSION_BLUETOOTH_CONNECT)
        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            super.onServicesDiscovered(gatt, status)
            if (status != BluetoothGatt.GATT_SUCCESS) return

            val address = addrKey(gatt.device.address)
            val devName = gatt.device.name ?: "Unnamed"

            gatt.getService(ESP32_SERVICE_UUID)?.getCharacteristic(ESP32_CHAR_UUID)?.let { tx ->
                gatt.setCharacteristicNotification(tx, true)
                tx.getDescriptor(CLIENT_CONFIG_DESCRIPTOR_UUID)?.let { cccd ->
                    writeClientConfigValue(gatt, cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                }

                val props = tx.properties
                val propsList = buildList {
                    if (props and BluetoothGattCharacteristic.PROPERTY_READ != 0) add("READ")
                    if (props and BluetoothGattCharacteristic.PROPERTY_WRITE != 0) add("WRITE")
                    if (props and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0) add("NOTIFY")
                    if (props and BluetoothGattCharacteristic.PROPERTY_INDICATE != 0) add("INDICATE")
                }.joinToString()

                val info = CharacteristicInfo(
                    ESP32_SERVICE_UUID,
                    standardServiceNames[ESP32_SERVICE_UUID] ?: "Environmental Sensing",
                    tx.uuid,
                    "ESP32 Sensor Data",
                    propsList,
                    deviceAddress = address,
                    deviceName = devName
                )

                activity.runOnUiThread { onCharacteristicsDiscovered(listOf(info)) }
            } ?: Log.e("BLE", "ESP32 characteristic not found")
        }

        override fun onCharacteristicRead(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                val address = addrKey(gatt.device.address)
                @Suppress("DEPRECATION")
                val bytes = characteristic.value
                readValues[readValueKey(address, characteristic.uuid)] =
                    bytes?.joinToString(" ") { it.toUByte().toString() } ?: "null"
            }
        }
    }

    @RequiresPermission(PERMISSION_BLUETOOTH_CONNECT)
    @SuppressLint("MissingPermission")
    private fun writeClientConfigValue(gatt: BluetoothGatt, descriptor: BluetoothGattDescriptor, value: ByteArray) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            gatt.writeDescriptor(descriptor, value)
        } else {
            @Suppress("DEPRECATION")
            runCatching {
                descriptor.javaClass.getMethod("setValue", ByteArray::class.java).invoke(descriptor, value)
                gatt.writeDescriptor(descriptor)
            }.onFailure { Log.e("BLE", "Legacy descriptor write failed", it) }
        }
    }

    @SuppressLint("MissingPermission")
    fun startBleScan() {
        if (!hasScanPermission()) {
            Toast.makeText(activity, "Scan permission not granted", Toast.LENGTH_SHORT).show()
            return
        }
        val scanner = bluetoothAdapter?.bluetoothLeScanner ?: run {
            Toast.makeText(activity, "BLE scanner unavailable", Toast.LENGTH_SHORT).show()
            return
        }
        scanner.startScan(
            null,
            ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(),
            bleScanCallback
        )
        bluetoothLeScanner = scanner
    }

    @SuppressLint("MissingPermission")
    fun stopBleScan() {
        if (!hasScanPermission()) return
        (bluetoothLeScanner ?: bluetoothAdapter?.bluetoothLeScanner)?.stopScan(bleScanCallback)
    }

    @RequiresPermission(PERMISSION_BLUETOOTH_CONNECT)
    @SuppressLint("MissingPermission")
    fun setNotificationsForCharacteristic(info: CharacteristicInfo, enable: Boolean): Boolean {
        val gatt = gattByAddress[addrKey(info.deviceAddress)] ?: return false
        val char = gatt.getService(info.serviceUuid)?.getCharacteristic(info.charUuid) ?: return false
        if (char.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY == 0) return false

        gatt.setCharacteristicNotification(char, enable)
        char.getDescriptor(CLIENT_CONFIG_DESCRIPTOR_UUID)?.let { desc ->
            writeClientConfigValue(
                gatt,
                desc,
                if (enable) BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                else BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE
            )
        }
        return true
    }

    @RequiresPermission(PERMISSION_BLUETOOTH_CONNECT)
    @SuppressLint("MissingPermission")
    fun readCharacteristicOnce(deviceAddress: String, charUuid: UUID, serviceUuid: UUID) {
        val gatt = gattByAddress[addrKey(deviceAddress)] ?: run {
            Log.e("BLE", "No GATT for $deviceAddress")
            return
        }
        gatt.getService(serviceUuid)?.getCharacteristic(charUuid)?.let { char ->
            val ok = gatt.readCharacteristic(char)
            Log.i("BLE", if (ok) "Read initiated" else "Read failed")
        } ?: Log.e("BLE", "Characteristic not found")
    }

    /** Connect to another BLE device; keeps existing connections. */
    @RequiresPermission(PERMISSION_BLUETOOTH_CONNECT)
    @SuppressLint("MissingPermission")
    fun connectToDevice(address: String) {
        if (!hasConnectPermission()) {
            Toast.makeText(activity, "Permission denied", Toast.LENGTH_SHORT).show()
            return
        }

        val key = addrKey(address)
        if (gattByAddress.containsKey(key)) {
            Toast.makeText(activity, "Already connected to this device", Toast.LENGTH_SHORT).show()
            return
        }
        if (key in connectingAddresses) {
            Toast.makeText(activity, "Connection already in progress", Toast.LENGTH_SHORT).show()
            return
        }

        stopBleScan()
        connectingAddresses.add(key)

        val device = bluetoothAdapter!!.getRemoteDevice(address)
        Handler(Looper.getMainLooper()).postDelayed({
            device.connectGatt(activity, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
            Toast.makeText(activity, "Connecting to $address", Toast.LENGTH_SHORT).show()
        }, 750)
    }

    /** Disconnect one board by MAC address. */
    @SuppressLint("MissingPermission")
    fun disconnectDevice(address: String) {
        val key = addrKey(address)
        val gatt = gattByAddress[key] ?: return
        try {
            gatt.disconnect()
        } catch (_: SecurityException) {
        }
    }

    /** Disconnect and close all GATT clients. */
    @SuppressLint("MissingPermission")
    fun disconnectAll() {
        for (addr in gattByAddress.keys.toList()) {
            disconnectDevice(addr)
        }
        connectingAddresses.clear()
    }

    /**
     * Briefly disables notifications on all connections, waits [stallAfterDisableMs], then re-enables.
     * ESP firmware resets its 1 Hz notify/LED phase on each re-enable so boards blink together without
     * extra write characteristics — only CCCD toggles.
     */
    @RequiresPermission(PERMISSION_BLUETOOTH_CONNECT)
    @SuppressLint("MissingPermission")
    fun resyncLedBlinkPhases(stallAfterDisableMs: Long = 220L) {
        if (!hasConnectPermission()) return
        val addresses = gattByAddress.keys.toList()
        if (addresses.size < 2) return

        for (addr in addresses) {
            val gatt = gattByAddress[addr] ?: continue
            val char = gatt.getService(ESP32_SERVICE_UUID)?.getCharacteristic(ESP32_CHAR_UUID) ?: continue
            if (char.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY == 0) continue
            gatt.setCharacteristicNotification(char, false)
            char.getDescriptor(CLIENT_CONFIG_DESCRIPTOR_UUID)?.let { desc ->
                writeClientConfigValue(gatt, desc, BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE)
            }
        }

        mainHandler.postDelayed({
            for (addr in addresses) {
                val gatt = gattByAddress[addr] ?: continue
                val char = gatt.getService(ESP32_SERVICE_UUID)?.getCharacteristic(ESP32_CHAR_UUID) ?: continue
                if (char.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY == 0) continue
                gatt.setCharacteristicNotification(char, true)
                char.getDescriptor(CLIENT_CONFIG_DESCRIPTOR_UUID)?.let { desc ->
                    writeClientConfigValue(gatt, desc, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                }
            }
            activity.runOnUiThread {
                Toast.makeText(activity, "LED notify phases re-aligned", Toast.LENGTH_SHORT).show()
            }
        }, stallAfterDisableMs)
    }
}
