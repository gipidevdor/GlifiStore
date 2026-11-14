#include <cstddef>
#include <vector>

int main() {
    std::vector<std::byte> values;
    values.assign(8, std::byte{0});
    return values.size() == 8 ? 0 : 1;
}
