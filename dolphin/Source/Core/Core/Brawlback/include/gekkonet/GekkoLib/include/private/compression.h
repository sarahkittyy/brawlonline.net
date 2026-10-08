#pragma once

#include "gekko_types.h"

#include <iostream>
#include <vector>

namespace Gekko {
    struct Compression {

        static void PrintArray(const uint8_t* data, u32 length) {
            for (u32 i = 0; i < length; i++) {
                std::cout << +data[i] << ' ';
            }
            std::cout << std::endl;
            std::cout << "_______________\n";
        }

        static std::vector<uint8_t> RLEEncode(const uint8_t* data, u32 length) {
            std::vector<uint8_t> result;

            u32 idx = 0;
            u8 count = 0;

            while (idx < length) {
                count = 1;
                while (count != UINT8_MAX && idx + 1 < length && data[idx] == data[idx + 1]) {
                    idx++;
                    count++;
                }
                result.push_back(count);
                result.push_back(data[idx]);
                idx++;
            }
            return result;
        }

        // max_out bounds the output (the input comes from the network and RLE expands up to
        // 127x); an input that would exceed it decodes to nothing.
        static std::vector<uint8_t> RLEDecode(const uint8_t* data, u32 length,
                                              u32 max_out = UINT32_MAX) {
            std::vector<uint8_t> result;

            u32 idx = 0;
            u8 count = 0;
            u8 value = 0;

            while (idx + 1 < length) {
                count = data[idx];
                value = data[idx + 1];

                if ((u64)result.size() + count > max_out) {
                    return {};
                }
                result.insert(result.end(), count, value);

                idx += 2;
            }

            return result;
        }

        static std::vector<uint8_t> DeltaEncode(const uint8_t* data, u32 length, u32 stride) {
            std::vector<uint8_t> result(length);
            for (u32 i = 0; i < length; i++) {
                result[i] = i < stride ? data[i] : (uint8_t)(data[i] ^ data[i - stride]);
            }
            return result;
        }

        static std::vector<uint8_t> DeltaDecode(const uint8_t* data, u32 length, u32 stride) {
            std::vector<uint8_t> result(length);
            for (u32 i = 0; i < length; i++) {
                result[i] = i < stride ? data[i] : (uint8_t)(data[i] ^ result[i - stride]);
            }
            return result;
        }

        Compression() = delete;
    };
}
