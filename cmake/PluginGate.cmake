# The plugin link gate (design.md §5, built-in plugins use the public API only).
#
# roost_check_plugin_links(<directory>) fails the configure if a target defined
# in <directory> or below
#   - links (LINK_LIBRARIES, INTERFACE_LINK_LIBRARIES) anything but Qt and the
#     other targets of that tree,
#   - has an include directory (INCLUDE_DIRECTORIES,
#     INTERFACE_INCLUDE_DIRECTORIES) under ${PROJECT_SOURCE_DIR}/src or above
#     it (an ancestor makes `#include "src/core/x.hpp"` work), or
#   - lists a source file (SOURCES, INTERFACE_SOURCES) under
#     ${PROJECT_SOURCE_DIR}/src.
# Paths are resolved (relative to the target's SOURCE_DIR, symlinks followed)
# before they are compared. A third-party plugin cannot reach roost's
# internals, so a built-in one must not either.
#
# Call it after the last add_subdirectory of the tree: it reads the properties
# of the targets that exist by then.
#
# Generator expressions are read structurally. A condition ($<BOOL:..>,
# $<AND:..>, $<CONFIG:..>, ...) around a value is skipped; the value must be a
# target (or $<TARGET_OBJECTS|TARGET_NAME|TARGET_FILE|TARGET_LINKER_FILE:T>,
# $<LINK_ONLY:T>, $<TARGET_PROPERTY:T,prop>) and every target it names must be
# Qt or a target of the tree. This is the shape qt_add_qml_module and
# qt_add_executable give their generated targets. Anything else (a literal
# path or flag inside a generator expression, an unknown generator expression)
# is an error naming the item.
#
# What it does not guard: a deliberate bypass. Link or compile options with
# -l or -I, link directories, and `#include "../../src/..."` by a relative path
# in the source text all pass. It keeps an honest plugin honest; it is not a
# sandbox.

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

# Sets <out> to the index of the '>' that closes the generator expression
# opened by the "$<" at <start> in <str>, or -1.
function(_roost_gate_close str start out)
    string(LENGTH "${str}" _len)
    set(_depth 0)
    set(_i ${start})
    while(_i LESS _len)
        string(SUBSTRING "${str}" ${_i} 2 _two)
        if(_two STREQUAL "$<")
            math(EXPR _depth "${_depth} + 1")
            math(EXPR _i "${_i} + 2")
            continue()
        endif()
        string(SUBSTRING "${str}" ${_i} 1 _c)
        if(_c STREQUAL ">")
            math(EXPR _depth "${_depth} - 1")
            if(_depth EQUAL 0)
                set(${out} ${_i} PARENT_SCOPE)
                return()
            endif()
        endif()
        math(EXPR _i "${_i} + 1")
    endwhile()
    set(${out} -1 PARENT_SCOPE)
endfunction()

# Sets <out> to "" if <name> is Qt or a target of the tree (_all, from the
# caller), else to the reason. An alias stands for the target it names.
function(_roost_gate_target name out)
    set(_name "${name}")
    if(_name MATCHES "^[$]<TARGET_NAME:([^$<>]*)>$")
        set(_name "${CMAKE_MATCH_1}")
    endif()
    if(_name MATCHES "[$<>]")
        set(${out} "cannot check the target name ${_name}" PARENT_SCOPE)
        return()
    endif()
    if(TARGET "${_name}")
        get_target_property(_aliased "${_name}" ALIASED_TARGET)
        if(_aliased)
            set(_name "${_aliased}")
        endif()
    endif()
    if(_name MATCHES "^(Qt6|Qt)::" OR _name IN_LIST _all)
        set(${out} "" PARENT_SCOPE)
    else()
        set(${out} "${_name} is not Qt or a target of the plugin tree" PARENT_SCOPE)
    endif()
endfunction()

# Sets <out> to "" if <expr> yields only Qt and targets of the tree, else to
# the reason. <mode> is LINK (the value is a link item) or INCLUDE (the value
# is a directory list; only property reads of targets are known there).
function(_roost_gate_value expr mode out)
    if(expr STREQUAL "")
        set(${out} "" PARENT_SCOPE)
        return()
    endif()
    if(NOT expr MATCHES "^[$]<")
        if(NOT mode STREQUAL "LINK")
            set(${out} "a literal path inside a generator expression" PARENT_SCOPE)
        elseif(expr MATCHES "[$]")
            set(${out} "cannot check text around a generator expression" PARENT_SCOPE)
        else()
            _roost_gate_target("${expr}" _why)
            set(${out} "${_why}" PARENT_SCOPE)
        endif()
        return()
    endif()

    string(LENGTH "${expr}" _len)
    math(EXPR _last "${_len} - 1")
    _roost_gate_close("${expr}" 0 _end)
    if(NOT _end EQUAL _last)
        set(${out} "cannot check text around a generator expression" PARENT_SCOPE)
        return()
    endif()
    math(EXPR _ilen "${_len} - 3")
    string(SUBSTRING "${expr}" 2 ${_ilen} _inner)

    # $<condition:value>: the condition yields a boolean and names nothing
    # that can reach the link line; the value is what counts.
    if(_inner MATCHES "^[$]<")
        _roost_gate_close("${_inner}" 0 _cend)
        math(EXPR _next "${_cend} + 1")
        string(LENGTH "${_inner}" _ilen2)
        if(_cend LESS 0 OR _next GREATER_EQUAL _ilen2)
            set(${out} "cannot check the generator expression" PARENT_SCOPE)
            return()
        endif()
        string(SUBSTRING "${_inner}" ${_next} 1 _colon)
        if(NOT _colon STREQUAL ":")
            set(${out} "cannot check the generator expression" PARENT_SCOPE)
            return()
        endif()
        math(EXPR _vstart "${_cend} + 2")
        string(SUBSTRING "${_inner}" ${_vstart} -1 _value)
        _roost_gate_value("${_value}" ${mode} _why)
        set(${out} "${_why}" PARENT_SCOPE)
        return()
    endif()

    if(NOT _inner MATCHES "^([A-Z_]+):(.*)$")
        set(${out} "cannot check the generator expression" PARENT_SCOPE)
        return()
    endif()
    set(_op "${CMAKE_MATCH_1}")
    set(_arg "${CMAKE_MATCH_2}")
    if(_op STREQUAL "TARGET_PROPERTY")
        if(_arg MATCHES "^([^,]*),")
            _roost_gate_target("${CMAKE_MATCH_1}" _why)
        elseif(_arg MATCHES "[$]")
            set(_why "cannot check the generator expression")
        else()
            # A property of the target the expression is attached to.
            set(_why "")
        endif()
    elseif(mode STREQUAL "LINK" AND _op STREQUAL "LINK_ONLY")
        _roost_gate_value("${_arg}" LINK _why)
    elseif(mode STREQUAL "LINK"
           AND _op MATCHES "^(TARGET_OBJECTS|TARGET_NAME|TARGET_FILE|TARGET_LINKER_FILE)$")
        _roost_gate_target("${_arg}" _why)
    else()
        set(_why "cannot check $<${_op}:...>")
    endif()
    set(${out} "${_why}" PARENT_SCOPE)
endfunction()

function(roost_check_plugin_links directory)
    _roost_plugin_gate_collect(${directory} _all)

    file(REAL_PATH "${PROJECT_SOURCE_DIR}/src" _src_root)
    set(_violations)
    foreach(_target IN LISTS _all)
        get_target_property(_source_dir ${_target} SOURCE_DIR)

        foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(_libs ${_target} ${_prop})
            if(NOT _libs)
                continue()
            endif()
            foreach(_lib IN LISTS _libs)
                _roost_gate_value("${_lib}" LINK _why)
                if(NOT _why STREQUAL "")
                    if(_lib MATCHES "[$]<")
                        list(APPEND _violations
                            "${_target}: links ${_lib} (generator expression: ${_why})")
                    else()
                        list(APPEND _violations "${_target}: links ${_lib}")
                    endif()
                endif()
            endforeach()
        endforeach()

        foreach(_prop INCLUDE_DIRECTORIES INTERFACE_INCLUDE_DIRECTORIES)
            get_target_property(_dirs ${_target} ${_prop})
            if(NOT _dirs)
                continue()
            endif()
            foreach(_dir IN LISTS _dirs)
                if(_dir MATCHES "[$]<")
                    _roost_gate_value("${_dir}" INCLUDE _why)
                    if(NOT _why STREQUAL "")
                        list(APPEND _violations
                            "${_target}: includes ${_dir} (generator expression: ${_why})")
                    endif()
                    continue()
                endif()
                file(REAL_PATH "${_dir}" _real BASE_DIRECTORY "${_source_dir}")
                # Under src/, or above it: from an ancestor
                # `#include "src/core/x.hpp"` works.
                cmake_path(IS_PREFIX _src_root "${_real}" NORMALIZE _under)
                cmake_path(IS_PREFIX _real "${_src_root}" NORMALIZE _above)
                if(_under OR _above)
                    list(APPEND _violations "${_target}: includes ${_dir}")
                endif()
            endforeach()
        endforeach()

        foreach(_prop SOURCES INTERFACE_SOURCES)
            get_target_property(_files ${_target} ${_prop})
            if(NOT _files)
                continue()
            endif()
            foreach(_file IN LISTS _files)
                if(_file MATCHES "[$]<")
                    # Not resolved; only a visible path into src/ counts.
                    string(FIND "${_file}" "${PROJECT_SOURCE_DIR}/src" _pos)
                    if(NOT _pos EQUAL -1)
                        list(APPEND _violations
                            "${_target}: lists source ${_file} (generator expression)")
                    endif()
                    continue()
                endif()
                file(REAL_PATH "${_file}" _real BASE_DIRECTORY "${_source_dir}")
                cmake_path(IS_PREFIX _src_root "${_real}" NORMALIZE _under)
                if(_under)
                    list(APPEND _violations "${_target}: lists source ${_file}")
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
