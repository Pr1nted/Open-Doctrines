# Satisfies libdatachannel's find_package(MbedTLS) using the mbedTLS this
# project already fetches, instead of one installed on the machine.
#
# WHY THIS FILE EXISTS
#
# libdatachannel looks for an INSTALLED mbedTLS: headers in a prefix, .a files
# on disk. Ours is built from source by FetchContent as part of this build, so
# at configure time the libraries do not exist yet and find_library cannot see
# them. Pointing it at paths that will exist later would also drop the
# dependency edge, so the link order would be luck.
#
# Handing it the TARGETS instead keeps that edge: CMake knows libdatachannel
# needs mbedTLS built first, because it says so.
#
# This is on CMAKE_MODULE_PATH ahead of libdatachannel's own module directory,
# so it is found first.

if(TARGET mbedtls AND TARGET mbedcrypto AND TARGET mbedx509)
    set(MbedTLS_FOUND TRUE)
    set(MBEDTLS_FOUND TRUE)

    # The version is read from the source tree, which does exist by now.
    if(DEFINED mbedtls_SOURCE_DIR)
        set(MbedTLS_INCLUDE_DIR "${mbedtls_SOURCE_DIR}/include")
        set(MBEDTLS_INCLUDE_DIRS "${MbedTLS_INCLUDE_DIR}")
    endif()

    # libdatachannel links these names; alias rather than duplicate, so there is
    # one mbedTLS in the build and not two with different settings.
    if(NOT TARGET MbedTLS::mbedtls)
        add_library(MbedTLS::mbedtls ALIAS mbedtls)
    endif()
    if(NOT TARGET MbedTLS::mbedcrypto)
        add_library(MbedTLS::mbedcrypto ALIAS mbedcrypto)
    endif()
    if(NOT TARGET MbedTLS::mbedx509)
        add_library(MbedTLS::mbedx509 ALIAS mbedx509)
    endif()

    # libdatachannel spells them capitalised. Both spellings are provided
    # because which one it uses is its business, not something to track.
    if(NOT TARGET MbedTLS::MbedTLS)
        add_library(MbedTLS::MbedTLS ALIAS mbedtls)
    endif()
    if(NOT TARGET MbedTLS::MbedCrypto)
        add_library(MbedTLS::MbedCrypto ALIAS mbedcrypto)
    endif()
    if(NOT TARGET MbedTLS::MbedX509)
        add_library(MbedTLS::MbedX509 ALIAS mbedx509)
    endif()

    set(MbedTLS_LIBRARY    mbedtls)
    set(MbedCrypto_LIBRARY mbedcrypto)
    set(MbedX509_LIBRARY   mbedx509)
    set(MBEDTLS_LIBRARIES  mbedtls mbedx509 mbedcrypto)
else()
    set(MbedTLS_FOUND FALSE)
endif()
