function(pitchnet_setup_asio out_var)
    set(ASIO_SDK_ROOT "" CACHE PATH "Path to ASIO SDK root (contains common/asio.h)")

    set(asio_candidates "")
    if(ASIO_SDK_ROOT)
        list(APPEND asio_candidates "${ASIO_SDK_ROOT}")
    endif()
    if(DEFINED ENV{ASIO_SDK_ROOT})
        list(APPEND asio_candidates "$ENV{ASIO_SDK_ROOT}")
    endif()
    list(APPEND asio_candidates
        "${CMAKE_SOURCE_DIR}/third_party/ASIO_SDK"
        "${CMAKE_SOURCE_DIR}/third_party/JUCE/modules/juce_audio_devices/native/asio"
    )

    set(asio_include_dir "")
    foreach(candidate IN LISTS asio_candidates)
        if(EXISTS "${candidate}/asio.h")
            set(asio_include_dir "${candidate}")
            break()
        endif()
        if(EXISTS "${candidate}/common/asio.h")
            set(asio_include_dir "${candidate}/common")
            break()
        endif()
    endforeach()

    if(NOT asio_include_dir)
        message(FATAL_ERROR
            "ASIO SDK not found. Set ASIO_SDK_ROOT to the SDK root or disable USE_ASIO. "
            "Tried: ${asio_candidates}")
    endif()

    set(${out_var} "${asio_include_dir}" PARENT_SCOPE)
endfunction()
