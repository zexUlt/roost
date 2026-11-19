# The install layout, one per OS (docs/design/11-build.md). The build tree does
# not change: binaries stay in build/<preset>/bin and the plugins in
# build/<preset>/roost-plugins. `cmake --install <build> --prefix <dir>` lays
# the files out again for the OS:
#
#   macOS   <prefix>/roost.app, with the plugin data in Contents/Resources/
#           roost-plugins/<id>/ and the plugin code in Contents/MacOS and
#           Contents/PlugIns/roost/, symlinked into the plugin directories.
#   Linux   <prefix> shaped as the usr/ of an AppDir: bin/, lib/roost/plugins/
#           <id>/ (data and code together), share/.
#
# roostd finds the built-in plugins from its own location
# (BundledPluginsDirectory in src/roostd/manifest.cpp): ../Resources/roost-plugins
# or ../lib/roost/plugins. Qt is not copied; the installed binaries load it from
# where it is installed (INSTALL_RPATH_USE_LINK_PATH, option
# ROOST_INSTALL_RPATH_TO_QT) until a deploy tool copies it in. Everything is in
# the install component `roost`.
#
# Defines, for src/ and plugins/:
#   ROOST_INSTALL_BINDIR           the directory of roost and roostd
#   ROOST_INSTALL_PLUGINS_DIR      the directory with one directory per plugin id
#   ROOST_INSTALL_RPATH            the INSTALL_RPATH of roost and roostd
#   ROOST_INSTALL_RPATH_TO_QT      option, ON by default: the directories of the
#                                  Qt the build linked go into the run path of
#                                  every installed target (before ours on
#                                  macOS, after on Linux). A packaging step
#                                  that deploys Qt sets it OFF.
#   roost_install_plugin_binary()  installs a target of a plugin

option(ROOST_INSTALL_RPATH_TO_QT
    "Put the directories of the linked Qt into the run path of installed binaries" ON)

# Fixed names, not GNUInstallDirs: on Debian with the prefix /usr it would put
# libraries in lib/<triplet>, where roostd does not look.

if(APPLE)
    set(_roost_app roost.app/Contents)
    set(ROOST_INSTALL_BINDIR ${_roost_app}/MacOS)
    set(ROOST_INSTALL_PLUGINS_DIR ${_roost_app}/Resources/roost-plugins)
    set(_roost_plugin_code_dir ${_roost_app}/PlugIns/roost)
    set(_roost_licenses_dir ${_roost_app}/Resources/licenses)
    set(ROOST_INSTALL_RPATH "@executable_path/../Frameworks")
else()
    set(ROOST_INSTALL_BINDIR bin)
    set(ROOST_INSTALL_PLUGINS_DIR lib/roost/plugins)
    set(_roost_licenses_dir share/licenses/roost)
    set(ROOST_INSTALL_RPATH "\$ORIGIN/../lib")
endif()

# The oldest macOS that runs the binaries: the deployment target of the build,
# or, when none is set, the one the compiler defaults to (the macOS it runs on).
if(APPLE)
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
        set(ROOST_MACOS_MINIMUM_VERSION ${CMAKE_OSX_DEPLOYMENT_TARGET})
    else()
        execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} -dM -E -x c++ /dev/null
            OUTPUT_VARIABLE _roost_macros
            RESULT_VARIABLE _roost_macros_result)
        # The macro is MMmmpp: 150000 is 15.0, 140400 is 14.4.
        if(NOT _roost_macros_result EQUAL 0 OR NOT _roost_macros MATCHES
            "__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ ([0-9][0-9])([0-9][0-9])[0-9][0-9]")
            message(FATAL_ERROR
                "Cannot tell the macOS deployment target of the compiler; "
                "set CMAKE_OSX_DEPLOYMENT_TARGET.")
        endif()
        math(EXPR _roost_major "${CMAKE_MATCH_1}")
        math(EXPR _roost_minor "${CMAKE_MATCH_2}")
        set(ROOST_MACOS_MINIMUM_VERSION ${_roost_major}.${_roost_minor})
    endif()
    configure_file(packaging/macos/Info.plist.in
        ${PROJECT_BINARY_DIR}/packaging/Info.plist @ONLY)
    install(FILES ${PROJECT_BINARY_DIR}/packaging/Info.plist
        DESTINATION ${_roost_app} COMPONENT roost)
else()
    install(FILES packaging/linux/roost.desktop
        DESTINATION share/applications COMPONENT roost)
    # A placeholder until there is an icon.
    install(FILES packaging/linux/roost.png
        DESTINATION share/icons/hicolor/256x256/apps COMPONENT roost)
endif()

install(FILES
    LICENSE
    NOTICE
    THIRD_PARTY_LICENSES
    licenses/LGPL-3.0.txt
    licenses/GPL-3.0.txt
    DESTINATION ${_roost_licenses_dir}
    COMPONENT roost)

# roost_install_plugin_binary(<id> <target> <subdir>)
#
# Installs the executable or module <target> of the plugin <id>, so that it is
# at <subdir>/<file> of the plugin directory, where the manifest or the qmldir
# names it. On Linux it is put there. On macOS code belongs outside Resources
# (codesign, notarization): an executable goes to Contents/MacOS, a module to
# Contents/PlugIns/roost, and a relative symlink at <subdir>/<file> points to
# it.
function(roost_install_plugin_binary id target subdir)
    set(plugin_dir ${ROOST_INSTALL_PLUGINS_DIR}/${id})
    get_target_property(type ${target} TYPE)
    # Sets the INSTALL_RPATH, replacing any the target had. With
    # ROOST_INSTALL_RPATH_TO_QT the directories of the Qt it linked are added.
    if(NOT APPLE)
        file(RELATIVE_PATH to_lib /p/${plugin_dir}/${subdir}
            /p/lib)
        string(REGEX REPLACE "/+$" "" to_lib "${to_lib}")
        set_target_properties(${target} PROPERTIES
            INSTALL_RPATH "\$ORIGIN/${to_lib}"
            INSTALL_RPATH_USE_LINK_PATH ${ROOST_INSTALL_RPATH_TO_QT})
        install(TARGETS ${target}
            RUNTIME DESTINATION ${plugin_dir}/${subdir} COMPONENT roost
            LIBRARY DESTINATION ${plugin_dir}/${subdir} COMPONENT roost)
        return()
    endif()

    if(type STREQUAL "EXECUTABLE")
        set(code_dir ${ROOST_INSTALL_BINDIR})
        set(rpath "@executable_path/../Frameworks")
    else()
        set(code_dir ${_roost_plugin_code_dir})
        # Loaded by the window (roost): its @executable_path. Through the
        # symlink, dyld may take the directory of the link or of the file for
        # @loader_path; both ways reach Contents/Frameworks.
        file(RELATIVE_PATH to_contents /p/${plugin_dir}/${subdir}
            /p/${_roost_app})
        string(REGEX REPLACE "/+$" "" to_contents "${to_contents}")
        set(rpath "@executable_path/../Frameworks"
            "@loader_path/../../Frameworks"
            "@loader_path/${to_contents}/Frameworks")
    endif()
    set_target_properties(${target} PROPERTIES
        INSTALL_RPATH "${rpath}"
        INSTALL_RPATH_USE_LINK_PATH ${ROOST_INSTALL_RPATH_TO_QT})
    install(TARGETS ${target}
        RUNTIME DESTINATION ${code_dir} COMPONENT roost
        LIBRARY DESTINATION ${code_dir} COMPONENT roost)

    file(RELATIVE_PATH link_to_code /p/${plugin_dir}/${subdir}
        /p/${code_dir})
    set(link_dir "\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${plugin_dir}/${subdir}")
    install(CODE "
        file(MAKE_DIRECTORY \"${link_dir}\")
        file(CREATE_LINK
            \"${link_to_code}/$<TARGET_FILE_NAME:${target}>\"
            \"${link_dir}/$<TARGET_FILE_NAME:${target}>\"
            SYMBOLIC)
    " COMPONENT roost)
endfunction()
