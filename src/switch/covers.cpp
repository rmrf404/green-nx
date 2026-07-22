#include "covers.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <fstream>

#include "../core/http.hpp"

namespace gnx {

Covers::Covers(gfx::Gfx& gfx, std::string cache_dir)
    : gfx_(gfx), cache_dir_(std::move(cache_dir)) {
    mkdir(cache_dir_.c_str(), 0755);
    thread_ = std::thread(&Covers::worker, this);
}

Covers::~Covers() {
    quit_ = true;
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    for (auto& [key, texture] : textures_)
        if (texture) SDL_DestroyTexture(texture);
}

SDL_Texture* Covers::get(const std::string& title_id, const std::string& url) {
    auto found = textures_.find(title_id);
    if (found != textures_.end()) return found->second;
    if (url.empty()) return nullptr;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_.count(title_id)) {
        pending_.insert(title_id);
        jobs_.push_back({title_id, url});
        wake_.notify_one();
    }
    return nullptr;
}

void Covers::pump() {
    for (int budget = 0; budget < 3; ++budget) {
        Done done;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (done_.empty()) return;
            done = std::move(done_.front());
            done_.pop_front();
        }
        SDL_Texture* texture =
            done.bytes.empty()
                ? nullptr
                : gfx_.texture_from_memory(done.bytes.data(),
                                           done.bytes.size());
        // Negative results are cached as nullptr so we don't retry every frame.
        textures_[done.title_id] = texture;
    }
}

void Covers::drop_textures() {
    for (auto& [key, texture] : textures_)
        if (texture) SDL_DestroyTexture(texture);
    textures_.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.clear();  // allow re-queue after the renderer is rebuilt
    done_.clear();     // bytes decoded for the old renderer are useless
}

void Covers::worker() {
    Http http;
    http.set_abort_flag(&quit_);  // unblock in-flight download on shutdown
    while (!quit_) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return quit_ || !jobs_.empty(); });
            if (quit_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }

        std::string path = cache_dir_ + "/" + job.title_id + ".img";
        std::vector<char> bytes;

        std::ifstream cached(path, std::ios::binary);
        if (cached) {
            bytes.assign(std::istreambuf_iterator<char>(cached), {});
        } else {
            try {
                HttpResponse response = http.get(job.url);
                if (response.ok() && !response.body.empty()) {
                    bytes.assign(response.body.begin(), response.body.end());
                    std::ofstream out(path, std::ios::binary | std::ios::trunc);
                    out.write(bytes.data(),
                              static_cast<std::streamsize>(bytes.size()));
                }
            } catch (const std::exception&) {
                // Leave bytes empty; pump() caches the miss.
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        done_.push_back({job.title_id, std::move(bytes)});
    }
}

}  // namespace gnx
