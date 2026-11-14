// Standalone capability probe: a working LeakSanitizer must diagnose this intentional leak.
#include <cstddef>

int main() {
    constexpr std::size_t kProbeBytes = 4096;
    auto* leaked = new char[kProbeBytes];
    leaked[0] = 1;
    static_cast<void>(leaked);
    return 0;
}
