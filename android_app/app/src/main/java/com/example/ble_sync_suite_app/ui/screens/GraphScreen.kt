package com.example.ble_sync_suite_app.ui.screens

import com.example.ble_sync_suite_app.EspPacket
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.flow.StateFlow
import kotlin.math.abs

// Sync statistics: CheepSync fit per board; pick a board when multiple connections are active.

@Composable
fun GraphScreen(
    onBack: () -> Unit,
    packets: List<EspPacket>,
    cheepSyncAlphaByDevice: StateFlow<Map<String, Double>>,
    cheepSyncBetaByDevice: StateFlow<Map<String, Double>>
) {
    val alphaMap by cheepSyncAlphaByDevice.collectAsState()
    val betaMap by cheepSyncBetaByDevice.collectAsState()

    val devices = remember(packets) {
        packets.map { it.deviceAddress }.filter { it.isNotBlank() }.distinct().sorted()
    }
    var selectedDevice by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(devices) {
        if (selectedDevice.isNullOrBlank() || selectedDevice !in devices) {
            selectedDevice = devices.firstOrNull()
        }
    }

    val filtered = remember(packets, selectedDevice) {
        val sel = selectedDevice
        if (sel.isNullOrBlank()) packets
        else packets.filter { it.deviceAddress == sel }
    }

    val alpha = selectedDevice?.let { alphaMap[it] } ?: 0.0
    val beta = selectedDevice?.let { betaMap[it] } ?: 1.0

    Column(modifier = Modifier.fillMaxSize().padding(24.dp)) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            TextButton(onClick = onBack) { Text("Back") }
            Text("Sync statistics", fontSize = 24.sp)
        }
        Spacer(Modifier.height(16.dp))

        if (devices.size > 1) {
            var menuExpanded by remember { mutableStateOf(false) }
            Box(Modifier.fillMaxWidth()) {
                TextButton(onClick = { menuExpanded = true }) {
                    Text("Board: ${selectedDevice ?: "—"}")
                }
                DropdownMenu(expanded = menuExpanded, onDismissRequest = { menuExpanded = false }) {
                    devices.forEach { addr ->
                        DropdownMenuItem(
                            text = { Text(addr) },
                            onClick = {
                                selectedDevice = addr
                                menuExpanded = false
                            }
                        )
                    }
                }
            }
            Spacer(Modifier.height(12.dp))
        }

        if (filtered.isEmpty()) {
            Text("No data yet. Waiting for ESP32 packets...", color = Color.Gray)
            return@Column
        }

        if (filtered.size >= 2) {
            val first = filtered.first()
            val residualsMs = filtered.map { p ->
                val Tr = p.receivedAtNs.toDouble()
                val tb = p.tUs * 1000.0
                (Tr - (alpha + beta * tb)) / 1_000_000.0
            }
            val meanAbsResidualMs = residualsMs.map { abs(it) }.average()
            val latestResidualMs = residualsMs.last()
            val clockSkew = beta - 1.0
            val droppedPacketCount = run {
                var valid = 1
                for (i in 1 until filtered.size) {
                    val prev = filtered[i - 1].seq
                    val curr = filtered[i].seq
                    if (curr == prev + 1L || (prev == 0xFFFF_FFFFL && curr == 0L)) valid++
                }
                filtered.size - valid
            }
            val intervalsUs = (1 until filtered.size).map { filtered[it].tUs - filtered[it - 1].tUs }.filter { it > 0 }
            val transmissionRateMs = if (intervalsUs.isNotEmpty()) intervalsUs.map { it.toDouble() }.average() / 1000.0 else 0.0
            val latest = filtered.last()
            val tUsDeltaMs = (latest.tUs - first.tUs) / 1000.0
            val rxDeltaMs = (latest.receivedAtNs - first.receivedAtNs) / 1_000_000.0

            Column(
                modifier = Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).background(Color(0xFF111111), RoundedCornerShape(8.dp)).padding(12.dp)
            ) {
                selectedDevice?.let { Text("Device: $it", fontSize = 12.sp, color = Color(0xFFB0BEC5)) }
                Spacer(Modifier.height(8.dp))
                Text("CheepSync Fit Metrics:", fontSize = 14.sp, fontWeight = FontWeight.Bold, color = Color.White)
                Text("  alpha (ns): ${"%.0f".format(alpha)}", fontSize = 12.sp, color = Color.White)
                Text("  beta (unitless): ${"%.9f".format(beta)}", fontSize = 12.sp, color = Color.White)
                Text("  skew = beta-1: ${"%.9f".format(clockSkew)}", fontSize = 12.sp, color = Color.White)
                Spacer(Modifier.height(8.dp))
                Text("Residual (sync error):", fontSize = 14.sp, fontWeight = FontWeight.Bold, color = Color.White)
                Text("  Mean |residual|: ${"%.3f".format(meanAbsResidualMs)} ms", fontSize = 12.sp, color = Color.White)
                Text("  Latest residual: ${"%.3f".format(latestResidualMs)} ms", fontSize = 12.sp, color = Color.White)
                Spacer(Modifier.height(8.dp))
                Text("Packet Statistics:", fontSize = 14.sp, fontWeight = FontWeight.Bold, color = Color.White)
                Text("  Total packets: ${filtered.size}", fontSize = 12.sp, color = Color.White)
                Text("  Dropped packets (seq gaps): $droppedPacketCount", fontSize = 12.sp, color = Color.White)
                Spacer(Modifier.height(8.dp))
                Text("Transmission Rate:", fontSize = 14.sp, fontWeight = FontWeight.Bold, color = Color.White)
                Text("  Avg interval: ${"%.2f".format(transmissionRateMs)} ms", fontSize = 12.sp, color = Color.White)
                if (transmissionRateMs > 0) Text("  Rate: ${"%.2f".format(1000.0 / transmissionRateMs)} packets/sec", fontSize = 12.sp, color = Color.White)
                Spacer(Modifier.height(8.dp))
                Text("Time Spans:", fontSize = 14.sp, fontWeight = FontWeight.Bold, color = Color.White)
                Text("  ESP32 span: ${"%.1f".format(tUsDeltaMs)} ms", fontSize = 12.sp, color = Color(0xFF90CAF9))
                Text("  Android Rx span: ${"%.1f".format(rxDeltaMs)} ms", fontSize = 12.sp, color = Color(0xFFA5D6A7))
            }
        } else {
            Text("Waiting for more packets to calculate sync metrics...", fontSize = 12.sp, color = Color.Gray)
        }
    }
}
