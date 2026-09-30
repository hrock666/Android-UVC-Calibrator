#!/usr/bin/env python3
"""Display the Capture Calibration 24-patch test pattern.

The output intentionally contains no labels, borders, or UI overlays so the
center 50% ROI of every patch remains uncontaminated.
"""

from __future__ import annotations

import argparse
import ctypes
import sys
import tkinter as tk
from dataclasses import dataclass


@dataclass(frozen=True)
class Patch:
    name: str
    rgb: tuple[int, int, int]


PATCHES: tuple[Patch, ...] = (
    Patch("Black", (0, 0, 0)),
    Patch("Gray 10%", (26, 26, 26)),
    Patch("Gray 25%", (64, 64, 64)),
    Patch("Gray 50%", (128, 128, 128)),
    Patch("Gray 75%", (192, 192, 192)),
    Patch("Gray 90%", (230, 230, 230)),
    Patch("White", (255, 255, 255)),
    Patch("Red", (255, 0, 0)),
    Patch("Green", (0, 255, 0)),
    Patch("Blue", (0, 0, 255)),
    Patch("Cyan", (0, 255, 255)),
    Patch("Magenta", (255, 0, 255)),
    Patch("Yellow", (255, 255, 0)),
    Patch("Mid Red", (128, 0, 0)),
    Patch("Mid Green", (0, 128, 0)),
    Patch("Mid Blue", (0, 0, 128)),
    Patch("Mid Cyan", (0, 128, 128)),
    Patch("Mid Magenta", (128, 0, 128)),
    Patch("Mid Yellow", (128, 128, 0)),
    Patch("Low Red", (64, 0, 0)),
    Patch("Low Green", (0, 64, 0)),
    Patch("Low Blue", (0, 0, 64)),
    Patch("Gray 40%", (102, 102, 102)),
    Patch("Gray 60%", (153, 153, 153)),
)

ROWS = 4
COLUMNS = 6
REFERENCE_WIDTH = 1280
REFERENCE_HEIGHT = 720


@dataclass(frozen=True)
class Display:
    name: str
    left: int
    top: int
    right: int
    bottom: int
    primary: bool

    @property
    def width(self) -> int:
        return self.right - self.left

    @property
    def height(self) -> int:
        return self.bottom - self.top


def enable_windows_dpi_awareness() -> None:
    if sys.platform != "win32":
        return
    try:
        # Per-monitor DPI awareness keeps Tk geometry in physical pixels.
        ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    except (AttributeError, OSError):
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(2)
        except (AttributeError, OSError):
            pass


def enumerate_windows_displays() -> list[Display]:
    if sys.platform != "win32":
        return []

    class Rect(ctypes.Structure):
        _fields_ = [
            ("left", ctypes.c_long),
            ("top", ctypes.c_long),
            ("right", ctypes.c_long),
            ("bottom", ctypes.c_long),
        ]

    class MonitorInfo(ctypes.Structure):
        _fields_ = [
            ("cbSize", ctypes.c_ulong),
            ("rcMonitor", Rect),
            ("rcWork", Rect),
            ("dwFlags", ctypes.c_ulong),
            ("szDevice", ctypes.c_wchar * 32),
        ]

    displays: list[Display] = []
    user32 = ctypes.windll.user32
    user32.GetMonitorInfoW.argtypes = [ctypes.c_void_p, ctypes.POINTER(MonitorInfo)]
    user32.GetMonitorInfoW.restype = ctypes.c_int
    callback_type = ctypes.WINFUNCTYPE(
        ctypes.c_int,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.POINTER(Rect),
        ctypes.c_longlong,
    )

    def callback(monitor: int, _dc: int, _rect: ctypes.POINTER(Rect), _data: int) -> int:
        info = MonitorInfo()
        info.cbSize = ctypes.sizeof(MonitorInfo)
        if user32.GetMonitorInfoW(monitor, ctypes.byref(info)):
            rect = info.rcMonitor
            displays.append(
                Display(
                    name=info.szDevice,
                    left=rect.left,
                    top=rect.top,
                    right=rect.right,
                    bottom=rect.bottom,
                    primary=bool(info.dwFlags & 1),
                )
            )
        return 1

    callback_ref = callback_type(callback)
    user32.EnumDisplayMonitors(None, None, callback_ref, 0)
    return displays


def rgb_hex(rgb: tuple[int, int, int]) -> str:
    return "#{:02x}{:02x}{:02x}".format(*rgb)


class PatternWindow:
    def __init__(self, root: tk.Tk, *, fullscreen: bool, width: int, height: int,
                 display: Display | None) -> None:
        self.root = root
        self.fullscreen = fullscreen
        self.windowed_size = (width, height)
        self.display = display

        root.title("Capture Calibration 24 Patch Pattern")
        root.configure(background="black")
        root.bind("<Escape>", lambda _event: root.destroy())
        root.bind("<F11>", self.toggle_fullscreen)
        root.bind("<Configure>", self.redraw)

        self.canvas = tk.Canvas(
            root,
            background="black",
            borderwidth=0,
            highlightthickness=0,
            cursor="none",
        )
        self.canvas.pack(fill=tk.BOTH, expand=True)

        self.apply_window_mode()
        root.after_idle(self.redraw)

    def apply_window_mode(self) -> None:
        if self.fullscreen and self.display is not None:
            self.root.attributes("-fullscreen", False)
            self.root.overrideredirect(True)
            self.root.geometry(
                f"{self.display.width}x{self.display.height}"
                f"{self.display.left:+d}{self.display.top:+d}"
            )
            self.root.lift()
            self.root.focus_force()
        elif self.fullscreen:
            self.root.overrideredirect(False)
            self.root.attributes("-fullscreen", True)
        else:
            self.root.attributes("-fullscreen", False)
            self.root.overrideredirect(False)
            width, height = self.windowed_size
            self.root.geometry(f"{width}x{height}")

    def toggle_fullscreen(self, _event: tk.Event[tk.Misc] | None = None) -> None:
        self.fullscreen = not self.fullscreen
        self.apply_window_mode()

    def redraw(self, _event: tk.Event[tk.Misc] | None = None) -> None:
        width = self.canvas.winfo_width()
        height = self.canvas.winfo_height()
        if width <= 1 or height <= 1:
            return

        self.canvas.delete("all")
        for index, patch in enumerate(PATCHES):
            row, column = divmod(index, COLUMNS)
            # Integer boundaries cover every output pixel without gaps.
            left = column * width // COLUMNS
            right = (column + 1) * width // COLUMNS
            top = row * height // ROWS
            bottom = (row + 1) * height // ROWS
            color = rgb_hex(patch.rgb)
            self.canvas.create_rectangle(
                left,
                top,
                right,
                bottom,
                fill=color,
                outline=color,
                width=0,
            )


def parse_size(value: str) -> tuple[int, int]:
    try:
        width_text, height_text = value.lower().split("x", 1)
        width, height = int(width_text), int(height_text)
    except (ValueError, TypeError) as error:
        raise argparse.ArgumentTypeError("size must be WIDTHxHEIGHT") from error
    if width <= 0 or height <= 0:
        raise argparse.ArgumentTypeError("width and height must be positive")
    return width, height


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Display the 6x4 Capture Calibration test pattern."
    )
    parser.add_argument(
        "--display",
        type=int,
        default=1,
        metavar="N",
        help="1-based display number used for fullscreen output (default: 1)",
    )
    parser.add_argument(
        "--list-displays",
        action="store_true",
        help="list available displays and exit",
    )
    parser.add_argument(
        "--windowed",
        action="store_true",
        help="start in a window instead of fullscreen",
    )
    parser.add_argument(
        "--size",
        type=parse_size,
        default=(REFERENCE_WIDTH, REFERENCE_HEIGHT),
        metavar="WIDTHxHEIGHT",
        help="window size used with --windowed (default: 1280x720)",
    )
    args = parser.parse_args()

    enable_windows_dpi_awareness()
    displays = enumerate_windows_displays()
    if args.list_displays:
        if not displays:
            print("Display enumeration is only available on Windows.")
        for number, display in enumerate(displays, start=1):
            primary = " primary" if display.primary else ""
            print(
                f"{number}: {display.name} {display.width}x{display.height} "
                f"at ({display.left},{display.top}){primary}"
            )
        return

    if args.display < 1:
        parser.error("--display must be 1 or greater")
    selected_display: Display | None = None
    if displays:
        if args.display > len(displays):
            parser.error(
                f"--display {args.display} is unavailable; "
                f"detected {len(displays)} display(s)"
            )
        selected_display = displays[args.display - 1]

    root = tk.Tk()
    PatternWindow(
        root,
        fullscreen=not args.windowed,
        width=args.size[0],
        height=args.size[1],
        display=selected_display,
    )
    root.mainloop()


if __name__ == "__main__":
    main()
