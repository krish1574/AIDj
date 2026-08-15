package dj.ai.library

import android.Manifest
import android.content.ContentUris
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.provider.MediaStore
import androidx.core.content.ContextCompat
import expo.modules.kotlin.exception.CodedException
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition
import java.io.FileInputStream
import java.security.MessageDigest

class LibraryPermissionException :
  CodedException("PERMISSION_DENIED", "Permission to read audio files was not granted.", null)

class LibraryIoException(message: String) :
  CodedException("LIBRARY_IO_ERROR", message, null)

/**
 * Reads the device's audio library through MediaStore.
 *
 * MediaStore is used rather than a filesystem walk because it is the only
 * source that already holds parsed tags and album art, and because scoped
 * storage makes a raw walk of shared media impossible on modern Android
 * anyway. We never write to it, and we never touch the user's files.
 */
class AiDjLibraryModule : Module() {

  private val readPermission: String
    get() = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      Manifest.permission.READ_MEDIA_AUDIO
    } else {
      Manifest.permission.READ_EXTERNAL_STORAGE
    }

  private fun hasPermission(): Boolean {
    val context = appContext.reactContext ?: return false
    return ContextCompat.checkSelfPermission(context, readPermission) ==
      PackageManager.PERMISSION_GRANTED
  }

  override fun definition() = ModuleDefinition {
    Name("AiDjLibrary")

    Function("hasPermission") { hasPermission() }

    AsyncFunction("requestPermission") { promise: expo.modules.kotlin.Promise ->
      if (hasPermission()) {
        promise.resolve(true)
        return@AsyncFunction
      }
      appContext.permissions?.askForPermissions(
        { result ->
          val granted = result[readPermission]?.status ==
            expo.modules.interfaces.permissions.PermissionsStatus.GRANTED
          promise.resolve(granted)
        },
        readPermission
      ) ?: promise.resolve(false)
    }

    /**
     * Full library scan. Returns metadata only - no hashing, no decoding - so
     * that a several-thousand-track library lists in well under a second.
     * Tracks shorter than `minDurationMs` are skipped; ringtones, notification
     * sounds and voice memo fragments otherwise flood the library.
     */
    AsyncFunction("scan") { minDurationMs: Int ->
      if (!hasPermission()) throw LibraryPermissionException()

      val context = appContext.reactContext
        ?: throw LibraryIoException("No application context.")

      val collection = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
        MediaStore.Audio.Media.getContentUri(MediaStore.VOLUME_EXTERNAL)
      } else {
        MediaStore.Audio.Media.EXTERNAL_CONTENT_URI
      }

      val projection = arrayOf(
        MediaStore.Audio.Media._ID,
        MediaStore.Audio.Media.TITLE,
        MediaStore.Audio.Media.ARTIST,
        MediaStore.Audio.Media.ALBUM,
        MediaStore.Audio.Media.ALBUM_ID,
        MediaStore.Audio.Media.DURATION,
        MediaStore.Audio.Media.MIME_TYPE,
        MediaStore.Audio.Media.SIZE,
        MediaStore.Audio.Media.DATE_MODIFIED
      )

      val selection = "${MediaStore.Audio.Media.IS_MUSIC} != 0 AND " +
        "${MediaStore.Audio.Media.DURATION} >= ?"
      val selectionArgs = arrayOf(minDurationMs.toString())

      val results = mutableListOf<Map<String, Any?>>()

      context.contentResolver.query(
        collection,
        projection,
        selection,
        selectionArgs,
        "${MediaStore.Audio.Media.TITLE} COLLATE NOCASE ASC"
      )?.use { cursor ->
        val idColumn = cursor.getColumnIndexOrThrow(MediaStore.Audio.Media._ID)
        val titleColumn = cursor.getColumnIndexOrThrow(MediaStore.Audio.Media.TITLE)
        val artistColumn = cursor.getColumnIndexOrThrow(MediaStore.Audio.Media.ARTIST)
        val albumColumn = cursor.getColumnIndexOrThrow(MediaStore.Audio.Media.ALBUM)
        val albumIdColumn = cursor.getColumnIndexOrThrow(MediaStore.Audio.Media.ALBUM_ID)
        val durationColumn = cursor.getColumnIndexOrThrow(MediaStore.Audio.Media.DURATION)
        val mimeColumn = cursor.getColumnIndexOrThrow(MediaStore.Audio.Media.MIME_TYPE)
        val sizeColumn = cursor.getColumnIndexOrThrow(MediaStore.Audio.Media.SIZE)
        val modifiedColumn = cursor.getColumnIndexOrThrow(MediaStore.Audio.Media.DATE_MODIFIED)

        while (cursor.moveToNext()) {
          val id = cursor.getLong(idColumn)
          val albumId = cursor.getLong(albumIdColumn)

          // MediaStore reports "<unknown>" rather than null for missing tags.
          val artist = cursor.getString(artistColumn)
            ?.takeIf { it.isNotBlank() && it != MediaStore.UNKNOWN_STRING }
          val album = cursor.getString(albumColumn)
            ?.takeIf { it.isNotBlank() && it != MediaStore.UNKNOWN_STRING }

          results.add(
            mapOf(
              "mediaStoreId" to id,
              "contentUri" to ContentUris.withAppendedId(collection, id).toString(),
              "title" to (cursor.getString(titleColumn) ?: "Unknown title"),
              "artist" to artist,
              "album" to album,
              "durationMs" to cursor.getLong(durationColumn),
              "mimeType" to (cursor.getString(mimeColumn) ?: "audio/*"),
              "sizeBytes" to cursor.getLong(sizeColumn),
              // MediaStore stores DATE_MODIFIED in seconds, not milliseconds.
              "modifiedAtMs" to cursor.getLong(modifiedColumn) * 1000L,
              "artworkUri" to ContentUris.withAppendedId(
                Uri.parse("content://media/external/audio/albumart"),
                albumId
              ).toString()
            )
          )
        }
      } ?: throw LibraryIoException("MediaStore query returned no cursor.")

      results
    }

    /**
     * Content identity for one track: SHA-256 over the first and last 1 MiB
     * plus the byte length. Full-file hashing would read gigabytes across a
     * library; this is collision-safe enough for personal use and survives
     * moves and renames, which a content:// URI does not.
     *
     * Called lazily - only when a track first enters a playlist - because even
     * 2 MiB per file is too much I/O to spend on a full-library scan.
     */
    AsyncFunction("hashTrack") { uri: String ->
      val context = appContext.reactContext
        ?: throw LibraryIoException("No application context.")

      val descriptor = try {
        context.contentResolver.openFileDescriptor(Uri.parse(uri), "r")
      } catch (error: Exception) {
        null
      } ?: throw LibraryIoException("Cannot open $uri")

      descriptor.use { parcel ->
        val length = parcel.statSize
        if (length <= 0) throw LibraryIoException("Empty or unsized file.")

        val digest = MessageDigest.getInstance("SHA-256")
        val window = 1024 * 1024L
        val buffer = ByteArray(64 * 1024)

        FileInputStream(parcel.fileDescriptor).use { stream ->
          val channel = stream.channel

          fun digestRange(start: Long, count: Long) {
            channel.position(start)
            var remaining = count
            while (remaining > 0) {
              val wanted = minOf(remaining, buffer.size.toLong()).toInt()
              val read = stream.read(buffer, 0, wanted)
              if (read <= 0) break
              digest.update(buffer, 0, read)
              remaining -= read
            }
          }

          if (length <= window * 2) {
            digestRange(0, length)
          } else {
            digestRange(0, window)
            digestRange(length - window, window)
          }
        }

        digest.update(length.toString().toByteArray())
        digest.digest().joinToString("") { "%02x".format(it) }
      }
    }
  }
}
