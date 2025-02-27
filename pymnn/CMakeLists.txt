# The CMakeLists.txt be used for PC (Windows, Mac, Linux) and Android

cmake_minimum_required(VERSION 3.4.1)
project(mnnpybridge)

option(DEPEND_AAPL_FMWK "use dependency library .framework instead of traditional .a/.dylib" OFF)
option(MNN_BUILD_SHARED_LIBS "MNN build shared or static lib" ON)
option(MNN_WIN_RUNTIME_MT "MNN use /MT on Windows dll" OFF)
option(PYMNN_USE_ALINNPYTHON "based on AliNNPython" ON)
option(PYMNN_RUNTIME_CHECK_VM "AliNNPython version (new/old) can be check on runtime" ON)
option(PYMNN_NEW_PYTHON "AliNNPython new version (when PYMNN_RUNTIME_CHECK_VM=OFF)" ON)
option(PYMNN_EXPR_API "MNN expr API be exposed" ON)
option(PYMNN_NUMPY_USABLE "Build based on numpy" ON)
option(PYMNN_TRAIN_API "MNN train API be exposed" OFF)

file(GLOB_RECURSE SRC ${CMAKE_CURRENT_LIST_DIR}/src/MNN.cc)
if (MNN_BUILD_SHARED_LIBS)
    add_library(mnnpybridge SHARED ${SRC})
else()
    add_library(mnnpybridge STATIC ${SRC})
endif()

if(PYMNN_USE_ALINNPYTHON)
    target_compile_definitions(mnnpybridge PRIVATE PYMNN_USE_ALINNPYTHON)
endif()
if(PYMNN_RUNTIME_CHECK_VM)
    target_compile_definitions(mnnpybridge PRIVATE PYMNN_RUNTIME_CHECK_VM)
endif()
if(PYMNN_NEW_PYTHON)
    target_compile_definitions(mnnpybridge PRIVATE PYMNN_NEW_PYTHON)
endif()
if(PYMNN_EXPR_API)
    target_compile_definitions(mnnpybridge PRIVATE PYMNN_EXPR_API)
endif()
if(PYMNN_NUMPY_USABLE)
    target_compile_definitions(mnnpybridge PRIVATE PYMNN_NUMPY_USABLE)
endif()
if(PYMNN_TRAIN_API)
    target_compile_definitions(mnnpybridge PRIVATE PYMNN_TRAIN_API)
endif()

if(CMAKE_SYSTEM_NAME MATCHES "^Android")
    add_definitions(-DMNN_USE_LOGCAT)
endif()

if(MSVC)
    foreach(flag_var
        CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
        CMAKE_C_FLAGS_MINSIZEREL CMAKE_C_FLAGS_RELWITHDEBINFO
        CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
        CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)
        if (MNN_WIN_RUNTIME_MT)
            if(${flag_var} MATCHES "/MD")
                string(REGEX REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
            endif()
        else ()
            if(${flag_var} MATCHES "/MT")
                string(REGEX REPLACE "/MT" "/MD" ${flag_var} "${${flag_var}}")
            endif()
        endif ()
    endforeach()
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4005 /wd4267")
    set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4005 /wd4267")
    SET(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")
    SET(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")
    SET(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /Zi")
    SET(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
else()
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-stack-protector -std=gnu99 -O2 -flax-vector-conversions -fvisibility=hidden")
    set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-stack-protector -std=c++11 -O2 -fvisibility=hidden -fvisibility-inlines-hidden")
endif()

if(PYMNN_TRAIN_API)
    set(MNN_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
    target_include_directories(mnnpybridge PRIVATE
        ${MNN_DIR}/tools/train/source/grad ${MNN_DIR}/tools/train/source/optimizer ${MNN_DIR}/tools/train/source/transformer
        ${MNN_DIR}/tools/train/source/data ${MNN_DIR}/schema/current ${MNN_DIR}/3rd_party/flatbuffers/include)
endif()

if(WIN32 OR APPLE OR CMAKE_SYSTEM_NAME MATCHES "^Linux")
    set(DEPEND_PATH "${CMAKE_CURRENT_LIST_DIR}/3rd_party")
    set(LIB_SUBPATH "")
    if(WIN32)
        if(NOT MNN_BUILD_SHARED_LIBS)
            set(LIB_SUBPATH "Static")
        elseif(MNN_WIN_RUNTIME_MT)
            set(LIB_SUBPATH "MT")
        else()
            set(LIB_SUBPATH "MD")
        endif()
    elseif(APPLE)
        if(MNN_BUILD_SHARED_LIBS)
            set(LIB_SUBPATH "Dynamic")
        else()
            set(LIB_SUBPATH "Static")
        endif()
    endif()
    if(CMAKE_BUILD_TYPE MATCHES Debug)
        set(LIB_SUBPATH "Debug/${LIB_SUBPATH}")
    else()
        set(LIB_SUBPATH "Release/${LIB_SUBPATH}")
    endif()
    if(WIN32)
        if("${CMAKE_SIZEOF_VOID_P}" STREQUAL "4")
            set(LIB_SUBPATH "x86/${LIB_SUBPATH}")
        else()
            set(LIB_SUBPATH "x64/${LIB_SUBPATH}")
        endif()
    endif()

    target_include_directories(mnnpybridge PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src ${DEPEND_PATH}/MNN/include)
    target_link_directories(mnnpybridge PRIVATE ${DEPEND_PATH}/MNN/lib/${LIB_SUBPATH})
    if(APPLE AND DEPEND_AAPL_FMWK)
        target_link_libraries(mnnpybridge PRIVATE "-framework MNN")
        set_target_properties(mnnpybridge PROPERTIES LINK_FLAGS "-Wl,-F${DEPEND_PATH}/MNN/lib/${LIB_SUBPATH}")
    else()
        target_link_libraries(mnnpybridge PRIVATE MNN)
    endif()

    if(PYMNN_USE_ALINNPYTHON)
        target_include_directories(mnnpybridge PRIVATE ${DEPEND_PATH}/AliNNPython/include)
        target_link_directories(mnnpybridge PRIVATE ${DEPEND_PATH}/AliNNPython/lib/${LIB_SUBPATH})
        if(APPLE AND DEPEND_AAPL_FMWK)
            target_link_libraries(mnnpybridge PRIVATE "-framework python")
            set_target_properties(mnnpybridge PROPERTIES LINK_FLAGS "-Wl,-F${DEPEND_PATH}/AliNNPython/lib/${LIB_SUBPATH}")
        else()
            target_link_libraries(mnnpybridge PRIVATE python)
        endif()
    endif()
    if(PYMNN_NUMPY_USABLE)
        target_include_directories(mnnpybridge PRIVATE ${DEPEND_PATH}/numpy/include)
        target_link_directories(mnnpybridge PRIVATE ${DEPEND_PATH}/numpy/lib/${LIB_SUBPATH})
        if(APPLE AND DEPEND_AAPL_FMWK)
            target_link_libraries(mnnpybridge PRIVATE "-framework numpy_python")
            set_target_properties(mnnpybridge PROPERTIES LINK_FLAGS "-Wl,-F${DEPEND_PATH}/numpy/lib/${LIB_SUBPATH}")
        else()
            target_link_libraries(mnnpybridge PRIVATE numpy_python)
        endif()
    endif()
else()
    target_include_directories(mnnpybridge PRIVATE ${MNN_DIR}/pymnn/src ${MNN_DIR}/pymnn/android/src/main/c/include)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${MNN_DIR}/pymnn/android/src/main/jniLibs/${ANDROID_ABI})
    target_link_libraries(mnnpybridge PRIVATE log MNN MNN_Express)
    if(PYMNN_USE_ALINNPYTHON)
        target_link_libraries(mnnpybridge PRIVATE AliNNPython)
    endif()
    if(PYMNN_NUMPY_USABLE)
        target_link_libraries(mnnpybridge PRIVATE numpy_python)
    endif()
endif()
