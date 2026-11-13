package com.secpen.halinject

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import java.io.File
import java.io.FileOutputStream

object ImagePrep {

    // The app runs as untrusted_app and cannot write /data/local/tmp/hal_inject
    // (shell_data_file). So decode the picked image, re-encode to a clean JPEG in
    // the app's own cache dir, and return that path. The root daemon copies it
    // into place and generates NV21/NV12 (SET_IMAGE -> jpeg_to_yuv).
    fun prepare(ctx: Context, uri: Uri): String? {
        return try {
            val bmp = ctx.contentResolver.openInputStream(uri)?.use {
                BitmapFactory.decodeStream(it)
            } ?: return null
            val jpeg = File(ctx.cacheDir, "pick.jpg")
            FileOutputStream(jpeg).use { bmp.compress(Bitmap.CompressFormat.JPEG, 92, it) }
            bmp.recycle()
            jpeg.setReadable(true, false)   // let the root daemon's cp read it
            jpeg.absolutePath
        } catch (e: Exception) {
            null
        }
    }
}
