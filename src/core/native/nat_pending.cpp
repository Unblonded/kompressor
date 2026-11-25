// Handlers and codecs not written yet: they recognise nothing. Each moves to its own file when done.
#include "nat.hpp"

namespace kz::native {

#define KZ_PENDING(id)                                                                          \
    const Handler id = {#id, [](Stream&, const std::vector<uint8_t>&, const std::string&) { return false; }, \
                        [](Ctx&, StreamPtr) { return false; }};
KZ_PENDING(hPpmd)
#undef KZ_PENDING

}  // namespace kz::native

