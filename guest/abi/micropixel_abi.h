#ifndef MICROPIXEL_GUEST_ABI_H
#define MICROPIXEL_GUEST_ABI_H

#include <stddef.h>
#include <stdint.h>

/* ABI 2.0 is a clean break from 1.x: every service interface restarts at 1.0. */
#define MICROPIXEL_ABI_VERSION_MAJOR 2U
#define MICROPIXEL_ABI_VERSION_MINOR 0U
#define MICROPIXEL_ABI_VERSION ((MICROPIXEL_ABI_VERSION_MAJOR << 16U) | MICROPIXEL_ABI_VERSION_MINOR)
#define MICROPIXEL_INTERFACE_VERSION(major, minor) (((uint32_t)(major) << 16U) | (uint32_t)(minor))
#define MICROPIXEL_ABI_MAX_LOG_BYTES 1024U
#define MICROPIXEL_GRAPHICS_INTERFACE_MAJOR 1U
#define MICROPIXEL_GRAPHICS_INTERFACE_MINOR 0U
#define MICROPIXEL_INPUT_INTERFACE_MAJOR 1U
#define MICROPIXEL_INPUT_INTERFACE_MINOR 0U
/* Scene channel messages: 'MPGS'. */
#define MICROPIXEL_GRAPHICS_SCENE_MAGIC 0x5347504dU
/* Raster channel draw lists: 'MPRS'. Warp entries carry 5 light bits and
 * INDEX8 texels address 256 palette entries; both are wire format, not policy. */
#define MICROPIXEL_GRAPHICS_RASTER_MAGIC 0x5352504dU
#define MICROPIXEL_GRAPHICS_RASTER_MAX_LIGHT_LEVELS 32U
#define MICROPIXEL_GRAPHICS_RASTER_PALETTE_ENTRIES 256U
/* Capacity limits (scene nodes, containers, batch instances, text bytes, submit
 * sizes, Direct Surface buffers) are Host policy reported by
 * micropixel_graphics_info_t, not ABI constants. */
/* Direct Surface: full-frame buffers presented in place. */
#define MICROPIXEL_SURFACE_BUFFER_ALIGNMENT 64U
#define MICROPIXEL_RESOURCE_INTERFACE_MAJOR 1U
#define MICROPIXEL_RESOURCE_INTERFACE_MINOR 0U
#define MICROPIXEL_AUDIO_INTERFACE_MAJOR 1U
#define MICROPIXEL_AUDIO_INTERFACE_MINOR 0U
#define MICROPIXEL_RANDOM_INTERFACE_MAJOR 1U
#define MICROPIXEL_RANDOM_INTERFACE_MINOR 0U
/* Upper bound of one PCM_STREAM_WRITE request, header plus interleaved int16 samples. */
#define MICROPIXEL_AUDIO_PCM_MAX_WRITE_BYTES 4096U
#define MICROPIXEL_AUDIO_PCM_MAX_CHANNELS 2U
#define MICROPIXEL_STORAGE_MAX_KEY_BYTES 15U
#define MICROPIXEL_STORAGE_MAX_VALUE_BYTES 4096U
#define MICROPIXEL_SYSTEM_INTERFACE_MAJOR 1U
#define MICROPIXEL_SYSTEM_INTERFACE_MINOR 0U
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
#define MICROPIXEL_POWER_INTERFACE_MAJOR 1U
#define MICROPIXEL_POWER_INTERFACE_MINOR 0U
#define MICROPIXEL_DEVICES_LIST_PAGE_SIZE 16U
#define MICROPIXEL_DEVICE_NAME_MAX_BYTES 39U

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
/* Resource handles are Guest-local, generation-checked; zero is invalid.
 * Raster texture_slot is a separate reusable index where zero is valid. */
typedef uint32_t micropixel_texture_handle_t;
typedef uint32_t micropixel_font_handle_t;
typedef uint32_t micropixel_audio_clip_handle_t;
typedef uint32_t micropixel_audio_playback_handle_t;
typedef uint32_t micropixel_audio_pcm_stream_handle_t;
typedef uint32_t micropixel_device_id_t;
typedef uint32_t micropixel_sensor_handle_t;
typedef uint32_t micropixel_gpio_handle_t;
typedef uint32_t micropixel_haptics_handle_t;
typedef uint32_t micropixel_surface_handle_t;

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
    MICROPIXEL_SERVICE_POWER = 23,
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

/* Method naming: fixed verb pairs. LOAD/UNLOAD for Bundle-backed resources,
 * CREATE/DESTROY for objects the Host builds from parameters, OPEN/CLOSE for
 * existing devices and streams. A service that hands out more than one kind of
 * handle prefixes the noun (TEXTURE_LOAD, PCM_STREAM_OPEN); a service with a
 * single handle kind uses the bare verb (Timer CREATE, Sensors OPEN).
 * Service-level methods without a handle keep a plain verb (GET_INFO, LIST). */

typedef enum micropixel_timer_method {
    MICROPIXEL_TIMER_METHOD_CREATE = 1,
    MICROPIXEL_TIMER_METHOD_START = 2,
    MICROPIXEL_TIMER_METHOD_CANCEL = 3,
    MICROPIXEL_TIMER_METHOD_DESTROY = 4,
} micropixel_timer_method_t;

typedef enum micropixel_storage_method {
    MICROPIXEL_STORAGE_METHOD_GET = 1,
    MICROPIXEL_STORAGE_METHOD_SET = 2,
    MICROPIXEL_STORAGE_METHOD_REMOVE = 3,
} micropixel_storage_method_t;

typedef enum micropixel_resource_method {
    /* micropixel_texture_load_request_t -> micropixel_texture_info_t */
    MICROPIXEL_RESOURCE_METHOD_TEXTURE_LOAD = 1,
    /* micropixel_handle_request_t; also destroys dynamic texture snapshots. */
    MICROPIXEL_RESOURCE_METHOD_TEXTURE_UNLOAD = 2,
    /* micropixel_font_load_request_t -> micropixel_font_info_t */
    MICROPIXEL_RESOURCE_METHOD_FONT_LOAD = 3,
    /* micropixel_handle_request_t */
    MICROPIXEL_RESOURCE_METHOD_FONT_UNLOAD = 4,
    /* micropixel_dynamic_texture_create_request_t -> micropixel_texture_info_t */
    MICROPIXEL_RESOURCE_METHOD_DYNAMIC_TEXTURE_CREATE = 5,
    /* micropixel_dynamic_texture_update_request_t -> micropixel_texture_info_t */
    MICROPIXEL_RESOURCE_METHOD_DYNAMIC_TEXTURE_UPDATE = 6,
} micropixel_resource_method_t;

/* Loads a Bundle bitmap asset. `scale_numerator / scale_denominator` is applied
 * to the authored size (1/1 keeps it); the response reports both the authored
 * (logical) and the decoded (physical) dimensions. */
typedef struct micropixel_texture_load_request {
    uint16_t size;
    uint16_t reserved0;
    uint32_t asset_id;
    uint32_t scale_numerator;
    uint32_t scale_denominator;
} micropixel_texture_load_request_t;

typedef struct micropixel_font_load_request {
    uint16_t size;
    uint16_t reserved0;
    uint32_t asset_id;
} micropixel_font_load_request_t;

/* Response of TEXTURE_LOAD and the DYNAMIC_TEXTURE methods. `width`/`height`
 * are the logical size the Guest addresses; `physical_*` is the stored bitmap.
 * They differ only for scaled TEXTURE_LOAD. */
typedef struct micropixel_texture_info {
    uint16_t size;
    uint16_t reserved0;
    micropixel_texture_handle_t texture_handle;
    uint32_t width;
    uint32_t height;
    uint32_t physical_width;
    uint32_t physical_height;
    uint32_t pixel_format;
    uint32_t flags;
} micropixel_texture_info_t;

/* Dynamic texture snapshots. Pixel offsets are resolved for this call only.
 * UPDATE returns a new immutable snapshot; it does not mutate or release the
 * source handle. The SDK keeps the logical identity and unloads its old ref.
 * A zero pixels/length/pitch triple creates a zero-initialized texture. */
typedef struct micropixel_dynamic_texture_create_request {
    uint16_t size;
    uint16_t reserved0;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t pixels;
    uint32_t length;
    uint32_t pitch;
} micropixel_dynamic_texture_create_request_t;

typedef struct micropixel_dynamic_texture_update_request {
    uint16_t size;
    uint16_t reserved0;
    micropixel_texture_handle_t texture_handle;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t pixels;
    uint32_t length;
    uint32_t pitch;
} micropixel_dynamic_texture_update_request_t;

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

/* LIST is paged: the response holds the matching devices starting at
 * `first_index`, at most DEVICES_LIST_PAGE_SIZE of them, plus the matching
 * total. A Guest walks pages while first_index < total_count and restarts
 * when `generation` changes between pages. */
typedef struct micropixel_devices_list_request {
    uint16_t size;
    uint16_t kind;
    uint16_t first_index;
    uint16_t reserved0;
} micropixel_devices_list_request_t;

typedef struct micropixel_devices_list_response {
    uint16_t size;
    uint16_t count;
    uint32_t generation;
    uint16_t total_count;
    uint16_t reserved0;
    micropixel_device_id_t devices[MICROPIXEL_DEVICES_LIST_PAGE_SIZE];
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
    MICROPIXEL_SENSORS_METHOD_CLOSE = 5,
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
    uint32_t min_interval_us;
    uint32_t max_interval_us;
    uint32_t reserved0[2];
} micropixel_sensor_info_t;

typedef struct micropixel_sensor_open_request {
    uint16_t size;
    uint16_t expected_kind;
    micropixel_device_id_t device;
} micropixel_sensor_open_request_t;

typedef struct micropixel_sensor_open_response {
    uint16_t size;
    uint16_t kind;
    micropixel_sensor_handle_t sensor_handle;
    micropixel_device_id_t device;
    uint32_t reserved0;
} micropixel_sensor_open_response_t;

typedef struct micropixel_sensor_reading {
    uint16_t size;
    uint16_t kind;
    micropixel_sensor_handle_t sensor_handle;
    micropixel_device_id_t device;
    uint32_t reserved0;
    micropixel_app_time_t timestamp_us;
    float values[4];
} micropixel_sensor_reading_t;

typedef struct micropixel_sensor_sample_interval_request {
    uint16_t size;
    uint16_t reserved0;
    micropixel_sensor_handle_t sensor_handle;
    uint64_t interval_us;
} micropixel_sensor_sample_interval_request_t;

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
    MICROPIXEL_GPIO_METHOD_CLOSE = 6,
} micropixel_gpio_method_t;

typedef enum micropixel_gpio_event_id {
    MICROPIXEL_GPIO_EVENT_EDGE = 1,
} micropixel_gpio_event_id_t;

typedef struct micropixel_gpio_info {
    uint16_t size;
    uint16_t line_number;
    micropixel_device_id_t device;
    uint32_t capabilities;
    uint32_t max_pwm_frequency_hz;
    uint32_t reserved0[3];
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
    micropixel_gpio_handle_t gpio_handle;
    micropixel_device_id_t device;
    uint32_t reserved0;
} micropixel_gpio_open_response_t;

typedef struct micropixel_gpio_value_request {
    uint16_t size;
    uint16_t reserved0;
    micropixel_gpio_handle_t gpio_handle;
    uint32_t value;
} micropixel_gpio_value_request_t;

typedef struct micropixel_gpio_value_response {
    uint16_t size;
    uint16_t reserved0;
    micropixel_gpio_handle_t gpio_handle;
    uint32_t value;
} micropixel_gpio_value_response_t;

/* micropixel_event_t.source carries the gpio handle. */
typedef struct micropixel_gpio_event_payload {
    uint32_t value;
    uint32_t edge;
    uint32_t reserved0[2];
} micropixel_gpio_event_payload_t;

typedef enum micropixel_haptics_capability {
    MICROPIXEL_HAPTICS_CAP_VARIABLE_STRENGTH = 1U << 0U,
} micropixel_haptics_capability_t;

typedef enum micropixel_haptics_method {
    MICROPIXEL_HAPTICS_METHOD_GET_INFO = 1,
    MICROPIXEL_HAPTICS_METHOD_OPEN = 2,
    MICROPIXEL_HAPTICS_METHOD_PLAY = 3,
    MICROPIXEL_HAPTICS_METHOD_STOP = 4,
    MICROPIXEL_HAPTICS_METHOD_CLOSE = 5,
} micropixel_haptics_method_t;

typedef enum micropixel_haptics_event_id {
    MICROPIXEL_HAPTICS_EVENT_FINISHED = 1,
} micropixel_haptics_event_id_t;

typedef struct micropixel_haptics_info {
    uint16_t size;
    uint16_t reserved0;
    micropixel_device_id_t device;
    uint32_t capabilities;
    uint32_t max_duration_ms;
    uint32_t reserved1;
} micropixel_haptics_info_t;

typedef struct micropixel_haptics_play_request {
    uint16_t size;
    uint16_t strength_per_mille;
    micropixel_haptics_handle_t haptics_handle;
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
    MICROPIXEL_POWER_METHOD_GET_INFO = 1,
} micropixel_power_info_method_t;

typedef enum micropixel_power_info_event_id {
    MICROPIXEL_POWER_EVENT_CHANGED = 1,
} micropixel_power_info_event_id_t;

typedef struct micropixel_power_info {
    uint16_t size;
    uint16_t source;
    micropixel_device_id_t device;
    uint32_t flags;
    uint8_t battery_percent;
    uint8_t reserved0[3];
    uint32_t reserved1[2];
} micropixel_power_info_t;

typedef enum micropixel_graphics_method {
    MICROPIXEL_GRAPHICS_METHOD_GET_INFO = 1,
    MICROPIXEL_GRAPHICS_METHOD_MEASURE_TEXT = 2,
    /* Direct Surface. */
    MICROPIXEL_GRAPHICS_METHOD_SURFACE_CREATE = 3,
    MICROPIXEL_GRAPHICS_METHOD_SURFACE_PRESENT = 4,
    MICROPIXEL_GRAPHICS_METHOD_SURFACE_DESTROY = 5,
    /* Raster kernels: Host-owned INDEX8 textures and lit palette used by
     * MICROPIXEL_GRAPHICS_CHANNEL_RASTER draw lists. */
    MICROPIXEL_GRAPHICS_METHOD_RASTER_TEXTURE_UPLOAD = 6,
    MICROPIXEL_GRAPHICS_METHOD_RASTER_PALETTE_UPLOAD = 7,
    /* Host-owned screen-to-texture warp maps used by WARP records. */
    MICROPIXEL_GRAPHICS_METHOD_RASTER_WARP_UPLOAD = 8,
} micropixel_graphics_method_t;

/* micropixel_service_descriptor_t.capabilities for MICROPIXEL_SERVICE_GRAPHICS. */
typedef enum micropixel_graphics_capability {
    /* RASTER_TEXTURE_UPLOAD / RASTER_PALETTE_UPLOAD / RASTER_WARP_UPLOAD and the
     * RASTER channel are available. */
    MICROPIXEL_GRAPHICS_CAP_RASTER = 1U << 0U,
    /* TRIANGLE and QUAD records (affine textured, per-vertex lit polygons) are
     * accepted by the RASTER channel. */
    MICROPIXEL_GRAPHICS_CAP_RASTER_POLYGON = 1U << 1U,
    /* SPRITE records accept MICROPIXEL_RASTER_SPRITE_ADDITIVE (saturating
     * per-channel add onto the target: glows, light pools, trails). */
    MICROPIXEL_GRAPHICS_CAP_RASTER_SPRITE_ADDITIVE = 1U << 2U,
} micropixel_graphics_capability_t;

typedef enum micropixel_graphics_event_id {
    /* A presented Direct Surface buffer is no longer read by the Host. */
    MICROPIXEL_GRAPHICS_EVENT_SURFACE_RELEASED = 1,
} micropixel_graphics_event_id_t;

/* micropixel_surface_create_response_t.native_flags / micropixel_graphics_info_t.native_flags */
typedef enum micropixel_surface_native_flag {
    /* The panel consumes RGB565 with the two bytes of every pixel swapped.
     * Presented buffers must hold that order; Host buffers always do. */
    MICROPIXEL_SURFACE_NATIVE_RGB565_BYTE_SWAPPED = 1U << 0U,
    /* The Host scans the presented buffer out without an App Surface copy. When
     * clear, presents are correct but go through the composited path. */
    MICROPIXEL_SURFACE_NATIVE_DIRECT_SCANOUT = 1U << 1U,
} micropixel_surface_native_flag_t;

/* micropixel_surface_create_request_t.flags */
typedef enum micropixel_surface_create_flag {
    /* The Guest allocates the buffers in its linear memory, writes pixels
     * itself and names them by address in every present. The Host keeps
     * pointers into Guest memory while a buffer is in flight, so the Bundle
     * must declare PINNED_MEMORY. Without this flag (the default) the Host
     * allocates and owns the buffers in the panel's native byte order; the
     * Guest never maps them and draws only through
     * MICROPIXEL_GRAPHICS_CHANNEL_RASTER lists that name a buffer index. */
    MICROPIXEL_SURFACE_CREATE_GUEST_BUFFERS = 1U << 0U,
} micropixel_surface_create_flag_t;

/* micropixel_surface_present_request_t.flags. Presented pixels are always in
 * the panel byte order advertised by native_flags: Host buffers are kept that
 * way, and a GUEST_BUFFERS Guest writes that way (RGB565_BYTE_SWAPPED tells it
 * to swap the two bytes of every pixel). */
typedef enum micropixel_surface_present_flag {
    /* The buffer (source_width/source_height) may be an integer fraction of the
     * panel on both axes; the Host enlarges with nearest-neighbour sampling.
     * Applies to Host and Guest buffers alike. */
    MICROPIXEL_SURFACE_PRESENT_SCALE_NEAREST = 1U << 0U,
} micropixel_surface_present_flag_t;

typedef struct micropixel_surface_create_request {
    uint16_t size;
    uint16_t reserved0;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    /* 1..micropixel_graphics_info_t.max_surface_buffers buffers that may be in flight. */
    uint32_t buffer_count;
    uint32_t flags;
} micropixel_surface_create_request_t;

typedef struct micropixel_surface_create_response {
    uint16_t size;
    uint16_t reserved0;
    micropixel_surface_handle_t surface_handle;
    uint32_t native_pixel_format;
    uint32_t native_flags;
    uint16_t max_full_frame_fps;
    uint16_t reserved1;
} micropixel_surface_create_response_t;

/* Host buffers: pixels and length are 0, pitch is source_width * 2 and
 * source_width/source_height equal the buffer size; buffer_index alone names the
 * buffer. GUEST_BUFFERS: `pixels` is a Guest linear-memory offset and the Host
 * validates pixels..pixels+length, MICROPIXEL_SURFACE_BUFFER_ALIGNMENT and
 * pitch * source_height <= length. Either way the buffer belongs to the Host until
 * MICROPIXEL_GRAPHICS_EVENT_SURFACE_RELEASED names it. */
typedef struct micropixel_surface_present_request {
    uint16_t size;
    uint16_t reserved0;
    micropixel_surface_handle_t surface_handle;
    uint32_t buffer_index;
    uint32_t pixels;
    uint32_t length;
    uint32_t pitch;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t flags;
} micropixel_surface_present_request_t;

typedef struct micropixel_surface_event_payload {
    micropixel_surface_handle_t surface_handle;
    uint32_t buffer_index;
    micropixel_app_time_t timestamp_us;
} micropixel_surface_event_payload_t;

typedef enum micropixel_graphics_channel {
    MICROPIXEL_GRAPHICS_CHANNEL_SCENE = 1,
    /* One micropixel_raster_header_t followed by raster records.
     * The Host rasterizes synchronously into the Host-owned Direct Surface
     * buffer named by the header before service_submit returns. */
    MICROPIXEL_GRAPHICS_CHANNEL_RASTER = 2,
} micropixel_graphics_channel_t;

/* ---- Raster kernels -----------------------------------------------------
 * The Guest keeps its geometry; the Host owns the Direct Surface buffers,
 * INDEX8 textures and a lit palette (light_levels x 256 canonical RGB565,
 * converted by the Host to the panel byte order once at upload) and runs the
 * per-pixel loops natively on the Guest task. Every color a record carries is
 * canonical RGB565 as well. Textures have arbitrary positive dimensions.
 * Power-of-two dimensions enable faster sampling. */

typedef enum micropixel_raster_texture_layout {
    /* texel(u, v) = pixels[u * height + v]; used by COLUMN records. */
    MICROPIXEL_RASTER_LAYOUT_COLUMN_MAJOR = 1,
    /* texel(u, v) = pixels[v * width + u]; used by SPAN_PAIR, SPAN, WARP and
     * polygon records. */
    MICROPIXEL_RASTER_LAYOUT_ROW_MAJOR = 2,
} micropixel_raster_texture_layout_t;

/* `pixels` is a Guest linear-memory offset of width * height INDEX8 texels in
 * `layout`; `length` is the byte size of that range and must equal
 * width * height. Uploading to an occupied slot replaces the texture. */
typedef struct micropixel_raster_texture_upload_request {
    uint16_t size;
    uint8_t texture_slot;
    uint8_t reserved0;
    uint16_t width;
    uint16_t height;
    uint16_t layout;
    uint16_t reserved1;
    uint32_t pixels;
    uint32_t length;
} micropixel_raster_texture_upload_request_t;

/* `entries` is a Guest linear-memory offset of light_levels *
 * RASTER_PALETTE_ENTRIES canonical RGB565 values; entry [light_level][index]
 * is the final pixel written for texel `index` at `light_level`. `length` is
 * the byte size of that range and must match the dimensions. Palette slots
 * are independent uint8 IDs like texture slots: a world palette with many
 * light levels can sit next to a flat sprite palette or a gradient. Uploading
 * to an occupied slot replaces it. */
typedef struct micropixel_raster_palette_upload_request {
    uint16_t size;
    uint8_t palette_slot;
    uint8_t reserved0;
    uint16_t light_levels;
    uint16_t reserved1;
    uint32_t entries;
    uint32_t length;
} micropixel_raster_palette_upload_request_t;

/* Warp map: `width` x `height` uint32 entries, one per target pixel, each
 * naming the texel and light level that pixel takes (see
 * micropixel_raster_warp_entry). Rows row0..row0+row_count-1 are replaced by
 * the `row_count * width` entries at Guest offset `entries`. An empty slot,
 * or one whose dimensions differ, is allocated anew with every other row set
 * to WARP_ENTRY_SKIP, so a large map can be streamed in over several
 * requests; a slot of the same size is updated in place. Failure leaves the
 * old map untouched. */
typedef struct micropixel_raster_warp_upload_request {
    uint16_t size;
    uint8_t warp_slot;
    uint8_t reserved0;
    uint16_t width;
    uint16_t height;
    uint16_t row0;
    uint16_t row_count;
    uint32_t entries;
    uint32_t length;
} micropixel_raster_warp_upload_request_t;

/* One warp map entry. Bit 31 set: the pixel is skipped (or filled with the
 * record's fill color). Otherwise bits 0..11 are u, bits 12..23 v and bits
 * 24..28 the light level (below RASTER_MAX_LIGHT_LEVELS). Bit 30
 * (WARP_ENTRY_SOLID) makes the pixel take palette entry (u & 0xFF) at that
 * light level without sampling the texture, so gradients, halos and vignettes
 * live in the same map as the textured pixels; bit 29 must be clear. u/v have
 * the record's offsets added and wrap on the texture size, which is why WARP
 * needs power-of-two textures. */
#define MICROPIXEL_RASTER_WARP_ENTRY_SKIP 0x80000000U
#define MICROPIXEL_RASTER_WARP_ENTRY_SOLID 0x40000000U
#define MICROPIXEL_RASTER_WARP_ENTRY_RESERVED 0x20000000U
#define MICROPIXEL_RASTER_WARP_U_MASK 0x00000FFFU
#define MICROPIXEL_RASTER_WARP_V_SHIFT 12U
#define MICROPIXEL_RASTER_WARP_V_MASK 0x00000FFFU
#define MICROPIXEL_RASTER_WARP_LIGHT_SHIFT 24U
#define MICROPIXEL_RASTER_WARP_LIGHT_MASK 0x0000001FU
#define MICROPIXEL_RASTER_WARP_MAX_TEXTURE_SIZE 4096U
#define MICROPIXEL_RASTER_WARP_MAX_U_FRACTION_BITS 4U

typedef enum micropixel_raster_record_type {
    /* micropixel_raster_column_t: vertical run of texels from one texture column. */
    MICROPIXEL_RASTER_RECORD_COLUMN = 1,
    /* micropixel_raster_span_pair_t: one floor row and one mirrored ceiling row. */
    MICROPIXEL_RASTER_RECORD_SPAN_PAIR = 2,
    /* micropixel_raster_sprite_t: scaled, clipped copy of a texture rectangle. */
    MICROPIXEL_RASTER_RECORD_SPRITE = 3,
    /* micropixel_raster_rect_t: solid or blended rectangle fill. */
    MICROPIXEL_RASTER_RECORD_RECT = 4,
    /* Shared Resource Texture, nearest sampling with BGRA/opacity blending. */
    MICROPIXEL_RASTER_RECORD_IMAGE = 5,
    /* micropixel_raster_warp_t: every pixel of a warp map sampled from one texture. */
    MICROPIXEL_RASTER_RECORD_WARP = 6,
    /* micropixel_raster_triangle_t: affine textured, per-vertex lit triangle. */
    MICROPIXEL_RASTER_RECORD_TRIANGLE = 7,
    /* micropixel_raster_quad_t: convex quadrilateral with the same sampling. */
    MICROPIXEL_RASTER_RECORD_QUAD = 8,
    /* micropixel_raster_span_t: one textured row (Mode-7 ground, road strips). */
    MICROPIXEL_RASTER_RECORD_SPAN = 9,
    /* micropixel_raster_text_t: system or loaded font text drawn by the Host. */
    MICROPIXEL_RASTER_RECORD_TEXT = 10,
} micropixel_raster_record_type_t;

typedef enum micropixel_raster_column_flag {
    /* Texel index 0 is transparent (sprites). */
    MICROPIXEL_RASTER_COLUMN_TRANSPARENT_INDEX0 = 1U << 0U,
} micropixel_raster_column_flag_t;

typedef enum micropixel_raster_sprite_flag {
    /* Texel index 0 is transparent. */
    MICROPIXEL_RASTER_SPRITE_TRANSPARENT_INDEX0 = 1U << 0U,
    /* Every texel that is drawn writes `color` instead of the lit palette entry
     * (glyph atlases, monochrome overlays); `light_level` is ignored. */
    MICROPIXEL_RASTER_SPRITE_SOLID_COLOR = 1U << 1U,
    /* Every drawn texel is added to the target pixel channel by channel with
     * saturation instead of replacing it (needs
     * MICROPIXEL_GRAPHICS_CAP_RASTER_SPRITE_ADDITIVE). Combines with
     * TRANSPARENT_INDEX0 and SOLID_COLOR. */
    MICROPIXEL_RASTER_SPRITE_ADDITIVE = 1U << 2U,
} micropixel_raster_sprite_flag_t;

typedef enum micropixel_raster_warp_flag {
    /* Skipped entries write `fill_color` instead of leaving the pixel alone. */
    MICROPIXEL_RASTER_WARP_FILL_SKIPPED = 1U << 0U,
} micropixel_raster_warp_flag_t;

typedef enum micropixel_raster_polygon_flag {
    /* Texel index 0 is transparent (grates, foliage). */
    MICROPIXEL_RASTER_POLYGON_TRANSPARENT_INDEX0 = 1U << 0U,
    /* No texture is sampled: every pixel takes the palette entry named by the
     * integer part of vertices[0].u (u >> 8) at the interpolated light level.
     * texture_slot is ignored. */
    MICROPIXEL_RASTER_POLYGON_FLAT_COLOR = 1U << 1U,
} micropixel_raster_polygon_flag_t;

/* Target is Host-owned buffer `buffer_index` of `surface_handle`,
 * which must not be in flight. The Host resolves its dimensions and pitch
 * before validating records. Records follow the header back to
 * back; any invalid record rejects the whole submission before a pixel is
 * written. */
typedef struct micropixel_raster_header {
    uint32_t magic;
    uint32_t total_size;
    micropixel_surface_handle_t surface_handle;
    uint32_t buffer_index;
    uint16_t record_count;
    uint16_t reserved0;
    uint32_t flags;
} micropixel_raster_header_t;

/* Pixels x, y0..y1 inclusive take texel (u, floor(v / 65536) mod height) of a
 * COLUMN_MAJOR texture at `light_level` of `palette_slot`; v advances by
 * v_step per pixel (16.16). */
typedef struct micropixel_raster_column {
    uint8_t type;
    uint8_t flags;
    uint8_t texture_slot;
    uint8_t light_level;
    uint16_t x;
    int16_t y0;
    int16_t y1;
    uint16_t u;
    int32_t v_start;
    int32_t v_step;
    uint8_t palette_slot;
    uint8_t reserved0[3];
} micropixel_raster_column_t;

/* Row y_floor takes floor_texture_slot and row y_ceiling takes ceiling_texture_slot over
 * x0..x1 inclusive, both ROW_MAJOR and sampled at the same (s, t) walk:
 * texel_x = floor(fract(s / 65536) * width), texel_y likewise with t,
 * so the integer part of s/t is the tile index and the fraction selects the
 * texel. s/t advance by ds/dt per pixel (16.16). */
typedef struct micropixel_raster_span_pair {
    uint8_t type;
    uint8_t flags;
    uint8_t floor_texture_slot;
    uint8_t ceiling_texture_slot;
    uint16_t y_floor;
    uint16_t y_ceiling;
    uint16_t x0;
    uint16_t x1;
    uint8_t light_level;
    uint8_t palette_slot;
    uint8_t reserved0[2];
    int32_t s;
    int32_t t;
    int32_t ds;
    int32_t dt;
} micropixel_raster_span_pair_t;

/* Texels (source_x..+source_width-1, source_y..+source_height-1) of a COLUMN_MAJOR texture
 * are scaled with nearest-neighbour sampling onto the target rectangle
 * (x, y, width, height), which may lie partly or wholly outside the target:
 * the Host clips. Drawn texels take the lit palette entry at `light_level`, or
 * `color` with SPRITE_SOLID_COLOR. Meant for weapons, HUD icons and glyphs;
 * depth-sorted world sprites use COLUMN records. */
typedef struct micropixel_raster_sprite {
    uint8_t type;
    uint8_t flags;
    uint8_t texture_slot;
    uint8_t light_level;
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
    uint16_t source_x;
    uint16_t source_y;
    uint16_t source_width;
    uint16_t source_height;
    uint16_t color;
    uint8_t palette_slot;
    uint8_t reserved0;
} micropixel_raster_sprite_t;

/* Uses a Resource Texture snapshot directly, without an INDEX8 slot or palette.
 * Source coordinates are authored physical texture pixels; destination is in
 * target-buffer pixels. The resolved bitmap is borrowed for synchronous draw. */
typedef struct micropixel_raster_image {
    uint8_t type;
    uint8_t opacity;
    uint16_t reserved0;
    micropixel_texture_handle_t texture_handle;
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
    uint16_t source_x;
    uint16_t source_y;
    uint16_t source_width;
    uint16_t source_height;
} micropixel_raster_image_t;

/* Fills the target rectangle (x, y, width, height), clipped, with `color`.
 * opacity 255 writes the color; a smaller opacity blends color over the
 * existing pixel per channel (opacity / 255). opacity 0 is rejected. */
typedef struct micropixel_raster_rect {
    uint8_t type;
    uint8_t flags;
    uint8_t opacity;
    uint8_t reserved0;
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
    uint16_t color;
    uint16_t reserved1;
} micropixel_raster_rect_t;

/* Draws warp map `warp_slot` with its entry (0, 0) at target pixel (x, y),
 * clipped. Textured entries sample ROW_MAJOR `texture_slot`, whose width and
 * height must be powers of two no larger than WARP_MAX_TEXTURE_SIZE, at
 * (((u + u_offset) >> u_fraction_bits) mod width, (v + v_offset) mod height)
 * and write the `palette_slot` entry at the entry's light level; SOLID
 * entries index the palette directly. `u_fraction_bits` (0..WARP_MAX_U_FRACTION_BITS)
 * makes the low bits of the entries' u and of u_offset a texel fraction, so a
 * scrolling texture can advance in sub-texel steps; the texture width times
 * 2^u_fraction_bits must not exceed WARP_MAX_TEXTURE_SIZE. Every light level
 * in the map must be below the palette's level count. Skipped entries leave
 * the pixel alone unless WARP_FILL_SKIPPED writes `fill_color` (canonical
 * RGB565). One record covers a whole sphere, tunnel or planar ground: the
 * per-frame cost is one offset change. */
typedef struct micropixel_raster_warp {
    uint8_t type;
    uint8_t flags;
    uint8_t warp_slot;
    uint8_t texture_slot;
    int16_t x;
    int16_t y;
    uint16_t u_offset;
    uint16_t v_offset;
    uint16_t fill_color;
    uint8_t palette_slot;
    uint8_t u_fraction_bits;
} micropixel_raster_warp_t;

/* One polygon corner. x/y are target pixels in signed 12.4 fixed point (the
 * sub-pixel bits keep slow-moving edges from jittering); u/v are texel
 * coordinates in 8.8 fixed point whose integer part wraps on the texture size;
 * light is the lit palette level at this corner. */
typedef struct micropixel_raster_vertex {
    int16_t x;
    int16_t y;
    uint16_t u;
    uint16_t v;
    uint8_t light;
    uint8_t reserved0;
} micropixel_raster_vertex_t;

/* Affine textured polygons (TRIANGLE / QUAD). The Host fills every pixel whose
 * centre lies inside the polygon, clipped to the target; a polygon may lie
 * partly or wholly outside. Vertices are in either winding order; a QUAD must
 * be convex (a concave one draws something bounded but unspecified). u, v and
 * light are interpolated linearly along the left and right edges and then
 * linearly across each scanline (no perspective correction; the Guest
 * subdivides large near polygons), so a quad has no diagonal seam. The
 * texture is ROW_MAJOR with power-of-two dimensions; the sampled texel is
 * (floor(u) mod width, floor(v) mod height) and the pixel written is the
 * `palette_slot` entry at the interpolated light level, which never leaves the
 * range spanned by the corner lights. Every corner light must be below the
 * palette's level count. Zero-area polygons draw nothing. */
typedef struct micropixel_raster_triangle {
    uint8_t type;
    uint8_t flags; /* micropixel_raster_polygon_flag_t */
    uint8_t texture_slot;
    uint8_t palette_slot;
    micropixel_raster_vertex_t vertices[3];
    uint16_t reserved0;
} micropixel_raster_triangle_t;

typedef struct micropixel_raster_quad {
    uint8_t type;
    uint8_t flags; /* micropixel_raster_polygon_flag_t */
    uint8_t texture_slot;
    uint8_t palette_slot;
    micropixel_raster_vertex_t vertices[4];
} micropixel_raster_quad_t;

/* Row y over x0..x1 inclusive takes ROW_MAJOR `texture_slot` sampled at the
 * same 16.16 (s, t) walk as SPAN_PAIR: texel_x = floor(fract(s / 65536) *
 * width), texel_y likewise with t, both advancing by ds/dt per pixel. The
 * single-row form of SPAN_PAIR for perspective ground planes, where every
 * screen row has its own depth and therefore its own step: a Mode-7 plane or
 * a pseudo-3D road is one record per row. The row and both ends must lie
 * inside the target. */
typedef struct micropixel_raster_span {
    uint8_t type;
    uint8_t flags;
    uint8_t texture_slot;
    uint8_t light_level;
    uint16_t y;
    uint16_t x0;
    uint16_t x1;
    uint8_t palette_slot;
    uint8_t reserved0;
    int32_t s;
    int32_t t;
    int32_t ds;
    int32_t dt;
} micropixel_raster_span_t;

/* Draws `text_length` bytes of UTF-8 (which follow this header, padded with
 * zero bytes to a multiple of 4; the record size is
 * sizeof(micropixel_raster_text_t) + that padded length) with `font_handle`
 * (a micropixel_system_font_handle_t or a FONT_LOAD handle) in canonical
 * RGB565 `color`, the text's top-left at (x, y), clipped to the target.
 * Metrics are those of TEXT_MEASURE and text_length obeys the same bound; an
 * invalid handle or malformed UTF-8 rejects the submission. Glyph coverage
 * is blended over the existing pixels, so HUD labels read over any ground. */
typedef struct micropixel_raster_text {
    uint8_t type;
    uint8_t flags;
    uint16_t text_length;
    int16_t x;
    int16_t y;
    uint16_t color;
    uint16_t reserved0;
    micropixel_font_handle_t font_handle;
} micropixel_raster_text_t;

typedef enum micropixel_graphics_scene_message_kind {
    MICROPIXEL_GRAPHICS_SCENE_KEYFRAME = 1,
    MICROPIXEL_GRAPHICS_SCENE_PATCH = 2,
} micropixel_graphics_scene_message_kind_t;

typedef enum micropixel_graphics_scene_record_opcode {
    MICROPIXEL_GRAPHICS_SCENE_OP_BACKGROUND = 1,
    MICROPIXEL_GRAPHICS_SCENE_OP_CONTAINER = 2,
    MICROPIXEL_GRAPHICS_SCENE_OP_NODE_LINK = 3,
    MICROPIXEL_GRAPHICS_SCENE_OP_RECT = 4,
    MICROPIXEL_GRAPHICS_SCENE_OP_ROUNDED_RECT = 5,
    MICROPIXEL_GRAPHICS_SCENE_OP_TEXTURE = 6,
    MICROPIXEL_GRAPHICS_SCENE_OP_TEXT = 7,
    MICROPIXEL_GRAPHICS_SCENE_OP_SPRITE_BATCH = 8,
    MICROPIXEL_GRAPHICS_SCENE_OP_BATCH_INSTANCES = 9,
} micropixel_graphics_scene_record_opcode_t;

typedef enum micropixel_graphics_scene_node_property {
    MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY = 1U << 0U,
    MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE = 1U << 1U,
    MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT = 1U << 2U,
    MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBILITY = 1U << 3U,
    MICROPIXEL_GRAPHICS_SCENE_NODE_KIND = 1U << 4U,
} micropixel_graphics_scene_node_property_t;

typedef enum micropixel_graphics_scene_container_property {
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_CLIP = 1U << 0U,
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_TRANSLATION = 1U << 1U,
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_APPEARANCE = 1U << 2U,
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_Z_ORDER = 1U << 3U,
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_STRUCTURE = 1U << 4U,
    /* Covers micropixel_graphics_scene_container_record_t::flags. */
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_FLAGS = 1U << 5U,
} micropixel_graphics_scene_container_property_t;

/* Rendering hints for a container subtree; they never change
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
    MICROPIXEL_AUDIO_METHOD_TONE_PLAY = 2,
    /* Stops every tone, playback and PCM stream of the session. */
    MICROPIXEL_AUDIO_METHOD_STOP_ALL = 3,
    MICROPIXEL_AUDIO_METHOD_CLIP_LOAD = 4,
    MICROPIXEL_AUDIO_METHOD_CLIP_UNLOAD = 5,
    MICROPIXEL_AUDIO_METHOD_PLAYBACK_START = 6,
    MICROPIXEL_AUDIO_METHOD_PLAYBACK_PAUSE = 7,
    MICROPIXEL_AUDIO_METHOD_PLAYBACK_RESUME = 8,
    MICROPIXEL_AUDIO_METHOD_PLAYBACK_SET_VOLUME = 9,
    MICROPIXEL_AUDIO_METHOD_PLAYBACK_STOP = 10,
    MICROPIXEL_AUDIO_METHOD_PLAYBACK_GET_STATE = 11,
    /* Guest-generated PCM. */
    MICROPIXEL_AUDIO_METHOD_PCM_STREAM_OPEN = 12,
    MICROPIXEL_AUDIO_METHOD_PCM_STREAM_WRITE = 13,
    MICROPIXEL_AUDIO_METHOD_PCM_STREAM_CLOSE = 14,
} micropixel_audio_method_t;

typedef enum micropixel_audio_capability {
    MICROPIXEL_AUDIO_CAPABILITY_OGG_OPUS = 1U << 0U,
    /* PCM_STREAM_* methods and the PCM_STREAM_LOW_WATER event are available. */
    MICROPIXEL_AUDIO_CAPABILITY_PCM_STREAM = 1U << 1U,
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
    /* Buffered frames dropped to or below the stream's low-water mark. Delivered
     * once per crossing; the next successful WRITE re-arms it. */
    MICROPIXEL_AUDIO_EVENT_PCM_STREAM_LOW_WATER = 2,
} micropixel_audio_event_id_t;

typedef enum micropixel_audio_pcm_stream_flag {
    /* No flags are defined yet; `flags` must be 0. Playback begins at the first WRITE. */
    MICROPIXEL_AUDIO_PCM_STREAM_NONE = 0U,
} micropixel_audio_pcm_stream_flag_t;

typedef struct micropixel_service_info {
    uint16_t size;
    uint16_t reserved0;
    uint32_t service_id;
    micropixel_service_handle_t service_handle;
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
    micropixel_timer_handle_t timer_handle;
    uint64_t initial_delay_us;
    uint64_t period_us;
} micropixel_timer_start_request_t;

/* GET and REMOVE: followed by key_length key bytes; `size` covers both. */
typedef struct micropixel_storage_key_request {
    uint16_t size;
    uint16_t key_length;
} micropixel_storage_key_request_t;

/* SET: followed by key_length key bytes and value_length opaque value bytes;
 * `size` covers all three. */
typedef struct micropixel_storage_set_request {
    uint16_t size;
    uint16_t key_length;
    uint32_t value_length;
} micropixel_storage_set_request_t;

typedef struct micropixel_font_info {
    uint16_t size;
    uint16_t reserved0;
    micropixel_font_handle_t font_handle;
    uint16_t font_size;
    uint16_t line_height;
    int16_t ascent;
    int16_t descent;
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
    uint16_t max_voices;
    uint32_t sample_rate;
    uint32_t capabilities;
    uint32_t supported_waveforms;
    uint32_t max_tone_duration_ms;
    uint16_t max_clips;
    uint16_t max_playbacks;
    /* 0 unless MICROPIXEL_AUDIO_CAPABILITY_PCM_STREAM is set. */
    uint16_t max_pcm_streams;
    uint16_t reserved0;
} micropixel_audio_info_t;

typedef struct micropixel_audio_tone {
    uint16_t size;
    uint16_t waveform;
    uint16_t volume_per_mille;
    uint16_t reserved0;
    uint32_t frequency_millihz;
    uint32_t duration_ms;
    uint16_t attack_ms;
    uint16_t release_ms;
} micropixel_audio_tone_t;

typedef struct micropixel_audio_clip_load_request {
    uint16_t size;
    uint16_t reserved0;
    uint32_t asset_id;
} micropixel_audio_clip_load_request_t;

typedef struct micropixel_audio_clip_info {
    uint16_t size;
    uint16_t reserved0;
    micropixel_audio_clip_handle_t clip_handle;
    uint32_t format;
} micropixel_audio_clip_info_t;

typedef struct micropixel_audio_playback_start_request {
    uint16_t size;
    uint16_t flags;
    micropixel_audio_clip_handle_t clip_handle;
    uint16_t volume_per_mille;
    uint16_t reserved0;
    uint32_t reserved1;
} micropixel_audio_playback_start_request_t;

typedef struct micropixel_audio_playback_volume_request {
    uint16_t size;
    uint16_t reserved0;
    micropixel_audio_playback_handle_t playback_handle;
    uint16_t volume_per_mille;
    uint16_t reserved1;
} micropixel_audio_playback_volume_request_t;

typedef struct micropixel_audio_playback_state_response {
    uint16_t size;
    uint16_t state;
    micropixel_audio_playback_handle_t playback_handle;
} micropixel_audio_playback_state_response_t;

typedef struct micropixel_audio_event_payload {
    micropixel_audio_playback_handle_t playback_handle;
    uint32_t reserved0[3];
} micropixel_audio_event_payload_t;

/* Guest PCM stream.
 * `sample_rate` must be the mix rate reported by GET_INFO or an integer divisor
 * of it; the Host upsamples by linear interpolation. `channels` is 1 or 2;
 * stereo is averaged down to the mono mixer. `capacity_frames` sizes the Host
 * ring buffer (PSRAM, rounded up to a power of two) and bounds how far ahead
 * the Guest can write;
 * `low_water_frames` < capacity_frames selects when PCM_STREAM_LOW_WATER fires
 * (0 leaves the event disabled). Underrun plays silence; the stream lives
 * until PCM_STREAM_CLOSE, STOP_ALL or session end and survives App suspend
 * with its buffered frames intact. */
typedef struct micropixel_audio_pcm_stream_open_request {
    uint16_t size;
    uint16_t volume_per_mille;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t flags;
    uint32_t capacity_frames;
    uint32_t low_water_frames;
} micropixel_audio_pcm_stream_open_request_t;

typedef struct micropixel_audio_pcm_stream_open_response {
    uint16_t size;
    uint16_t reserved0;
    micropixel_audio_pcm_stream_handle_t stream_handle;
    /* Actual ring capacity after Host clamping. */
    uint32_t capacity_frames;
    uint32_t reserved1;
} micropixel_audio_pcm_stream_open_response_t;

/* Followed by exactly frame_count * channels little-endian int16 samples;
 * `size` covers the header and the samples and the whole request is
 * <= MICROPIXEL_AUDIO_PCM_MAX_WRITE_BYTES. The Host copies
 * as many leading frames as fit and reports the count; the Guest re-sends the
 * remainder. */
typedef struct micropixel_audio_pcm_stream_write_request {
    uint16_t size;
    uint16_t reserved0;
    micropixel_audio_pcm_stream_handle_t stream_handle;
    uint32_t frame_count;
} micropixel_audio_pcm_stream_write_request_t;

typedef struct micropixel_audio_pcm_stream_write_response {
    uint16_t size;
    uint16_t reserved0;
    uint32_t accepted_frames;
    /* Free ring frames after this write, in the stream's own sample rate. */
    uint32_t free_frames;
} micropixel_audio_pcm_stream_write_response_t;

typedef struct micropixel_audio_pcm_event_payload {
    micropixel_audio_pcm_stream_handle_t stream_handle;
    uint32_t free_frames;
    uint32_t reserved0[2];
} micropixel_audio_pcm_event_payload_t;

typedef enum micropixel_pixel_format {
    /* Canonical byte order in Guest memory: B, G, R. */
    MICROPIXEL_PIXEL_FORMAT_BGR888 = 1,
    /* Canonical byte order in Guest memory: B, G, R, A. */
    MICROPIXEL_PIXEL_FORMAT_BGRA8888 = 2,
    /* Canonical Guest-memory layout: little-endian RGB565 uint16_t. */
    MICROPIXEL_PIXEL_FORMAT_RGB565 = 3,
} micropixel_pixel_format_t;

/* micropixel_texture_info_t.flags. */
typedef enum micropixel_texture_flag {
    /* Created by DYNAMIC_TEXTURE_CREATE; accepts DYNAMIC_TEXTURE_UPDATE. */
    MICROPIXEL_TEXTURE_FLAG_DYNAMIC = 1U << 0U,
} micropixel_texture_flag_t;

typedef enum micropixel_system_font_handle {
    MICROPIXEL_SYSTEM_FONT_SMALL = 1,
    MICROPIXEL_SYSTEM_FONT_MEDIUM = 2,
    MICROPIXEL_SYSTEM_FONT_LARGE = 3,
    MICROPIXEL_SYSTEM_FONT_TITLE = 4,
} micropixel_system_font_handle_t;

typedef struct micropixel_graphics_info {
    uint16_t size;
    uint16_t reserved0;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    /* Native-display pixel insets for the largest unobscured content area. */
    uint16_t safe_inset_top;
    uint16_t safe_inset_right;
    uint16_t safe_inset_bottom;
    uint16_t safe_inset_left;
    /* Panel-native Direct Surface format and flags. */
    uint32_t native_pixel_format;
    uint32_t native_flags;
    uint16_t max_full_frame_fps;
    /* Host capacity policy. Scene nodes, containers and batch instances are
     * bounded only by their uint16 wire ids and by Host memory: a submit that
     * cannot be stored fails with RESOURCE_EXHAUSTED and keeps the old scene.
     * A Direct Surface may have 1..max_surface_buffers buffers; a MEASURE_TEXT
     * or TEXT record carries at most max_text_bytes of UTF-8; max_scene_bytes
     * and max_raster_bytes bound one submit on each channel (max_raster_bytes
     * is 0 without CAP_RASTER). */
    uint16_t max_surface_buffers;
    uint16_t max_text_bytes;
    uint16_t reserved1;
    uint32_t max_scene_bytes;
    uint32_t max_raster_bytes;
} micropixel_graphics_info_t;

/* Followed by text_length UTF-8 bytes without a trailing NUL. */
typedef struct micropixel_graphics_measure_text_request {
    uint16_t size;
    uint16_t text_length;
    micropixel_font_handle_t font_handle;
} micropixel_graphics_measure_text_request_t;

typedef struct micropixel_text_metrics {
    uint16_t size;
    uint16_t reserved0;
    uint32_t width;
    uint32_t height;
    int32_t baseline;
} micropixel_text_metrics_t;

/* Service versions are negotiated by service_open; the header carries none.
 * Counts are bounded only by the uint16 ids and by node_count +
 * batch_instance_count <= 65535 (each is one Host draw operation);
 * container_count leaves id 0 for the root. Storage beyond that is Host
 * memory: a submit that does not fit fails with RESOURCE_EXHAUSTED. */
typedef struct micropixel_graphics_scene_header {
    uint32_t magic;
    uint16_t kind;
    uint16_t flags;
    uint32_t total_size;
    uint32_t generation;
    uint32_t base_revision;
    uint32_t revision;
    uint16_t record_count;
    uint16_t node_count;
    uint16_t container_count;
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

/* Container IDs are dense 1..container_count; 0 is the implicit Scene root.
 * An empty clip (clip_width == 0 && clip_height == 0) inherits the parent clip without adding one. */
typedef struct micropixel_graphics_scene_container_record {
    micropixel_graphics_scene_record_header_t record;
    uint16_t container_id;
    uint16_t parent_container_id;
    uint32_t property_mask;
    int32_t clip_x;
    int32_t clip_y;
    int32_t clip_width;
    int32_t clip_height;
    int32_t translate_x;
    int32_t translate_y;
    int16_t z_order;
    uint8_t opacity;
    uint8_t visible;
    uint16_t sibling_order;
    /* micropixel_graphics_scene_container_flag_t bits. */
    uint16_t flags;
} micropixel_graphics_scene_container_record_t;

/* One keyframe record is required for every drawable node. */
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
    uint8_t flags;
    uint8_t reserved0;
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

/* A zero stroke_width disables the stroke. */
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
    micropixel_texture_handle_t texture_handle;
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
    micropixel_font_handle_t font_handle;
    uint16_t text_length;
    uint16_t reserved0;
} micropixel_graphics_scene_text_record_t;

/* texture_handle == 0 selects untextured, per-instance RGB888 fills. */
typedef struct micropixel_graphics_scene_sprite_batch_record {
    micropixel_graphics_scene_node_header_t node;
    micropixel_texture_handle_t texture_handle;
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
    uint16_t max_touch_points;
    uint32_t capabilities;
    uint32_t logical_width;
    uint32_t logical_height;
    uint32_t reserved0;
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

/* Wire layout guards; one definition serves C and C++. */
#if defined(__cplusplus)
#define MICROPIXEL_ABI_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#define MICROPIXEL_ABI_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif

MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_event_t) == 48U, "micropixel_event_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_system_launch_arguments_response_t) == 552U,
                             "micropixel_system_launch_arguments_response_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_devices_list_request_t) == 8U,
                             "micropixel_devices_list_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_devices_list_response_t) == 76U,
                             "micropixel_devices_list_response_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_device_request_t) == 8U, "micropixel_device_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_device_info_t) == 72U, "micropixel_device_info_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_device_event_payload_t) == 16U,
                             "micropixel_device_event_payload_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_sensor_info_t) == 32U, "micropixel_sensor_info_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_sensor_open_request_t) == 8U,
                             "micropixel_sensor_open_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_sensor_open_response_t) == 16U,
                             "micropixel_sensor_open_response_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_sensor_reading_t) == 40U,
                             "micropixel_sensor_reading_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_sensor_sample_interval_request_t) == 16U,
                             "micropixel_sensor_sample_interval_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_sensor_event_payload_t) == 16U,
                             "micropixel_sensor_event_payload_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_gpio_info_t) == 28U, "micropixel_gpio_info_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_gpio_open_request_t) == 20U,
                             "micropixel_gpio_open_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_gpio_open_response_t) == 16U,
                             "micropixel_gpio_open_response_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_gpio_value_request_t) == 12U,
                             "micropixel_gpio_value_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_gpio_value_response_t) == 12U,
                             "micropixel_gpio_value_response_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_gpio_event_payload_t) == 16U,
                             "micropixel_gpio_event_payload_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_haptics_info_t) == 20U, "micropixel_haptics_info_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_haptics_play_request_t) == 16U,
                             "micropixel_haptics_play_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_power_info_t) == 24U, "micropixel_power_info_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_timer_event_payload_t) == 16U,
                             "micropixel_timer_event_payload_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_touch_event_payload_t) == 16U,
                             "micropixel_touch_event_payload_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_key_event_payload_t) == 16U,
                             "micropixel_key_event_payload_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(offsetof(micropixel_raster_texture_upload_request_t, texture_slot) == 2U,
                             "micropixel_raster_texture_upload_request_t.texture_slot ABI offset changed");
MICROPIXEL_ABI_STATIC_ASSERT(offsetof(micropixel_raster_texture_upload_request_t, width) == 4U,
                             "micropixel_raster_texture_upload_request_t.width ABI offset changed");
MICROPIXEL_ABI_STATIC_ASSERT(offsetof(micropixel_raster_texture_upload_request_t, pixels) == 12U,
                             "micropixel_raster_texture_upload_request_t.pixels ABI offset changed");
MICROPIXEL_ABI_STATIC_ASSERT(offsetof(micropixel_raster_column_t, texture_slot) == 2U,
                             "micropixel_raster_column_t.texture_slot ABI offset changed");
MICROPIXEL_ABI_STATIC_ASSERT(offsetof(micropixel_raster_column_t, light_level) == 3U,
                             "micropixel_raster_column_t.light_level ABI offset changed");
MICROPIXEL_ABI_STATIC_ASSERT(offsetof(micropixel_raster_span_pair_t, floor_texture_slot) == 2U,
                             "micropixel_raster_span_pair_t.floor_texture_slot ABI offset changed");
MICROPIXEL_ABI_STATIC_ASSERT(offsetof(micropixel_raster_span_pair_t, ceiling_texture_slot) == 3U,
                             "micropixel_raster_span_pair_t.ceiling_texture_slot ABI offset changed");
MICROPIXEL_ABI_STATIC_ASSERT(offsetof(micropixel_raster_sprite_t, texture_slot) == 2U,
                             "micropixel_raster_sprite_t.texture_slot ABI offset changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_graphics_info_t) == 48U, "micropixel_graphics_info_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_raster_texture_upload_request_t) == 20U,
                             "micropixel_raster_texture_upload_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_raster_palette_upload_request_t) == 16U,
                             "micropixel_raster_palette_upload_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_raster_header_t) == 24U, "micropixel_raster_header_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_raster_column_t) == 24U, "micropixel_raster_column_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_raster_span_pair_t) == 32U,
                             "micropixel_raster_span_pair_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_raster_sprite_t) == 24U, "micropixel_raster_sprite_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_raster_rect_t) == 16U, "micropixel_raster_rect_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_raster_warp_upload_request_t) == 20U,
                             "micropixel_raster_warp_upload_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_raster_warp_t) == 16U, "micropixel_raster_warp_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_raster_vertex_t) == 10U, "micropixel_raster_vertex_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_raster_triangle_t) == 36U,
                             "micropixel_raster_triangle_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_raster_quad_t) == 44U, "micropixel_raster_quad_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_raster_span_t) == 28U, "micropixel_raster_span_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(offsetof(micropixel_raster_span_t, s) == 12U, "micropixel_raster_span_t.s ABI offset changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_raster_text_t) == 16U, "micropixel_raster_text_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(offsetof(micropixel_raster_text_t, font_handle) == 12U,
                             "micropixel_raster_text_t.font_handle ABI offset changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_surface_create_request_t) == 24U,
                             "micropixel_surface_create_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_surface_create_response_t) == 20U,
                             "micropixel_surface_create_response_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_surface_present_request_t) == 36U,
                             "micropixel_surface_present_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_surface_event_payload_t) == 16U,
                             "micropixel_surface_event_payload_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_graphics_scene_header_t) == 32U,
                             "micropixel_graphics_scene_header_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_graphics_scene_record_header_t) == 4U,
                             "micropixel_graphics_scene_record_header_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_graphics_scene_background_record_t) == 12U,
                             "micropixel_graphics_scene_background_record_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_graphics_scene_container_record_t) == 44U,
                             "micropixel_graphics_scene_container_record_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_graphics_scene_node_link_record_t) == 12U,
                             "micropixel_graphics_scene_node_link_record_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_graphics_scene_node_header_t) == 12U,
                             "micropixel_graphics_scene_node_header_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_graphics_scene_rect_record_t) == 36U,
                             "micropixel_graphics_scene_rect_record_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_graphics_scene_rounded_rect_record_t) == 48U,
                             "micropixel_graphics_scene_rounded_rect_record_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_graphics_scene_texture_record_t) == 52U,
                             "micropixel_graphics_scene_texture_record_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_graphics_scene_text_record_t) == 32U,
                             "micropixel_graphics_scene_text_record_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_graphics_scene_sprite_batch_record_t) == 20U,
                             "micropixel_graphics_scene_sprite_batch_record_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_graphics_scene_sprite_instance_t) == 40U,
                             "micropixel_graphics_scene_sprite_instance_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_graphics_scene_batch_instances_record_t) == 16U,
                             "micropixel_graphics_scene_batch_instances_record_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_texture_info_t) == 32U, "micropixel_texture_info_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_input_info_t) == 20U, "micropixel_input_info_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_audio_info_t) == 28U, "micropixel_audio_info_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_audio_tone_t) == 20U, "micropixel_audio_tone_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_audio_clip_load_request_t) == 8U,
                             "micropixel_audio_clip_load_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_audio_clip_info_t) == 12U,
                             "micropixel_audio_clip_info_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_audio_playback_start_request_t) == 16U,
                             "micropixel_audio_playback_start_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_audio_playback_volume_request_t) == 12U,
                             "micropixel_audio_playback_volume_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_audio_playback_state_response_t) == 8U,
                             "micropixel_audio_playback_state_response_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_audio_event_payload_t) == 16U,
                             "micropixel_audio_event_payload_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_audio_pcm_stream_open_request_t) == 20U,
                             "micropixel_audio_pcm_stream_open_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_audio_pcm_stream_open_response_t) == 16U,
                             "micropixel_audio_pcm_stream_open_response_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_audio_pcm_stream_write_request_t) == 12U,
                             "micropixel_audio_pcm_stream_write_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_audio_pcm_stream_write_response_t) == 12U,
                             "micropixel_audio_pcm_stream_write_response_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_audio_pcm_event_payload_t) == 16U,
                             "micropixel_audio_pcm_event_payload_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_service_info_t) == 48U, "micropixel_service_info_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_handle_request_t) == 8U, "micropixel_handle_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_handle_response_t) == 8U,
                             "micropixel_handle_response_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_timer_start_request_t) == 24U,
                             "micropixel_timer_start_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_storage_key_request_t) == 4U,
                             "micropixel_storage_key_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_storage_set_request_t) == 8U,
                             "micropixel_storage_set_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_random_u32_response_t) == 8U,
                             "micropixel_random_u32_response_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_texture_load_request_t) == 16U,
                             "micropixel_texture_load_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_font_load_request_t) == 8U,
                             "micropixel_font_load_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_dynamic_texture_create_request_t) == 28U,
                             "micropixel_dynamic_texture_create_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_dynamic_texture_update_request_t) == 36U,
                             "micropixel_dynamic_texture_update_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_font_info_t) == 16U, "micropixel_font_info_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_graphics_measure_text_request_t) == 8U,
                             "micropixel_graphics_measure_text_request_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_raster_image_t) == 24U, "micropixel_raster_image_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_system_locale_response_t) == 36U,
                             "micropixel_system_locale_response_t ABI size changed");
MICROPIXEL_ABI_STATIC_ASSERT(sizeof(micropixel_text_metrics_t) == 16U, "micropixel_text_metrics_t ABI size changed");

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
int32_t micropixel_service_call(micropixel_service_handle_t service_handle, uint32_t method_id, const uint8_t* request,
                                uint32_t request_size, uint8_t* response, uint32_t response_capacity,
                                uint32_t* response_size_out);

MICROPIXEL_ABI_IMPORT("service_submit")
int32_t micropixel_service_submit(micropixel_service_handle_t service_handle, uint32_t channel_id, const uint8_t* bytes,
                                  uint32_t length);

#ifdef __cplusplus
}
#endif

#undef MICROPIXEL_ABI_IMPORT

#endif
