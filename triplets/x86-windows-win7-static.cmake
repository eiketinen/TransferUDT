# Windows 7 SP1 (x86) static triplet for the Agent's dependencies.
# Same as the stock x86-windows-static triplet, but injects the Win7 down-level
# targeting macros so spdlog/openssl/fmt/sqlite3/udt are built without Win8+
# code paths. See docs/rfcs/0002-windows7-agent-support.md.
set(VCPKG_TARGET_ARCHITECTURE x86)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_PLATFORM_TOOLSET v143)

set(_win7_defines "/D_WIN32_WINNT=0x0601 /DWINVER=0x0601 /DNTDDI_VERSION=0x06010000")
set(VCPKG_C_FLAGS "${VCPKG_C_FLAGS} ${_win7_defines}")
set(VCPKG_CXX_FLAGS "${VCPKG_CXX_FLAGS} ${_win7_defines}")
