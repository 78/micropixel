list(APPEND micropixel_sources
    "platform/graphics/app_surface_compositor.cpp"
    "platform/graphics/guest_scene.cpp"
    "platform/graphics/pixel_compositor.cpp"
    "platform/lvgl/display/dirty_region_coalescer.cpp"
    "platform/lvgl/display/jpeg_cover_decoder.cpp"
    "platform/lvgl/display/png_cover_decoder.cpp"
    "platform/lvgl/display/screen_capture.cpp"
    "platform/lvgl/fonts/bitmap_font_rasterizer.cpp"
    "platform/lvgl/fonts/font_cbin_loader.cpp"
    "platform/lvgl/fonts/font_registry.cpp"
    "platform/lvgl/guest_graphics_engine.cpp"
    "platform/lvgl/guest_graphics_operations.cpp"
    "platform/lvgl/lvgl_software_pixel_compositor.cpp"
)

# P4 and S31 expose PPA/DMA2D; S3 is CPU-only. Derive this source set from the
# SoC capabilities instead of the selected board.
if(CONFIG_SOC_PPA_SUPPORTED OR CONFIG_SOC_DMA2D_SUPPORTED)
    list(APPEND micropixel_sources
        "platform/graphics/esp_pixel_compositor.cpp"
        "platform/lvgl/display/ppa_srm_blitter.cpp"
        "platform/lvgl/display/system_transition_compositor.cpp"
    )
endif()
