# GlifiStore C++ documentation {#glifistore_documentation}

<div class="glifi-intro">
A segmented, log-indexed, memory-first key-value engine in C++23.
Browse the API, follow the ownership model, and trace each contract to its specification.
</div>

> **Architectural prototype.** This reference describes the current source tree. It certifies
> neither release readiness nor platform durability; see the
> [quality gates](../production-readiness.md) and the
> [platform durability evidence](../architecture/platform-durability-evidence.md).

## Start with an interface

| Interface | Entry points | Contract |
| --- | --- | --- |
| Embedded C++ Store | glifistore::Store, glifistore::StoreConfig, glifistore::OwnedValue | [Public C++ API](../reference/cpp-api.md) |
| C++ TCP client | glifistore::client::Client, glifistore::client::ClientConfig, glifistore::client::MutationResult | [C++ client](../reference/cpp-client-api.md) |
| Errors and results | glifistore::Result, glifistore::Status, glifistore::ErrorCode | [Error taxonomy v1](../spec/error-taxonomy-v1.md) |
| C ABI | [glifistore.h](../../include/glifistore/abi/glifistore.h) | [C ABI v1](../spec/c-abi-v1.md) |

**Classes**, **Namespaces** and **Files** in the navigation lead to declarations, overloads,
template parameters, include dependencies and linked header source; search runs in the browser. The
header index also lists implementation components. Being listed here does not make a symbol part of
the supported or installed API: the contracts above define that boundary.

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
visibility, TTL, errors and lifecycle rules are in the [public C++ API](../reference/cpp-api.md);
a persistent Store needs an explicit configuration that follows that contract.

## Understand the implementation

- [Documentation map](../README.md): every specification, guide and record.
- [Architecture specification](../spec/architecture.md): components and the paired Reader–Writer
  runtime.
- [Code tour](../development/code-tour.md): the implementation by responsibility.
- [Concurrency and memory model](../spec/concurrency-memory-model.md): ownership and publication.
- [Persistence v1](../spec/persistence-v1.md) and [wire protocol v2](../spec/wire-protocol-v2.md):
  versioned formats and compatibility.
- [Documentation](../development/documentation.md): how these pages are written and built.

Specifications and accepted ADRs keep their authority over generated descriptions. Most
declarations are indexed automatically; detailed inline documentation is being added
incrementally, so read an unannotated declaration together with its contract.

Built with [Doxygen](https://www.doxygen.nl/) and
[Doxygen Awesome](https://jothepro.github.io/doxygen-awesome-css/); the theme's MIT license ships
with the site as `doxygen-awesome-LICENSE.txt`.
