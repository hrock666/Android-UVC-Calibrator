package com.hev.uvccapturecalibration

import android.content.Context
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.ViewConfiguration
import android.widget.ScrollView
import kotlin.math.abs

class ResultScrollView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : ScrollView(context, attrs) {

    private val touchSlop = ViewConfiguration.get(context).scaledTouchSlop
    private var downX = 0f
    private var downY = 0f
    private var directionDecided = false

    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = event.x
                downY = event.y
                directionDecided = false
                parent.requestDisallowInterceptTouchEvent(true)
            }

            MotionEvent.ACTION_MOVE -> {
                val deltaX = event.x - downX
                val deltaY = event.y - downY
                if (!directionDecided &&
                    (abs(deltaX) > touchSlop || abs(deltaY) > touchSlop)
                ) {
                    directionDecided = true
                    if (abs(deltaX) > abs(deltaY)) {
                        parent.requestDisallowInterceptTouchEvent(false)
                    }
                }
                if (directionDecided && abs(deltaY) >= abs(deltaX)) {
                    val scrollDirection = if (deltaY < 0f) 1 else -1
                    parent.requestDisallowInterceptTouchEvent(
                        canScrollVertically(scrollDirection)
                    )
                }
            }

            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL ->
                parent.requestDisallowInterceptTouchEvent(false)
        }
        return super.dispatchTouchEvent(event)
    }
}
