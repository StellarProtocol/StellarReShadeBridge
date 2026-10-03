// Stellar ReShade bridge — request queue and snapshot types.
//
// Two small hand-offs between threads:
//   * RequestQueue: any thread (the framework) -> render thread. Callers push; the reshade_present callback swaps the
//     whole queue out under the lock and applies it afterwards, outside the lock.
//   * SnapshotStore: render thread -> any thread. The reshade_present callback rebuilds the Snapshot in a private buffer
//     when something changed and swaps it in under the lock (otherwise it only advances the frame counter); readers copy
//     only the fields they need under the lock.
// Locks are held only to swap or copy small data. Nothing here calls into ReShade or into the caller.
#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace stellar_rsb
{
    enum class RequestKind
    {
        effects_enabled, // on = global effects on/off
        technique,       // effect = effect file name ("" = any), text = technique name, on = enabled, save = persist
        preset,          // text = preset path
        search_paths,    // text = effect search paths, text2 = texture search paths (';'-separated, either may be empty)
    };

    struct Request
    {
        RequestKind kind = RequestKind::effects_enabled;
        std::string effect;
        std::string text;
        std::string text2;
        bool on = false;
        bool save = false;
    };

    enum class PushResult
    {
        queued,
        dropped_first, // the queue is full; first drop since it last drained (worth one log line)
        dropped,       // the queue is full; already reported
    };

    class RequestQueue
    {
    public:
        // Upper bound on queued requests so a caller that keeps pushing while ReShade never finishes loading cannot grow
        // memory without limit. Requests beyond it are dropped.
        static constexpr std::size_t max_pending = 4096;

        PushResult push(Request &&request)
        {
            std::lock_guard<std::mutex> lock(_lock);
            if (_pending.size() >= max_pending)
            {
                if (_overflow_reported)
                    return PushResult::dropped;
                _overflow_reported = true;
                return PushResult::dropped_first;
            }
            _pending.push_back(std::move(request));
            return PushResult::queued;
        }

        // Moves every pending request into `out` (which must be empty), preserving request order.
        void take_all(std::vector<Request> &out)
        {
            std::lock_guard<std::mutex> lock(_lock);
            out.swap(_pending);
            _overflow_reported = false;
        }

        // Puts `requests[from..]` back at the FRONT of the queue (ahead of anything pushed meanwhile), keeping their order.
        // The cap still holds: if the queue would exceed it, the NEWEST requests (at the back) are dropped. Returns how
        // many were dropped.
        std::size_t put_back_front(std::vector<Request> &requests, std::size_t from)
        {
            if (from >= requests.size())
                return 0;
            std::lock_guard<std::mutex> lock(_lock);
            _pending.insert(_pending.begin(), std::make_move_iterator(requests.begin() + static_cast<std::ptrdiff_t>(from)),
                std::make_move_iterator(requests.end()));
            if (_pending.size() <= max_pending)
                return 0;
            const std::size_t dropped = _pending.size() - max_pending;
            _pending.erase(_pending.begin() + static_cast<std::ptrdiff_t>(max_pending), _pending.end());
            return dropped;
        }

    private:
        std::mutex _lock;
        std::vector<Request> _pending;
        bool _overflow_reported = false;
    };

    struct TechniqueState
    {
        std::string name;
        std::string effect; // effect file name, e.g. "Clarity.fx"
        bool enabled = false;
    };

    struct Snapshot
    {
        std::vector<TechniqueState> techniques;
        std::size_t technique_count = 0; // valid entries in `techniques` (the vector is reused, so it may be longer)
        std::string preset_path;
        bool effects_enabled = false;
        bool loading = true;
    };

    // Copies `src` into `dst` (capacity `dst_len` bytes, NUL-terminated, truncated if needed).
    // Returns the full length of `src` in bytes (excluding the NUL), so a caller can detect truncation.
    inline int copy_out(const std::string &src, char *dst, int dst_len)
    {
        if (dst != nullptr && dst_len > 0)
        {
            const std::size_t n = src.size() < static_cast<std::size_t>(dst_len - 1) ? src.size() : static_cast<std::size_t>(dst_len - 1);
            std::memcpy(dst, src.data(), n);
            dst[n] = '\0';
        }
        return static_cast<int>(src.size());
    }

    class SnapshotStore
    {
    public:
        // Render thread: publishes `fresh` and hands the previous snapshot back in `fresh` for reuse as the next buffer.
        void publish(Snapshot &fresh)
        {
            std::lock_guard<std::mutex> lock(_lock);
            std::swap(_current, fresh);
            _frames++;
        }

        // Render thread: nothing changed this frame; the published snapshot is still current as of this frame.
        void tick()
        {
            std::lock_guard<std::mutex> lock(_lock);
            _frames++;
        }

        void reset()
        {
            std::lock_guard<std::mutex> lock(_lock);
            _current.technique_count = 0;
            _current.preset_path.clear();
            _current.effects_enabled = false;
            _current.loading = true;
        }

        // Frames counted so far, clamped to INT_MAX for the int ABI.
        int frames()
        {
            std::lock_guard<std::mutex> lock(_lock);
            return _frames > INT_MAX ? INT_MAX : static_cast<int>(_frames);
        }

        int loading()
        {
            std::lock_guard<std::mutex> lock(_lock);
            return _current.loading ? 1 : 0;
        }

        int effects_enabled()
        {
            std::lock_guard<std::mutex> lock(_lock);
            return _current.effects_enabled ? 1 : 0;
        }

        int technique_count()
        {
            std::lock_guard<std::mutex> lock(_lock);
            return static_cast<int>(_current.technique_count);
        }

        // Returns 1 and fills the buffers, or 0 if `index` is out of range.
        int technique_at(int index, char *name, int name_len, char *effect, int effect_len, int *enabled)
        {
            std::lock_guard<std::mutex> lock(_lock);
            if (index < 0 || static_cast<std::size_t>(index) >= _current.technique_count)
                return 0;
            const TechniqueState &t = _current.techniques[static_cast<std::size_t>(index)];
            copy_out(t.name, name, name_len);
            copy_out(t.effect, effect, effect_len);
            if (enabled != nullptr)
                *enabled = t.enabled ? 1 : 0;
            return 1;
        }

        int preset(char *buf, int len)
        {
            std::lock_guard<std::mutex> lock(_lock);
            return copy_out(_current.preset_path, buf, len);
        }

    private:
        std::mutex _lock;
        Snapshot _current;
        std::int64_t _frames = 0;
    };
}
