if(NOT DEFINED DEST)
    message(FATAL_ERROR "DEST is required")
endif()

file(GLOB onnxruntime_dylibs "${DEST}/libonnxruntime*.dylib")
if(onnxruntime_dylibs)
    file(REMOVE ${onnxruntime_dylibs})
endif()
