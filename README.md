# Recovery Console

A complete replacement for the standard Android Recovery UI. It provides a robust terminal environment allowing users to access an interactive shell or boot full Linux distributions via [Droidspaces](https://github.com/ravindu644/Droidspaces-OSS).

### Key Features

- **Backends**: Supports both modern Atomic DRM and legacy Framebuffer.
- **VT Aware**: Supports Virtual Terminals (Wayland/X11 co-existence), though stability varies by vendor DRM implementation.
- **Power Management**: Automatic display sleep and manual power-off via Power Key.
- **Intuitive Controls**: Volume buttons for scrolling and full physical keyboard support.
- **Low-Level**: Operates directly on top of the kernel, independent of Android framework services.

## ⚠️ No Universal Build!

This project is **not universal**. Because Android kernels vary wildly in how they handle display hardware (DRM vs FBdev), backlight sysfs paths, screen rotations, and notches, you **must fork this repository** and customize it for your specific device.

### Customization Guide

All device-specific logic is centralized in [`include/config.h`](./include/config.h). Before building, you must edit this file to match your hardware:

1.  **Backlight**: Update `BACKLIGHT_PATH` to your kernel's brightness control file.
2.  **Display**: Adjust `ROTATION` (0-3) and `MARGIN_TOP`/`BOTTOM`/`LEFT`/`RIGHT` to handle notches or UI safe areas.
3.  **Color Mode**: Toggle `COLOR_BGR` if your display colors appear swapped.
4.  **Backend**: The console will try [Atomic KMS](https://en.wikipedia.org/wiki/Direct_Rendering_Manager#Atomic_Display_Framework) first and fall back to legacy FrameBuffer (`/dev/fb0`) if needed.

## 🍳 Cooking

Once you have customized `config.h`, you can use the built-in GitHub CI to "cook" your binaries:

1.  **Fork** this repository.
2.  **Commit** your changes to `include/config.h`.
3.  Go to the **Actions** tab in your fork.
4.  Select the **Recovery Console CI** workflow.
5.  Click **Run workflow**, toggle **Create an Official GitHub Release**, and provide a tag name (e.g., `v1.0.0`).
6.  The CI will cross-compile for four architectures (`aarch64`, `armhf`, `x86_64`, `x86`) and upload a versioned tarball to your Releases.

## 💎 Credits & Acknowledgments

This project stands on the shoulders of several incredible open-source projects:

*   **[yaft (yet another framebuffer terminal)](https://github.com/uobikiemukot/yaft)**: The project's core foundation and framebuffer rendering logic.
*   **[st (simple terminal)](https://st.suckless.org/)**: The cursor engine and deferred-wrap logic.
*   **[TWRP (TeamWin Recovery Project)](https://twrp.me/)**: Display power management and DRM kickstart logic.
*   **[JetBrains Mono](https://www.jetbrains.com/lp/mono/)**: The default font, licensed under the [SIL Open Font License 1.1](https://openfontlicense.org).
*   **[FreeType](https://www.freetype.org/)**: Statically-linked font rendering engine.
*   **[Droidspaces](https://github.com/ravindu644/Droidspaces-OSS)**: For the cross-compilation toolchain and CI infrastructure.

---

### 🛠️ Disclaimer

This is a **fun project** and not a professional, production-ready tool. It was built with **heavy AI involvement** and may contain bugs, edge cases, or broken implementations on certain hardware. Use it at your own risk!

---
*Created with ❤️ for the Linux on Android Community.*
