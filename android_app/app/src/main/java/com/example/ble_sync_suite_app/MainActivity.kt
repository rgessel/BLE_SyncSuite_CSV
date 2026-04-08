package com.example.ble_sync_suite_app

// Main entry: permissions, navigation (welcome -> menu -> scanner -> data -> sync stats), BleManager wiring.

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.Modifier
import androidx.compose.material3.Surface
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshots.SnapshotStateList
import androidx.core.content.ContextCompat
import com.example.ble_sync_suite_app.ui.screens.DataDisplayScreen
import com.example.ble_sync_suite_app.ui.screens.GraphScreen
import com.example.ble_sync_suite_app.ui.screens.MainMenuScreen
import com.example.ble_sync_suite_app.ui.screens.MainScannerScreen
import com.example.ble_sync_suite_app.ui.screens.WelcomeScreen
import com.example.ble_sync_suite_app.ui.theme.BleSyncSuiteAppTheme
import kotlinx.coroutines.flow.asStateFlow

class MainActivity : ComponentActivity() {

    private var showWelcomeScreen by mutableStateOf(true)
    private var showMainMenu by mutableStateOf(false)
    private var showScannerScreen by mutableStateOf(false)
    private var showDataScreen by mutableStateOf(false)
    private var showGraphScreen by mutableStateOf(false)

    private var startScanWhenPermissionGranted = false
    private var isScanning by mutableStateOf(false)
    private var searchQuery by mutableStateOf("")
    private val scannedDevices: SnapshotStateList<String> = mutableStateListOf()
    private val connectedBoards: SnapshotStateList<ConnectedBoard> = mutableStateListOf()
    private val characteristicInfoList = mutableStateListOf<CharacteristicInfo>()
    /** All boards merged; capped at 5000 rows for memory. */
    private val esp32PacketHistory = mutableStateListOf<EspPacket>()
    private val _latestEspPacket = kotlinx.coroutines.flow.MutableStateFlow<EspPacket?>(null)
    val latestEspPacket = _latestEspPacket.asStateFlow()

    private val isAtLeastS get() = Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
    private fun permissionsToRequest(): Array<String> {
        val required = mutableListOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION)
        if (isAtLeastS) required += listOf(PERMISSION_BLUETOOTH_SCAN, PERMISSION_BLUETOOTH_CONNECT)
        return required.toTypedArray()
    }
    private fun hasConnectPermission(context: Context = this) =
        !isAtLeastS || ContextCompat.checkSelfPermission(context, PERMISSION_BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
    private fun hasScanPermission() =
        !isAtLeastS || ContextCompat.checkSelfPermission(this, PERMISSION_BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED

    private lateinit var bleManager: BleManager
    private val mainHandler = Handler(Looper.getMainLooper())
    /** Last [connectedBoards.size] we ran auto LED phase resync for (avoid duplicate toasts; re-run when count changes). */
    private var lastLedResyncForBoardCount: Int = 0

    private val permissionLauncher = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { results ->
        if (results.any { !it.value }) {
            Toast.makeText(this, "Some permissions were denied.", Toast.LENGTH_LONG).show()
            startScanWhenPermissionGranted = false
        } else {
            Toast.makeText(this, "All permissions granted!", Toast.LENGTH_SHORT).show()
            if (startScanWhenPermissionGranted && hasScanPermission()) {
                startScanWhenPermissionGranted = false
                isScanning = true
                bleManager.startBleScan()
            } else startScanWhenPermissionGranted = false
        }
    }

    private fun addrKey(addr: String) = addr.uppercase()

    @SuppressLint("MissingPermission")
    private fun shareMergedCsv() {
        val intent = CsvExport.buildShareIntent(this, esp32PacketHistory.toList()) ?: run {
            Toast.makeText(this, "Could not create CSV", Toast.LENGTH_SHORT).show()
            return
        }
        startActivity(Intent.createChooser(intent, "Export BLE sync CSV"))
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val bluetoothManager = getSystemService(BLUETOOTH_SERVICE) as? android.bluetooth.BluetoothManager
        val adapter = bluetoothManager?.adapter
        if (adapter == null) {
            Log.e("BLE", "Bluetooth not supported")
            Toast.makeText(this, "Bluetooth not supported", Toast.LENGTH_LONG).show()
            return
        }

        bleManager = BleManager(
            activity = this,
            hasScanPermission = { hasScanPermission() },
            hasConnectPermission = { hasConnectPermission() },
            onDeviceFound = { display -> if (!scannedDevices.contains(display)) scannedDevices.add(display) },
            onConnected = { name, address ->
                val board = ConnectedBoard(name, address)
                if (connectedBoards.none { addrKey(it.address) == addrKey(address) }) {
                    connectedBoards.add(board)
                }
                showScannerScreen = false
                showDataScreen = true
            },
            onDisconnected = { address ->
                val key = addrKey(address)
                val removed = connectedBoards.removeAll { addrKey(it.address) == key }
                if (removed) {
                    characteristicInfoList.removeAll { addrKey(it.deviceAddress) == key }
                    if (connectedBoards.isEmpty()) {
                        isScanning = false
                        showDataScreen = false
                        showGraphScreen = false
                        showScannerScreen = false
                        showMainMenu = true
                    }
                }
            },
            onCharacteristicsDiscovered = { list ->
                if (list.isNotEmpty()) {
                    val key = addrKey(list.first().deviceAddress)
                    characteristicInfoList.removeAll { addrKey(it.deviceAddress) == key }
                    characteristicInfoList.addAll(list)
                    if (connectedBoards.size >= 2 && connectedBoards.size != lastLedResyncForBoardCount) {
                        lastLedResyncForBoardCount = connectedBoards.size
                        mainHandler.postDelayed({
                            try {
                                bleManager.resyncLedBlinkPhases(stallAfterDisableMs = 220L)
                            } catch (e: SecurityException) {
                                Log.e("BLE", "LED phase resync rejected", e)
                            }
                        }, 400L)
                    }
                }
            },
            onPacketReceived = { packet ->
                _latestEspPacket.value = packet
                esp32PacketHistory.add(packet)
                if (esp32PacketHistory.size > 5000) esp32PacketHistory.removeAt(0)
            }
        )

        setContent {
            BleSyncSuiteAppTheme {
                Surface(color = MaterialTheme.colorScheme.background, modifier = Modifier.fillMaxSize()) {
                    val connectedAddresses = connectedBoards.map { addrKey(it.address) }.toSet()
                    when {
                        showWelcomeScreen -> WelcomeScreen { showWelcomeScreen = false; showMainMenu = true }
                        showGraphScreen -> GraphScreen(
                            onBack = { showGraphScreen = false },
                            packets = esp32PacketHistory.toList(),
                            cheepSyncAlphaByDevice = bleManager.cheepSyncAlphaByDevice,
                            cheepSyncBetaByDevice = bleManager.cheepSyncBetaByDevice
                        )
                        showDataScreen -> DataDisplayScreen(
                            connectedBoards = connectedBoards.toList(),
                            latestEspPacket = latestEspPacket,
                            characteristicInfoList = characteristicInfoList,
                            onSyncLedPhases = {
                                if (hasConnectPermission()) {
                                    try {
                                        bleManager.resyncLedBlinkPhases(stallAfterDisableMs = 220L)
                                    } catch (e: SecurityException) {
                                        Log.e("BLE", "LED phase resync rejected", e)
                                    }
                                } else Toast.makeText(this@MainActivity, "Bluetooth permission not granted", Toast.LENGTH_SHORT).show()
                            },
                            onBack = {
                                showDataScreen = false
                                showGraphScreen = false
                                if (hasScanPermission()) bleManager.stopBleScan()
                                isScanning = false
                                bleManager.disconnectAll()
                                connectedBoards.clear()
                                characteristicInfoList.clear()
                                esp32PacketHistory.clear()
                                _latestEspPacket.value = null
                                lastLedResyncForBoardCount = 0
                                showMainMenu = true
                            },
                            onAddDevice = {
                                showDataScreen = false
                                showScannerScreen = true
                            },
                            onExportCsv = { shareMergedCsv() },
                            hasConnectPermission = { context -> this@MainActivity.hasConnectPermission(context) },
                            readCharacteristicOnce = { deviceAddress, charUuid, serviceUuid ->
                                try {
                                    bleManager.readCharacteristicOnce(deviceAddress, charUuid, serviceUuid)
                                } catch (e: SecurityException) {
                                    Log.e("BLE", "Read rejected", e)
                                }
                            },
                            setNotificationsForCharacteristic = { info, enable ->
                                try {
                                    bleManager.setNotificationsForCharacteristic(info, enable)
                                } catch (e: SecurityException) {
                                    Log.e("BLE", "Notify rejected", e); false
                                }
                            },
                            onOpenGraph = { showGraphScreen = true },
                            onNotifyEnabledOpenGraph = { _ -> showGraphScreen = true }
                        )
                        showScannerScreen -> MainScannerScreen(
                            isScanning = isScanning,
                            searchQuery = searchQuery,
                            scannedDevices = scannedDevices,
                            connectedAddresses = connectedAddresses,
                            onBack = {
                                bleManager.stopBleScan()
                                isScanning = false
                                showScannerScreen = false
                                if (connectedBoards.isNotEmpty()) showDataScreen = true
                                else showMainMenu = true
                            },
                            onScanToggle = { toggled ->
                                if (toggled) {
                                    scannedDevices.clear()
                                    ensureScanPermission {
                                        isScanning = true
                                        bleManager.startBleScan()
                                    }
                                    if (!hasScanPermission()) isScanning = false
                                } else {
                                    isScanning = false
                                    bleManager.stopBleScan()
                                    scannedDevices.clear()
                                }
                            },
                            onQueryChanged = { searchQuery = it },
                            onDeviceClick = { address, name ->
                                if (hasConnectPermission()) {
                                    try {
                                        bleManager.connectToDevice(address)
                                    } catch (e: SecurityException) {
                                        Log.e("BLE", "Connect rejected", e)
                                        Toast.makeText(this, "Unable to connect without Bluetooth permission", Toast.LENGTH_SHORT).show()
                                    }
                                } else Toast.makeText(this, "Bluetooth permission not granted", Toast.LENGTH_SHORT).show()
                            }
                        )
                        else -> MainMenuScreen { showMainMenu = false; showScannerScreen = true }
                    }
                }
            }
        }

        requestPermissionsModernWay()
    }

    private fun requestPermissionsModernWay() {
        val needed = permissionsToRequest().filter { ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED }
        if (needed.isNotEmpty()) permissionLauncher.launch(needed.toTypedArray())
        else Toast.makeText(this, "All permissions already granted!", Toast.LENGTH_SHORT).show()
    }

    private fun ensureScanPermission(onGranted: () -> Unit) {
        if (!isAtLeastS || hasScanPermission()) onGranted()
        else {
            startScanWhenPermissionGranted = true
            permissionLauncher.launch(arrayOf(PERMISSION_BLUETOOTH_SCAN))
            Toast.makeText(this, "Scan permission is required to discover devices.", Toast.LENGTH_SHORT).show()
        }
    }
}
