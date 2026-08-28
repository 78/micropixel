#pragma once

#include <cstddef>

namespace micropixel::device {

using LocalControlCommandSink = void (*)(void* context, const char* command);
using LocalControlResponseSource = bool (*)(void* context, char* response, size_t capacity);

// Hardware-independent, line-framed development control transport. The
// platform owns the physical byte stream; Host code owns command semantics.
class LocalControlBackend {
   public:
    virtual ~LocalControlBackend() = default;

    virtual void Bind(LocalControlCommandSink command_sink, LocalControlResponseSource response_source,
                      void* context) = 0;
    virtual void Unbind(void* context) = 0;
    // May be called from a Host or timer task. Signal the transport owner that
    // a response is ready; never drain the source inline.
    virtual void NotifyResponseReady() = 0;

   protected:
    LocalControlBackend() = default;
};

}  // namespace micropixel::device
