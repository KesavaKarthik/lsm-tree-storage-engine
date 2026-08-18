#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "kvstore/record.hpp"

namespace kvstore::testing_support {

// Builds a record header by hand, bypassing encode(), so a test can supply size
// fields no honest writer would ever produce. This is the input class that
// matters most for a storage engine: key_size and value_size come off disk
// unvalidated and are then used to size reads, so a corrupt one is an
// attacker-or-accident-controlled length.
//
// The crc is left zero -- these headers exist to be rejected on their shape,
// before the crc is ever consulted.
inline std::vector<std::uint8_t> make_raw_header(std::uint32_t key_size,
                                                 std::uint32_t value_size,
                                                 std::uint8_t flags = 0) {
    std::vector<std::uint8_t> header(record::kHeaderSize, 0);
    header[12] = flags;
    for (std::size_t i = 0; i < 4; ++i) {
        header[13 + i] = static_cast<std::uint8_t>(key_size >> (8 * i));
        header[17 + i] = static_cast<std::uint8_t>(value_size >> (8 * i));
    }
    return header;
}

}  // namespace kvstore::testing_support
