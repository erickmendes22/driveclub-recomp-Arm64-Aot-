// PRT coverage must survive backing unmaps, while disabling an aperture must
// retain real mappings. Uses the same range model as the Vulkan rasterizer.
#include "../gpu/shim/bbport_mapped_ranges.h"
#include <cassert>
#include <cstdio>

using Ranges = BbGpu::MappedMemoryRanges;
static bool Covers(const Ranges::Set& set, std::uintptr_t address, std::uint64_t size) {
    return boost::icl::contains(set, Ranges::Set::interval_type::right_open(address, address + size));
}

int main() {
    constexpr std::uintptr_t base = 0x1000000000;
    constexpr std::uint64_t page = 16384;
    Ranges ranges;
    ranges.Map(base + page, 2 * page);
    ranges.SetPrt(0, base, 4 * page);
    assert(Covers(ranges.Mapped(), base, 4 * page));
    assert(!Covers(ranges.Physical(), base, 4 * page));
    assert(Covers(ranges.Physical(), base + page, 2 * page));
    // CPU fault ownership uses the faulting byte: aperture holes must never
    // be reported as host-backed pages that a readback could repair.
    assert(!Covers(ranges.Physical(), base + page - 1, 1));
    assert(Covers(ranges.Physical(), base + page, 1));
    assert(Covers(ranges.Physical(), base + 3 * page - 1, 1));
    assert(!Covers(ranges.Physical(), base + 3 * page, 1));

    ranges.Unmap(base + page, page);
    assert(Covers(ranges.Mapped(), base, 4 * page));
    assert(!Covers(ranges.Physical(), base + page, page));
    ranges.SetPrt(0, base, 0);
    assert(!Covers(ranges.Mapped(), base, 4 * page));
    assert(Covers(ranges.Mapped(), base + 2 * page, page));

    // Overlapping apertures have independent ownership; moving one cannot
    // remove the other or the physical page left inside their overlap.
    ranges.SetPrt(0, base, 3 * page);
    ranges.SetPrt(1, base + 2 * page, 3 * page);
    assert(Covers(ranges.Mapped(), base, 5 * page));
    ranges.SetPrt(0, base + 8 * page, page);
    assert(!Covers(ranges.Mapped(), base, page));
    assert(Covers(ranges.Mapped(), base + 2 * page, 3 * page));
    ranges.SetPrt(1, base + 2 * page, 0);
    assert(Covers(ranges.Mapped(), base + 2 * page, page));
    assert(!Covers(ranges.Mapped(), base + 3 * page, page));
    ranges.Unmap(base + 2 * page, page);
    ranges.SetPrt(0, base + 8 * page, 0);
    assert(ranges.Physical().empty() && ranges.Mapped().empty());
    std::puts("PASS: PRT/physical coverage, partial unmap, aperture replacement and overlapping slots");
}
