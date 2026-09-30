package com.hev.uvccapturecalibration

import org.junit.Assert.assertEquals
import org.junit.Test

class ProfileFileNamingTest {

    @Test
    fun internalNameContainsCaptureAndInputContract() {
        assertEquals(
            "UVC_334D_2130_1280x720p60_BT709_SDR_RGB_FULL.json",
            ProfileFileNaming.build(
                vendorId = 0x334D,
                productId = 0x2130,
                serial = null,
                width = 1280,
                height = 720,
                frameRate = 60,
                colorimetry = "BT709",
                transferCharacteristics = "SDR",
                inputEncoding = "RGB",
                range = "FULL"
            )
        )
    }

    @Test
    fun exportNameSanitizesSerialAndIncludesTimestamp() {
        assertEquals(
            "UVC_334D_2130_SN_12_34_1280x720p60_BT2020_PQ_YCBCR_422_LIMITED_20260930T173500Z.json",
            ProfileFileNaming.build(
                vendorId = 0x334D,
                productId = 0x2130,
                serial = "SN 12/34",
                width = 1280,
                height = 720,
                frameRate = 60,
                colorimetry = "BT2020",
                transferCharacteristics = "PQ",
                inputEncoding = "YCBCR_422",
                range = "LIMITED",
                timestampUtc = "20260930T173500Z"
            )
        )
    }
}
