#pragma once

#include <SDL2/SDL.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gfx.hpp"

namespace gnx {

// Box-art cache: downloads covers on a worker thread to the SD card, decodes
// them off the render thread, and hands raw bytes to the main thread which
// turns them into textures (SDL textures must be created on the render thread).
class Covers {
public:
    Covers(gfx::Gfx& gfx, std::string cache_dir);
    ~Covers();

    // Returns the texture if ready, nullptr otherwise (and queues the fetch).
    SDL_Texture* get(const std::string& title_id, const std::string& url);

    // Main-thread pump: turn finished downloads into textures (a few per frame).
    void pump();

    // Destroy all cached textures (call before the renderer is torn down for a
    // deko3d stream). They reload lazily from the on-disk cache afterwards.
    void drop_textures();

private:
    void worker();

    gfx::Gfx& gfx_;
    std::string cache_dir_;

    std::unordered_map<std::string, SDL_Texture*> textures_;
    std::unordered_set<std::string> pending_;

    struct Job {
        std::string title_id;
        std::string url;
    };
    struct Done {
        std::string title_id;
        std::vector<char> bytes;
    };

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::deque<Done> done_;
    std::atomic<bool> quit_{false};
    std::thread thread_;
};

}  // namespace gnx
