#include "names.hpp"

#include <unordered_map>

#include "../core/http.hpp"

namespace gnx {

namespace {

// One displaycatalog round trip is ~1 MB for 20 products, so a batch this
// size keeps memory bounded while still resolving a screenful in one go.
constexpr size_t kBatch = 20;

}  // namespace

Names::Names() : thread_(&Names::worker, this) {}

Names::~Names() {
    quit_ = true;
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Names::request(const Game& game) {
    if (game.product_id.empty() || !game.name.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (requested_.insert(game.product_id).second) {
        jobs_.push_back(game.product_id);
        wake_.notify_one();
    }
}

void Names::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.clear();
    requested_.clear();
    done_.clear();
}

bool Names::pump(std::vector<Game>& games) {
    std::vector<Game> resolved;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (done_.empty()) return false;
        resolved.swap(done_);
    }

    std::unordered_map<std::string, const Game*> by_product;
    for (const Game& game : resolved)
        if (!game.name.empty()) by_product[game.product_id] = &game;
    if (by_product.empty()) return false;  // every lookup in this batch missed

    bool changed = false;
    for (Game& game : games) {
        auto found = by_product.find(game.product_id);
        if (found == by_product.end() || !game.name.empty()) continue;
        game.name = found->second->name;
        game.box_art_url = found->second->box_art_url;
        changed = true;
    }
    return changed;
}

void Names::worker() {
    Http http;
    http.set_abort_flag(&quit_);  // unblock an in-flight lookup on shutdown
    while (!quit_) {
        std::vector<Game> batch;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return quit_ || !jobs_.empty(); });
            if (quit_) return;
            while (!jobs_.empty() && batch.size() < kBatch) {
                Game stub;
                stub.product_id = std::move(jobs_.front());
                jobs_.pop_front();
                batch.push_back(std::move(stub));
            }
        }

        busy_ = true;
        try {
            fetch_names(http, batch);
        } catch (const std::exception&) {
            // Metadata is best-effort: the ids stay in requested_ so a failed
            // batch is not retried in a loop, and the cards keep their id.
        }
        busy_ = false;

        std::lock_guard<std::mutex> lock(mutex_);
        for (Game& game : batch) done_.push_back(std::move(game));
    }
}

}  // namespace gnx
