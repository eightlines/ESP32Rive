# Rive renderer backend built on ThorVG's software rasteriser: records Rive's
# draw calls (ThorvgRenderer), renders them with retained ThorVG paints in one
# or more parallel lanes (ThorvgLane), plus a RivePlayer that ties it together.
idf_component_register(
    SRCS
        "src/thorvg_factory.cpp"
        "src/thorvg_renderer.cpp"
        "src/thorvg_lane.cpp"
        "src/rive_player.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES rive_runtime thorvg
    PRIV_REQUIRES pthread # std::mutex
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)
