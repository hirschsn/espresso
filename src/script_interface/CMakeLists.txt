file(GLOB_RECURSE EspressoScriptInterface_SRC
          "*.?pp"
          )

add_library(EspressoScriptInterface SHARED ${EspressoScriptInterface_SRC})
add_dependencies(EspressoScriptInterface EspressoConfig EspressoConfig)
set_target_properties(EspressoScriptInterface PROPERTIES MACOSX_RPATH TRUE)
target_link_libraries(EspressoScriptInterface EspressoCore)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${CMAKE_SOURCE_DIR}/src/core)
include_directories(${CMAKE_BINARY_DIR}/src/core)
