# Portable mdoc(7) manuals for every installed runtime binary.
# Sources are .in templates; configure_file injects PROJECT_VERSION and MAN_DATE.
# Installed uncompressed under GNUInstallDirs MANDIR so Linux, macOS, FreeBSD,
# and OpenBSD all resolve pages via man(1)/mandoc without a gzip dependency at
# install time. Distro packaging may compress after install.

set(GLIFISTORE_MAN_DATE "" CACHE STRING
    "Date stamp for .Dd in man pages (mdoc); empty uses configure-time local date")
if(GLIFISTORE_MAN_DATE STREQUAL "")
    string(TIMESTAMP GLIFISTORE_MAN_DATE "%B %d, %Y")
endif()
set(MAN_DATE "${GLIFISTORE_MAN_DATE}")

option(GLIFISTORE_COMPRESS_MANPAGES
    "Install gzip-compressed man pages (optional; packagers often compress later)"
    OFF)

set(GLIFISTORE_MAN_PAGE_INS
    man1/glifistore_demo.1.in
    man1/glifistore_inspect_segment.1.in
    man1/glifistore_verify_store.1.in
    man1/glifistore_backup_store.1.in
    man1/glifistore_migrate_store.1.in
    man1/glifistore_repair_store.1.in
    man1/glifistore_rebuild_index.1.in
    man7/glifistore.7.in
    man8/glifistored.8.in
)

set(GLIFISTORE_MAN_GENERATED "")
foreach(man_in IN LISTS GLIFISTORE_MAN_PAGE_INS)
    get_filename_component(man_rel "${man_in}" NAME_WLE) # e.g. glifistored.8
    get_filename_component(man_section_dir "${man_in}" DIRECTORY) # man1 / man7 / man8
    set(man_out "${CMAKE_CURRENT_BINARY_DIR}/${man_section_dir}/${man_rel}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/${man_in}"
        "${man_out}"
        @ONLY
    )
    list(APPEND GLIFISTORE_MAN_GENERATED "${man_out}")
endforeach()

add_custom_target(glifistore_manpages DEPENDS ${GLIFISTORE_MAN_GENERATED})
set_target_properties(glifistore_manpages PROPERTIES FOLDER "Documentation")

if(GLIFISTORE_COMPRESS_MANPAGES)
    find_program(GLIFISTORE_GZIP_EXECUTABLE gzip REQUIRED)
    set(GLIFISTORE_MAN_INSTALL_FILES "")
    foreach(man_out IN LISTS GLIFISTORE_MAN_GENERATED)
        set(man_gz "${man_out}.gz")
        add_custom_command(
            OUTPUT "${man_gz}"
            COMMAND /bin/sh -c "\"${GLIFISTORE_GZIP_EXECUTABLE}\" -n -c \"${man_out}\" > \"${man_gz}\""
            DEPENDS "${man_out}"
            VERBATIM
        )
        list(APPEND GLIFISTORE_MAN_INSTALL_FILES "${man_gz}")
    endforeach()
    add_custom_target(glifistore_manpages_gz ALL DEPENDS ${GLIFISTORE_MAN_INSTALL_FILES})
    add_dependencies(glifistore_manpages_gz glifistore_manpages)
    set_target_properties(glifistore_manpages_gz PROPERTIES FOLDER "Documentation")
    set(_glifistore_man_install_list "${GLIFISTORE_MAN_INSTALL_FILES}")
else()
    set(_glifistore_man_install_list "${GLIFISTORE_MAN_GENERATED}")
endif()

# Map each generated page to its section destination.
foreach(man_file IN LISTS _glifistore_man_install_list)
    get_filename_component(man_name "${man_file}" NAME) # glifistored.8 or .8.gz
    if(man_name MATCHES "\\.1(\\.gz)?$")
        set(_sec man1)
    elseif(man_name MATCHES "\\.7(\\.gz)?$")
        set(_sec man7)
    elseif(man_name MATCHES "\\.8(\\.gz)?$")
        set(_sec man8)
    else()
        message(FATAL_ERROR "Unrecognized man page section for ${man_file}")
    endif()
    # Manuals are part of the deployable Runtime component.
    install(FILES "${man_file}"
        DESTINATION "${CMAKE_INSTALL_MANDIR}/${_sec}"
        COMPONENT Runtime
    )
endforeach()
