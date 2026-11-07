#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/server/socket.hpp"

namespace glifistore::server {

class Wakeup final {
  public:
    [[nodiscard]] static auto create() -> Result<Wakeup>;

    [[nodiscard]] auto descriptor() const noexcept -> int {
        return reader_.descriptor();
    }

    [[nodiscard]] auto notify() const -> Status;
    [[nodiscard]] auto drain() const -> Status;

  private:
    Wakeup(SocketHandle reader, SocketHandle writer) noexcept
        : reader_(std::move(reader)), writer_(std::move(writer)) {}

    SocketHandle reader_;
    SocketHandle writer_;
};

} // namespace glifistore::server
