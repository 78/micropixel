// Copyright 2025 Terrence
// SPDX-License-Identifier: Apache-2.0
// 78/uart-eth-modem: 0.4.2

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include "nt26_tx_pool.h"
#include "nt26_at_response.h"
#include "nt26_frame.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "uart_uhci.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "iot_eth.h"
#include "iot_eth_netif_glue.h"

/**
* @brief UART Ethernet Modem driver for 4G modules (EC801E, NT26K, etc.)
*
* This class encapsulates the UART-based Ethernet communication with 4G modules,
* providing a simple interface for network connectivity. It handles:
* - UART communication with frame protocol
* - AT command processing
* - Network state management
* - Integration with ESP-IDF's iot_eth and esp_netif
*
* MRDY/SRDY Protocol:
*   - MRDY (Master Busy): AP -> Modem, low = busy/working, high = idle/can sleep
*   - SRDY (Slave Busy): Modem -> AP, low = busy/has data, high = idle/can sleep
*   - After sending a frame, wait for receiver's 50us high pulse as ACK
*   - Idle timeout triggers PendingIdle state, then Idle when both are high
*
*/
class UartEthModem {
public:
    // Working state machine for low-power management
    enum class WorkingState {
        Idle,           // Both sides idle, DMA stopped
        PendingActive,  // Master wants to send, waiting for slave to wake up (SRDY low)
        Active,         // Active communication, DMA running
        PendingIdle,    // Master idle, waiting for slave to idle (SRDY high)
    };

    // MRDY signal levels
    enum class MrdyLevel {
        Low,
        High,
    };

    // Event types for the main task queue
    enum class EventType : uint8_t {
        None = 0,
        TxRequest,
        SrdyLow,
        SrdyHigh,
        RxData,
        Stop,
    };

    // Event structure for the unified event queue
    struct Event {
        EventType type;
        UartUhci::RxBuffer* rx_buffer;  // For RxData events
    };

    // Network event enumeration (aligned with NetworkModemEvent)
    enum class UartEthModemEvent {
        Connecting,              // Network is connecting/searching (LinkUp, Searching)
        Connected,               // Network connected successfully (Ready, got IP address)
        Disconnected,            // Network disconnected (LinkDown)
        InFlightMode,            // Flight mode initialized (modem/SIM info available, no network)
        ErrorNoSim,              // No SIM card detected
        ErrorRegistrationDenied, // Network registration denied (CEREG=3)
        ErrorInitFailed,         // Modem initialization failed (general error)
        ErrorNoCarrier,          // No carrier signal
        RequestingPdpContext,    // Driver is about to configure PDP; clients may
                                 // synchronously inject APN via SetPdpContext().
    };

    // Cell information from CEREG
    struct CellInfo {
        int stat = 0;
        std::string tac;
        std::string ci;
        int act = 0;
    };

    // Configuration
    struct Config {
        uart_port_t uart_num = UART_NUM_1;
        int baud_rate = 3000000;
        gpio_num_t tx_pin = GPIO_NUM_NC;
        gpio_num_t rx_pin = GPIO_NUM_NC;
        gpio_num_t mrdy_pin = GPIO_NUM_NC;   // Master Ready (DTR, low=busy)
        gpio_num_t srdy_pin = GPIO_NUM_NC;   // Slave Ready (RI, low=busy)
        size_t rx_buffer_count = 4;          // Number of DMA RX buffers
        size_t rx_buffer_size = 1600;        // Size of each DMA RX buffer
    };

    // Callback types
    using UartEthModemEventCallback = std::function<void(UartEthModemEvent event)>;

    explicit UartEthModem(const Config& config);
    ~UartEthModem();

    // Non-copyable
    UartEthModem(const UartEthModem&) = delete;
    UartEthModem& operator=(const UartEthModem&) = delete;

    /**
    * @brief Start the modem
    *
    * This will:
    * 1. Initialize UART and GPIO
    * 2. Create TX/RX tasks
    * 3. Run initialization sequence (AT commands)
    * 4. Establish handshake with modem
    * 5. Install iot_eth driver and create netif
    *
    * @param flight_mode If true, enter flight mode (AT+CFUN=4) instead of full mode,
    *                    only query modem/SIM info without network registration
    * @return ESP_OK on success
    */
    esp_err_t Start(bool flight_mode = false);

    /**
    * @brief Stop the modem
    *
    * This will stop all tasks and release resources.
    *
    * @return ESP_OK on success
    */
    esp_err_t Stop();

    /**
    * @brief Send AT command synchronously (thread-safe)
    *
    * @param cmd AT command string (without trailing \r)
    * @param response Output response string
    * @param timeout_ms Timeout in milliseconds
    * @return ESP_OK on success, ESP_ERR_TIMEOUT on timeout
    */
    esp_err_t SendAt(const std::string& cmd, std::string& response, uint32_t timeout_ms = 1000);

    /**
     * @brief Send AT command and keep collecting URCs until a marker appears.
     *
     * Used for commands like AT+ECPING that return OK first, then async URCs.
     *
     * @param done_marker Substring that marks completion (e.g. "+ECPING: DONE")
     */
    esp_err_t SendAtCollectUntil(const std::string& cmd, std::string& response,
                                 uint32_t timeout_ms,
                                 const char* done_marker);

    /**
     * @brief Check if network is initialized
     *
     * @return true if initialized, false otherwise
     */
    bool IsInitialized() const { return initialized_.load(); }

    /**
     * @brief Set callback for network event changes
     *
     * This callback is only triggered when network is initialized.
     * It reports network event changes.
     */
    void SetNetworkEventCallback(UartEthModemEventCallback callback);

    /**
     * @brief Enable or disable debug logging
     *
     * When debug is enabled, detailed debug information (previously ESP_LOGD)
     * will be displayed using ESP_LOGI.
     *
     * @param enabled true to enable debug logging, false to disable
     */
    void SetDebug(bool enabled);

    // Modem information getters
    std::string GetImei();
    std::string GetImsi();
    std::string GetIccid();
    std::string GetCarrierName();
    std::string GetModuleRevision();
    int GetSignalStrength();  // CSQ value (0-31, 99=unknown)
    CellInfo GetCellInfo();
    /**
     * @brief Set APN and PDP type for network registration.
     *
     * Call this either before Start(), or synchronously from the
     * RequestingPdpContext event callback (the driver fires that event right
     * before configuring PDP context, giving clients a chance to inject the
     * APN). Asynchronous callbacks dispatched to another task are too late to
     * influence the current init sequence; in that case use the pre-Start path.
     *
     * If APN is empty, PDP context configuration is skipped and the modem
     * default is used. Not thread-safe.
     */
    esp_err_t SetPdpContext(const std::string& apn, const std::string& pdp_type = "IP") {
        if (apn.size() > 127 || apn.find_first_of("\"\r\n") != std::string::npos ||
            apn.find('\0') != std::string::npos ||
            (pdp_type != "IP" && pdp_type != "IPV6" && pdp_type != "IPV4V6")) return ESP_ERR_INVALID_ARG;
        apn_ = apn;
        pdp_type_ = pdp_type;
        return ESP_OK;
    }

    /**
    * @brief Get the network interface
    *
    * Use this for network operations after network is ready.
    *
    * @return esp_netif_t pointer, or nullptr if not initialized
    */
    esp_netif_t* GetNetif() const { return eth_netif_; }

    /**
     * @brief Get network event name for logging
     */
    static const char* GetNetworkEventName(UartEthModemEvent event);

private:
    using FrameType = micropixel::nt26::FrameType;
    using FrameHeader = micropixel::nt26::FrameHeader;

    using TxFrame = micropixel::nt26::TxPool::Slot;
    micropixel::nt26::TxPool tx_pool_;
    std::mutex tx_mutex_;
    std::array<StaticSemaphore_t, micropixel::nt26::TxPool::kCapacity> tx_done_storage_{};
    std::array<SemaphoreHandle_t, micropixel::nt26::TxPool::kCapacity> tx_done_{};
    void CompleteTx(TxFrame* frame, esp_err_t result);
    esp_err_t SubmitFrame(const uint8_t* data, size_t length, FrameType type, bool wait);
    void RequestStop();
    esp_err_t SendAtCommon(const std::string& cmd, std::string& response,
                           uint32_t timeout_ms, std::string_view marker);

    // Initialization and cleanup
    esp_err_t InitUart();
    esp_err_t FailStart(esp_err_t error);
    esp_err_t InitGpio();
    esp_err_t InitIotEth();
    void DeinitUart();
    void DeinitGpio();
    bool gpio_isr_installed_ = false;
    void NotifyLink(bool up);
    void DeinitIotEth();

    // Task implementation (single task with UHCI DMA)
    void MainTaskRun();
    void InitTaskRun();
    void TxTaskRun();  // Dedicated TX task for non-blocking transmit

    // Event handling
    void HandleEvent(const Event& event);
    void HandleSrdyLow();
    void HandleSrdyHigh();
    void HandleRxData(UartUhci::RxBuffer* buffer);
    void HandleIdleTimeout();

    // Working state machine
    void EnterPendingActiveState();  // Master initiates wakeup, wait for slave
    void EnterActiveState();
    void EnterPendingIdleState();
    void EnterIdleState();
    TickType_t CalculateNextTimeout();

    // UHCI DMA callbacks (called from ISR context, must be in IRAM)
    static bool IRAM_ATTR UhciRxCallbackStatic(const UartUhci::RxEventData& data, void* user_data);

    // Frame processing
    esp_err_t SendFrame(const uint8_t* data, size_t length, FrameType type);
    esp_err_t EnqueueTxFrame(const uint8_t* buf, size_t len);
    void ProcessReceivedFrame(uint8_t* data, size_t size);
    void HandleEthFrame(uint8_t* data, size_t length);
    void HandleAtResponse(const char* data, size_t length);

    // Start next DMA receive
    void StartDmaReceive();

    // AT command helpers
    esp_err_t SendAtWithRetry(const std::string& cmd, std::string& response, uint32_t timeout_ms, int max_retries);
    void ParseAtResponse(std::string_view response);

    // Initialization sequence
    esp_err_t RunFlightModeInitSequence();
    esp_err_t RunNormalModeInitSequence();
    bool CheckSimCard();
    bool WaitForRegistration(uint32_t timeout_ms);
    void QueryModemInfo();
    esp_err_t AtDetect();
    esp_err_t ConfigurePdp();

    // GPIO control (low level = busy)
    void SetMrdy(MrdyLevel level);
    bool IsSrdyLow();
    void SendAckPulse();
    void ConfigureSrdyInterrupt(bool for_wakeup);
    static void IRAM_ATTR SrdyIsrHandler(void* arg);

    // State management
    void SetNetworkEvent(UartEthModemEvent event);

    // Resource cleanup
    void CleanupResources(bool cleanup_iot_eth = true);

    // IP event handler
    static void IpEventHandler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data);

    // Configuration
    Config config_;
    int detect_baud_rate_ = 0;

    // Network initialization flag (atomic for thread safety)
    std::atomic<bool> initialized_{false};

    // Network event (atomic for thread safety)
    std::atomic<UartEthModemEvent> network_event_{UartEthModemEvent::Disconnected};

    // Synchronization primitives
    QueueHandle_t event_queue_ = nullptr;
    std::mutex at_mutex_;
    EventGroupHandle_t event_group_ = nullptr;

    std::array<std::atomic<UartUhci::RxBuffer*>, 32> dropped_rx_{};

    // UHCI DMA
    UartUhci uart_uhci_;

    // Frame reassembly buffer for incomplete frames
    // 用于不完整帧的重组缓冲区
    static constexpr size_t kMaxFrameSize = 1600;
    uint8_t* reassembly_buffer_ = nullptr;
    size_t reassembly_size_ = 0;
    size_t reassembly_expected_ = 0;  // Expected total frame size (header + payload)

    // Task handles
    TaskHandle_t main_task_ = nullptr;
    TaskHandle_t init_task_ = nullptr;
    TaskHandle_t tx_task_ = nullptr;
    bool isr_barrier_pending_ = false;

    // TX queue for non-blocking transmit from LWIP
    QueueHandle_t tx_queue_ = nullptr;
    static constexpr size_t kTxQueueDepth = 32;  // Max pending TX frames

    // State flags (atomic for thread safety)
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> handshake_done_{false};
    std::atomic<bool> initializing_{false};
    std::atomic<uint8_t> seq_no_{0};
    std::atomic<bool> debug_enabled_{false};
    bool flight_mode_{false};  // Flight mode: only query modem info, no network registration

    // Working state machine
    std::atomic<WorkingState> working_state_{WorkingState::Idle};
    std::atomic<bool> mrdy_is_low_{false};
    std::atomic<int64_t> last_activity_time_us_{0};
    static constexpr int64_t kIdleTimeoutMs = 500;
    static constexpr int64_t kAckTimeoutMs = 100;
    static constexpr int64_t kAckPulseUs = 50;

    // Modem information (cached)
    std::string imei_;
    std::string iccid_;
    std::string imsi_;
    std::string carrier_name_;
    std::string module_revision_;
    std::string pdp_type_="IP";  // Default PDP type
    std::string apn_;
    int signal_strength_ = 99;
    CellInfo cell_info_;
    uint8_t mac_addr_[6] = {0};

    // AT response handling
    std::mutex response_mutex_;
    std::mutex info_mutex_;
    int RegistrationStatus();
    micropixel::nt26::AtResponse at_response_;
    bool waiting_for_at_response_ = false;

    // Callback
    UartEthModemEventCallback network_event_callback_;

    std::mutex netif_mutex_;
    bool netif_ready_ = false;
    bool link_up_ = false;

    // iot_eth related
    iot_eth_driver_t driver_{};
    iot_eth_handle_t eth_handle_ = nullptr;
    iot_eth_mediator_t* mediator_ = nullptr;
    iot_eth_netif_glue_handle_t glue_ = nullptr;
    esp_netif_t* eth_netif_ = nullptr;
    esp_event_handler_instance_t ip_event_handler_instance_ = nullptr;

    // Event bits
    static constexpr uint32_t kEventStart = (1 << 0);
    static constexpr uint32_t kEventHandshakeDone = (1 << 1);
    static constexpr uint32_t kEventStop = (1 << 2);
    static constexpr uint32_t kEventNetworkReady = (1 << 3);
    static constexpr uint32_t kEventAtResponse = (1 << 4);
    static constexpr uint32_t kEventInitDone = (1 << 5);
    static constexpr uint32_t kEventNetworkEventChanged = (1 << 6);
    static constexpr uint32_t kEventSrdyHigh = (1 << 7);
    static constexpr uint32_t kEventActiveState = (1 << 11);  // Set when entered active state with DMA ready

    static constexpr uint32_t kEventMainTaskDone = (1 << 8);
    static constexpr uint32_t kEventInitTaskDone = (1 << 10);
    static constexpr uint32_t kEventTxTaskDone = (1 << 12);
    static constexpr uint32_t kEventIsrDrained = (1 << 13);

    static constexpr uint32_t kEventAllTasksDone =
            kEventMainTaskDone | kEventInitTaskDone | kEventTxTaskDone;

    // Timing constants
    static constexpr uint32_t kHandshakeTimeoutMs = 5000;
    static constexpr uint32_t kModemSleepTimeoutS = 3;

    // SRDY interrupt configuration constants
    static constexpr bool kSrdyInterruptForWakeup = true;   // Low level trigger for light sleep wakeup
    static constexpr bool kSrdyInterruptForAck = false;    // Edge detection for SRDY high (ACK)

    // Handshake magic bytes
    static constexpr uint8_t kHandshakeRequest[] = {
            0x53, 0x50, 0x49, 0x43, 0x02, 0x04, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00};
    static constexpr uint8_t kHandshakeAck[] = {0x53, 0x50, 0x49, 0x43, 0x01,
                                                                                            0x80};

    static constexpr const char* kTag = "UartEthModem";
};
