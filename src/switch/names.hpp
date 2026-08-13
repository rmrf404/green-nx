#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../core/catalog.hpp"

namespace gnx {

// Resolves display names and box art for catalog entries on demand.
//
// The playable library is resolved up front at load time, but search reaches
// the whole ~2400-title catalog and resolving all of that would pull ~130 MB
// of store metadata. So the entries the user is actually looking at get
// resolved here instead, a batch at a time and off the UI thread: request()
// queues an id, pump() copies whatever came back into the library. Same shape
// as Covers, which does this for the images.
class Names {
public:
    Names();
    ~Names();

    // Queue a title whose name is still unknown. Cheap to call every frame:
    // ids already queued (or already looked up, hit or miss) are ignored.
    void request(const Game& game);

    // Main thread: apply finished lookups to games. True if anything changed,
    // which is the caller's cue to re-sort or re-filter.
    bool pump(std::vector<Game>& games);

    // A lookup is in flight — the UI shows this as a "resolving names" note.
    bool busy() const { return busy_; }

    // Forget everything: the library was reloaded or the account changed, so
    // ids withheld as "already requested" must be allowed through again.
    void reset();

private:
    void worker();

    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> busy_{false};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> jobs_;                // product ids to look up
    std::unordered_set<std::string> requested_;   // queued or already answered
    std::vector<Game> done_;                      // stubs carrying the answers
};

}  // namespace gnx
