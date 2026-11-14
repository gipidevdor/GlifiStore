# Discovers LibreSSL (first-class on OpenBSD) or OpenSSL 3.x for the secure profile.
# Sets:
#   GLIFISTORE_TLS_FOUND
#   GLIFISTORE_TLS_BACKEND   ("LibreSSL" or "OpenSSL")
#   GlifiStore::tls          INTERFACE imported target (when found)

include_guard(GLOBAL)

set(GLIFISTORE_TLS_FOUND FALSE)
set(GLIFISTORE_TLS_BACKEND "")

if(CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
    find_path(GLIFISTORE_TLS_INCLUDE_DIR
        NAMES openssl/ssl.h
        PATHS /usr/include
        NO_DEFAULT_PATH
    )
    find_library(GLIFISTORE_TLS_SSL_LIBRARY
        NAMES ssl libssl
        PATHS /usr/lib
        NO_DEFAULT_PATH
    )
    find_library(GLIFISTORE_TLS_CRYPTO_LIBRARY
        NAMES crypto libcrypto
        PATHS /usr/lib
        NO_DEFAULT_PATH
    )
    if(GLIFISTORE_TLS_INCLUDE_DIR AND GLIFISTORE_TLS_SSL_LIBRARY AND GLIFISTORE_TLS_CRYPTO_LIBRARY)
        set(GLIFISTORE_TLS_FOUND TRUE)
        set(GLIFISTORE_TLS_BACKEND "LibreSSL")
        if(NOT TARGET GlifiStore::tls)
            add_library(GlifiStore::tls INTERFACE IMPORTED)
            target_include_directories(GlifiStore::tls INTERFACE "${GLIFISTORE_TLS_INCLUDE_DIR}")
            target_link_libraries(GlifiStore::tls INTERFACE
                "${GLIFISTORE_TLS_SSL_LIBRARY}"
                "${GLIFISTORE_TLS_CRYPTO_LIBRARY}"
            )
        endif()
    endif()
else()
    set(_glifistore_tls_prefixes ${CMAKE_PREFIX_PATH})
    if(DEFINED OPENSSL_ROOT_DIR)
        list(PREPEND _glifistore_tls_prefixes "${OPENSSL_ROOT_DIR}")
    endif()
    if(DEFINED ENV{OPENSSL_ROOT_DIR})
        list(PREPEND _glifistore_tls_prefixes "$ENV{OPENSSL_ROOT_DIR}")
    endif()
    foreach(_root IN ITEMS
            /opt/local
            /opt/homebrew/opt/openssl@3
            /usr/local/opt/openssl@3
            /usr/local/opt/openssl)
        if(EXISTS "${_root}")
            list(APPEND _glifistore_tls_prefixes "${_root}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _glifistore_tls_prefixes)

    set(_glifistore_tls_saved_prefix "${CMAKE_PREFIX_PATH}")
    set(CMAKE_PREFIX_PATH "${_glifistore_tls_prefixes}")
    find_package(OpenSSL 3 QUIET)
    set(CMAKE_PREFIX_PATH "${_glifistore_tls_saved_prefix}")
    unset(_glifistore_tls_prefixes)
    unset(_glifistore_tls_saved_prefix)

    if(OpenSSL_FOUND)
        set(GLIFISTORE_TLS_FOUND TRUE)
        set(GLIFISTORE_TLS_BACKEND "OpenSSL")
        if(NOT TARGET GlifiStore::tls)
            add_library(GlifiStore::tls INTERFACE IMPORTED)
            target_link_libraries(GlifiStore::tls INTERFACE OpenSSL::SSL OpenSSL::Crypto)
        endif()
    endif()
endif()

mark_as_advanced(
    GLIFISTORE_TLS_INCLUDE_DIR
    GLIFISTORE_TLS_SSL_LIBRARY
    GLIFISTORE_TLS_CRYPTO_LIBRARY
)
