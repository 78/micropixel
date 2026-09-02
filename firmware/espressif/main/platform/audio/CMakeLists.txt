list(APPEND micropixel_sources
    "platform/audio/audio_engine.cpp"
    "platform/audio/audio_mixer.cpp"
)

if(IDF_TARGET STREQUAL "esp32s31" OR IDF_TARGET STREQUAL "esp32s3")
    list(APPEND micropixel_sources "platform/audio/es8311_i2s_audio_sink.cpp")
    list(APPEND micropixel_requires esp_codec_dev)
endif()
