# The .riv chosen in menuconfig (Rive player -> Embedded .riv file) is
# copied to a fixed name, so the embedded symbol is always
# _binary_rive_file_riv_start whichever file it is.
if(CONFIG_RIVE_EMBEDDED_FILE_NAME)
    set(RIVE_FILE_NAME "${CONFIG_RIVE_EMBEDDED_FILE_NAME}")
else()
    set(RIVE_FILE_NAME "sample.riv") # requirements scan: no sdkconfig yet
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/assets/${RIVE_FILE_NAME}"
               "${CMAKE_CURRENT_BINARY_DIR}/rive_file.riv" COPYONLY)

# Audio output (Rive player > Audio output): esp_codec_dev comes from
# idf_component.yml.
set(AUDIO_SRCS)
if(CONFIG_RIVE_AUDIO_OUT)
    set(AUDIO_SRCS "audio_p4.cpp")
endif()
set(NET_SRCS)
if(CONFIG_RIVE_NET_CONTROL)
    set(NET_SRCS "net_control.cpp")
endif()

idf_component_register(
    SRCS
        "main.cpp"
        "display_p4.c"
        "raster_profile.cpp"
        "pie_selftest.cpp"
        "touch_gt911.c"
        "board_i2c.c"
        ${AUDIO_SRCS}
        ${NET_SRCS}
    INCLUDE_DIRS "."
    REQUIRES rive_thorvg esp_lcd esp_driver_ppa esp_driver_ledc esp_driver_i2c esp_driver_i2s esp_timer esp_eth esp_netif esp_event lwip
    EMBED_FILES "${CMAKE_CURRENT_BINARY_DIR}/rive_file.riv"
)

# On-device ThorVG profiling: route ThorVG's raster / preparation calls
# through the counting wrappers in raster_profile.cpp (linker --wrap).
if(CONFIG_RIVE_RASTER_PROFILE)
    foreach(sym
            _Z11rasterShapeP9SwSurfaceP7SwShapeRKN3tvg12RenderRegionERNS3_4RGBAE
            _Z12rasterStrokeP9SwSurfaceP7SwShapeRKN3tvg12RenderRegionERNS3_4RGBAE
            _Z19rasterGradientShapeP9SwSurfaceP7SwShapeRKN3tvg12RenderRegionEPKNS3_4FillEh
            _Z20rasterGradientStrokeP9SwSurfaceP7SwShapeRKN3tvg12RenderRegionEPKNS3_4FillEh
            _Z11rasterClearP9SwSurfacemmmm
            _Z11shapeGenRleR7SwShapePKN3tvg11RenderShapeERKNS1_6MatrixERKNS1_12RenderRegionERS8_P7SwMpooljbb
            _Z9rleRenderP5SwRlePK9SwOutlineRKN3tvg12RenderRegionEP7SwMpooljb
            _Z17shapeGenStrokeRleR7SwShapePKN3tvg11RenderShapeERKNS1_6MatrixERKNS1_12RenderRegionERS8_P7SwMpooljb
            _Z11fillPrepareRP6SwFillPKN3tvg4FillERKNS2_6MatrixEP9SwSurfacehb)
        target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${sym}")
    endforeach()
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)
