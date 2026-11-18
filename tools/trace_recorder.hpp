#ifndef MGMEM_TRACE_RECORDER_HPP
#define MGMEM_TRACE_RECORDER_HPP

// Optional consumer-side trace recorder for tools/shadow_estimate.py.
//
// Record the allocation *requests* at the API layer you own (the
// provider hook cannot see arena sub-allocations). The recorder only
// writes JSONL rows; it never allocates through Memory itself, holds
// no Memory handles and must be flushed/closed by the caller outside
// any measured region if timings matter. Thread safety: one mutex
// serializes rows; ids are handed out in call order.
//
// Example:
//   mgmem_trace::Recorder trace("site.jsonl");
//   const auto id = trace.alloc(bytes, alignment);
//   ... allocate and use the memory ...
//   trace.free(id);          // when the owner is released
//   trace.reset();           // at an arena reset / epoch boundary
//   trace.close();           // before reading the file back
//
// Feed the file to: python tools/shadow_estimate.py --trace site.jsonl
//
// Failure handling: stream exceptions stay disabled, so a failed open
// or write never throws into the measured path; it shows up only as
// good()==false. Check good() after construction and after close();
// rows are not durable until flush()/close() (destruction also closes
// the stream, silently).

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

namespace mgmem_trace {

class Recorder {
public:
    explicit Recorder(const std::string &path)
        : out_(path, std::ios::out | std::ios::trunc) {}

    Recorder(const Recorder &) = delete;
    Recorder &operator=(const Recorder &) = delete;

    bool good() const { return static_cast<bool>(out_); }

    std::uint64_t alloc(std::uint64_t bytes, std::uint64_t alignment) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::uint64_t id = ++next_id_;
        out_ << "{\"op\":\"alloc\",\"id\":" << id << ",\"bytes\":" << bytes
             << ",\"alignment\":" << alignment << "}\n";
        return id;
    }

    void free(std::uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        out_ << "{\"op\":\"free\",\"id\":" << id << "}\n";
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        out_ << "{\"op\":\"reset\"}\n";
        next_id_ = 0;
    }

    void flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        out_.flush();
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        out_.flush();
        out_.close();
    }

private:
    std::ofstream out_;
    std::mutex mutex_;
    std::uint64_t next_id_ = 0;
};

} // namespace mgmem_trace

#endif
