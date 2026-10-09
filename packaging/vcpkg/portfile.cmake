vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO physicalcontextprotocol/pmcp-cpp
    REF "v${VERSION}"
    SHA512 fcea1b97c5cfb4b03690a81abcba47e27073634afb9f78c5616c8dc5840ad8d588d88f577e7c839fff5ea0f336c3d707666f5a5cd90fbe801e92fd00cd187c10
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DPMCP_BUILD_TESTS=OFF
        -DPMCP_BUILD_EXAMPLES=OFF
        -DPMCP_WITH_ROS2=OFF
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/pmcp")

# Static library: drop the debug copies of headers and the CMake package files
# that vcpkg_cmake_config_fixup leaves behind under debug/.
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/lib/cmake"
)

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
