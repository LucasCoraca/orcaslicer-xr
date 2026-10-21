# OrcaSlicer XR: OrcaSlicer for Android (the same engine, GUI library and entry point) plus the XR core, packaged as a
# headset app. Added to the OrcaSlicer for Android build by its android/app/CMakeLists.txt when ORCA_XR_DIR points here
# (build.sh does that); uses that file's variables (ORCA_WX_SRC, ORCA_APP_PACKAGE_DIR, orca_resources_zip).
set(XR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(XR_PACKAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/package")
set(XR_CORE "${XR_DIR}/core")

# package: our activity and manifest, the wx Java bridges, the launcher icon, Orca's resources and the controller models
file(MAKE_DIRECTORY "${XR_PACKAGE_DIR}/src/org/orcaport/wx" "${XR_PACKAGE_DIR}/src/org/orcaport/xr" "${XR_PACKAGE_DIR}/assets/models")
file(GLOB _wx_java "${ORCA_WX_SRC}/src/qt/android/java/org/orcaport/wx/*.java")
foreach (_f ${_wx_java})
    get_filename_component(_n "${_f}" NAME)
    configure_file("${_f}" "${XR_PACKAGE_DIR}/src/org/orcaport/wx/${_n}" COPYONLY)
endforeach ()
file(GLOB _xr_java "${CMAKE_CURRENT_SOURCE_DIR}/package/src/org/orcaport/xr/*.java")
foreach (_f ${_xr_java})
    get_filename_component(_n "${_f}" NAME)
    configure_file("${_f}" "${XR_PACKAGE_DIR}/src/org/orcaport/xr/${_n}" COPYONLY)
endforeach ()
if (ANDROID_ABI STREQUAL "arm64-v8a")
    set(XR_HEADTRACKING_REQUIRED "true")
    set(XR_HEADSET_NOTE "headset build")
else ()
    set(XR_HEADTRACKING_REQUIRED "false")   # the emulator build installs anywhere (no headset: the touch preview)
    set(XR_HEADSET_NOTE "emulator build")
endif ()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/package/AndroidManifest.xml.in" "${XR_PACKAGE_DIR}/AndroidManifest.xml" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/package/build.gradle" "${XR_PACKAGE_DIR}/build.gradle" COPYONLY)
file(GLOB_RECURSE _xr_res RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/package" CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/package/res/*")
foreach (_f ${_xr_res})
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/package/${_f}" "${XR_PACKAGE_DIR}/${_f}" COPYONLY)
endforeach ()
foreach (_m touch-plus-left touch-plus-right)
    configure_file("${XR_DIR}/assets/models/${_m}.glb" "${XR_PACKAGE_DIR}/assets/models/${_m}.glb" COPYONLY)
endforeach ()
add_custom_command(
    OUTPUT "${XR_PACKAGE_DIR}/assets/resources.zip"
    COMMAND ${CMAKE_COMMAND} -E copy "${ORCA_APP_PACKAGE_DIR}/assets/resources.zip" "${XR_PACKAGE_DIR}/assets/resources.zip"
    DEPENDS orca_resources_zip "${ORCA_APP_PACKAGE_DIR}/assets/resources.zip")
add_custom_target(orca_xr_resources DEPENDS "${XR_PACKAGE_DIR}/assets/resources.zip")

qt_add_executable(orcaslicer_xr
    "${CMAKE_SOURCE_DIR}/src/OrcaSlicer.cpp"
    "${CMAKE_SOURCE_DIR}/src/OrcaSlicer.hpp"
    "${CMAKE_SOURCE_DIR}/../android/app/bootstrap.cpp"
    orca_xr.cpp
    qt_gl.cpp
    xr_glue.cpp
    "${XR_CORE}/scene.cpp" "${XR_CORE}/renderer.cpp" "${XR_CORE}/interaction.cpp" "${XR_CORE}/hands.cpp"
    "${XR_CORE}/gltf_model.cpp" "${XR_CORE}/xr_session.cpp")
add_dependencies(orcaslicer_xr orca_xr_resources)
target_include_directories(orcaslicer_xr PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/libslic3r"
                           "${CMAKE_SOURCE_DIR}/../android/app/glfw_stub" "${XR_CORE}" "${XR_DIR}/third_party/openxr/include")
set(XR_OPENXR_LOADER "${XR_DIR}/third_party/openxr/lib/${ANDROID_ABI}/libopenxr_loader.so")
target_link_libraries(orcaslicer_xr PRIVATE libslic3r_gui libslic3r cereal::cereal miniz
                      Qt6::Widgets Qt6::OpenGLWidgets Qt6::OpenGL Qt6::PrintSupport Qt6::GuiPrivate
                      "${XR_OPENXR_LOADER}" EGL GLESv3 android log ${CMAKE_DL_LIBS})
target_compile_definitions(orcaslicer_xr PRIVATE wxDEBUG_LEVEL=0)

set_target_properties(orcaslicer_xr PROPERTIES
    QT_ANDROID_PACKAGE_SOURCE_DIR "${XR_PACKAGE_DIR}"
    QT_ANDROID_EXTRA_LIBS "${XR_OPENXR_LOADER}"
    QT_ANDROID_MIN_SDK_VERSION 29
    QT_ANDROID_TARGET_SDK_VERSION 34
    QT_ANDROID_APP_NAME "OrcaSlicer XR"
    QT_ANDROID_VERSION_NAME "0.1.1"
    QT_ANDROID_VERSION_CODE 2)
