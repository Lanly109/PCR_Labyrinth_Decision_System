# A GUI save edits the generated config. Rebuilding must preserve that file.
if(NOT EXISTS "${DESTINATION}")
    configure_file("${SOURCE}" "${DESTINATION}" COPYONLY)
endif()
