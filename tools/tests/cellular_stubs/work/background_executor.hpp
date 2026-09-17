#pragma once
#include <deque>
namespace micropixel::work {
class BackgroundExecutor {
   public:
    bool accepting = true;
    unsigned capacity = 1;
    bool Submit(void (*function)(void*), void* context) {
        if (!accepting || pending_.size() >= capacity) return false;
        pending_.push_back({function, context});
        return true;
    }
    void Shutdown() {
        while (!pending_.empty()) Run();
        accepting = false;
    }
    void Run() {
        if (pending_.empty()) return;
        const auto job = pending_.front();
        pending_.pop_front();
        job.function(job.context);
    }

   private:
    struct Job {
        void (*function)(void*);
        void* context;
    };
    std::deque<Job> pending_;
};
}  // namespace micropixel::work
