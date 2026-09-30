package com.hev.uvccapturecalibration

internal object ProfileFileNaming {

    fun build(
        vendorId: Int,
        productId: Int,
        serial: String?,
        width: Int,
        height: Int,
        frameRate: Int,
        colorimetry: String,
        transferCharacteristics: String,
        inputEncoding: String,
        range: String,
        timestampUtc: String? = null
    ): String = buildString {
        append("UVC_%04X_%04X".format(vendorId, productId))
        sanitizeToken(serial)?.let { append("_").append(it) }
        append("_").append(width).append("x").append(height).append("p").append(frameRate)
        append("_").append(requireToken(colorimetry))
        append("_").append(requireToken(transferCharacteristics))
        append("_").append(requireToken(inputEncoding))
        append("_").append(requireToken(range))
        sanitizeToken(timestampUtc)?.let { append("_").append(it) }
        append(".json")
    }

    private fun requireToken(value: String): String =
        requireNotNull(sanitizeToken(value)) { "Profile filename token must not be blank" }

    private fun sanitizeToken(value: String?): String? = value
        ?.trim()
        ?.takeIf { it.isNotEmpty() }
        ?.replace(Regex("[^A-Za-z0-9.-]"), "_")
        ?.take(64)
}
