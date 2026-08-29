# Host control owns App lifecycle orchestration plus local and remote control
# entry points. Transport-neutral command types stay at this module root.
list(APPEND micropixel_sources
    "host/controller/app_controller.cpp"
    "host/controller/control_dispatcher.cpp"
    "host/controller/guest_log_buffer.cpp"
    "host/controller/host_controller.cpp"
    "host/controller/host_power_coordinator.cpp"
    "host/controller/local/local_control_agent.cpp"
    "host/controller/remote/remote_control_agent.cpp"
    "host/controller/remote/remote_control_protocol.cpp"
    "host/controller/remote/remote_identity_store.cpp"
)
