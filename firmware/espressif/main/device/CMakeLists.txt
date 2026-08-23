# Device façades are platform-independent and only depend on the abstract
# backend contracts injected by the application composition root.
list(APPEND micropixel_sources
    "device/device_services.cpp"
)
