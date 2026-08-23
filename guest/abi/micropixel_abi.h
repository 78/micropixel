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
#define MICROPIXEL_GRAPHICS_INTERFACE_MINOR 1U
#define MICROPIXEL_GRAPHICS_COMMAND_MAGIC 0x4652474dU
#define MICROPIXEL_GRAPHICS_MAX_COMMAND_BYTES 4096U
#define MICROPIXEL_GRAPHICS_MAX_COMMANDS 128U
#define MICROPIXEL_GRAPHICS_MAX_FRAME_COMMAND_BYTES 16384U
#define MICROPIXEL_GRAPHICS_MAX_FRAME_COMMANDS 256U
#define MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES 128U
#define MICROPIXEL_RESOURCE_INTERFACE_MAJOR 1U
#define MICROPIXEL_RESOURCE_INTERFACE_MINOR 2U
#define MICROPIXEL_OFFSCREEN_SURFACE_MAX_UPDATE_BYTES 4096U
#define MICROPIXEL_AUDIO_INTERFACE_MAJOR 1U
#define MICROPIXEL_AUDIO_INTERFACE_MINOR 0U
#define MICROPIXEL_RANDOM_INTERFACE_MAJOR 1U
#define MICROPIXEL_RANDOM_INTERFACE_MINOR 0U
#define MICROPIXEL_AUDIO_MAX_TONE_DURATION_MS 5000U
#define MICROPIXEL_STORAGE_MAX_KEY_BYTES 15U
#define MICROPIXEL_STORAGE_MAX_VALUE_BYTES 4096U

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
} micropixel_status_t;

typedef uint64_t micropixel_app_time_t;
typedef uint32_t micropixel_service_handle_t;
typedef uint32_t micropixel_timer_handle_t;
typedef uint32_t micropixel_load_request_handle_t;
typedef uint32_t micropixel_bitmap_handle_t;

typedef enum micropixel_service_id {
    MICROPIXEL_SERVICE_TIMER = 1,
    MICROPIXEL_SERVICE_STORAGE = 2,
    MICROPIXEL_SERVICE_RESOURCE = 3,
    MICROPIXEL_SERVICE_RANDOM = 4,
    MICROPIXEL_SERVICE_GRAPHICS = 16,
    MICROPIXEL_SERVICE_INPUT = 17,
    MICROPIXEL_SERVICE_AUDIO = 18,
    MICROPIXEL_SERVICE_NETWORK = 19,
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
    MICROPIXEL_RESOURCE_METHOD_LOAD = 1,
    MICROPIXEL_RESOURCE_METHOD_CANCEL = 2,
    MICROPIXEL_RESOURCE_METHOD_BITMAP_GET_INFO = 3,
    MICROPIXEL_RESOURCE_METHOD_BITMAP_RELEASE = 4,
    MICROPIXEL_RESOURCE_METHOD_OFFSCREEN_SURFACE_CREATE = 5,
    MICROPIXEL_RESOURCE_METHOD_OFFSCREEN_SURFACE_UPDATE = 6,
    MICROPIXEL_RESOURCE_METHOD_OFFSCREEN_FRAME_BEGIN = 7,
    MICROPIXEL_RESOURCE_METHOD_OFFSCREEN_FRAME_COMMIT = 8,
} micropixel_resource_method_t;

typedef enum micropixel_random_method {
    MICROPIXEL_RANDOM_METHOD_GET_U32 = 1,
} micropixel_random_method_t;

typedef enum micropixel_graphics_method {
    MICROPIXEL_GRAPHICS_METHOD_GET_INFO = 1,
    MICROPIXEL_GRAPHICS_METHOD_FRAME_BEGIN = 2,
    MICROPIXEL_GRAPHICS_METHOD_FRAME_COMMIT = 3,
} micropixel_graphics_method_t;

typedef enum micropixel_graphics_channel {
    MICROPIXEL_GRAPHICS_CHANNEL_COMMANDS = 1,
} micropixel_graphics_channel_t;

typedef enum micropixel_input_method {
    MICROPIXEL_INPUT_METHOD_GET_INFO = 1,
} micropixel_input_method_t;

typedef enum micropixel_audio_method {
    MICROPIXEL_AUDIO_METHOD_GET_INFO = 1,
    MICROPIXEL_AUDIO_METHOD_PLAY_TONE = 2,
    MICROPIXEL_AUDIO_METHOD_STOP_ALL = 3,
} micropixel_audio_method_t;

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

typedef struct micropixel_resource_load_request {
    uint16_t size;
    uint16_t reserved0;
    uint32_t asset_id;
} micropixel_resource_load_request_t;

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
    uint32_t reserved[2];
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

typedef enum micropixel_pixel_format {
    /* Little-endian RGB888 storage: B, G, R bytes in memory. */
    MICROPIXEL_PIXEL_FORMAT_RGB888 = 1,
    /* Little-endian ARGB8888 storage: B, G, R, A bytes in memory. */
    MICROPIXEL_PIXEL_FORMAT_ARGB8888 = 2,
} micropixel_pixel_format_t;

typedef enum micropixel_bitmap_flag {
    MICROPIXEL_BITMAP_FLAG_MUTABLE = 1U << 0U,
} micropixel_bitmap_flag_t;

typedef struct micropixel_offscreen_surface_create_request {
    uint16_t size;
    uint16_t reserved0;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
} micropixel_offscreen_surface_create_request_t;

/* Followed by tightly packed native-format pixels for the dirty rectangle. */
typedef struct micropixel_offscreen_surface_update_request {
    uint16_t size;
    uint16_t reserved0;
    micropixel_bitmap_handle_t bitmap;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t reserved1;
} micropixel_offscreen_surface_update_request_t;

typedef enum micropixel_graphics_opcode {
    MICROPIXEL_GRAPHICS_OP_CLEAR = 1,
    MICROPIXEL_GRAPHICS_OP_FILL_RECT = 2,
    MICROPIXEL_GRAPHICS_OP_DRAW_TEXT = 3,
    MICROPIXEL_GRAPHICS_OP_DRAW_BITMAP = 4,
    /* x is the horizontal center; Host uses the selected font's real metrics. */
    MICROPIXEL_GRAPHICS_OP_DRAW_TEXT_CENTERED = 5,
    /*
     * Commands between BEGIN_SURFACE and END_SURFACE are retained below one
     * Host compositor surface.  While ACTIVE is set, the Host may cache that
     * surface and translate the cached image as one draw operation.
     */
    MICROPIXEL_GRAPHICS_OP_BEGIN_SURFACE = 6,
    MICROPIXEL_GRAPHICS_OP_END_SURFACE = 7,
    /* Blend one solid RGB color over the current target using opacity [0, 255]. */
    MICROPIXEL_GRAPHICS_OP_BLEND_RECT = 8,
    /* Blend Bitmap source alpha multiplied by one uniform draw opacity [0, 255]. */
    MICROPIXEL_GRAPHICS_OP_BLEND_BITMAP = 9,
} micropixel_graphics_opcode_t;

typedef enum micropixel_graphics_capability {
    MICROPIXEL_GRAPHICS_CAP_SURFACE_TRANSLATION = 1U << 0U,
    MICROPIXEL_GRAPHICS_CAP_MULTI_SUBMIT_FRAME = 1U << 1U,
} micropixel_graphics_capability_t;

typedef enum micropixel_graphics_surface_flag {
    MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION_ACTIVE = 1U << 0U,
} micropixel_graphics_surface_flag_t;

typedef struct micropixel_graphics_info {
    uint16_t size;
    uint16_t interface_major;
    uint16_t interface_minor;
    uint16_t max_frame_commands;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t capabilities;
    uint32_t max_command_bytes;
    uint16_t max_commands;
    uint16_t max_text_bytes;
} micropixel_graphics_info_t;

typedef struct micropixel_graphics_command_header {
    uint32_t magic;
    uint16_t interface_major;
    uint16_t interface_minor;
    uint32_t total_size;
    uint32_t command_count;
} micropixel_graphics_command_header_t;

typedef struct micropixel_graphics_record_header {
    uint16_t opcode;
    uint16_t size;
} micropixel_graphics_record_header_t;

typedef struct micropixel_graphics_clear_command {
    micropixel_graphics_record_header_t record;
    uint32_t rgb888;
} micropixel_graphics_clear_command_t;

typedef struct micropixel_graphics_fill_rect_command {
    micropixel_graphics_record_header_t record;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint32_t rgb888;
} micropixel_graphics_fill_rect_command_t;

typedef struct micropixel_graphics_blend_rect_command {
    micropixel_graphics_record_header_t record;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint32_t rgb888;
    uint8_t opacity;
    uint8_t reserved[3];
} micropixel_graphics_blend_rect_command_t;

typedef struct micropixel_graphics_draw_text_command {
    micropixel_graphics_record_header_t record;
    int32_t x;
    int32_t y;
    uint32_t rgb888;
    uint16_t font_size_px;
    uint16_t text_length;
} micropixel_graphics_draw_text_command_t;

typedef struct micropixel_graphics_draw_bitmap_command {
    micropixel_graphics_record_header_t record;
    micropixel_bitmap_handle_t bitmap;
    int32_t x;
    int32_t y;
    int32_t source_x;
    int32_t source_y;
    int32_t width;
    int32_t height;
} micropixel_graphics_draw_bitmap_command_t;

typedef struct micropixel_graphics_blend_bitmap_command {
    micropixel_graphics_record_header_t record;
    micropixel_bitmap_handle_t bitmap;
    int32_t x;
    int32_t y;
    int32_t source_x;
    int32_t source_y;
    int32_t width;
    int32_t height;
    uint8_t opacity;
    uint8_t reserved[3];
} micropixel_graphics_blend_bitmap_command_t;

typedef struct micropixel_graphics_begin_surface_command {
    micropixel_graphics_record_header_t record;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    int32_t translate_x;
    int32_t translate_y;
    uint32_t flags;
} micropixel_graphics_begin_surface_command_t;

typedef struct micropixel_graphics_end_surface_command {
    micropixel_graphics_record_header_t record;
} micropixel_graphics_end_surface_command_t;

typedef struct micropixel_bitmap_info {
    uint16_t size;
    uint16_t interface_major;
    uint16_t interface_minor;
    uint16_t reserved0;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format;
    uint32_t flags;
    uint32_t reserved1;
} micropixel_bitmap_info_t;

typedef enum micropixel_touch_phase {
    MICROPIXEL_TOUCH_DOWN = 1,
    MICROPIXEL_TOUCH_MOVE = 2,
    MICROPIXEL_TOUCH_UP = 3,
    MICROPIXEL_TOUCH_CANCEL = 4,
} micropixel_touch_phase_t;

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

typedef enum micropixel_resource_event_id {
    MICROPIXEL_RESOURCE_EVENT_READY = 1,
} micropixel_resource_event_id_t;

typedef enum micropixel_input_event_id {
    MICROPIXEL_INPUT_EVENT_TOUCH = 1,
} micropixel_input_event_id_t;

typedef struct micropixel_timer_event_payload {
    uint64_t elapsed_us;
    uint32_t missed_count;
    uint32_t reserved0;
} micropixel_timer_event_payload_t;

typedef struct micropixel_touch_event_payload {
    int32_t x;
    int32_t y;
    uint16_t pressure;
    uint16_t phase;
    uint32_t reserved0;
} micropixel_touch_event_payload_t;

typedef struct micropixel_resource_event_payload {
    micropixel_bitmap_handle_t bitmap;
    uint32_t reserved[3];
} micropixel_resource_event_payload_t;

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
static_assert(sizeof(micropixel_timer_event_payload_t) == 16U, "micropixel_timer_event_payload_t ABI size changed");
static_assert(sizeof(micropixel_touch_event_payload_t) == 16U, "micropixel_touch_event_payload_t ABI size changed");
static_assert(sizeof(micropixel_resource_event_payload_t) == 16U,
              "micropixel_resource_event_payload_t ABI size changed");
static_assert(sizeof(micropixel_graphics_info_t) == 32U, "micropixel_graphics_info_t ABI size changed");
static_assert(sizeof(micropixel_graphics_command_header_t) == 16U,
              "micropixel_graphics_command_header_t ABI size changed");
static_assert(sizeof(micropixel_graphics_fill_rect_command_t) == 24U,
              "micropixel_graphics_fill_rect_command_t ABI size changed");
static_assert(sizeof(micropixel_graphics_blend_rect_command_t) == 28U,
              "micropixel_graphics_blend_rect_command_t ABI size changed");
static_assert(sizeof(micropixel_graphics_draw_text_command_t) == 20U,
              "micropixel_graphics_draw_text_command_t ABI size changed");
static_assert(sizeof(micropixel_graphics_draw_bitmap_command_t) == 32U,
              "micropixel_graphics_draw_bitmap_command_t ABI size changed");
static_assert(sizeof(micropixel_graphics_blend_bitmap_command_t) == 36U,
              "micropixel_graphics_blend_bitmap_command_t ABI size changed");
static_assert(sizeof(micropixel_graphics_begin_surface_command_t) == 32U,
              "micropixel_graphics_begin_surface_command_t ABI size changed");
static_assert(sizeof(micropixel_graphics_end_surface_command_t) == 4U,
              "micropixel_graphics_end_surface_command_t ABI size changed");
static_assert(sizeof(micropixel_bitmap_info_t) == 32U, "micropixel_bitmap_info_t ABI size changed");
static_assert(sizeof(micropixel_offscreen_surface_create_request_t) == 16U,
              "micropixel_offscreen_surface_create_request_t ABI size changed");
static_assert(sizeof(micropixel_offscreen_surface_update_request_t) == 32U,
              "micropixel_offscreen_surface_update_request_t ABI size changed");
static_assert(sizeof(micropixel_input_info_t) == 32U, "micropixel_input_info_t ABI size changed");
static_assert(sizeof(micropixel_audio_info_t) == 32U, "micropixel_audio_info_t ABI size changed");
static_assert(sizeof(micropixel_audio_tone_t) == 32U, "micropixel_audio_tone_t ABI size changed");
static_assert(sizeof(micropixel_service_info_t) == 48U, "micropixel_service_info_t ABI size changed");
static_assert(sizeof(micropixel_handle_request_t) == 8U, "micropixel_handle_request_t ABI size changed");
static_assert(sizeof(micropixel_handle_response_t) == 8U, "micropixel_handle_response_t ABI size changed");
static_assert(sizeof(micropixel_timer_start_request_t) == 24U, "micropixel_timer_start_request_t ABI size changed");
static_assert(sizeof(micropixel_storage_key_request_t) == 20U, "micropixel_storage_key_request_t ABI size changed");
static_assert(sizeof(micropixel_storage_set_request_t) == 8U, "micropixel_storage_set_request_t ABI size changed");
static_assert(sizeof(micropixel_resource_load_request_t) == 8U, "micropixel_resource_load_request_t ABI size changed");
static_assert(sizeof(micropixel_random_u32_response_t) == 8U, "micropixel_random_u32_response_t ABI size changed");
#else
_Static_assert(sizeof(micropixel_event_t) == 48U, "micropixel_event_t ABI size changed");
_Static_assert(sizeof(micropixel_timer_event_payload_t) == 16U, "micropixel_timer_event_payload_t ABI size changed");
_Static_assert(sizeof(micropixel_touch_event_payload_t) == 16U, "micropixel_touch_event_payload_t ABI size changed");
_Static_assert(sizeof(micropixel_resource_event_payload_t) == 16U,
               "micropixel_resource_event_payload_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_info_t) == 32U, "micropixel_graphics_info_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_command_header_t) == 16U,
               "micropixel_graphics_command_header_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_fill_rect_command_t) == 24U,
               "micropixel_graphics_fill_rect_command_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_blend_rect_command_t) == 28U,
               "micropixel_graphics_blend_rect_command_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_draw_text_command_t) == 20U,
               "micropixel_graphics_draw_text_command_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_draw_bitmap_command_t) == 32U,
               "micropixel_graphics_draw_bitmap_command_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_blend_bitmap_command_t) == 36U,
               "micropixel_graphics_blend_bitmap_command_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_begin_surface_command_t) == 32U,
               "micropixel_graphics_begin_surface_command_t ABI size changed");
_Static_assert(sizeof(micropixel_graphics_end_surface_command_t) == 4U,
               "micropixel_graphics_end_surface_command_t ABI size changed");
_Static_assert(sizeof(micropixel_bitmap_info_t) == 32U, "micropixel_bitmap_info_t ABI size changed");
_Static_assert(sizeof(micropixel_offscreen_surface_create_request_t) == 16U,
               "micropixel_offscreen_surface_create_request_t ABI size changed");
_Static_assert(sizeof(micropixel_offscreen_surface_update_request_t) == 32U,
               "micropixel_offscreen_surface_update_request_t ABI size changed");
_Static_assert(sizeof(micropixel_input_info_t) == 32U, "micropixel_input_info_t ABI size changed");
_Static_assert(sizeof(micropixel_audio_info_t) == 32U, "micropixel_audio_info_t ABI size changed");
_Static_assert(sizeof(micropixel_audio_tone_t) == 32U, "micropixel_audio_tone_t ABI size changed");
_Static_assert(sizeof(micropixel_service_info_t) == 48U, "micropixel_service_info_t ABI size changed");
_Static_assert(sizeof(micropixel_handle_request_t) == 8U, "micropixel_handle_request_t ABI size changed");
_Static_assert(sizeof(micropixel_handle_response_t) == 8U, "micropixel_handle_response_t ABI size changed");
_Static_assert(sizeof(micropixel_timer_start_request_t) == 24U, "micropixel_timer_start_request_t ABI size changed");
_Static_assert(sizeof(micropixel_storage_key_request_t) == 20U, "micropixel_storage_key_request_t ABI size changed");
_Static_assert(sizeof(micropixel_storage_set_request_t) == 8U, "micropixel_storage_set_request_t ABI size changed");
_Static_assert(sizeof(micropixel_resource_load_request_t) == 8U, "micropixel_resource_load_request_t ABI size changed");
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
