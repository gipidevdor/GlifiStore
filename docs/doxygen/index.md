# GlifiStore C++ documentation {#glifistore_documentation}

<div class="glifi-intro">
A segmented, log-indexed, memory-first key-value engine in C++23.
Browse the API, follow the ownership model, and trace each contract to its specification.
</div>

> **Architectural prototype.** Generated API documentation describes the current source tree;
> it does not certify release readiness or platform durability. See the
> [quality gates](../production-readiness.md) and
> [platform durability evidence](../architecture/platform-durability-evidence.md).

## Start with an API

| Interface | Entry points | Contract |
| --- | --- | --- |
| Embedded C++ Store | glifistore::Store, glifistore::StoreConfig, glifistore::OwnedValue | [Public C++ API](../reference/cpp-api.md) |
| C++ TCP client | glifistore::client::Client, glifistore::client::ClientConfig, glifistore::client::MutationResult | [C++ client API](../reference/cpp-client-api.md) |
| Errors and results | glifistore::Result, glifistore::Status, glifistore::ErrorCode | [Error taxonomy v1](../spec/error-taxonomy-v1.md) |
| C ABI | [glifistore.h](../../include/glifistore/abi/glifistore.h) | [C ABI v1](../spec/c-abi-v1.md) |

Use the **Classes**, **Namespaces**, and **Files** navigation entries to explore declarations,
overloads, template parameters, include dependencies, and linked header source. Search works
entirely in the browser. The header index also includes implementation components; presence here
does not make a symbol part of the supported or installed API. The reference contracts above
define that boundary.

## Embed a volatile Store

```cpp
#include <glifistore/store/store.hpp>

#include <cstddef>
#include <span>
#include <string_view>
#include <utility>

int main() {
    auto opened = glifistore::Store::open();
    if (!opened) {
        return 1;
    }
    auto store = std::move(opened).value();
    const std::string_view payload = "hello";
    const auto bytes = std::as_bytes(std::span{payload.data(), payload.size()});
    if (!store->put("greeting", bytes)) {
        return 1;
    }
    auto value = store->get("greeting");
    if (!value) {
        return 1;
    }
    return value->bytes.size() == payload.size() ? 0 : 1;
}
```

`Store::get()` returns an owning value. Durability policies, acknowledgement points, batch
visibility, TTL, errors, and lifecycle rules are specified in the
[C++ API contract](../reference/cpp-api.md); choose a persistent configuration explicitly when
following that contract.

## Understand the implementation

- [Documentation map](../README.md): specifications, architecture, operations, and assurance.
- [Architecture specification](../spec/architecture.md): components and the official paired
  Reader–Writer runtime.
- [Code tour](../development/code-tour.md): navigate the implementation by responsibility.
- [Concurrency and memory model](../spec/concurrency-memory-model.md): ownership and publication.
- [Persistence v1](../spec/persistence-v1.md) and [wire protocol v2](../spec/wire-protocol-v2.md):
  versioned formats and compatibility.
- [Build and publish this documentation](../development/documentation.md).

Specifications and accepted ADRs retain their authority over generated descriptions. Most
declarations are indexed automatically; detailed inline documentation is being added
incrementally. Unannotated declarations should be read together with their reference contract.

The site uses [Doxygen](https://www.doxygen.nl/) and
[Doxygen Awesome](https://jothepro.github.io/doxygen-awesome-css/). The theme's MIT license is
included in the generated site as `doxygen-awesome-LICENSE.txt`.
