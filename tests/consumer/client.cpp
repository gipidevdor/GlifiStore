#include "glifistore/client/client.hpp"

int main() {
    auto invalid = glifistore::client::Client::connect({.port = 0});
    if (!invalid && invalid.error().code == glifistore::ErrorCode::invalid_argument) {
        return 0;
    }
    return 1;
}
