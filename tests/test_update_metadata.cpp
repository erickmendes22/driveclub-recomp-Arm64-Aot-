#include "../launcher/prepare.h"
#include <cassert>
#include <cstring>

using Bytes = std::vector<uint8_t>;

// A two-entry PSF with TITLE_ID and a caller-selected version key.
static Bytes fixture(std::string title, std::string key, std::string version) {
    std::string keys("TITLE_ID\0", 9);
    keys += key;
    keys.push_back(0);
    title.push_back(0);
    version.push_back(0);
    Bytes data(52);
    std::memcpy(data.data(), "\0PSF", 4);
    auto put = [&](size_t pos, uint32_t value) {
        for (int i = 0; i < 4; ++i) data[pos + i] = uint8_t(value >> (8 * i));
    };
    put(4, 0x101);
    put(8, 52);
    put(12, 52 + keys.size());
    put(16, 2);
    put(20, 0x02040000);
    put(24, title.size());
    put(28, title.size());
    put(36, 0x02040009);
    put(40, version.size());
    put(44, version.size());
    put(48, title.size());
    for (const auto& part : {keys, title, version}) data.insert(data.end(), part.begin(), part.end());
    return data;
}

int main() {
    using M = UpdateMetadata;
    assert(InspectUpdateMetadata({}) == M::invalid);
    assert(InspectUpdateMetadata(fixture("CUSA00093", "APP_VER", "01.28")) == M::driveclub_128);
    assert(InspectUpdateMetadata(fixture("CUSA00003", "VERSION", "01.28")) == M::driveclub_128);
    assert(InspectUpdateMetadata(fixture("CUSA00003", "APP_VER", "01.27")) == M::other_version);
    assert(InspectUpdateMetadata(fixture("CUSA99999", "APP_VER", "01.28")) == M::other_title);
    auto truncated = fixture("CUSA00093", "APP_VER", "01.28");
    truncated.pop_back();
    assert(InspectUpdateMetadata(truncated) == M::invalid);
}
