# The plugin link gate (design.md §5, built-in plugins use the public API only).
#
# roost_check_plugin_links(<directory>) fails the configure if a target defined
# in <directory> or below links anything but Qt and the other targets of that
# tree, or includes a path under ${PROJECT_SOURCE_DIR}/src. A third-party plugin
# cannot reach roost's internals, so a built-in one must not either.
#
# Call it after the last add_subdirectory of the tree: it reads the properties
# of the targets that exist by then. Anything it cannot read with certainty (a
# generator expression other than $<LINK_ONLY:...>, a linker flag, a file path)
# is an error, so nothing passes unchecked. The one exception is the condition
# Qt wraps around its static plugin initialisers.

# Sets <out> in the caller to the targets of <directory> and its subdirectories.
function(_roost_plugin_gate_collect directory out)
    get_property(_found DIRECTORY ${directory} PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_subdirs DIRECTORY ${directory} PROPERTY SUBDIRECTORIES)
    foreach(_sub IN LISTS _subdirs)
        _roost_plugin_gate_collect(${_sub} _sub_targets)
        list(APPEND _found ${_sub_targets})
    endforeach()
    set(${out} ${_found} PARENT_SCOPE)
endfunction()

function(roost_check_plugin_links directory)
    _roost_plugin_gate_collect(${directory} _all)

    set(_src_root "${PROJECT_SOURCE_DIR}/src")
    set(_violations)
    foreach(_target IN LISTS _all)
        foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(_libs ${_target} ${_prop})
            if(NOT _libs)
                continue()
            endif()
            foreach(_lib IN LISTS _libs)
                set(_name "${_lib}")
                if(_name MATCHES "^\\$<LINK_ONLY:([^<>$]*)>$")
                    set(_name "${CMAKE_MATCH_1}")
                endif()
                # qt_add_executable appends the initialisers of static Qt
                # plugins as $<$<BOOL:condition>:Qt6::Name_init>. A condition
                # around a Qt target names nothing of roost, so that one shape
                # is read as the Qt target it guards.
                if(_name MATCHES "^[$]<[$]<BOOL:.*>:((Qt6|Qt)::[A-Za-z0-9_]+)>$")
                    set(_name "${CMAKE_MATCH_1}")
                endif()
                # An alias stands for the target it names.
                if(TARGET "${_name}")
                    get_target_property(_aliased "${_name}" ALIASED_TARGET)
                    if(_aliased)
                        set(_name "${_aliased}")
                    endif()
                endif()
                if(_name MATCHES "[$]<|>")
                    list(APPEND _violations
                        "${_target}: links ${_lib} (generator expression, cannot check)")
                elseif(_name MATCHES "^(Qt6|Qt)::" OR _name IN_LIST _all)
                    # Qt, or a target of the plugin tree itself.
                else()
                    list(APPEND _violations "${_target}: links ${_lib}")
                endif()
            endforeach()
        endforeach()

        foreach(_prop INCLUDE_DIRECTORIES INTERFACE_INCLUDE_DIRECTORIES)
            get_target_property(_dirs ${_target} ${_prop})
            if(NOT _dirs)
                continue()
            endif()
            foreach(_dir IN LISTS _dirs)
                # A generator expression in a path is not resolved here.
                if(_dir MATCHES "[$]<")
                    list(APPEND _violations
                        "${_target}: includes ${_dir} (generator expression, cannot check)")
                    continue()
                endif()
                cmake_path(IS_PREFIX _src_root "${_dir}" NORMALIZE _under)
                if(_under)
                    list(APPEND _violations "${_target}: includes ${_dir}")
                endif()
            endforeach()
        endforeach()
    endforeach()

    if(_violations)
        list(JOIN _violations "\n  " _text)
        message(FATAL_ERROR
            "Built-in plugins use the public API only (design.md §5): Qt and "
            "their own targets, nothing from src/.\n  ${_text}")
    endif()
endfunction()
