package com.hev.uvccapturecalibration

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.util.AttributeSet
import android.view.View

class PreviewGuideView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {

    private val cellPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(150, 255, 255, 255)
        style = Paint.Style.STROKE
        strokeWidth = resources.displayMetrics.density
    }
    private val roiPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(210, 55, 191, 208)
        style = Paint.Style.STROKE
        strokeWidth = resources.displayMetrics.density * 1.5f
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val cellWidth = width / 6f
        val cellHeight = height / 4f
        for (column in 0..6) {
            val x = column * cellWidth
            canvas.drawLine(x, 0f, x, height.toFloat(), cellPaint)
        }
        for (row in 0..4) {
            val y = row * cellHeight
            canvas.drawLine(0f, y, width.toFloat(), y, cellPaint)
        }
        for (row in 0 until 4) {
            for (column in 0 until 6) {
                val left = column * cellWidth + cellWidth * 0.25f
                val top = row * cellHeight + cellHeight * 0.25f
                val right = (column + 1) * cellWidth - cellWidth * 0.25f
                val bottom = (row + 1) * cellHeight - cellHeight * 0.25f
                canvas.drawRect(left, top, right, bottom, roiPaint)
            }
        }
    }
}
