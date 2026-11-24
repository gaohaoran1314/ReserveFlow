# =============================================================================
# ReserveFlow P0 — 根构建脚本
# 目标：CLion / 命令行 直接打开即可构建，**构建期零联网**（依赖全部预置于 third_party/）
# 工具链：CMake >= 3.20 + Ninja + MSVC 14.51，C++20
# =============================================================================
cmake_minimum_required(VERSION 3.20)

# CMake 4.x 默认拒绝 cmake_minimum_required < 3.5 的子工程；此处放开策略下限，
# 保证引入的第三方旧工程（若将来 add_subdirectory）不会因策略版本报错。
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# =============================================================================
# Windows 下自动装配 MSVC 环境（必须在 project() 之前执行）
#
# 背景：CLion 默认工具链是自带的 MinGW，而本工程依赖 MSVC 专有的 localtime_s
#       与 MySQL 的 libmysql.lib，用 MinGW 编不过；而 CLion 又探测不到 VS 2026
#       的工具集（v18 太新）。所以这里自己去找 cl.exe，把 INCLUDE / LIB / PATH
#       装配好，让任何配置（包括默认的 MinGW 配置）都能构建成功。
# 若已在 vcvars 环境里（INCLUDE 已含 MSVC 路径），则完全跳过，不干扰正常用法。
# =============================================================================
if(WIN32 AND NOT DEFINED RF_MSVC_BOOTSTRAP_DONE)
    set(RF_MSVC_BOOTSTRAP_DONE TRUE)

    if(NOT "$ENV{INCLUDE}" MATCHES "MSVC")
        set(_rf_vs_roots
            "C:/Program Files/Microsoft Visual Studio/18/Community"
            "C:/Program Files/Microsoft Visual Studio/18/Professional"
            "C:/Program Files/Microsoft Visual Studio/18/Enterprise"
            "C:/Program Files/Microsoft Visual Studio/2022/Community"
            "C:/Program Files/Microsoft Visual Studio/2022/Professional"
            "C:/Program Files/Microsoft Visual Studio/2022/Enterprise"
            "C:/Program Files (x86)/Microsoft Visual Studio/2019/Community")

        set(RF_MSVC_DIR "")
        foreach(_root IN LISTS _rf_vs_roots)
            if(EXISTS "${_root}/VC/Tools/MSVC")
                file(GLOB _vers LIST_DIRECTORIES true "${_root}/VC/Tools/MSVC/*")
                list(SORT _vers)
                foreach(_v IN LISTS _vers)
                    if(IS_DIRECTORY "${_v}" AND EXISTS "${_v}/bin/Hostx64/x64/cl.exe")
                        set(RF_MSVC_DIR "${_v}")
                    endif()
                endforeach()
            endif()
            if(RF_MSVC_DIR)
                break()
            endif()
        endforeach()

        set(RF_SDK_DIR "")
        if(EXISTS "C:/Program Files (x86)/Windows Kits/10/Include")
            file(GLOB _sdk_vers LIST_DIRECTORIES true "C:/Program Files (x86)/Windows Kits/10/Include/10.0.*")
            list(SORT _sdk_vers)
            foreach(_s IN LISTS _sdk_vers)
                if(IS_DIRECTORY "${_s}" AND EXISTS "${_s}/um/windows.h")
                    set(RF_SDK_DIR "${_s}")
                endif()
            endforeach()
        endif()

        if(RF_MSVC_DIR AND RF_SDK_DIR)
            get_filename_component(_sdk_ver "${RF_SDK_DIR}" NAME)
            set(_sdk_root "C:/Program Files (x86)/Windows Kits/10")
            set(ENV{INCLUDE} "${RF_MSVC_DIR}/include;${_sdk_root}/Include/${_sdk_ver}/ucrt;${_sdk_root}/Include/${_sdk_ver}/shared;${_sdk_root}/Include/${_sdk_ver}/um;${_sdk_root}/Include/${_sdk_ver}/winrt")
            set(ENV{LIB} "${RF_MSVC_DIR}/lib/x64;${_sdk_root}/Lib/${_sdk_ver}/ucrt/x64;${_sdk_root}/Lib/${_sdk_ver}/um/x64")
            set(ENV{PATH} "${RF_MSVC_DIR}/bin/Hostx64/x64;${_sdk_root}/bin/${_sdk_ver}/x64;$ENV{PATH}")
            set(CMAKE_C_COMPILER   "${RF_MSVC_DIR}/bin/Hostx64/x64/cl.exe" CACHE FILEPATH "" FORCE)
            set(CMAKE_CXX_COMPILER "${RF_MSVC_DIR}/bin/Hostx64/x64/cl.exe" CACHE FILEPATH "" FORCE)
            set(CMAKE_LINKER       "${RF_MSVC_DIR}/bin/Hostx64/x64/link.exe" CACHE FILEPATH "" FORCE)

            # 关键：环境变量只影响 configure 进程，编译/链接由 ninja 另行拉起，
            # 因此必须把搜索路径直接写进编译与链接命令（/I 与 /LIBPATH）。
            set(RF_MSVC_INCLUDE_DESC
                "${RF_MSVC_DIR}/include;${_sdk_root}/Include/${_sdk_ver}/ucrt;${_sdk_root}/Include/${_sdk_ver}/shared;${_sdk_root}/Include/${_sdk_ver}/um;${_sdk_root}/Include/${_sdk_ver}/winrt"
                CACHE STRING "" FORCE)
            set(RF_MSVC_LIB_DESC
                "${RF_MSVC_DIR}/lib/x64;${_sdk_root}/Lib/${_sdk_ver}/ucrt/x64;${_sdk_root}/Lib/${_sdk_ver}/um/x64"
                CACHE STRING "" FORCE)

            message(STATUS "ReserveFlow: 自动装配 MSVC -> ${RF_MSVC_DIR}")
            message(STATUS "ReserveFlow: 自动装配 Windows SDK -> ${_sdk_ver}")
        else()
            message(WARNING "ReserveFlow: 未找到 Visual Studio (MSVC) 或 Windows SDK，构建可能失败")
        endif()
    endif()
endif()

project(reserveflow VERSION 0.1.0 LANGUAGES C CXX)

# 把自动装配得到的头文件/库搜索路径注入所有目标（见上方说明）
if(RF_MSVC_INCLUDE_DESC)
    include_directories(${RF_MSVC_INCLUDE_DESC})
endif()
if(RF_MSVC_LIB_DESC)
    link_directories(${RF_MSVC_LIB_DESC})
endif()

# ---------------------------------------------------------------- 工具链闸门
# 本项目必须用 MSVC 构建，原因有二，均无法绕开：
#   1) src/Base.h 使用 MSVC 专有的 localtime_s(struct tm*, const time_t*)，GCC/MinGW 无此函数
#   2) 需要链接 MySQL 官方的 libmysql.lib（MSVC 导入库格式）与 Windows CNG（bcrypt）
# 若此处不拦截，MinGW 会抛出一整屏看不懂的模板报错，故直接给出可执行的修复指引。
if(NOT MSVC)
    message(FATAL_ERROR
        "\n"
        "  [ReserveFlow] 当前编译器不是 MSVC，无法构建本项目。\n"
        "  当前编译器: ${CMAKE_CXX_COMPILER}\n"
        "\n"
        "  修复步骤（CLion）：\n"
        "    1. Settings → Build, Execution, Deployment → Toolchains\n"
        "    2. 新增或选择一个 Visual Studio 工具链（本机为 VS 2026，自带 MSVC 14.51）\n"
        "    3. Settings → Build, Execution, Deployment → CMake → 把 Toolchain 改成上面这个\n"
        "    4. 删除项目下的 cmake-build-debug 目录，然后 Reload CMake Project\n"
        "\n")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 单配置生成器（Ninja）下未指定构建类型时默认 Debug，避免 CLion 直接打开无构建类型
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ------------------------------------------------------------------ 路径
set(RF_ROOT        ${CMAKE_CURRENT_SOURCE_DIR})
set(RF_THIRD_PARTY ${RF_ROOT}/third_party)

# MySQL C API 安装位置（可用 -DMYSQL_ROOT=... 覆盖）
set(MYSQL_ROOT "C:/Program Files/MySQL/MySQL Server 8.0" CACHE PATH "MySQL Server 8.0 root")
set(MYSQL_INCLUDE_DIR "${MYSQL_ROOT}/include")
set(MYSQL_LIB         "${MYSQL_ROOT}/lib/libmysql.lib")
set(MYSQL_DLL         "${MYSQL_ROOT}/lib/libmysql.dll")

if(NOT EXISTS "${MYSQL_INCLUDE_DIR}/mysql.h")
    message(FATAL_ERROR "找不到 ${MYSQL_INCLUDE_DIR}/mysql.h，请用 -DMYSQL_ROOT=<MySQL 安装目录> 指定")
endif()
if(NOT EXISTS "${MYSQL_LIB}")
    message(FATAL_ERROR "找不到 ${MYSQL_LIB}，请用 -DMYSQL_ROOT=<MySQL 安装目录> 指定")
endif()

# 依赖完整性检查：构建期绝不联网，缺文件直接报错退出
foreach(required_file
        "${RF_THIRD_PARTY}/httplib/httplib.h"
        "${RF_THIRD_PARTY}/nlohmann/json.hpp"
        "${RF_THIRD_PARTY}/spdlog/include/spdlog/spdlog.h"
        "${RF_THIRD_PARTY}/hiredis/hiredis.c"
        "${RF_THIRD_PARTY}/hiredis/hiredis.h")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "third_party 依赖缺失：${required_file}（本工程禁止构建期下载，请先补齐 third_party/）")
    endif()
endforeach()

# ------------------------------------------------------------------ hiredis 静态库
# 不用其自带 CMakeLists（本机 CMake 4.0 与 MSVC 组合易踩策略/安装规则），
# 直接按源码编译静态库；sockcompat.c 提供 Winsock 兼容层，Windows 必需。
add_library(rf_hiredis STATIC
    "${RF_THIRD_PARTY}/hiredis/hiredis.c"
    "${RF_THIRD_PARTY}/hiredis/net.c"
    "${RF_THIRD_PARTY}/hiredis/read.c"
    "${RF_THIRD_PARTY}/hiredis/sds.c"
    "${RF_THIRD_PARTY}/hiredis/alloc.c"
    "${RF_THIRD_PARTY}/hiredis/async.c"
    "${RF_THIRD_PARTY}/hiredis/sockcompat.c")

target_include_directories(rf_hiredis PUBLIC "${RF_THIRD_PARTY}/hiredis")
target_compile_definitions(rf_hiredis PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    _WINSOCK_DEPRECATED_NO_WARNINGS)

if(WIN32)
    target_link_libraries(rf_hiredis PUBLIC ws2_32)
endif()
if(MSVC)
    target_compile_options(rf_hiredis PRIVATE /utf-8 /W0)
endif()

# ------------------------------------------------------------------ 主程序
set(RF_SOURCES
    src/main.cpp
    src/JwtUtil.cpp
    src/DbPool.cpp
    src/RedisClient.cpp
    src/UserRepo.cpp
    src/ResourceRepo.cpp
    src/AuditRepo.cpp
    src/AuthService.cpp
    src/ResourceService.cpp
    src/AuthController.cpp
    src/ResourceController.cpp
    src/SystemController.cpp
    src/AuthMiddleware.cpp
    src/StateMachine.cpp
    src/DistributedLock.cpp
    src/ReservationRepo.cpp
    src/ReservationService.cpp
    src/ReservationController.cpp
    src/SettlementRepo.cpp
    src/SettlementService.cpp
    src/WaitlistRepo.cpp
    src/WaitlistService.cpp
    src/WaitlistController.cpp
    src/Scheduler.cpp
    src/MaintenanceJob.cpp
    src/RuleRepo.cpp
    src/RuleCache.cpp
    src/ConditionEval.cpp
    src/RuleEngine.cpp
    src/CreditRepo.cpp
    src/CreditService.cpp
    src/RuleController.cpp
    src/CreditController.cpp
    src/Application.cpp)

add_executable(reserveflow ${RF_SOURCES})

target_include_directories(reserveflow PRIVATE
    "${RF_ROOT}/src"
    "${RF_THIRD_PARTY}/httplib"
    "${RF_THIRD_PARTY}/nlohmann"
    "${RF_THIRD_PARTY}/spdlog/include"
    "${MYSQL_INCLUDE_DIR}")

# spdlog 默认即 header-only（除非定义 SPDLOG_COMPILED_LIB），此处不重复定义
# SPDLOG_HEADER_ONLY，避免 C4005 宏重定义告警；只使用其 bundled fmt。
target_compile_definitions(reserveflow PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    _WINSOCK_DEPRECATED_NO_WARNINGS
    UNICODE
    _UNICODE)

if(MSVC)
    # /utf-8 必须加，否则源码中文字面量在 MSVC 下会被按 GBK 解释导致乱码/警告
    target_compile_options(reserveflow PRIVATE /utf-8 /W3 /bigobj /MP)
    target_compile_options(reserveflow PRIVATE $<$<CONFIG:Debug>:/Od>)
endif()

target_link_libraries(reserveflow PRIVATE
    rf_hiredis
    "${MYSQL_LIB}"
    bcrypt
    ws2_32)

# ------------------------------------------------------------------ 运行期产物布置
# MySQL 导入库对应的 DLL 必须与 exe 同目录，否则启动即 0xc0000135
if(EXISTS "${MYSQL_DLL}")
    add_custom_command(TARGET reserveflow POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MYSQL_DLL}" "$<TARGET_FILE_DIR:reserveflow>/libmysql.dll"
        COMMENT "Copying libmysql.dll -> $<TARGET_FILE_DIR:reserveflow>")
endif()

# 运行期目录（日志、/debug 静态页）预创建，避免首次运行因目录缺失报错
add_custom_command(TARGET reserveflow POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:reserveflow>/logs"
    COMMENT "Ensuring logs/ directory")

# /debug 自测台：单文件页面拷到可执行目录的 debug/ 下，
# 服务端按 <exeDir>/debug 挂载（见 Application::resolveDebugRoot）
add_custom_command(TARGET reserveflow POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:reserveflow>/debug"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RF_ROOT}/web/debug/index.html"
            "$<TARGET_FILE_DIR:reserveflow>/debug/index.html"
    COMMENT "Copying web/debug/index.html -> $<TARGET_FILE_DIR:reserveflow>/debug")


# =============================================================================
# P4 T12：把前端产物拷到可执行文件旁边（让 resolveAdminRoot 的首选项 <exeDir>/admin 命中）
#
# ⚠️ copy_directory 与 copy_if_different 不同：**源目录不存在会直接中断 ninja**。
#    所以必须用 if(EXISTS) 在**配置期**守卫；dist 还没 build 时跳过，构建照常成功。
#    另外先 remove_directory 再拷：Vite 每次构建换 hash 文件名，不清会堆旧 assets。
# =============================================================================
# RF_ROOT = <repo>/back/ReserveFlow，仓库根是 ${RF_ROOT}/../..
set(RF_ADMIN_DIST "${RF_ROOT}/../../front/admin/dist")
if(EXISTS "${RF_ADMIN_DIST}")
    add_custom_command(TARGET reserveflow POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove_directory "$<TARGET_FILE_DIR:reserveflow>/admin"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${RF_ADMIN_DIST}" "$<TARGET_FILE_DIR:reserveflow>/admin"
        COMMENT "拷贝管理后台到 /admin（${RF_ADMIN_DIST}）")
else()
    message(STATUS "未找到 ${RF_ADMIN_DIST}，跳过 /admin 静态托管（先 cd front/admin && npm run build）")
endif()

# =============================================================================
# P5 T12：把用户端 H5 产物拷到可执行文件旁边（让 resolveH5Root 的首选项 <exeDir>/h5 命中）
# 同样用 if(EXISTS) 在配置期守卫：copy_directory 源目录不存在会直接中断 ninja。
# =============================================================================
set(RF_H5_DIST "${RF_ROOT}/../../front/h5/dist")
if(EXISTS "${RF_H5_DIST}")
    add_custom_command(TARGET reserveflow POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove_directory "$<TARGET_FILE_DIR:reserveflow>/h5"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${RF_H5_DIST}" "$<TARGET_FILE_DIR:reserveflow>/h5"
        COMMENT "拷贝用户端 H5 到 /h5（${RF_H5_DIST}）")
else()
    message(STATUS "未找到 ${RF_H5_DIST}，跳过 /h5 静态托管（先 cd front/h5 && npm run build）")
endif()

message(STATUS "ReserveFlow: C++20 / ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "ReserveFlow: MySQL include=${MYSQL_INCLUDE_DIR}")
message(STATUS "ReserveFlow: third_party=${RF_THIRD_PARTY}")
