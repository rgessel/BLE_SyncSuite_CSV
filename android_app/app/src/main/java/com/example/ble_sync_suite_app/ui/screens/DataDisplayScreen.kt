package com.example.ble_sync_suite_app.ui.screens

import com.example.ble_sync_suite_app.CharacteristicInfo
import com.example.ble_sync_suite_app.ConnectedBoard
import com.example.ble_sync_suite_app.ESP32_CHAR_UUID
import com.example.ble_sync_suite_app.readValueKey
import com.example.ble_sync_suite_app.readValues
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.relocation.BringIntoViewRequester
import androidx.compose.foundation.relocation.bringIntoViewRequester
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import android.content.Context
import android.util.Log
import android.widget.Toast
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import java.util.UUID

// Screen after connecting: merged packet view, characteristics per board, CSV export, add another board.

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun DataDisplayScreen(
    connectedBoards: List<ConnectedBoard>,
    latestEspPacket: StateFlow<com.example.ble_sync_suite_app.EspPacket?>,
    characteristicInfoList: List<CharacteristicInfo>,
    /** CCCD off → stall → on all boards so ESP LEDs share the same 1 Hz phase (no extra GATT payload). */
    onSyncLedPhases: () -> Unit,
    onBack: () -> Unit,
    onAddDevice: () -> Unit,
    onExportCsv: () -> Unit,
    hasConnectPermission: (Context) -> Boolean,
    readCharacteristicOnce: (deviceAddress: String, charUuid: UUID, serviceUuid: UUID) -> Unit,
    setNotificationsForCharacteristic: (CharacteristicInfo, Boolean) -> Boolean,
    onOpenGraph: () -> Unit,
    onNotifyEnabledOpenGraph: (CharacteristicInfo) -> Unit
) {
    var visible by remember { mutableStateOf(false) }
    /** Which characteristic row is expanded: UUID + device MAC (same UUID can exist on multiple boards). */
    var expandedChar by remember { mutableStateOf<Pair<UUID, String>?>(null) }
    val context = LocalContext.current
    val listState = rememberLazyListState()
    val notificationStates = remember { mutableStateMapOf<String, Boolean>() }
    val alpha by animateFloatAsState(if (visible) 1f else 0f, tween(1000), label = "fadeIn")
    val latestPacket by latestEspPacket.collectAsState()

    fun notifyKey(info: CharacteristicInfo) = "${info.deviceAddress}|${info.charUuid}"

    LaunchedEffect(Unit) {
        visible = true
        listState.scrollToItem(0)
    }

    Column(
        modifier = Modifier.fillMaxSize().alpha(alpha).padding(start = 24.dp, end = 24.dp, top = 36.dp, bottom = 48.dp)
    ) {
        Box(Modifier.fillMaxWidth()) {
            TextButton(onClick = onBack, modifier = Modifier.align(Alignment.CenterStart)) { Text("Back to Menu") }
            Text(
                if (connectedBoards.size <= 1) {
                    "Connected: ${connectedBoards.firstOrNull()?.name ?: "—"}"
                } else {
                    "${connectedBoards.size} boards: " + connectedBoards.joinToString { it.name }
                },
                fontSize = 20.sp,
                textAlign = TextAlign.End,
                modifier = Modifier.align(Alignment.CenterEnd)
            )
        }
        Spacer(Modifier.height(12.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Button(onClick = onAddDevice) { Text("Add device") }
            Button(onClick = onExportCsv) { Text("Export CSV") }
            if (connectedBoards.size >= 2) {
                Button(onClick = onSyncLedPhases) { Text("Sync LED phases") }
            }
        }
        Spacer(Modifier.height(16.dp))

        if (latestPacket != null) {
            Column(
                modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp).background(Color(0xFFE3F2FD), RoundedCornerShape(8.dp)).padding(16.dp)
            ) {
                Text("Latest packet (${latestPacket!!.deviceName})", fontSize = 18.sp, color = Color.Black)
                Spacer(Modifier.height(8.dp))
                Text("seq: ${latestPacket!!.seq}", fontSize = 14.sp, color = Color.Black)
                Text("tUs: ${latestPacket!!.tUs}", fontSize = 14.sp, color = Color.Black)
                Text("receivedAtNs: ${latestPacket!!.receivedAtNs}", fontSize = 14.sp, color = Color.Black)
                Text("syncedAtNs (CheepSync): ${latestPacket!!.syncedAtNs}", fontSize = 14.sp, color = Color.Black)
                Text("CheepSync α (ns): ${latestPacket!!.cheepSyncAlphaNs}  β: ${latestPacket!!.cheepSyncBeta}", fontSize = 14.sp, color = Color.Black)
            }
            Spacer(Modifier.height(8.dp))
        }

        LazyColumn(state = listState) {
            items(characteristicInfoList) { info ->
                val bringIntoViewRequester = remember { BringIntoViewRequester() }
                val scope = rememberCoroutineScope()
                val isNotificationActive = notificationStates[notifyKey(info)] == true
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .bringIntoViewRequester(bringIntoViewRequester)
                        .clickable {
                            if (info.charUuid == ESP32_CHAR_UUID) onOpenGraph()
                            else {
                                val key = info.charUuid to info.deviceAddress
                                expandedChar = if (expandedChar == key) null else key
                                scope.launch { bringIntoViewRequester.bringIntoView() }
                            }
                        }
                        .padding(16.dp)
                ) {
                    Text("${info.deviceName} [${info.deviceAddress}]")
                    Text("Characteristic: ${info.charName}")
                    Text("Properties: ${info.properties}")
                    Text("UUID: ${info.charUuid}", fontSize = 12.sp, color = Color.Gray)
                    if (expandedChar == (info.charUuid to info.deviceAddress)) {
                        Column(Modifier.padding(top = 12.dp)) {
                            Button(onClick = {
                                if (hasConnectPermission(context)) {
                                    try {
                                        readCharacteristicOnce(info.deviceAddress, info.charUuid, info.serviceUuid)
                                    } catch (e: SecurityException) {
                                        Log.e("BLE", "Read rejected", e)
                                        Toast.makeText(context, "Unable to read without Bluetooth permission", Toast.LENGTH_SHORT).show()
                                    }
                                } else Toast.makeText(context, "Bluetooth permission not granted", Toast.LENGTH_SHORT).show()
                            }) { Text("Read") }
                            Button(onClick = {
                                if (!hasConnectPermission(context)) {
                                    Toast.makeText(context, "Bluetooth permission not granted", Toast.LENGTH_SHORT).show()
                                    return@Button
                                }
                                try {
                                    val key = notifyKey(info)
                                    val updated = if (isNotificationActive) {
                                        setNotificationsForCharacteristic(info, false).also { if (it) notificationStates[key] = false }
                                    } else {
                                        setNotificationsForCharacteristic(info, true).also {
                                            if (it) {
                                                notificationStates[key] = true
                                                onNotifyEnabledOpenGraph(info)
                                            }
                                        }
                                    }
                                    if (!updated) Toast.makeText(context, "Unable to update notifications", Toast.LENGTH_SHORT).show()
                                } catch (e: SecurityException) {
                                    Log.e("BLE", "Notification toggle rejected", e)
                                    Toast.makeText(context, "OS rejected notification request", Toast.LENGTH_SHORT).show()
                                }
                            }) { Text(if (isNotificationActive) "Stop Notify" else "Notify") }
                            Text(
                                "Last Value: ${readValues[readValueKey(info.deviceAddress, info.charUuid)] ?: "—"}",
                                fontSize = 14.sp
                            )
                        }
                    }
                }
            }
        }
    }
}
