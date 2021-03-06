if(ProjectWabtIncluded)
    return()
endif()
set(ProjectWabtIncluded TRUE)

include(ExternalProject)

set(prefix ${CMAKE_BINARY_DIR})
set(source_dir ${prefix}/wabt)
set(binary_dir ${prefix}/wabt-build)
set(include_dir ${source_dir})
set(wabt_library ${binary_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}wabt${CMAKE_STATIC_LIBRARY_SUFFIX})

ExternalProject_Add(wabt
    PREFIX ${prefix}
	DOWNLOAD_NAME wabt-1.0.13.tar.gz
	DOWNLOAD_DIR ${CMAKE_SOURCE_DIR}/downloads
    SOURCE_DIR ${source_dir}
    BINARY_DIR ${binary_dir}
	URL https://github.com/WebAssembly/wabt/archive/1.0.13.tar.gz
	URL_HASH SHA256=496dc0477029526aeaceafe0c2492e458ce89dada0e4f9640ecce1500f308a3d
    PATCH_COMMAND patch < ${CMAKE_CURRENT_LIST_DIR}/wabt-1_0_13.patch
    CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
    -DCMAKE_BUILD_TYPE=Release
    -DWITH_EXCEPTIONS=ON
    -DBUILD_TESTS=OFF
    -DBUILD_TOOLS=OFF
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_CXX_FLAGS=-fvisibility=hidden
    -DCMAKE_C_FLAGS=-fvisibility=hidden
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${wabt_library}
)

add_library(wabt::wabt STATIC IMPORTED)

set_target_properties(
    wabt::wabt
    PROPERTIES
    IMPORTED_CONFIGURATIONS Release
    IMPORTED_LOCATION_RELEASE ${wabt_library}
    INTERFACE_INCLUDE_DIRECTORIES "${include_dir};${binary_dir}"
)

add_dependencies(wabt::wabt wabt)
