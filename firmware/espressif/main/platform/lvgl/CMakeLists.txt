list(APPEND micropixel_sources
    "platform/graphics/command_stream.cpp"
    "platform/lvgl/display/dirty_region_coalescer.cpp"
    "platform/lvgl/display/jpeg_cover_decoder.cpp"
    "platform/lvgl/display/png_cover_decoder.cpp"
    "platform/lvgl/display/ppa_srm_blitter.cpp"
    "platform/lvgl/display/retained_scene.cpp"
    "platform/lvgl/display/screen_capture.cpp"
    "platform/lvgl/display/system_transition_compositor.cpp"
    "platform/lvgl/fonts/font_cbin_loader.cpp"
    "platform/lvgl/fonts/font_registry.cpp"
    "platform/lvgl/guest_graphics_engine.cpp"
    "platform/lvgl/guest_graphics_operations.cpp"
)

if(CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION)
    list(APPEND micropixel_sources "platform/lvgl/display/retained_surface.cpp")
endif()
