set(WOLFSSL_LIBRARY wolfssl)
# Resolve relative to this file, not CMAKE_SOURCE_DIR: when rpcs3 is pulled in
# as a subproject (e.g. the Android port), CMAKE_SOURCE_DIR is the superproject.
get_filename_component(WOLFSSL_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../3rdparty/wolfssl" ABSOLUTE)
set(WOLFSSL_FOUND TRUE)
