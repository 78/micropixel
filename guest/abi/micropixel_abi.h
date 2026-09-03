#ifndef MICROPIXEL_GUEST_ABI_H
#define MICROPIXEL_GUEST_ABI_H

#include <stdint.h>

#define MICROPIXEL_ABI_VERSION_MAJOR 1U
#define MICROPIXEL_ABI_VERSION_MINOR 0U
#define MICROPIXEL_ABI_VERSION ((MICROPIXEL_ABI_VERSION_MAJOR << 16U) | MICROPIXEL_ABI_VERSION_MINOR)
#define MICROPIXEL_INTERFACE_VERSION(major, minor) (((uint32_t)(major) << 16U) | (uint32_t)(minor))
#define MICROPIXEL_ABI_MAX_LOG_BYTES 1024U
#define MICROPIXEL_MAX_TOUCH_POINTS 5U
#define MICROPIXEL_GRAPHICS_INTERFACE_MAJOR 1U
#define MICROPIXEL_GRAPHICS_INTERFACE_MINOR 4U
#define MICROPIXEL_INPUT_INTERFACE_MAJOR 1U
#define MICROPIXEL_INPUT_INTERFACE_MINOR 1U
#define MICROPIXEL_GRAPHICS_SCENE_MAGIC 0x5347504dU
#define MICROPIXEL_GRAPHICS_MAX_SCENE_BYTES 24576U
#define MICROPIXEL_GRAPHICS_MAX_SCENE_NODES 256U
#define MICROPIXEL_GRAPHICS_MAX_LAYERS 4U
#define MICROPIXEL_GRAPHICS_MAX_CONTAINERS 64U
#define MICROPIXEL_GRAPHICS_MAX_SPRITE_BATCHES 8U
#define MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES 256U
#define MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES 128U
#define MICROPIXEL_RESOURCE_INTERFACE_MAJOR 1U
#define MICROPIXEL_RESOURCE_INTERFACE_MINOR 2U
#define MICROPIXEL_STREAMING_TEXTURE_MAX_UPDATE_BYTES 4096U
#define MICROPIXEL_AUDIO_INTERFACE_MAJOR 1U
#define MICROPIXEL_AUDIO_INTERFACE_MINOR 1U
#define MICROPIXEL_RANDOM_INTERFACE_MAJOR 1U
#define MICROPIXEL_RANDOM_INTERFACE_MINOR 0U
#define MICROPIXEL_AUDIO_MAX_TONE_DURATION_MS 5000U
#define MICROPIXEL_STORAGE_MAX_KEY_BYTES 15U
#define MICROPIXEL_STORAGE_MAX_VALUE_BYTES 4096U
#define MICROPIXEL_SYSTEM_INTERFACE_MAJOR 1U
#define MICROPIXEL_SYSTEM_INTERFACE_MINOR 1U
#define MICROPIXEL_LOCALE_TAG_MAX_BYTES 31U
#define MICROPIXEL_LAUNCH_ARGUMENT_MAX_COUNT 16U
#define MICROPIXEL_LAUNCH_ARGUMENT_MAX_BYTES 512U
#define MICROPIXEL_DEVICES_INTERFACE_MAJOR 1U
#define MICROPIXEL_DEVICES_INTERFACE_MINOR 0U
#define MICROPIXEL_SENSORS_INTERFACE_MAJOR 1U
#define MICROPIXEL_SENSORS_INTERFACE_MINOR 0U
#define MICROPIXEL_GPIO_INTERFACE_MAJOR 1U
#define MICROPIXEL_GPIO_INTERFACE_MINOR 0U
#define MICROPIXEL_HAPTICS_INTERFACE_MAJOR 1U
#define MICROPIXEL_HAPTICS_INTERFACE_MINOR 0U
#define MICROPIXEL_POWER_INFO_INTERFACE_MAJOR 1U
#define MICROPIXEL_POWER_INFO_INTERFACE_MINOR 0U
#define MICROPIXEL_MAX_DEVICES 64U
#define MICROPIXEL_DEVICE_NAME_MAX_BYTES 39U
#define MICROPIXEL_MAX_SENSOR_HANDLES 8U
#define MICROPIXEL_MAX_GPIO_HANDLES 16U
#define MICROPIXEL_MAX_HAPTIC_HANDLES 2U

typedef enum micropixel_status {
    MICROPIXEL_STATUS_OK = 0,
    MICROPIXEL_STATUS_INVALID_ARGUMENT = -1,
    MICROPIXEL_STATUS_INVALID_MEMORY = -2,
    MICROPIXEL_STATUS_UNSUPPORTED = -3,
    MICROPIXEL_STATUS_RESOURCE_EXHAUSTED = -4,
    MICROPIXEL_STATUS_INTERNAL = -5,
    MICROPIXEL_STATUS_NOT_FOUND = -6,
    MICROPIXEL_STATUS_PERMISSION_DENIED = -7,
    MICROPIXEL_STATUS_BUFFER_TOO_SMALL = -8,
    MICROPIXEL_STATUS_RATE_LIMITED = -9,
    MICROPIXEL_STATUS_WOULD_BLOCK = -10,
    MICROPIXEL_STATUS_TIMEOUT = -11,
    MICROPIXEL_STATUS_CANCELLED = -12,
    MICROPIXEL_STATUS_CLOSED = -13,
    MICROPIXEL_STATUS_VERSION_MISMATCH = -14,
    MICROPIXEL_STATUS_STALE_STATE = -15,
} micropixel_status_t;

typedef uint64_t micropixel_app_time_t;
typedef uint32_t micropixel_service_handle_t;
typedef uint32_t micropixel_timer_handle_t;
typedef uint32_t micropixel_texture_handle_t;
typedef uint16_t micropixel_font_handle_t;
typedef uint32_t micropixel_audio_clip_handle_t;
typedef uint32_t micropixel_audio_playback_handle_t;
typedef uint32_t micropixel_device_id_t;
typedef uint32_t micropixel_sensor_handle_t;
typedef uint32_t micropixel_gpio_handle_t;
typedef uint32_t micropixel_haptic_handle_t;

typedef enum micropixel_service_id {
    MICROPIXEL_SERVICE_TIMER = 1,
    MICROPIXEL_SERVICE_STORAGE = 2,
    MICROPIXEL_SERVICE_RESOURCE = 3,
    MICROPIXEL_SERVICE_RANDOM = 4,
    MICROPIXEL_SERVICE_SYSTEM = 5,
    MICROPIXEL_SERVICE_DEVICES = 6,
    MICROPIXEL_SERVICE_GRAPHICS = 16,
    MICROPIXEL_SERVICE_INPUT = 17,
    MICROPIXEL_SERVICE_AUDIO = 18,
    MICROPIXEL_SERVICE_NETWORK = 19,
    MICROPIXEL_SERVICE_SENSORS = 20,
    MICROPIXEL_SERVICE_GPIO = 21,
    MICROPIXEL_SERVICE_HAPTICS = 22,
    MICROPIXEL_SERVICE_POWER_INFO = 23,
} micropixel_service_id_t;

typedef enum micropixel_service_flag {
    MICROPIXEL_SERVICE_FLAG_CALL = 1U << 0U,
    MICROPIXEL_SERVICE_FLAG_SUBMIT = 1U << 1U,
    MICROPIXEL_SERVICE_FLAG_EVENTS = 1U << 2U,
} micropixel_service_flag_t;

typedef enum micropixel_log_level {
    MICROPIXEL_LOG_DEBUG = 1,
    MICROPIXEL_LOG_INFO = 2,
    MICROPIXEL_LOG_WARNING = 3,
    MICROPIXEL_LOG_ERROR = 4,
} micropixel_log_level_t;

typedef enum micropixel_timer_method {
    MICROPIXEL_TIMER_METHOD_CREATE = 1,
    MICROPIXEL_TIMER_METHOD_START = 2,
    MICROPIXEL_TIMER_METHOD_CANCEL = 3,
    MICROPIXEL_TIMER_METHOD_RELEASE = 4,
} micropixel_timer_method_t;

typedef enum micropixel_storage_method {
    MICROPIXEL_STORAGE_METHOD_GET = 1,
    MICROPIXEL_STORAGE_METHOD_SET = 2,
    MICROPIXEL_STORAGE_METHOD_REMOVE = 3,
} micropixel_storage_method_t;

typedef enum micropixel_resource_method {
    MICROPIXEL_RESOURCE_METHOD_LOAD_TEXTURE = 1,
    MICROPIXEL_RESOURCE_METHOD_TEXTURE_RELEASE = 2,
    MICROPIXEL_RESOURCE_METHOD_STREAMING_TEXTURE_CREATE = 3,
    MICROPIXEL_RESOURCE_METHOD_STREAMING_TEXTURE_UPDATE = 4,
    MICROPIXEL_RESOURCE_METHOD_TEXTURE_UPDATE_BATCH_BEGIN = 5,
    MICROPIXEL_RESOURCE_METHOD_TEXTURE_UPDATE_BATCH_FINISH = 6,
    MICROPIXEL_RESOURCE_METHOD_LOAD_FONT = 7,
    MICROPIXEL_RESOURCE_METHOD_FONT_RELEASE = 8,
    MICROPIXEL_RESOURCE_METHOD_LOAD_ADAPTIVE_TEXTURE = 9,
} micropixel_resource_method_t;

typedef struct micropixel_resource_load_adaptive_texture_request {
    uint16_t size;
    uint16_t reserved0;
    uint32_t asset_id;
    uint32_t scale_numerator;
    uint32_t scale_denominator;
} micropixel_resource_load_adaptive_texture_request_t;

typedef enum micropixel_random_method {
    MICROPIXEL_RANDOM_METHOD_GET_U32 = 1,
} micropixel_random_method_t;

typedef enum micropixel_system_method {
    MICROPIXEL_SYSTEM_METHOD_GET_LOCALE = 1,
    MICROPIXEL_SYSTEM_METHOD_GET_LAUNCH_ARGUMENTS = 2,
} micropixel_system_method_t;

typedef struct micropixel_system_locale_response {
    uint16_t size;
    uint16_t tag_length;
    char tag[MICROPIXEL_LOCALE_TAG_MAX_BYTES + 1U];
} micropixel_system_locale_response_t;

// Arguments are stored as count NUL-terminated UTF-8 strings in bytes.
// offsets[index] points at the first byte of each argument.
typedef struct micropixel_system_launch_arguments_response {
    uint16_t size;
    uint16_t count;
    uint16_t bytes_length;
    uint16_t reserved0;
    uint16_t offsets[MICROPIXEL_LAUNCH_ARGUMENT_MAX_COUNT];
    char bytes[MICROPIXEL_LAUNCH_ARGUMENT_MAX_BYTES];
} micropixel_system_launch_arguments_response_t;

typedef enum micropixel_device_kind {
    MICROPIXEL_DEVICE_KIND_ANY = 0,
    MICROPIXEL_DEVICE_KIND_DISPLAY = 1,
    MICROPIXEL_DEVICE_KIND_TOUCH = 2,
    MICROPIXEL_DEVICE_KIND_AUDIO_INPUT = 3,
    MICROPIXEL_DEVICE_KIND_AUDIO_OUTPUT = 4,
    MICROPIXEL_DEVICE_KIND_SENSOR = 5,
    MICROPIXEL_DEVICE_KIND_GPIO_LINE = 6,
    MICROPIXEL_DEVICE_KIND_HAPTICS = 7,
    MICROPIXEL_DEVICE_KIND_POWER = 8,
    MICROPIXEL_DEVICE_KIND_GAMEPAD = 9,
    MICROPIXEL_DEVICE_KIND_CAMERA = 10,
    MICROPIXEL_DEVICE_KIND_LOCATION = 11,
    MICROPIXEL_DEVICE_KIND_STORAGE = 12,
    MICROPIXEL_DEVICE_KIND_NETWORK = 13,
} micropixel_device_kind_t;

typedef enum micropixel_device_capability {
    MICROPIXEL_DEVICE_CAP_READ = 1ULL << 0U,
    MICROPIXEL_DEVICE_CAP_WRITE = 1ULL << 1U,
    MICROPIXEL_DEVICE_CAP_EVENTS = 1ULL << 2U,
    MICROPIXEL_DEVICE_CAP_HOTPLUGGABLE = 1ULL << 3U,
} micropixel_device_capability_t;

typedef enum micropixel_devices_method {
    MICROPIXEL_DEVICES_METHOD_LIST = 1,
    MICROPIXEL_DEVICES_METHOD_GET_INFO = 2,
} micropixel_devices_method_t;

typedef enum micropixel_devices_event_id {
    MICROPIXEL_DEVICES_EVENT_ADDED = 1,
    MICROPIXEL_DEVICES_EVENT_REMOVED = 2,
} micropixel_devices_event_id_t;

typedef struct micropixel_devices_list_request {
    uint16_t size;
    uint16_t kind;
    uint32_t reserved0;
} micropixel_devices_list_request_t;

typedef struct micropixel_devices_list_response {
    uint16_t size;
    uint16_t count;
    uint32_t generation;
    micropixel_device_id_t devices[MICROPIXEL_MAX_DEVICES];
} micropixel_devices_list_response_t;

typedef struct micropixel_device_request {
    uint16_t size;
    uint16_t reserved0;
    micropixel_device_id_t device;
} micropixel_device_request_t;

typedef struct micropixel_device_info {
    uint16_t size;
    uint16_t kind;
    micropixel_device_id_t device;
    micropixel_device_id_t parent;
    uint32_t reserved0;
    uint64_t capabilities;
    uint16_t name_length;
    uint16_t reserved1;
    char name[MICROPIXEL_DEVICE_NAME_MAX_BYTES + 1U];
} micropixel_device_info_t;

typedef struct micropixel_device_event_payload {
    micropixel_device_id_t device;
    uint16_t kind;
    uint16_t reserved0;
    uint32_t generation;
    uint32_t reserved1;
} micropixel_device_event_payload_t;

typedef enum micropixel_sensor_kind {
    MICROPIXEL_SENSOR_ACCELERATION = 1,
    MICROPIXEL_SENSOR_ANGULAR_VELOCITY = 2,
    MICROPIXEL_SENSOR_MAGNETIC_FIELD = 3,
    MICROPIXEL_SENSOR_TEMPERATURE = 4,
    MICROPIXEL_SENSOR_ILLUMINANCE = 5,
    MICROPIXEL_SENSOR_PRESSURE = 6,
    MICROPIXEL_SENSOR_RELATIVE_HUMIDITY = 7,
    MICROPIXEL_SENSOR_PROXIMITY = 8,
    MICROPIXEL_SENSOR_ORIENTATION = 9,
} micropixel_sensor_kind_t;

typedef enum micropixel_sensor_placement {
    MICROPIXEL_SENSOR_PLACEMENT_UNKNOWN = 0,
    MICROPIXEL_SENSOR_PLACEMENT_BUILT_IN = 1,
    MICROPIXEL_SENSOR_PLACEMENT_EXTERNAL = 2,
    MICROPIXEL_SENSOR_PLACEMENT_LEFT = 3,
    MICROPIXEL_SENSOR_PLACEMENT_RIGHT = 4,
} micropixel_sensor_placement_t;

typedef enum micropixel_sensors_method {
    MICROPIXEL_SENSORS_METHOD_GET_INFO = 1,
    MICROPIXEL_SENSORS_METHOD_OPEN = 2,
    MICROPIXEL_SENSORS_METHOD_READ = 3,
    MICROPIXEL_SENSORS_METHOD_SET_SAMPLE_INTERVAL = 4,
    MICROPIXEL_SENSORS_METHOD_SET_UPDATE_INTERVAL = MICROPIXEL_SENSORS_METHOD_SET_SAMPLE_INTERVAL,
    MICROPIXEL_SENSORS_METHOD_RELEASE = 5,
} micropixel_sensors_method_t;

typedef enum micropixel_sensors_event_id {
    MICROPIXEL_SENSORS_EVENT_READING = 1,
} micropixel_sensors_event_id_t;

typedef struct micropixel_sensor_info {
    uint16_t size;
    uint16_t kind;
    micropixel_device_id_t device;
    micropixel_device_id_t parent;
    uint16_t placement;
    uint16_t value_count;
    uint32_t minimum_interval_us;
    uint32_t maximum_interval_us;
    uint32_t reserved[2];
} micropixel_sensor_info_t;

typedef struct micropixel_sensor_open_request {
    uint16_t size;
    uint16_t expected_kind;
    micropixel_device_id_t device;
} micropixel_sensor_open_request_t;

typedef struct micropixel_sensor_open_response {
    uint16_t size;
    uint16_t kind;
    micropixel_sensor_handle_t sensor;
    micropixel_device_id_t device;
    uint32_t reserved0;
} micropixel_sensor_open_response_t;

typedef struct micropixel_sensor_reading {
    uint16_t size;
    uint16_t kind;
    micropixel_sensor_handle_t sensor;
    micropixel_device_id_t device;
    uint32_t reserved0;
    micropixel_app_time_t timestamp_us;
    float values[4];
} micropixel_sensor_reading_t;

typedef struct micropixel_sensor_update_request {
    uint16_t size;
    uint16_t reserved0;
    micropixel_sensor_handle_t sensor;
    uint64_t interval_us;
} micropixel_sensor_update_request_t;

typedef micropixel_sensor_update_request_t micropixel_sensor_sample_interval_request_t;

typedef struct micropixel_sensor_event_payload {
    float values[4];
} micropixel_sensor_event_payload_t;

typedef enum micropixel_gpio_capability {
    MICROPIXEL_GPIO_CAP_INPUT = 1U << 0U,
    MICROPIXEL_GPIO_CAP_OUTPUT = 1U << 1U,
    MICROPIXEL_GPIO_CAP_PULL_UP = 1U << 2U,
    MICROPIXEL_GPIO_CAP_PULL_DOWN = 1U << 3U,
    MICROPIXEL_GPIO_CAP_EDGE_EVENTS = 1U << 4U,
    MICROPIXEL_GPIO_CAP_PWM = 1U << 5U,
} micropixel_gpio_capability_t;

typedef enum micropixel_gpio_mode {
    MICROPIXEL_GPIO_MODE_INPUT = 1,
    MICROPIXEL_GPIO_MODE_OUTPUT = 2,
    MICROPIXEL_GPIO_MODE_PWM = 3,
} micropixel_gpio_mode_t;

typedef enum micropixel_gpio_pull {
    MICROPIXEL_GPIO_PULL_NONE = 0,
    MICROPIXEL_GPIO_PULL_UP = 1,
    MICROPIXEL_GPIO_PULL_DOWN = 2,
} micropixel_gpio_pull_t;

typedef enum micropixel_gpio_edge {
    MICROPIXEL_GPIO_EDGE_NONE = 0,
    MICROPIXEL_GPIO_EDGE_RISING = 1,
    MICROPIXEL_GPIO_EDGE_FALLING = 2,
    MICROPIXEL_GPIO_EDGE_BOTH = 3,
} micropixel_gpio_edge_t;

typedef enum micropixel_gpio_method {
    MICROPIXEL_GPIO_METHOD_GET_INFO = 1,
    MICROPIXEL_GPIO_METHOD_OPEN = 2,
    MICROPIXEL_GPIO_METHOD_READ = 3,
    MICROPIXEL_GPIO_METHOD_WRITE = 4,
    MICROPIXEL_GPIO_METHOD_SET_PWM_DUTY = 5,
    MICROPIXEL_GPIO_METHOD_RELEASE = 6,
} micropixel_gpio_method_t;

typedef enum micropixel_gpio_event_id {
    MICROPIXEL_GPIO_EVENT_EDGE = 1,
} micropixel_gpio_event_id_t;

typedef struct micropixel_gpio_info {
    uint16_t size;
    uint16_t line_number;
    micropixel_device_id_t device;
    uint32_t capabilities;
    uint32_t maximum_pwm_frequency_hz;
    uint32_t reserved[3];
} micropixel_gpio_info_t;

typedef struct micropixel_gpio_open_request {
    uint16_t size;
    uint16_t mode;
    micropixel_device_id_t device;
    uint16_t pull;
    uint16_t edge;
    uint32_t initial_value;
    uint32_t pwm_frequency_hz;
} micropixel_gpio_open_request_t;

typedef struct micropixel_gpio_open_response {
    uint16_t size;
    uint16_t mode;
    micropixel_gpio_handle_t gpio;
    micropixel_device_id_t device;
    uint32_t reserved0;
} micropixel_gpio_open_response_t;

typedef struct micropixel_gpio_value_request {
    uint16_t size;
    uint16_t reserved0;
    micropixel_gpio_handle_t gpio;
    uint32_t value;
} micropixel_gpio_value_request_t;

typedef struct micropixel_gpio_value_response {
    uint16_t size;
    uint16_t reserved0;
    micropixel_gpio_handle_t gpio;
    uint32_t value;
} micropixel_gpio_value_response_t;

typedef struct micropixel_gpio_event_payload {
    micropixel_device_id_t device;
    uint32_t value;
    uint32_t edge;
    uint32_t reserved0;
} micropixel_gpio_event_payload_t;

typedef enum micropixel_haptics_capability {
    MICROPIXEL_HAPTICS_CAP_VARIABLE_STRENGTH = 1U << 0U,
} micropixel_haptics_capability_t;

typedef enum micropixel_haptics_method {
    MICROPIXEL_HAPTICS_METHOD_GET_INFO = 1,
    MICROPIXEL_HAPTICS_METHOD_OPEN = 2,
    MICROPIXEL_HAPTICS_METHOD_PLAY = 3,
    MICROPIXEL_HAPTICS_METHOD_STOP = 4,
    MICROPIXEL_HAPTICS_METHOD_RELEASE = 5,
} micropixel_haptics_method_t;

typedef enum micropixel_haptics_event_id {
    MICROPIXEL_HAPTICS_EVENT_FINISHED = 1,
} micropixel_haptics_event_id_t;

typedef struct micropixel_haptics_info {
    uint16_t size;
    uint16_t capabilities;
    micropixel_device_id_t device;
    uint32_t maximum_duration_ms;
    uint32_t reserved[2];
} micropixel_haptics_info_t;

typedef struct micropixel_haptics_play_request {
    uint16_t size;
    uint16_t strength_per_mille;
    micropixel_haptic_handle_t haptic;
    uint32_t duration_ms;
    uint32_t reserved0;
} micropixel_haptics_play_request_t;

typedef enum micropixel_power_source {
    MICROPIXEL_POWER_SOURCE_UNKNOWN = 0,
    MICROPIXEL_POWER_SOURCE_BATTERY = 1,
    MICROPIXEL_POWER_SOURCE_EXTERNAL = 2,
} micropixel_power_source_t;

typedef enum micropixel_power_state_flag {
    MICROPIXEL_POWER_STATE_HAS_BATTERY = 1U << 0U,
    MICROPIXEL_POWER_STATE_CHARGING = 1U << 1U,
    MICROPIXEL_POWER_STATE_DISCHARGING = 1U << 2U,
    MICROPIXEL_POWER_STATE_EXTERNAL_CONNECTED = 1U << 3U,
} micropixel_power_state_flag_t;

typedef enum micropixel_power_info_method {
    MICROPIXEL_POWER_INFO_METHOD_GET = 1,
} micropixel_power_info_method_t;

typedef enum micropixel_power_info_event_id {
    MICROPIXEL_POWER_INFO_EVENT_CHANGED = 1,
} micropixel_power_info_event_id_t;

typedef struct micropixel_power_info_response {
    uint16_t size;
    uint16_t source;
    micropixel_device_id_t device;
    uint32_t flags;
    uint8_t battery_percent;
    uint8_t reserved0[3];
    uint32_t reserved1[2];
} micropixel_power_info_response_t;

typedef enum micropixel_graphics_method {
    MICROPIXEL_GRAPHICS_METHOD_GET_INFO = 1,
    MICROPIXEL_GRAPHICS_METHOD_MEASURE_TEXT = 2,
} micropixel_graphics_method_t;

typedef enum micropixel_graphics_channel {
    MICROPIXEL_GRAPHICS_CHANNEL_SCENE = 1,
} micropixel_graphics_channel_t;

typedef enum micropixel_graphics_scene_message_kind {
    MICROPIXEL_GRAPHICS_SCENE_KEYFRAME = 1,
    MICROPIXEL_GRAPHICS_SCENE_PATCH = 2,
} micropixel_graphics_scene_message_kind_t;

typedef enum micropixel_graphics_scene_record_opcode {
    MICROPIXEL_GRAPHICS_SCENE_OP_BACKGROUND = 1,
    MICROPIXEL_GRAPHICS_SCENE_OP_LAYER = 2,
    MICROPIXEL_GRAPHICS_SCENE_OP_RECT = 3,
    MICROPIXEL_GRAPHICS_SCENE_OP_TEXTURE = 4,
    MICROPIXEL_GRAPHICS_SCENE_OP_TEXT = 5,
    MICROPIXEL_GRAPHICS_SCENE_OP_SPRITE_BATCH = 6,
    MICROPIXEL_GRAPHICS_SCENE_OP_BATCH_INSTANCES = 7,
    MICROPIXEL_GRAPHICS_SCENE_OP_CONTAINER = 8,
    MICROPIXEL_GRAPHICS_SCENE_OP_NODE_LINK = 9,
    MICROPIXEL_GRAPHICS_SCENE_OP_ROUNDED_RECT = 10,
} micropixel_graphics_scene_record_opcode_t;

typedef enum micropixel_graphics_scene_node_property {
    MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY = 1U << 0U,
    MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE = 1U << 1U,
    MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT = 1U << 2U,
    MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBILITY = 1U << 3U,
    MICROPIXEL_GRAPHICS_SCENE_NODE_LAYER = 1U << 4U,
    MICROPIXEL_GRAPHICS_SCENE_NODE_KIND = 1U << 5U,
} micropixel_graphics_scene_node_property_t;

typedef enum micropixel_graphics_scene_layer_property {
    MICROPIXEL_GRAPHICS_SCENE_LAYER_CLIP = 1U << 0U,
    MICROPIXEL_GRAPHICS_SCENE_LAYER_TRANSLATION = 1U << 1U,
    MICROPIXEL_GRAPHICS_SCENE_LAYER_APPEARANCE = 1U << 2U,
    MICROPIXEL_GRAPHICS_SCENE_LAYER_Z_ORDER = 1U << 3U,
} micropixel_graphics_scene_layer_property_t;

typedef enum micropixel_graphics_scene_container_property {
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_CLIP = 1U << 0U,
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_TRANSLATION = 1U << 1U,
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_APPEARANCE = 1U << 2U,
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_Z_ORDER = 1U << 3U,
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_STRUCTURE = 1U << 4U,
    /* Graphics 1.4+. Covers micropixel_graphics_scene_container_record_t::flags. */
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_FLAGS = 1U << 5U,
} micropixel_graphics_scene_container_property_t;

/* Graphics 1.4+. Rendering hints for a container subtree; they never change
 * what is drawn, only how the Host may retain it. */
typedef enum micropixel_graphics_scene_container_flag {
    /* The subtree changes rarely relative to how often its translation
     * changes (a scrolling map). The Host may rasterize it once into a retained
     * cache in the container's local coordinates and re-composite that cache on
     * every translation. The cache is composited as an opaque layer: pixels not
     * covered by a descendant show the Scene background color, so descendants
     * below this container in draw order never show through it. */
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_FLAG_CACHED_CONTENT = 1U << 0U,
} micropixel_graphics_scene_container_flag_t;

typedef enum micropixel_graphics_scene_background_property {
    MICROPIXEL_GRAPHICS_SCENE_BACKGROUND_COLOR = 1U << 0U,
} micropixel_graphics_scene_background_property_t;

typedef enum micropixel_graphics_scene_node_flag {
    MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE = 1U << 0U,
    MICROPIXEL_GRAPHICS_SCENE_TEXT_CENTERED = 1U << 1U,
} micropixel_graphics_scene_node_flag_t;

typedef enum micropixel_graphics_scene_batch_instance_property {
    MICROPIXEL_GRAPHICS_SCENE_INSTANCE_GEOMETRY = 1U << 0U,
    MICROPIXEL_GRAPHICS_SCENE_INSTANCE_CONTENT = 1U << 1U,
    MICROPIXEL_GRAPHICS_SCENE_INSTANCE_APPEARANCE = 1U << 2U,
    MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBILITY = 1U << 3U,
} micropixel_graphics_scene_batch_instance_property_t;

typedef enum micropixel_graphics_scene_batch_instance_flag {
    MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBLE = 1U << 0U,
} micropixel_graphics_scene_batch_instance_flag_t;

typedef enum micropixel_input_method {
    MICROPIXEL_INPUT_METHOD_GET_INFO = 1,
} micropixel_input_method_t;

typedef enum micropixel_audio_method {
    MICROPIXEL_AUDIO_METHOD_GET_INFO = 1,
    MICROPIXEL_AUDIO_METHOD_PLAY_TONE = 2,
    MICROPIXEL_AUDIO_METHOD_STOP_ALL = 3,
    MICROPIXEL_AUDIO_METHOD_CLIP_LOAD = 4,
    MICROPIXEL_AUDIO_METHOD_CLIP_RELEASE = 5,
    MICROPIXEL_AUDIO_METHOD_PLAYBACK_START = 6,
    MICROPIXEL_AUDIO_METHOD_PLAYBACK_PAUSE = 7,
    MICROPIXEL_AUDIO_METHOD_PLAYBACK_RESUME = 8,
    MICROPIXEL_AUDIO_METHOD_PLAYBACK_SET_VOLUME = 9,
    MICROPIXEL_AUDIO_METHOD_PLAYBACK_STOP = 10,
    MICROPIXEL_AUDIO_METHOD_PLAYBACK_GET_STATE = 11,
} micropixel_audio_method_t;

typedef enum micropixel_audio_capability {
    MICROPIXEL_AUDIO_CAPABILITY_OGG_OPUS = 1U << 0U,
} micropixel_audio_capability_t;

typedef enum micropixel_audio_format {
    MICROPIXEL_AUDIO_FORMAT_OGG_OPUS = 1,
} micropixel_audio_format_t;

typedef enum micropixel_audio_playback_flag {
    MICROPIXEL_AUDIO_PLAYBACK_LOOP = 1U << 0U,
} micropixel_audio_playback_flag_t;

typedef enum micropixel_audio_playback_state {
    MICROPIXEL_AUDIO_PLAYBACK_STATE_PLAYING = 1,
    MICROPIXEL_AUDIO_PLAYBACK_STATE_PAUSED = 2,
    MICROPIXEL_AUDIO_PLAYBACK_STATE_FINISHED = 3,
    MICROPIXEL_AUDIO_PLAYBACK_STATE_FAILED = 4,
} micropixel_audio_playback_state_t;

typedef enum micropixel_audio_event_id {
    MICROPIXEL_AUDIO_EVENT_PLAYBACK_FINISHED = 1,
} micropixel_audio_event_id_t;

typedef struct micropixel_service_info {
    uint16_t size;
    uint16_t reserved0;
    uint32_t service_id;
    micropixel_service_handle_t handle;
    uint16_t interface_major;
    uint16_t interface_minor;
    uint32_t flags;
    uint32_t reserved1;
    uint64_t capabilities;
    uint32_t max_request_bytes;
    uint32_t max_response_bytes;
    uint32_t max_submit_bytes;
    uint32_t reserved2;
} micropixel_service_info_t;

typedef struct micropixel_handle_request {
    uint16_t size;
    uint16_t reserved0;
    uint32_t handle;
} micropixel_handle_request_t;

typedef struct micropixel_handle_response {
    uint16_t size;
    uint16_t reserved0;
    uint32_t handle;
} micropixel_handle_response_t;

typedef struct micropixel_timer_start_request {
    uint16_t size;
    uint16_t reserved0;
    micropixel_timer_handle_t timer;
    uint64_t initial_delay_us;
    uint64_t period_us;
} micropixel_timer_start_request_t;

typedef struct micropixel_storage_key_request {
    uint16_t size;
    uint16_t key_length;
    char key[MICROPIXEL_STORAGE_MAX_KEY_BYTES];
    uint8_t reserved0;
} micropixel_storage_key_request_t;

/* Followed by key_length key bytes and value_length opaque value bytes. */
typedef struct micropixel_storage_set_request {
    uint16_t size;
    uint16_t key_length;
    uint32_t value_length;
} micropixel_storage_set_request_t;

typedef struct micropixel_resource_load_texture_request {
    uint16_t size;
    uint16_t reserved0;
    uint32_t asset_id;
} micropixel_resource_load_texture_request_t;

typedef struct micropixel_resource_load_font_request {
    uint16_t size;
    uint16_t reserved0;
    uint32_t resource_id;
} micropixel_resource_load_font_request_t;

typedef struct micropixel_font_info {
    uint16_t size;
    uint16_t interface_major;
    uint16_t interface_minor;
    micropixel_font_handle_t font;
    uint16_t font_size;
    uint16_t line_height;
    int16_t ascent;
    int16_t descent;
    uint32_t reserved[2];
} micropixel_font_info_t;

typedef struct micropixel_random_u32_response {
    uint16_t size;
    uint16_t reserved0;
    uint32_t value;
} micropixel_random_u32_response_t;

typedef enum micropixel_audio_waveform {
    MICROPIXEL_AUDIO_WAVE_SINE = 1,
    MICROPIXEL_AUDIO_WAVE_SQUARE = 2,
    MICROPIXEL_AUDIO_WAVE_TRIANGLE = 3,
    MICROPIXEL_AUDIO_WAVE_NOISE = 4,
} micropixel_audio_waveform_t;

typedef struct micropixel_audio_info {
    uint16_t size;
    uint16_t interface_major;
    uint16_t interface_minor;
    uint16_t max_voices;
    uint32_t sample_rate;
    uint32_t capabilities;
    uint32_t supported_waveforms;
    uint32_t max_tone_duration_ms;
    uint16_t max_clips;
    uint16_t max_playbacks;
    uint32_t reserved;
} micropixel_audio_info_t;

typedef struct micropixel_audio_tone {
    uint16_t size;
    uint16_t interface_major;
    uint16_t waveform;
    uint16_t volume_per_mille;
    uint32_t frequency_millihz;
    uint32_t duration_ms;
    uint16_t attack_ms;
    uint16_t release_ms;
    uint32_t reserved[3];
} micropixel_audio_tone_t;

typedef struct micropixel_audio_clip_load_request {
    uint16_t size;
    uint16_t reserved0;
    uint32_t asset_id;
} micropixel_audio_clip_load_request_t;

typedef struct micropixel_audio_clip_info {
    uint16_t size;
    uint16_t interface_major;
    uint16_t interface_minor;
    uint16_t reserved0;
    micropixel_audio_clip_handle_t clip;
    uint32_t format;
} micropixel_audio_clip_info_t;

typedef struct micropixel_audio_playback_start_request {
    uint16_t size;
    uint16_t flags;
    micropixel_audio_clip_handle_t clip;
    uint16_t volume_per_mille;
    uint16_t reserved0;
    uint32_t reserved1;
} micropixel_audio_playback_start_request_t;

typedef struct micropixel_audio_playback_volume_request {
    uint16_t size;
    uint16_t reserved0;
    micropixel_audio_playback_handle_t playback;
    uint16_t volume_per_mille;
    uint16_t reserved1;
} micropixel_audio_playback_volume_request_t;

typedef struct micropixel_audio_playback_state_response {
    uint16_t size;
    uint16_t state;
    micropixel_audio_playback_handle_t playback;
} micropixel_audio_playback_state_response_t;

typedef struct micropixel_audio_event_payload {
    micropixel_audio_playback_handle_t playback;
    uint32_t reserved[3];
} micropixel_audio_event_payload_t;

typedef enum micropixel_pixel_format {
    /* Canonical byte order in Guest memory: B, G, R. */
    MICROPIXEL_PIXEL_FORMAT_BGR888 = 1,
    /* Canonical byte order in Guest memory: B, G, R, A. */
    MICROPIXEL_PIXEL_FORMAT_BGRA8888 = 2,
    /* Canonical Guest-memory layout: little-endian RGB565 uint16_t. */
    MICROPIXEL_PIXEL_FORMAT_RGB565 = 3,
} micropixel_pixel_format_t;

typedef enum micropixel_texture_flag {
    MICROPIXEL_TEXTURE_FLAG_STREAMING = 1U << 0U,
} micropixel_texture_flag_t;

typedef struct micropixel_streaming_texture_create_request {
    uint16_t size;
    uint16_t reserved0;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
} micropixel_streaming_texture_create_request_t;

/* Followed by tightly packed canonical-format pixels for the dirty rectangle. */
typedef struct micropixel_streaming_texture_update_request {
    uint16_t size;
    uint16_t reserved0;
    micropixel_texture_handle_t texture;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t reserved1;
} micropixel_streaming_texture_update_request_t;

typedef enum micropixel_system_font_handle {
    MICROPIXEL_SYSTEM_FONT_SMALL = 1,
    MICROPIXEL_SYSTEM_FONT_MEDIUM = 2,
    MICROPIXEL_SYSTEM_FONT_LARGE = 3,
    MICROPIXEL_SYSTEM_FONT_TITLE = 4,
} micropixel_system_font_handle_t;

typedef struct micropixel_graphics_info {
    uint16_t size;
    uint16_t interface_major;
    uint16_t interface_minor;
    union {
        /* Graphics 1.0-1.1 retained Layer capacity. */
        uint16_t max_layers;
        /* Graphics 1.2+ retained ContainerNode capacity. */
        uint16_t max_containers;
    };
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t max_scene_bytes;
    uint16_t max_scene_nodes;
    uint16_t max_batch_instances;
    uint16_t max_sprite_batches;
    uint16_t reserved0;
    /* Native-display pixel insets for the largest unobscured content area. */
    uint16_t safe_inset_top;
    uint16_t safe_inset_right;
    uint16_t safe_inset_bottom;
    uint16_t safe_inset_left;
} micropixel_graphics_info_t;

/* Followed by text_length UTF-8 bytes without a trailing NUL. */
typedef struct micropixel_graphics_measure_text_request {
    uint16_t size;
    micropixel_font_handle_t font;
    uint16_t text_length;
    uint16_t reserved0;
} micropixel_graphics_measure_text_request_t;

typedef struct micropixel_text_metrics {
    uint16_t size;
    uint16_t reserved0;
    uint32_t width;
    uint32_t height;
    int32_t baseline;
} micropixel_text_metrics_t;

typedef struct micropixel_graphics_scene_header {
    uint32_t magic;
    uint16_t interface_major;
    uint16_t interface_minor;
    uint16_t kind;
    uint16_t flags;
    uint32_t total_size;
    uint32_t generation;
    uint32_t base_revision;
    uint32_t revision;
    uint16_t record_count;
    uint16_t node_count;
    union {
        /* Graphics 1.0-1.1 retained Layer count. */
        uint16_t layer_count;
        /* Graphics 1.2+ retained ContainerNode count. */
        uint16_t container_count;
    };
    uint16_t batch_instance_count;
} micropixel_graphics_scene_header_t;

typedef struct micropixel_graphics_scene_record_header {
    uint16_t opcode;
    uint16_t size;
} micropixel_graphics_scene_record_header_t;

typedef struct micropixel_graphics_scene_background_record {
    micropixel_graphics_scene_record_header_t record;
    uint32_t property_mask;
    uint32_t rgb888;
} micropixel_graphics_scene_background_record_t;

typedef struct micropixel_graphics_scene_layer_record {
    micropixel_graphics_scene_record_header_t record;
    uint16_t layer_id;
    uint16_t reserved0;
    uint32_t property_mask;
    int32_t clip_x;
    int32_t clip_y;
    int32_t width;
    int32_t height;
    int32_t translate_x;
    int32_t translate_y;
    int16_t z_order;
    uint8_t opacity;
    uint8_t visible;
} micropixel_graphics_scene_layer_record_t;

/* Graphics 1.2+. Container IDs are dense 1..container_count; 0 is the implicit Scene root.
 * An empty clip (width == 0 && height == 0) inherits the parent clip without adding one. */
typedef struct micropixel_graphics_scene_container_record {
    micropixel_graphics_scene_record_header_t record;
    uint16_t container_id;
    uint16_t parent_container_id;
    uint32_t property_mask;
    int32_t clip_x;
    int32_t clip_y;
    int32_t width;
    int32_t height;
    int32_t translate_x;
    int32_t translate_y;
    int16_t z_order;
    uint8_t opacity;
    uint8_t visible;
    uint16_t sibling_order;
    /* Graphics 1.4+: micropixel_graphics_scene_container_flag_t bits, written
     * under MICROPIXEL_GRAPHICS_SCENE_CONTAINER_FLAGS. Was reserved0 (must be 0)
     * before 1.4, so the record size and older messages are unchanged. */
    uint16_t flags;
} micropixel_graphics_scene_container_record_t;

/* Graphics 1.2+. One keyframe record is required for every drawable node. */
typedef struct micropixel_graphics_scene_node_link_record {
    micropixel_graphics_scene_record_header_t record;
    uint16_t node_id;
    uint16_t parent_container_id;
    uint16_t sibling_order;
    uint16_t reserved0;
} micropixel_graphics_scene_node_link_record_t;

typedef struct micropixel_graphics_scene_node_header {
    micropixel_graphics_scene_record_header_t record;
    uint16_t node_id;
    union {
        /* Graphics 1.0-1.1 parent Layer. Graphics 1.2 messages set this to zero and use NODE_LINK. */
        uint8_t layer_id;
        uint8_t container_id;
    };
    uint8_t flags;
    uint32_t property_mask;
} micropixel_graphics_scene_node_header_t;

typedef struct micropixel_graphics_scene_rect_record {
    micropixel_graphics_scene_node_header_t node;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint32_t rgb888;
    uint8_t opacity;
    uint8_t reserved0[3];
} micropixel_graphics_scene_rect_record_t;

/* Graphics 1.3+. A zero stroke_width disables the stroke. */
typedef struct micropixel_graphics_scene_rounded_rect_record {
    micropixel_graphics_scene_node_header_t node;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint32_t fill_rgb888;
    uint32_t stroke_rgb888;
    uint32_t radius;
    uint32_t stroke_width;
    uint8_t opacity;
    uint8_t reserved0[3];
} micropixel_graphics_scene_rounded_rect_record_t;

typedef struct micropixel_graphics_scene_texture_record {
    micropixel_graphics_scene_node_header_t node;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    micropixel_texture_handle_t texture;
    int32_t source_x;
    int32_t source_y;
    int32_t source_width;
    int32_t source_height;
    uint8_t opacity;
    uint8_t reserved0[3];
} micropixel_graphics_scene_texture_record_t;

/* Followed by text_length UTF-8 bytes and zero padding to a 4-byte record size. */
typedef struct micropixel_graphics_scene_text_record {
    micropixel_graphics_scene_node_header_t node;
    int32_t x;
    int32_t y;
    uint32_t rgb888;
    micropixel_font_handle_t font;
    uint16_t text_length;
} micropixel_graphics_scene_text_record_t;

typedef struct micropixel_graphics_scene_sprite_batch_record {
    micropixel_graphics_scene_node_header_t node;
    micropixel_texture_handle_t texture;
    uint16_t capacity;
    uint8_t opacity;
    uint8_t reserved0;
} micropixel_graphics_scene_sprite_batch_record_t;

typedef struct micropixel_graphics_scene_sprite_instance {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    int32_t source_x;
    int32_t source_y;
    int32_t source_width;
    int32_t source_height;
    uint32_t rgb888;
    uint8_t opacity;
    uint8_t flags;
    uint16_t reserved0;
} micropixel_graphics_scene_sprite_instance_t;

/* Followed by instance_count contiguous micropixel_graphics_scene_sprite_instance_t values. */
typedef struct micropixel_graphics_scene_batch_instances_record {
    micropixel_graphics_scene_record_header_t record;
    uint16_t batch_node_id;
    uint16_t first_instance;
    uint16_t instance_count;
    uint16_t reserved0;
    uint32_t property_mask;
} micropixel_graphics_scene_batch_instances_record_t;

typedef struct micropixel_texture_info {
    uint16_t size;
    uint16_t interface_major;
    uint16_t interface_minor;
    uint16_t reserved0;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format;
    uint32_t flags;
    micropixel_texture_handle_t texture;
} micropixel_texture_info_t;

typedef struct micropixel_adaptive_texture_info {
    uint16_t size;
    uint16_t interface_major;
    uint16_t interface_minor;
    uint16_t reserved0;
    uint32_t logical_width;
    uint32_t logical_height;
    uint32_t physical_width;
    uint32_t physical_height;
    uint32_t stride;
    uint32_t pixel_format;
    uint32_t flags;
    micropixel_texture_handle_t texture;
} micropixel_adaptive_texture_info_t;

typedef enum micropixel_touch_phase {
    MICROPIXEL_TOUCH_DOWN = 1,
    MICROPIXEL_TOUCH_MOVE = 2,
    MICROPIXEL_TOUCH_UP = 3,
    MICROPIXEL_TOUCH_CANCEL = 4,
} micropixel_touch_phase_t;

typedef enum micropixel_input_capability {
    MICROPIXEL_INPUT_CAP_PRESSURE = 1U << 0U,
    MICROPIXEL_INPUT_CAP_KEY_EVENTS = 1U << 1U,
} micropixel_input_capability_t;

typedef enum micropixel_key_code {
    MICROPIXEL_KEY_UP = 1,
    MICROPIXEL_KEY_DOWN = 2,
    MICROPIXEL_KEY_LEFT = 3,
    MICROPIXEL_KEY_RIGHT = 4,
    MICROPIXEL_KEY_CONFIRM = 5,
    MICROPIXEL_KEY_BACK = 6,
    MICROPIXEL_KEY_MENU = 7,
    MICROPIXEL_KEY_GAMEPAD_SOUTH = 8,
    MICROPIXEL_KEY_GAMEPAD_EAST = 9,
    MICROPIXEL_KEY_GAMEPAD_WEST = 10,
    MICROPIXEL_KEY_GAMEPAD_NORTH = 11,
} micropixel_key_code_t;

typedef enum micropixel_key_phase {
    MICROPIXEL_KEY_DOWN_PHASE = 1,
    MICROPIXEL_KEY_UP_PHASE = 2,
    MICROPIXEL_KEY_REPEAT_PHASE = 3,
    MICROPIXEL_KEY_CANCEL_PHASE = 4,
} micropixel_key_phase_t;

typedef struct micropixel_input_info {
    uint16_t size;
    uint16_t interface_major;
    uint16_t interface_minor;
    uint16_t capabilities;
    uint32_t logical_width;
    uint32_t logical_height;
    uint16_t max_touch_points;
    uint16_t reserved0;
    uint32_t reserved[3];
} micropixel_input_info_t;

typedef enum micropixel_core_event_id {
    MICROPIXEL_CORE_EVENT_HOST_WAKE = 1,
    MICROPIXEL_CORE_EVENT_RESUME = 2,
    MICROPIXEL_CORE_EVENT_STOP = 3,
} micropixel_core_event_id_t;

typedef enum micropixel_timer_event_id {
    MICROPIXEL_TIMER_EVENT_EXPIRED = 1,
} micropixel_timer_event_id_t;

typedef enum micropixel_input_event_id {
    MICROPIXEL_INPUT_EVENT_TOUCH = 1,
    MICROPIXEL_INPUT_EVENT_KEY = 2,
} micropixel_input_event_id_t;

typedef struct micropixel_timer_event_payload {
    uint64_t elapsed_us;
    uint32_t missed_count;
    uint32_t reserved0;
} micropixel_timer_event_payload_t;

typedef struct micropixel_touch_event_payload {
    int32_t x;
    int32_t y;
    /* Meaningful only when MICROPIXEL_INPUT_CAP_PRESSURE is advertised. */
    uint16_t pressure_per_mille;
    uint16_t phase;
    uint32_t reserved0;
} micropixel_touch_event_payload_t;

typedef struct micropixel_key_event_payload {
    uint16_t code;
    uint16_t phase;
    uint32_t repeat_count;
    uint32_t modifiers;
    uint32_t reserved0;
} micropixel_key_event_payload_t;

/* Fixed-size envelope. Event IDs are scoped by service_id. */
typedef struct micropixel_event {
    uint16_t size;
    uint16_t event_id;
    uint32_t service_id;
    uint32_t flags;
    uint32_t source;
    micropixel_app_time_t timestamp_us;
    uint32_t sequence;
    int32_t status;
    uint8_t payload[16];
} micropixel_event_t;

#if defined(__cplusplus)
static_assert(sizeof(micropixel_event_t) == 48U, "micropixel_event_t ABI size changed");
static_assert(sizeof(micropixel_system_launch_arguments_response_t) == 552U,
              "micropixel_system_launch_arguments_response_t ABI size changed");
static_assert(sizeof(micropixel_devices_list_request_t) == 8U, "micropixel_devices_list_request_t ABI size changed");
static_assert(sizeof(micropixel_devices_list_response_t) == 264U,
              "micropixel_devices_list_response_t ABI size changed");
static_assert(sizeof(micropixel_device_request_t) == 8U, "micropixel_device_request_t ABI size changed");
static_assert(sizeof(micropixel_device_info_t) == 72U, "micropixel_device_info_t ABI size changed");
static_assert(sizeof(micropixel_device_event_payload_t) == 16U, "micropixel_device_event_payload_t ABI size changed");
static_assert(sizeof(micropixel_sensor_info_t) == 32U, "micropixel_sensor_info_t ABI size changed");
static_assert(sizeof(micropixel_sensor_open_request_t) == 8U, "micropixel_sensor_open_request_t ABI size changed");
static_assert(sizeof(micropixel_sensor_open_response_t) == 16U, "micropixel_sensor_open_response_t ABI size changed");
static_assert(sizeof(micropixel_sensor_reading_t) == 40U, "micropixel_sensor_reading_t ABI size changed");
static_assert(sizeof(micropixel_sensor_update_request_t) == 16U, "micropixel_sensor_update_request_t ABI size changed");
static_assert(sizeof(micropixel_sensor_event_payload_t) == 16U, "micropixel_sensor_event_payload_t ABI size changed");
static_assert(sizeof(micropixel_gpio_info_t) == 28U, "micropixel_gpio_info_t ABI size changed");
static_assert(sizeof(micropixel_gpio_open_request_t) == 20U, "micropixel_gpio_open_request_t ABI size changed");
static_assert(sizeof(micropixel_gpio_open_response_t) == 16U, "micropixel_gpio_open_response_t ABI size changed");
static_assert(sizeof(micropixel_gpio_value_request_t) == 12U, "micropixel_gpio_value_request_t ABI size changed");
static_assert(sizeof(micropixel_gpio_value_response_t) == 12U, "micropixel_gpio_value_response_t ABI size changed");
static_assert(sizeof(micropixel_gpio_event_payload_t) == 16U, "micropixel_gpio_event_payload_t ABI size changed");
static_assert(sizeof(micropixel_haptics_info_t) == 20U, "micropixel_haptics_info_t ABI size changed");
static_assert(sizeof(micropixel_haptics_play_request_t) == 16U, "micropixel_haptics_play_request_t ABI size changed");
static_assert(sizeof(micropixel_power_info_response_t) == 24U, "micropixel_power_info_response_t ABI size changed");
static_assert(sizeof(micropixel_timer_event_payload_t) == 16U, "micropixel_timer_event_payload_t ABI size changed");
static_assert(sizeof(micropixel_touch_event_payload_t) == 16U, "micropixel_touch_event_payload_t ABI size changed");
static_assert(sizeof(micropixel_key_event_payload_t) == 16U, "micropixel_key_event_payload_t ABI size changed");
static_assert(sizeof(micropixel_graphics_info_t) == 40U, "micropixel_graphics_info_t ABI size changed");
static_assert(sizeof(micropixel_graphics_scene_header_t) == 36U, "micropixel_graphics_scene_header_t ABI size changed");
static_assert(sizeof(micropixel_graphics_scene_record_header_t) == 4U,
              "micropixel_graphics_scene_record_header_t ABI size changed");
static_assert(sizeof(micropixel_graphics_scene_background_record_t) == 12U,
              "micropixel_graphics_scene_background_record_t ABI size changed");
static_assert(sizeof(micropixel_graphics_scene_layer_record_t) == 40U,
              "micropixel_graphics_scene_layer_record_t ABI size changed");
static_assert(sizeof(micropixel_graphics_scene_container_record_t) == 44U,
              "micropixel_graphics_scene_container_record_t ABI size changed");
static_assert(sizeof(micropixel_graphics_scene_node_link_record_t) == 12U,
              "micropixel_graphics_scene_node_link_record_t ABI size changed");
static_assert(sizeof(micropixel_graphics_scene_node_header_t) == 12U,
              "micropixel_graphics_scene_node_header_t ABI size changed");
static_assert(sizeof(micropixel_graphics_scene_rect_record_t) == 36U,
              "micropixel_graphics_scene_rect_record_t ABI size changed");
static_assert(sizeof(micropixel_graphics_scene_rounded_rect_record_t) == 48U,
              "micropixel_graphics_scene_rounded_rect_record_t ABI size changed");
static_assert(sizeof(micropixel_graphics_scene_texture_record_t) == 52U,
              "micropixel_graphics_scene_texture_record_t ABI size changed");
static_assert(sizeof(micropixel_graphics_scene_text_record_t) == 28U,
              "micropixel_graphics_scene_text_record_t ABI size changed");
static_assert(sizeof(micropixel_graphics_scene_sprite_batch_record_t) == 20U,
              "micropixel_graphics_scene_sprite_batch_record_t ABI size changed");
static_assert(sizeof(micropixel_graphics_scene_sprite_instance_t) == 40U,
              "micropixel_graphics_scene_sprite_instance_t ABI size changed");
static_assert(sizeof(micropixel_graphics_scene_batch_instances_record_t) == 16U,
              "micropixel_graphics_scene_batch_instances_record_t ABI size changed");
static_assert(sizeof(micropixel_texture_info_t) == 32U, "micropixel_texture_info_t ABI size changed");
static_assert(sizeof(micropixel_adaptive_texture_info_t) == 40U, "micropixel_adaptive_texture_info_t ABI size changed");
static_assert(sizeof(micropixel_streaming_texture_create_request_t) == 16U,
              "micropixel_streaming_texture_create_request_t ABI size changed");
static_assert(sizeof(micropixel_streaming_texture_update_request_t) == 32U,
              "micropixel_streaming_texture_update_request_t ABI size changed");
static_assert(sizeof(micropixel_input_info_t) == 32U, "micropixel_input_info_t ABI size changed");
static_assert(sizeof(micropixel_audio_info_t) == 32U, "micropixel_audio_info_t ABI size changed");
static_assert(sizeof(micropixel_audio_tone_t) == 32U, "micropixel_audio_tone_t ABI size changed");
static_assert(sizeof(micropixel_audio_clip_load_request_t) == 8U,
              "micropixel_audio_clip_load_request_t ABI size changed");
static_assert(sizeof(micropixel_audio_clip_info_t) == 16U, "micropixel_audio_clip_info_t ABI size changed");
static_assert(sizeof(micropixel_audio_playback_start_request_t) == 16U,
              "micropixel_audio_playback_start_request_t ABI size changed");
static_assert(sizeof(micropixel_audio_playback_volume_request_t) == 12U,
              "micropixel_audio_playback_volume_request_t ABI size changed");
static_assert(sizeof(micropixel_audio_playback_state_response_t) == 8U,
              "micropixel_audio_playback_state_response_t ABI size changed");
static_assert(sizeof(micropixel_audio_event_payload_t) == 16U, "micropixel_audio_event_payload_t ABI size changed");
static_assert(sizeof(micropixel_service_info_t) == 48U, "micropixel_service_info_t ABI size changed");
static_assert(sizeof(micropixel_handle_request_t) == 8U, "micropixel_handle_request_t ABI size changed");
static_assert(sizeof(micropixel_handle_response_t) == 8U, "micropixel_handle_response_t ABI size changed");
static_assert(sizeof(micropixel_timer_start_request_t) == 24U, "micropixel_timer_start_request_t ABI size changed");
static_assert(sizeof(micropixel_storage_key_request_t) == 20U, "micropixel_storage_key_request_t ABI size changed");
static_assert(sizeof(micropixel_storage_set_request_t) == 8U, "micropixel_storage_set_request_t ABI size changed");
static_assert(sizeof(micropixel_resource_load_texture_request_t) == 8U,
              "micropixel_resource_load_texture_request_t ABI size changed");
static_assert(sizeof(micropixel_resource_load_adaptive_texture_request_t) == 16U,
              "micropixel_resource_load_adaptive_texture_request_t ABI size changed");
static_assert(sizeof(micropixel_random_u32_response_t) == 8U, "micropixel_random_u32_response_t ABI size changed");
#else
_Static_assert(sizeof(micropixel_event_t) == 48U, "micropixel_event_t ABI size changed");
_Static_assert(sizeof(micropixel_system_launch_arguments_response_t) == 552U,
               "micropixel_system_launch_arguments_response_t ABI size changed");
_Static_assert(sizeof(micropixel_devices_list_request_t) == 8U, "micropixel_devices_list_request_t ABI size changed");
_Static_assert(sizeof(micropixel_devices_list_response_t) == 264U,
               "micropixel_devices_list_response_t ABI size changed");
_Static_assert(sizeof(micropixel_device_request_t) == 8U, "micropixel_device_request_t ABI size changed");
_Static_assert(sizeof(micropixel_device_info_t) == 72U, "micropixel_device_info_t ABI size changed");
_Static_assert(sizeof(micropixel_device_event_payload_t) == 16U, "micropixel_device_event_payload_t ABI size changed");
_Static_assert(sizeof(micropixel_sensor_info_t) == 32U, "micropixel_sensor_info_t ABI size changed");
_Static_assert(sizeof(micropixel_sensor_open_request_t) == 8U, "micropixel_sensor_open_request_t ABI size changed");
_Static_assert(sizeof(micropixel_sensor_open_response_t) == 16U, "micropixel_sensor_open_response_t ABI size changed");
_Static_assert(sizeof(micropixel_sensor_reading_t) == 40U, "micropixel_sensor_reading_t ABI size changed");
_Static_assert(sizeof(micropixel_sensor_update_request_t) == 16U,
               "micropixel_sensor_update_request_t ABI size changed");
_Static_assert(sizeof(micropixel_sensor_event_payload_t) == 16U, "micropixel_sensor_event_payload_t ABI size changed");
_Static_assert(sizeof(micropixel_gpio_info_t) == 28U, "micropixel_gpio_info_t ABI size changed");
_Static_assert(sizeof(micropixel_gpio_open_request_t) == 20U, "micropixel_gpio_open_request_t ABI size changed");
_Static_assert(sizeof(micropixel_gpio_open_response_t) == 16U, "micropixel_gpio_open_response_t ABI size changed");
_Static_assert(sizeof(micropixel_gpio_value_request_t) == 12U, "micropixel_gpio_value_request_t ABI size changed");
_Static_assert(sizeof(micropixel_gpio_value_response_t) == 12U, "micropixel_gpio_value_response_t ABI size changed");
_Static_assert(sizeof(micropixel_gpio_event_payload_t) == 16U, "micropixel_gpio_event_payload_t ABI size changed");
_Static_assert(sizeof(micropixel_haptics_info_t) == 20U, "micropixel_haptics_info_t ABI size changed");
_Static_assert(sizeof(micropixel_haptics_play_request_t) == 16U, "micropixel_haptics_play_request_t ABI size changed");
_Static_assert(sizeof(micropixel_power_info_response_t) == 24U, "micropixel_power_info_response_t ABI size changed");
_Static_assert(sizeof(micropixel_timer_event_payload_t) == 16U, "micropixel_timer_event_payload_t ABI size changed");
_Static_assert(sizeof(micropixel_touch_event_payload_t) == 16U, "micropixel_touch_event_payload_t ABI size changed");
_Static_assert(sizeof(micropixel_key_event_payload_t) == 16U, "micropixel_key_event_payload_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_info_t) == 40U, "micropixel_graphics_info_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_scene_header_t) == 36U,
               "micropixel_graphics_scene_header_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_scene_record_header_t) == 4U,
               "micropixel_graphics_scene_record_header_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_scene_background_record_t) == 12U,
               "micropixel_graphics_scene_background_record_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_scene_layer_record_t) == 40U,
               "micropixel_graphics_scene_layer_record_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_scene_container_record_t) == 44U,
               "micropixel_graphics_scene_container_record_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_scene_node_link_record_t) == 12U,
               "micropixel_graphics_scene_node_link_record_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_scene_node_header_t) == 12U,
               "micropixel_graphics_scene_node_header_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_scene_rect_record_t) == 36U,
               "micropixel_graphics_scene_rect_record_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_scene_rounded_rect_record_t) == 48U,
               "micropixel_graphics_scene_rounded_rect_record_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_scene_texture_record_t) == 52U,
               "micropixel_graphics_scene_texture_record_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_scene_text_record_t) == 28U,
               "micropixel_graphics_scene_text_record_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_scene_sprite_batch_record_t) == 20U,
               "micropixel_graphics_scene_sprite_batch_record_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_scene_sprite_instance_t) == 40U,
               "micropixel_graphics_scene_sprite_instance_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_scene_batch_instances_record_t) == 16U,
               "micropixel_graphics_scene_batch_instances_record_t ABI size changed");
_Static_assert(sizeof(micropixel_texture_info_t) == 32U, "micropixel_texture_info_t ABI size changed");
_Static_assert(sizeof(micropixel_adaptive_texture_info_t) == 40U,
               "micropixel_adaptive_texture_info_t ABI size changed");
_Static_assert(sizeof(micropixel_streaming_texture_create_request_t) == 16U,
               "micropixel_streaming_texture_create_request_t ABI size changed");
_Static_assert(sizeof(micropixel_streaming_texture_update_request_t) == 32U,
               "micropixel_streaming_texture_update_request_t ABI size changed");
_Static_assert(sizeof(micropixel_input_info_t) == 32U, "micropixel_input_info_t ABI size changed");
_Static_assert(sizeof(micropixel_audio_info_t) == 32U, "micropixel_audio_info_t ABI size changed");
_Static_assert(sizeof(micropixel_audio_tone_t) == 32U, "micropixel_audio_tone_t ABI size changed");
_Static_assert(sizeof(micropixel_audio_clip_load_request_t) == 8U,
               "micropixel_audio_clip_load_request_t ABI size changed");
_Static_assert(sizeof(micropixel_audio_clip_info_t) == 16U, "micropixel_audio_clip_info_t ABI size changed");
_Static_assert(sizeof(micropixel_audio_playback_start_request_t) == 16U,
               "micropixel_audio_playback_start_request_t ABI size changed");
_Static_assert(sizeof(micropixel_audio_playback_volume_request_t) == 12U,
               "micropixel_audio_playback_volume_request_t ABI size changed");
_Static_assert(sizeof(micropixel_audio_playback_state_response_t) == 8U,
               "micropixel_audio_playback_state_response_t ABI size changed");
_Static_assert(sizeof(micropixel_audio_event_payload_t) == 16U, "micropixel_audio_event_payload_t ABI size changed");
_Static_assert(sizeof(micropixel_service_info_t) == 48U, "micropixel_service_info_t ABI size changed");
_Static_assert(sizeof(micropixel_handle_request_t) == 8U, "micropixel_handle_request_t ABI size changed");
_Static_assert(sizeof(micropixel_handle_response_t) == 8U, "micropixel_handle_response_t ABI size changed");
_Static_assert(sizeof(micropixel_timer_start_request_t) == 24U, "micropixel_timer_start_request_t ABI size changed");
_Static_assert(sizeof(micropixel_storage_key_request_t) == 20U, "micropixel_storage_key_request_t ABI size changed");
_Static_assert(sizeof(micropixel_storage_set_request_t) == 8U, "micropixel_storage_set_request_t ABI size changed");
_Static_assert(sizeof(micropixel_resource_load_texture_request_t) == 8U,
               "micropixel_resource_load_texture_request_t ABI size changed");
_Static_assert(sizeof(micropixel_resource_load_adaptive_texture_request_t) == 16U,
               "micropixel_resource_load_adaptive_texture_request_t ABI size changed");
_Static_assert(sizeof(micropixel_random_u32_response_t) == 8U, "micropixel_random_u32_response_t ABI size changed");
#endif

#if defined(__wasm__)
#define MICROPIXEL_ABI_IMPORT(name) __attribute__((import_module("micropixel"), import_name(name)))
#else
#define MICROPIXEL_ABI_IMPORT(name)
#endif

#ifdef __cplusplus
extern "C" {
#endif

MICROPIXEL_ABI_IMPORT("abi_version")
uint32_t micropixel_abi_version(void);

MICROPIXEL_ABI_IMPORT("log_write")
int32_t micropixel_log_write(uint32_t level, const uint8_t* bytes, uint32_t length);

MICROPIXEL_ABI_IMPORT("event_wait")
int32_t micropixel_event_wait(micropixel_event_t* event_out, uint32_t event_size, uint64_t timeout_us);

MICROPIXEL_ABI_IMPORT("clock_now")
micropixel_app_time_t micropixel_clock_now(void);

MICROPIXEL_ABI_IMPORT("service_open")
int32_t micropixel_service_open(uint32_t service_id, uint32_t required_interface_version,
                                micropixel_service_info_t* info_out, uint32_t info_capacity);

MICROPIXEL_ABI_IMPORT("service_call")
int32_t micropixel_service_call(micropixel_service_handle_t service, uint32_t method_id, const uint8_t* request,
                                uint32_t request_size, uint8_t* response, uint32_t response_capacity,
                                uint32_t* response_size_out);

MICROPIXEL_ABI_IMPORT("service_submit")
int32_t micropixel_service_submit(micropixel_service_handle_t service, uint32_t channel_id, const uint8_t* bytes,
                                  uint32_t length);

#ifdef __cplusplus
}
#endif

#undef MICROPIXEL_ABI_IMPORT

#endif
