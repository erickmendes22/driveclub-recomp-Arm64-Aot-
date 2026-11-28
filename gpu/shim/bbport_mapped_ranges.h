// SPDX-License-Identifier: GPL-2.0-or-later
// GPU address coverage has two independent sources: real host mappings and
// partially resident texture apertures. A hole in the latter is still a valid
// sparse GPU address, but never becomes a CPU mapping. The caller owns locking.
#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <boost/icl/interval_set.hpp>

namespace BbGpu {
class MappedMemoryRanges {
public:
    using Address = std::uintptr_t;
    using Set = boost::icl::interval_set<Address>;
    using Aperture = std::pair<Address, std::uint64_t>;
    static constexpr unsigned ApertureCount = 3;

    const Set& Mapped() const { return mapped; }
    const Set& Physical() const { return physical; }
    Aperture Prt(unsigned id) const { return prt.at(id); }

    void Map(Address address, std::uint64_t size) {
        const auto range = Set::interval_type::right_open(address, address + size);
        physical += range;
        mapped += range;
    }
    void Unmap(Address address, std::uint64_t size) {
        const auto range = Set::interval_type::right_open(address, address + size);
        physical -= range;
        mapped -= range;
        AddPrtRanges();
    }
    void SetPrt(unsigned id, Address address, std::uint64_t size) {
        prt.at(id) = {address, size};
        Rebuild();
    }

private:
    void Rebuild() {
        mapped = physical;
        AddPrtRanges();
    }
    void AddPrtRanges() {
        for (const auto& [address, size] : prt) {
            if (size) mapped += Set::interval_type::right_open(address, address + size);
        }
    }
    Set physical, mapped;
    std::array<Aperture, ApertureCount> prt{};
};
} // namespace BbGpu
