cmake_minimum_required(VERSION 3.10)
project(mbedtls VERSION 2.28.8)

# Отключаем всё лишнее
option(ENABLE_PROGRAMS "Build mbedTLS programs" OFF)
option(ENABLE_TESTING "Build mbedTLS tests" OFF)
option(ENABLE_ZLIB_SUPPORT "Build with zlib support" OFF)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Включаем поддержку Windows
if (MINGW OR WIN32)
    add_definitions(-D_WIN32_WINNT=0x0600)
    add_definitions(-DMBEDTLS_PLATFORM_SNPRINTF_MACRO=snprintf)
    add_definitions(-DMBEDTLS_ENTROPY_HARDWARE_ALT)
endif()

# Создаём фейковые цели, чтобы избежать ошибок
add_custom_target(scripts COMMAND echo "scripts disabled")
add_custom_target(pkgconfig COMMAND echo "pkgconfig disabled")

# Добавляем поддиректорию library
add_subdirectory(library)

# Устанавливаем include файлы
install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/mbedtls DESTINATION include)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/psa DESTINATION include)