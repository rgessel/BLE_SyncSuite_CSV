package com.example.ble_sync_suite_app

import android.content.Context
import android.content.Intent
import android.os.Environment
import androidx.core.content.FileProvider
import java.io.File

/**
 * Single merged CSV for all boards: one row per sample, sorted by receive time.
 */
object CsvExport {

    fun buildCsv(packets: List<EspPacket>): String {
        val rows = packets
            .sortedBy { it.receivedAtNs }
            .map { p ->
                listOf(
                    escapeCsv(p.deviceAddress),
                    escapeCsv(p.deviceName),
                    p.seq.toString(),
                    p.tUs.toString(),
                    p.receivedAtNs.toString()
                ).joinToString(",")
            }
        val header = "device_address,device_name,seq,t_us,received_at_ns"
        return (listOf(header) + rows).joinToString("\n")
    }

    private fun escapeCsv(s: String): String {
        if (s.contains(',') || s.contains('"') || s.contains('\n') || s.contains('\r')) {
            return '"' + s.replace("\"", "\"\"") + '"'
        }
        return s
    }

    /**
     * Writes app-private Documents file and returns a share [Intent], or null on failure.
     */
    fun buildShareIntent(context: Context, packets: List<EspPacket>): Intent? {
        val dir = context.getExternalFilesDir(Environment.DIRECTORY_DOCUMENTS) ?: return null
        val name = "ble_sync_${System.currentTimeMillis()}.csv"
        val file = File(dir, name)
        return try {
            file.writeText(buildCsv(packets))
            val uri = FileProvider.getUriForFile(
                context,
                "${context.packageName}.fileprovider",
                file
            )
            Intent(Intent.ACTION_SEND).apply {
                type = "text/csv"
                putExtra(Intent.EXTRA_STREAM, uri)
                putExtra(Intent.EXTRA_SUBJECT, name)
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
        } catch (_: Exception) {
            null
        }
    }
}
