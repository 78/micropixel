// Copyright 2025 Terrence
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace micropixel::nt26 {
    // Frame type
    enum class FrameType : uint8_t {
        kEthernet = 0,
        kAtCommand = 1
    };

    // Frame header structure (4 bytes, compatible with original protocol)
    // Layout:
    //   Byte 0: payload_length[7:0]
    //   Byte 1: seq_no[7:4], payload_length[11:8]
    //   Byte 2: reserved[7:4], type[3:2], continue[1], flow_control[0]
    //   Byte 3: checksum
    struct FrameHeader {
        uint8_t raw[4];

        uint16_t GetPayloadLength() const {
            return raw[0] | ((raw[1] & 0x0F) << 8);
        }
        void SetPayloadLength(uint16_t len) {
            raw[0] = len & 0xFF;
            raw[1] = (raw[1] & 0xF0) | ((len >> 8) & 0x0F);
        }

        uint8_t GetSequence() const { return (raw[1] >> 4) & 0x0F; }
        void SetSequence(uint8_t seq) {
            raw[1] = (raw[1] & 0x0F) | ((seq & 0x0F) << 4);
        }

        // Flow control: 0 = XON (permit to send), 1 = XOFF (shall not send)
        bool GetFlowControl() const { return raw[2] & 0x01; }
        void SetFlowControl(bool xoff) {
            raw[2] = (raw[2] & 0xFE) | (xoff ? 1 : 0);
        }

        bool GetContinue() const { return (raw[2] >> 1) & 0x01; }
        void SetContinue(bool cont) {
            raw[2] = (raw[2] & 0xFD) | ((cont ? 1 : 0) << 1);
        }

        FrameType GetType() const {
            return static_cast<FrameType>((raw[2] >> 2) & 0x03);
        }
        void SetType(FrameType type) {
            raw[2] = (raw[2] & 0xF3) | ((static_cast<uint8_t>(type) & 0x03) << 2);
        }

        uint8_t GetChecksum() const { return raw[3]; }
        void SetChecksum(uint8_t sum) { raw[3] = sum; }

        uint8_t CalculateChecksum() const {
            uint32_t sum = raw[0] + raw[1] + raw[2];
            return static_cast<uint8_t>((sum >> 8) ^ sum ^ 0x03);
        }

        bool ValidateChecksum() const {
            return GetChecksum() == CalculateChecksum();
        }
        void UpdateChecksum() { SetChecksum(CalculateChecksum()); }
    } __attribute__((packed));

    static_assert(sizeof(FrameHeader) == 4, "FrameHeader must be 4 bytes");


}  // namespace micropixel::nt26
