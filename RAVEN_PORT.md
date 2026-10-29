# Raven Liquid Glass port (experimental)

This branch replaces Swappy's GTK3 application and Glade interface with [Raven](https://github.com/oscar-soirey/Raven), a native C GUI and Vulkan renderer. The annotation engine stays Cairo/Pango-based; image loading and encoding use `gdk-pixbuf` **without GTK**.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/swappy -f screenshot.png
# or: grim - | ./build/swappy -f -
```

The CMake build fetches a pinned Raven commit if it is not available through a CMake package. Install a C compiler, CMake, Ninja, Vulkan SDK headers/loader, X11 development libraries, Cairo, Pango, GLib, GIO, gdk-pixbuf, and Tesseract. Copying uses `wl-copy`, then `xclip` as fallback.

## UI

The painting controls float over the image as a refractive Raven glass panel. The window has a minimum width/height to hold the panel even when the original image is tiny, while the exported image contains **only the screenshot and annotations**, not the toolbar or background.

At startup, Swappy queries the cross-desktop `org.freedesktop.portal.Settings` portal's `org.freedesktop.appearance/color-scheme` value. Explicit dark chooses `R_THEME_GLASS_DARK` and tinted glass; explicit light chooses `R_THEME_GLASS_LIGHT` and frosted glass. When unavailable, it checks the GNOME GSettings key if installed, then falls back to light. Raven exposes theme selection but does not expose OS appearance detection.

## Platform caveat

As of the pinned Raven revision, the Linux platform implementation is X11, not native Wayland. On KDE Plasma/Hyprland Wayland the editor therefore needs XWayland, although PNG clipboard integration can use `wl-copy`. A native Wayland renderer/windowing backend would require further work in Raven itself.

## Known issues / validation

**Not yet build-tested or runtime-tested.** This is a substantial UI migration and needs compilation against Raven, functional testing, and visual polish before production use. The legacy Meson build is disabled rather than accidentally building GTK. The historic Glade resources and old selection test executable have been removed from the active build; the old Meson translation files remain for reference.

Recommended testing: small and large images; drawing and undo/redo; text editing; image crop and selection; clipboard on both Wayland and X11; PNG alpha handling; OCR; save-to-stdout; KDE dark/light switching at restart; high-DPI input coordinate mapping; rendering on XWayland.
