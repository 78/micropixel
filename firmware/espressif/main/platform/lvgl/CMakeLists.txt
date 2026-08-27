list(APPEND micropixel_sources
    "platform/common/graphics_adapter.cpp"
    "platform/common/system_ui_adapter.cpp"
    "platform/graphics/command_stream.cpp"
    "platform/lvgl/display/dirty_region_coalescer.cpp"
    "platform/lvgl/display/jpeg_cover_decoder.cpp"
    "platform/lvgl/display/png_cover_decoder.cpp"
    "platform/lvgl/display/retained_scene.cpp"
    "platform/lvgl/display/system_transition_compositor.cpp"
    "platform/lvgl/fonts/font_cbin_loader.cpp"
    "platform/lvgl/fonts/font_registry.cpp"
    "platform/lvgl/guest_graphics_engine.cpp"
    "platform/lvgl/ui/square_720/icons/wifi_status_icons.c"
    "platform/lvgl/ui/square_720/status_layer_ui.cpp"
    "platform/lvgl/ui/square_720/system_detail_ui.cpp"
    "platform/lvgl/ui/square_720/system_menu_ui.cpp"
    "platform/lvgl/ui/square_720/wifi_settings_ui.cpp"
)

if(CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION)
    list(APPEND micropixel_sources "platform/lvgl/display/retained_surface.cpp")
endif()
