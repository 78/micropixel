#pragma once
namespace micropixel::work {
class BackgroundExecutor {
   public:
    bool accepting = true;
    bool Submit(void (*function)(void*), void* context) {
        if (!accepting || pending_) return false;
        pending_ = function;
        context_ = context;
        return true;
    }
    void Run() {
        auto function = pending_;
        pending_ = nullptr;
        if (function) function(context_);
    }

   private:
    void (*pending_)(void*){};
    void* context_{};
};
}  // namespace micropixel::work
