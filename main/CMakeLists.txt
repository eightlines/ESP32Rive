idf_component_register(
    SRCS
        "main.cpp"
        "display_p4.c"
        "raster_profile.cpp"
        "pie_selftest.cpp"
        "touch_gt911.c"
    INCLUDE_DIRS "."
    REQUIRES rive_thorvg esp_lcd esp_driver_ppa esp_driver_ledc esp_driver_i2c esp_timer
    EMBED_FILES "assets/sample.riv"
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
