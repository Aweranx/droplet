if(NOT DEFINED DROPLET_DEPS_STAMP_FILE OR NOT DEFINED DROPLET_DEPS_STAMP_VALUE)
    message(FATAL_ERROR "Dependency stamp file and value are required")
endif()

get_filename_component(stamp_directory "${DROPLET_DEPS_STAMP_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${stamp_directory}")
file(WRITE "${DROPLET_DEPS_STAMP_FILE}" "${DROPLET_DEPS_STAMP_VALUE}\n")
