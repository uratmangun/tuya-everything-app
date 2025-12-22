##
# @file CMakeLists.txt
# @brief 
#/

# APP_PATH
set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})

# Read .env file for Tuya credentials
if(EXISTS "${APP_PATH}/.env")
    file(STRINGS "${APP_PATH}/.env" ENV_LINES)
    foreach(LINE ${ENV_LINES})
        if(LINE MATCHES "^export ([A-Z_]+)=\"(.*)\"$")
            set(${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
        endif()
    endforeach()
endif()

# APP_NAME
get_filename_component(APP_NAME ${APP_PATH} NAME)

# APP_SRCS
aux_source_directory(${APP_PATH}/src APP_SRCS)

# APP_INC
set(APP_INC
    ${APP_PATH}/src
)

########################################
# Target Configure
########################################
add_library(${EXAMPLE_LIB})

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
    )

target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_INC}
    )

# Pass Tuya credentials from .env as compile definitions
if(TUYA_PRODUCT_ID)
    target_compile_definitions(${EXAMPLE_LIB} PRIVATE TUYA_PRODUCT_ID="${TUYA_PRODUCT_ID}")
endif()
if(TUYA_OPENSDK_UUID)
    target_compile_definitions(${EXAMPLE_LIB} PRIVATE TUYA_OPENSDK_UUID="${TUYA_OPENSDK_UUID}")
endif()
if(TUYA_OPENSDK_AUTHKEY)
    target_compile_definitions(${EXAMPLE_LIB} PRIVATE TUYA_OPENSDK_AUTHKEY="${TUYA_OPENSDK_AUTHKEY}")
endif()

# TCP Server configuration for web app communication
if(TCP_SERVER_HOST)
    target_compile_definitions(${EXAMPLE_LIB} PRIVATE TCP_SERVER_HOST="${TCP_SERVER_HOST}")
endif()
if(TCP_SERVER_PORT)
    target_compile_definitions(${EXAMPLE_LIB} PRIVATE TCP_SERVER_PORT=${TCP_SERVER_PORT})
endif()
if(TCP_AUTH_TOKEN)
    target_compile_definitions(${EXAMPLE_LIB} PRIVATE TCP_AUTH_TOKEN="${TCP_AUTH_TOKEN}")
endif()

# RTSP Tunnel configuration
if(RTSP_CAMERA_HOST)
    target_compile_definitions(${EXAMPLE_LIB} PRIVATE RTSP_CAMERA_HOST="${RTSP_CAMERA_HOST}")
endif()
if(RTSP_CAMERA_PORT)
    target_compile_definitions(${EXAMPLE_LIB} PRIVATE RTSP_CAMERA_PORT=${RTSP_CAMERA_PORT})
endif()
if(RTSP_VPS_HOST)
    target_compile_definitions(${EXAMPLE_LIB} PRIVATE RTSP_VPS_HOST="${RTSP_VPS_HOST}")
endif()
if(RTSP_VPS_PORT)
    target_compile_definitions(${EXAMPLE_LIB} PRIVATE RTSP_VPS_PORT=${RTSP_VPS_PORT})
endif()
if(RTSP_TUNNEL_ENABLED)
    target_compile_definitions(${EXAMPLE_LIB} PRIVATE RTSP_TUNNEL_ENABLED=${RTSP_TUNNEL_ENABLED})
endif()

# Skip Tuya Cloud services (web app only mode)
if(SKIP_TUYA_CLOUD)
    target_compile_definitions(${EXAMPLE_LIB} PRIVATE SKIP_TUYA_CLOUD=${SKIP_TUYA_CLOUD})
endif()

########################################
# Add subdirectory
########################################
add_subdirectory(${APP_PATH}/../../tuya.ai/ai_components/ai_audio ai_audio)
