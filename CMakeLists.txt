cmake_minimum_required(VERSION 3.10)

# Название проекта
project(emonitor CXX)

# Установка стандартной версии C++
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Определение путей
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../functions/src)
set(INC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../functions/src)

# Добавление директории с заголовочными файлами
include_directories(${INC_DIR})

# Поиск всех исходных файлов
set(SOURCES
    main.cpp
    ${SRC_DIR}/mtimer.cpp
    ${SRC_DIR}/mlogger.cpp
    ${SRC_DIR}/iniobject.cpp
)



# Находим пакет glib-2.0 и его заголовочные файлы
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB2 REQUIRED glib-2.0)
find_package(CURL REQUIRED)


# Поиск и добавление библиотеки libmodbus
find_library(LIBMODBUS_LIB modbus)
if(NOT LIBMODBUS_LIB)
    message(FATAL_ERROR "Библиотека libmodbus не найдена. Установите libmodbus-dev")
endif()

# Поиск заголовочных файлов modbus в указанном пути
find_path(LIBMODBUS_INCLUDE_DIR
    NAMES
    	modbus.h
    HINTS
        /usr/local/include/modbus
    PATH_SUFFIXES
        include
    DOC "Путь к заголовочным файлам modbus"
)
if(NOT LIBMODBUS_INCLUDE_DIR)
    message(FATAL_ERROR
        "Заголовочные файлы modbus не найдены.
        Проверьте установку libmodbus.")
endif()

# Добавление исполняемого файла
add_executable(${PROJECT_NAME} ${SOURCES})

# Указываем директории для поиска заголовочных файлов
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../my_inc
    ${LIBMODBUS_INCLUDE_DIR}
    ${GLIB2_INCLUDE_DIRS}    
)

# Добавляем общие флаги компилятора
target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall
    -Wextra
    -pipe
    -march=native
    -MMD
    -MP
    -Wpmf-conversions
    -fexceptions
)

# Настройки для конфигурации Debug
target_compile_definitions(${PROJECT_NAME}
    PRIVATE
        $<$<CONFIG:Debug>:__DEBUG__>
)
target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Debug>:-g -O0>
)

# Настройки для конфигурации Release
target_compile_definitions(${PROJECT_NAME}
    PRIVATE
        $<$<CONFIG:Release>:__RELEASE__>
)
target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Release>:-O2 -DNDEBUG>
)


# Связывание с библиотекой libmodbus
target_link_libraries(${PROJECT_NAME} 
        pthread
        ${GLIB2_LIBRARIES}
        ${LIBMODBUS_LIB}
        ${CURL_LIBRARIES}
)



