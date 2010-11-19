
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86")
    add_definitions(-D__i386__)
endif()

add_definitions(-Dinline=__inline)

if(NOT CMAKE_CROSSCOMPILING)


else()

add_definitions(/GS- /Zl /Zi)
add_definitions(-Dinline=__inline -D__STDC__=1)

IF(${_MACHINE_ARCH_FLAG} MATCHES X86)
  SET (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /SAFESEH:NO")
  SET (CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /SAFESEH:NO")
  SET (CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} /SAFESEH:NO")
ENDIF()

link_directories("${REACTOS_BINARY_DIR}/importlibs" ${REACTOS_BINARY_DIR}/lib/3rdparty/mingw)

set(CMAKE_RC_CREATE_SHARED_LIBRARY "<CMAKE_C_COMPILER> <CMAKE_SHARED_LIBRARY_C_FLAGS> <LINK_FLAGS> <CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>")

macro(add_linkerflag MODULE _flag)
    set(NEW_LINKER_FLAGS ${_flag})
    get_target_property(LINKER_FLAGS ${MODULE} LINK_FLAGS)
    if(LINKER_FLAGS)
        set(NEW_LINKER_FLAGS "${LINKER_FLAGS} ${NEW_LINKER_FLAGS}")
    endif()
    set_target_properties(${MODULE} PROPERTIES LINK_FLAGS ${NEW_LINKER_FLAGS})
endmacro()

macro(set_entrypoint MODULE ENTRYPOINT)
    if(${ENTRYPOINT} STREQUAL "0")
        add_linkerflag(${MODULE} "/ENTRY:0")
    else()
        add_linkerflag(${MODULE} "/ENTRY:${ENTRYPOINT}")
    endif()
endmacro()

macro(set_subsystem MODULE SUBSYSTEM)
    add_linkerflag(${MODULE} "/subsystem:${SUBSYSTEM}")
endmacro()

macro(set_image_base MODULE IMAGE_BASE)
    add_linkerflag(${MODULE} "/BASE:${IMAGE_BASE}")
endmacro()

macro(set_module_type MODULE TYPE)
    add_dependencies(${MODULE} psdk buildno_header)
    if(${TYPE} MATCHES nativecui)
        set_subsystem(${MODULE} native)
        add_importlibs(${MODULE} ntdll)
    endif()
    if (${TYPE} MATCHES win32gui)
        set_subsystem(${MODULE} windows)
        set_entrypoint(${MODULE} WinMainCRTStartup)
		target_link_libraries(${MODULE} mingw_common mingw_wmain)
    endif ()
    if (${TYPE} MATCHES win32cui)
        set_subsystem(${MODULE} console)
        set_entrypoint(${MODULE} mainCRTStartup)
		target_link_libraries(${MODULE} mingw_common mingw_wmain)
    endif ()
    if(${TYPE} MATCHES win32dll)
        # Need this only because mingw library is broken
        set_entrypoint(${MODULE} DllMainCRTStartup@12)
		if(DEFINED baseaddress_${MODULE})
			set_image_base(${MODULE} ${baseaddress_${MODULE}})
		else()
			message(STATUS "${MODULE} has no base address")
		endif()
		target_link_libraries(${MODULE} mingw_common mingw_dllmain)
		add_importlibs(${MODULE} msvcrt kernel32)
        add_linkerflag(${MODULE} "/DLL")
    endif()

endmacro()

macro(set_unicode)
    add_definitions(-DUNICODE -D_UNICODE)
endmacro()

set(CMAKE_C_FLAGS_DEBUG_INIT "/D_DEBUG /MDd /Zi  /Ob0 /Od")
set(CMAKE_CXX_FLAGS_DEBUG_INIT "/D_DEBUG /MDd /Zi /Ob0 /Od")

macro(set_rc_compiler)
# dummy, this workaround is only needed in mingw due to lack of RC support in cmake
endmacro()

#idl files support
set(IDL_COMPILER midl)
set(IDL_FLAGS /win32 /Dstrict_context_handle=)
set(IDL_HEADER_ARG /h) #.h
set(IDL_TYPELIB_ARG /tlb) #.tlb
set(IDL_SERVER_ARG /sstub) #.c for stub server library
set(IDL_CLIENT_ARG /cstub) #.c for stub client library

# Thanks MS for creating a stupid linker
macro(add_importlib_target _spec_file)
    get_filename_component(_name ${_spec_file} NAME_WE)

    # Generate the asm stub file
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/importlibs/lib${_name}_stubs.asm
        COMMAND native-spec2pdef -s ${CMAKE_CURRENT_SOURCE_DIR}/${_spec_file} ${CMAKE_BINARY_DIR}/importlibs/lib${_name}_stubs.asm
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${_spec_file})

    # Generate a the export def file
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/importlibs/lib${_name}_exp.def
        COMMAND native-spec2pdef -n -r ${CMAKE_CURRENT_SOURCE_DIR}/${_spec_file} ${CMAKE_BINARY_DIR}/importlibs/lib${_name}_exp.def
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${_spec_file})

    # Assemble the file
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/importlibs/lib${_name}_stubs.obj
        COMMAND ${CMAKE_ASM_COMPILER} /NOLOGO /Fo${CMAKE_BINARY_DIR}/importlibs/lib${_name}_stubs.obj /c /Ta ${CMAKE_BINARY_DIR}/importlibs/lib${_name}_stubs.asm
        DEPENDS "${CMAKE_BINARY_DIR}/importlibs/lib${_name}_stubs.asm"
    )

    # Add neccessary importlibs for redirections
    set(_libraries "")
    foreach(_lib ${ARGN})
        list(APPEND _libraries "${CMAKE_BINARY_DIR}/importlibs/${_lib}.lib")
    endforeach()

    # Build the importlib
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/importlibs/lib${_name}.lib
        COMMAND LINK /LIB /NOLOGO /MACHINE:X86 /DEF:${CMAKE_BINARY_DIR}/importlibs/lib${_name}_exp.def /OUT:${CMAKE_BINARY_DIR}/importlibs/lib${_name}.lib ${CMAKE_BINARY_DIR}/importlibs/lib${_name}_stubs.obj ${_libraries}
        DEPENDS ${CMAKE_BINARY_DIR}/importlibs/lib${_name}_stubs.obj ${CMAKE_BINARY_DIR}/importlibs/lib${_name}_exp.def ${_libraries}
    )

    # Add the importlib target
    add_custom_target(
        lib${_name}
        DEPENDS ${CMAKE_BINARY_DIR}/importlibs/lib${_name}.lib
    )
endmacro()

macro(add_importlibs MODULE)
    foreach(LIB ${ARGN})
        target_link_libraries(${MODULE} ${CMAKE_BINARY_DIR}/importlibs/lib${LIB}.lib)
        add_dependencies(${MODULE} lib${LIB})
    endforeach()
endmacro()

MACRO(spec2def _dllname _spec_file)
    get_filename_component(_file ${_spec_file} NAME_WE)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${_file}.def
        COMMAND native-spec2pdef -n  --dll ${_dllname} ${CMAKE_CURRENT_SOURCE_DIR}/${_spec_file} ${CMAKE_CURRENT_BINARY_DIR}/${_file}.def
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${_spec_file})
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/${_file}.def
        PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
ENDMACRO(spec2def _dllname _spec_file)

macro(pdef2def _pdef_file)
    get_filename_component(_file ${_pdef_file} NAME_WE)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${_file}.def
        COMMAND ${CMAKE_C_COMPILER} /EP /c ${CMAKE_CURRENT_SOURCE_DIR}/${_pdef_file} > ${CMAKE_CURRENT_BINARY_DIR}/${_file}.def
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${_pdef_file})
    add_custom_target(
        ${_file}_def
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${_file}.def)
endmacro(pdef2def _pdef_file)

macro(set_pdef_file _module _pdef_file)
    pdef2def(${_pdef_file})
    get_filename_component(_file ${_pdef_file} NAME_WE)
    add_linkerflag(${_module} "/DEF:${CMAKE_CURRENT_BINARY_DIR}/${_file}.def")
    add_dependencies(${_module} ${_file}_def)
endmacro()

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/importlibs)

#pseh workaround
set(PSEH_LIB "")

endif()
