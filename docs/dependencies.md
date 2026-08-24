# Third-party dependencies and licenses

The headless kernel depends only on the platform C++ standard library at runtime.
Python-enabled builds additionally link the platform CPython runtime.

| Dependency | Purpose | License | Acquisition |
| --- | --- | --- | --- |
| Catch2 3.7.1 | C++ tests | Boost Software License 1.0 | Ubuntu package or pinned, SHA-256-verified source archive |
| Google Benchmark 1.9.1 | Native microbenchmarks | Apache License 2.0 | Ubuntu package or pinned, SHA-256-verified source archive |
| CPython 3.10+ | Trusted rule runtime | Python Software Foundation License 2.0 | Ubuntu package |
| pybind11 3.1.0 | C++/Python bindings and embedding | BSD 3-Clause | Installed package or pinned, SHA-256-verified source archive |
| GTK 4 | Native windowing and input | LGPL 2.1 or later | Ubuntu package, transitively through gtkmm development files |
| gtkmm 4 | Official C++ bindings for GTK 4 | LGPL 2.1 or later | Ubuntu `libgtkmm-4.0-dev` package |
| libepoxy 1.5+ | Portable OpenGL function dispatch | MIT | Ubuntu `libepoxy-dev` package |
| libpng 1.6+ | Bounded package PNG decoding | libpng License | Ubuntu `libpng-dev` package |
| Cairo | Software 2D renderer | LGPL 2.1 or MPL 1.1 | Supplied by GTK 4 development packages |
| Pango | Consistent UTF-8 text layout | LGPL 2.1 or later | Supplied by GTK 4 development packages |
| pytest 9.1.1 | Python SDK tests | MIT | Pinned development requirement |
| CMake 3.28+ | Build generation | BSD 3-Clause | Ubuntu package |
| Ninja | Build executor | Apache License 2.0 | Ubuntu package |

GTK/OpenGL dependencies are optional and discovered through `pkg-config`. If either
gtkmm 4 or libepoxy development metadata is absent, CMake leaves `ludus-gtk`,
`ludus-player`, and `ludus-studio` disabled while all headless targets remain
available. Set
`LUDUS_BUILD_GUI=OFF` to skip discovery explicitly. The OpenGL API is supplied by the
platform graphics stack through libepoxy; no proprietary SDK is required.

Use `cmake --preset gui` for a reproducible GUI build gate. That preset enables
`LUDUS_REQUIRE_GUI`, which turns missing `gtkmm-4.0`/`epoxy` metadata or disabled Python
game packages into a configure error instead of quietly producing a headless-only
build. The ordinary `dev` preset intentionally preserves optional discovery for
contributors working on kernel-only systems.

The GUI presents one `BoardCanvas` API while selecting exactly one active backend.
Automatic mode explicitly tries desktop OpenGL 3.3, OpenGL ES 3.0, then Cairo/Pango;
it does not rely on GTK's implicit EGL choice. Use `--renderer gl`, `gles`, or
`software` to force one path, and `--renderer-info` to print the selected API, vendor,
renderer, version, and fallback reason.

The Studio package reader uses a deliberately bounded TOML subset implemented in
`ludus-studio-core`; it does not add a third-party parser. Supported values are quoted
strings, signed integers, booleans, arrays of quoted strings, ordinary tables, and
array tables. Unknown schema keys and unsupported syntax are rejected with file, line,
and column diagnostics.

PNG is the supported package image format. The legacy PPM decoder remains only for
compatibility tests and old local overrides. Package visuals are optional; packages
without a theme continue to receive procedural defaults.
