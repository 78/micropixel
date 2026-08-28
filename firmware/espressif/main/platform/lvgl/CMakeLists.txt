list(APPEND micropixel_sources
    "platform/common/graphics_adapter.cpp"
    "platform/common/system_ui_adapter.cpp"
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
    "platform/lvgl/ui/square_common/default_keyboard.cpp"
    "platform/lvgl/ui/square_common/hall_card_ui.cpp"
    "platform/lvgl/ui/square_common/hall_cover_cache.cpp"
    "platform/lvgl/ui/square_common/hall_cover_codec.cpp"
    "platform/lvgl/ui/square_common/hall_scene_ui.cpp"
    "platform/lvgl/ui/square_common/hall_transition_ui.cpp"
    "platform/lvgl/ui/square_common/status_layer_ui.cpp"
    "platform/lvgl/ui/square_common/status_layer_transition.cpp"
    "platform/lvgl/ui/square_common/system_detail_ui.cpp"
    "platform/lvgl/ui/square_common/system_menu_ui.cpp"
    "platform/lvgl/ui/square_common/wifi_settings_ui.cpp"
    "platform/lvgl/ui/square_common/icons/wifi_status_icons.c"
)

if(CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION)
    list(APPEND micropixel_sources "platform/lvgl/display/retained_surface.cpp")
endif()
