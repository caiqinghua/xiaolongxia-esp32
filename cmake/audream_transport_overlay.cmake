# Keep Component Manager's downloaded sources pristine. This overlay is only
# selected by the combined watch profile, after IDF has registered its targets.
function(audream_prepare_transport_overlay component_dir output_dir)
    set(expected_files
        "include/tcp.h|b8003d4aee0bb11365bb2217c81b5f0eb6922b3351cfdde2211310b2fac1e993"
        "include/web_socket.h|6da01f1e761fc1dbbd4f7c6a47860769b3340c73871222d9ed104eb91bdefa57"
        "src/esp/esp_tcp.h|e56c291964a95684360860bf8efaac68891f778f0b43a752349be4f5d505a4b8"
        "src/esp/esp_ssl.h|5acd9801d0fb5b4aaa1656ebeb92f5a5420d3f3be21cbd0b00e26f0f789b2aaf"
        "src/esp/esp_tcp.cc|0e3647b95261a98a1dc952df41fcb4606fa51bbfb508ce0a802bc07d6b70e11b"
        "src/esp/esp_ssl.cc|d37ffe4b1dca5cdaba5059c44c606d93972adc5ed0f1d1e5610fbdf4e7b2e700"
        "src/web_socket.cc|ea5398bce8e283e1e3cf41da384e11a241afdf88ae5a980b7594fe549d0758fe")
    foreach(entry IN LISTS expected_files)
        string(REPLACE "|" ";" fields "${entry}")
        list(GET fields 0 relative_path)
        list(GET fields 1 expected_hash)
        file(SHA256 "${component_dir}/${relative_path}" actual_hash)
        if(NOT actual_hash STREQUAL expected_hash)
            message(FATAL_ERROR "Audream transport overlay requires the reviewed esp-ml307 3.6.4 source: ${relative_path}")
        endif()
    endforeach()
    file(MAKE_DIRECTORY "${output_dir}")
    file(COPY "${component_dir}/src" "${component_dir}/include" "${component_dir}/LICENSE"
        DESTINATION "${output_dir}")
    find_program(AUDREAM_GIT_EXECUTABLE git REQUIRED)
    get_filename_component(git_ceiling "${output_dir}/.." REALPATH)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${git_ceiling}"
            "${AUDREAM_GIT_EXECUTABLE}" apply --recount --unidiff-zero "${AUDREAM_TRANSPORT_PATCH}"
        WORKING_DIRECTORY "${output_dir}"
        RESULT_VARIABLE result ERROR_VARIABLE failure)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Failed to apply Audream transport overlay: ${failure}")
    endif()
    foreach(entry IN LISTS expected_files)
        string(REPLACE "|" ";" fields "${entry}")
        list(GET fields 0 relative_path)
        list(GET fields 1 original_hash)
        file(SHA256 "${output_dir}/${relative_path}" patched_hash)
        if(patched_hash STREQUAL original_hash)
            message(FATAL_ERROR "Transport patch was not applied: ${relative_path}")
        endif()
    endforeach()
endfunction()

function(audream_enable_transport_overlay)
    if(NOT CONFIG_AUDREAM_WATCH_UI)
        return()
    endif()
    idf_component_get_property(component_dir 78__esp-ml307 COMPONENT_DIR)
    idf_component_get_property(component_lib 78__esp-ml307 COMPONENT_LIB)
    set(output_dir "${CMAKE_BINARY_DIR}/audream_components/esp-ml307")
    audream_prepare_transport_overlay("${component_dir}" "${output_dir}")

    # Redirect all component sources, including constructors in esp_network.cc,
    # so every translation unit observes the same patched class definitions.
    get_target_property(source_dir ${component_lib} SOURCE_DIR)
    get_target_property(sources ${component_lib} SOURCES)
    set(replacement_sources)
    foreach(source IN LISTS sources)
        if(source MATCHES "\\$<")
            message(FATAL_ERROR "Unsupported generated source in transport overlay: ${source}")
        endif()
        get_filename_component(absolute_source "${source}" ABSOLUTE BASE_DIR "${source_dir}")
        file(RELATIVE_PATH relative_source "${component_dir}" "${absolute_source}")
        if(relative_source MATCHES "^src/")
            list(APPEND replacement_sources "${output_dir}/${relative_source}")
        else()
            message(FATAL_ERROR "Unreviewed transport component source: ${source}")
        endif()
    endforeach()
    set_property(TARGET ${component_lib} PROPERTY SOURCES "${replacement_sources}")
    foreach(property IN ITEMS INCLUDE_DIRECTORIES INTERFACE_INCLUDE_DIRECTORIES)
        get_target_property(directories ${component_lib} ${property})
        if(directories)
            string(REPLACE "${component_dir}" "${output_dir}" directories "${directories}")
            set_property(TARGET ${component_lib} PROPERTY ${property} "${directories}")
        endif()
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${AUDREAM_TRANSPORT_PATCH}")
endfunction()

get_filename_component(AUDREAM_TRANSPORT_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/../patches/esp-ml307-3.6.4-lifecycle.patch" ABSOLUTE)
