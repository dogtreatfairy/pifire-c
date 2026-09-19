cmake_minimum_required(VERSION 3.22)
project(pifire VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(PF_WITH_MQTT   "Build MQTT/Home Assistant integration (libmosquitto)" ON)
option(PF_WITH_CURL   "Build webhook delivery (libcurl)" ON)
option(PF_WITH_BLE    "Build BlueZ D-Bus probe support (libsystemd sd-bus)" ON)
option(PF_SANITIZE    "Enable ASan/UBSan (dev builds)" OFF)
option(PF_BUILD_TESTS "Build unit tests" ON)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET sqlite3)

set(PF_OPTIONAL_LIBS "")
if(PF_WITH_MQTT)
  pkg_check_modules(MOSQ REQUIRED IMPORTED_TARGET libmosquitto)
  list(APPEND PF_OPTIONAL_LIBS PkgConfig::MOSQ)
endif()
if(PF_WITH_CURL)
  pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
  list(APPEND PF_OPTIONAL_LIBS PkgConfig::CURL)
endif()
if(PF_WITH_BLE)
  pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
  list(APPEND PF_OPTIONAL_LIBS PkgConfig::SYSTEMD)
endif()

# ---- warnings for our own code only -----------------------------------------
set(PF_WARN -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes
            -Wno-unused-parameter -Wformat=2 -Wno-format-nonliteral)
if(PF_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---- third party (no -Werror) -----------------------------------------------
add_library(civetweb STATIC third_party/civetweb/civetweb.c)
target_include_directories(civetweb PUBLIC third_party/civetweb)
target_compile_definitions(civetweb PUBLIC
  NO_SSL NO_CGI NO_CACHING NO_FILES USE_WEBSOCKET USE_IPV6)
target_compile_options(civetweb PRIVATE -w)
target_link_libraries(civetweb PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

add_library(cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cjson)
target_compile_options(cjson PRIVATE -w)

# ---- embedded assets ---------------------------------------------------------
add_executable(bin2c tools/bin2c.c)

set(PF_EMBED_INPUTS
  ${CMAKE_SOURCE_DIR}/share/settings.default.json
  ${CMAKE_SOURCE_DIR}/share/manifest.json
  ${CMAKE_SOURCE_DIR}/share/fonts/Inter-Regular.ttf
  ${CMAKE_SOURCE_DIR}/share/fonts/Inter-SemiBold.ttf)
set(PF_EMBED_OUT ${CMAKE_BINARY_DIR}/gen/embedded_share.c)
add_custom_command(
  OUTPUT ${PF_EMBED_OUT}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/gen
  COMMAND bin2c -o ${PF_EMBED_OUT} -p pf_share ${PF_EMBED_INPUTS}
  DEPENDS bin2c ${PF_EMBED_INPUTS}
  COMMENT "Embedding share/ files")

# ---- web assets: gzip at build time, embed ---------------------------------
set(PF_WEB_FILES index.html style.css app.js sw.js manifest.webmanifest icon.svg
    pages/home.js pages/history.js pages/cook.js pages/settings.js pages/more.js pages/network.js pages/pellets.js pages/learning.js)
set(PF_WEB_GZ "")
foreach(f ${PF_WEB_FILES})
  get_filename_component(base ${f} NAME)
  set(out ${CMAKE_BINARY_DIR}/gen/web/${base}.gz)
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/gen/web
    COMMAND gzip -9 -n -c ${CMAKE_SOURCE_DIR}/web/${f} > ${out}
    DEPENDS ${CMAKE_SOURCE_DIR}/web/${f} VERBATIM)
  list(APPEND PF_WEB_GZ ${out})
endforeach()
foreach(f uPlot.iife.min.js uPlot.min.css)
  set(out ${CMAKE_BINARY_DIR}/gen/web/${f}.gz)
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/gen/web
    COMMAND gzip -9 -n -c ${CMAKE_SOURCE_DIR}/third_party/uplot/${f} > ${out}
    DEPENDS ${CMAKE_SOURCE_DIR}/third_party/uplot/${f} VERBATIM)
  list(APPEND PF_WEB_GZ ${out})
endforeach()
set(PF_WEB_OUT ${CMAKE_BINARY_DIR}/gen/embedded_web.c)
add_custom_command(OUTPUT ${PF_WEB_OUT}
  COMMAND bin2c -o ${PF_WEB_OUT} -p pf_web ${PF_WEB_GZ}
  DEPENDS bin2c ${PF_WEB_GZ}
  COMMENT "Embedding web/ assets")

# ---- core library (everything except main) -----------------------------------
add_library(pfcore STATIC
  src/core/log.c
  src/core/util.c
  src/core/sha256.c
  src/core/sdnotify.c
  src/core/settings.c
  src/core/db.c
  src/core/embedded.c
  src/core/env.c
  src/core/cycle.c
  src/core/outputs.c
  src/core/cmdq.c
  src/core/status.c
  src/core/history.c
  src/core/safety.c
  src/core/events.c
  src/core/notify.c
  src/core/control.c
  src/core/threads.c
  src/hal/gpio.c
  src/hal/pwm.c
  src/hal/i2c.c
  src/hal/spi.c
  src/platform/sim.c
  src/platform/rpi.c
  src/probes/shh.c
  src/probes/tempq.c
  src/probes/registry.c
  src/probes/sim_probe.c
  src/probes/ads1x15.c
  src/probes/max31865.c
  src/probes/mcp9600.c
  src/probes/ds18b20.c
  src/probes/virtual.c
  src/probes/ble/bluez.c
  src/probes/ble/ibbq.c
  src/probes/ble/meater.c
  src/probes/probes.c
  src/controllers/registry.c
  src/controllers/pid.c
  src/controllers/pid_variants.c
  src/controllers/adaptive.c
  src/features/learning.c
  src/features/mqtt.c
  src/features/webhook.c
  src/features/update.c
  src/display/registry.c
  src/display/gfx.c
  src/display/screens.c
  src/display/ili9341.c
  src/distance/vl53l0x.c
  src/distance/registry.c
  src/distance/hcsr04.c
  src/features/pellets.c
  src/features/cookfile.c
  src/features/recipe.c
  src/net/sysinfo.c
  src/net/wifi.c
  src/net/netmgr.c
  src/web/api.c
  src/web/server.c
  ${PF_EMBED_OUT}
  ${PF_WEB_OUT})
target_include_directories(pfcore PUBLIC include src third_party/stb)
target_compile_options(pfcore PRIVATE ${PF_WARN})
target_link_libraries(pfcore PUBLIC cjson civetweb PkgConfig::SQLITE Threads::Threads m ${PF_OPTIONAL_LIBS})
if(PF_WITH_MQTT)
  target_compile_definitions(pfcore PUBLIC PF_WITH_MQTT=1)
endif()
if(PF_WITH_CURL)
  target_compile_definitions(pfcore PUBLIC PF_WITH_CURL=1)
endif()
if(PF_WITH_BLE)
  target_compile_definitions(pfcore PUBLIC PF_WITH_BLE=1)
endif()
set(PF_VERSION "" CACHE STRING "Version string baked into the binary (release builds pass the git tag); empty = project version")
if(NOT PF_VERSION)
  set(PF_VERSION "${PROJECT_VERSION}")
endif()
target_compile_definitions(pfcore PUBLIC
  PF_VERSION="${PF_VERSION}"
  PF_DEFAULT_CONFIG="/etc/pifire/settings.json"
  PF_DEFAULT_DATA_DIR="/var/lib/pifire")

# ---- daemon ------------------------------------------------------------------
add_executable(pifired src/core/main.c)
target_compile_options(pifired PRIVATE ${PF_WARN})
target_link_libraries(pifired PRIVATE pfcore civetweb ${PF_OPTIONAL_LIBS})

install(TARGETS pifired RUNTIME DESTINATION bin)
install(DIRECTORY share/ DESTINATION share/pifire)

# ---- tests -------------------------------------------------------------------
if(PF_BUILD_TESTS)
  enable_testing()
  add_library(unity STATIC third_party/unity/unity.c)
  target_include_directories(unity PUBLIC third_party/unity)
  target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE)
  target_compile_options(unity PRIVATE -w)
  function(pf_test name)
    add_executable(${name} tests/${name}.c)
    target_compile_options(${name} PRIVATE ${PF_WARN})
    target_link_libraries(${name} PRIVATE pfcore unity)
    add_test(NAME ${name} COMMAND ${name})
  endfunction()
  pf_test(test_settings)
  pf_test(test_cycle)
  pf_test(test_shh)
  pf_test(test_sim_cook)
  pf_test(test_controllers)
  pf_test(test_learning)
  pf_test(test_gfx)
endif()
