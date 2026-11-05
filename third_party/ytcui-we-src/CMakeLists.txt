cmake_minimum_required(VERSION 3.16)

find_package(CURL REQUIRED)

find_package(Threads REQUIRED)

add_library(pugixml_we STATIC vendor/pugixml/pugixml.cpp)
target_include_directories(pugixml_we PUBLIC vendor/pugixml)
set_target_properties(pugixml_we PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(ytcui-we cli/ytcui-we.cpp)
target_include_directories(ytcui-we PRIVATE
    include
    vendor/pugixml
    cli
)
target_compile_features(ytcui-we PRIVATE cxx_std_17)
target_link_libraries(ytcui-we PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    CURL::libcurl
    Threads::Threads
    pugixml_we
)
target_compile_options(ytcui-we PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
        -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
    >
    $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,13.0>>:-Wno-restrict>
)

# Copy EJS solver assets next to the binary at build time.
# ytcui-we looks for vendor/ejs/ relative to its own executable.
add_custom_command(TARGET ytcui-we POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/ejs
        $<TARGET_FILE_DIR:ytcui-we>/vendor/ejs
    COMMENT "Installing EJS solver assets"
)

install(TARGETS ytcui-we DESTINATION bin)
install(DIRECTORY vendor/ejs DESTINATION share/ytcui-we)
