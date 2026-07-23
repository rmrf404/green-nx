#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace gnx {

struct HttpResponse {
    long status = 0;
    std::string body;

    bool ok() const { return status >= 200 && status < 300; }
};

// Thin blocking wrapper over libcurl. One instance per thread.
class Http {
public:
    Http();
    ~Http();
    Http(const Http&) = delete;
    Http& operator=(const Http&) = delete;

    HttpResponse get(const std::string& url,
                     const std::vector<std::string>& headers = {});
    HttpResponse post(const std::string& url, const std::string& body,
                      const std::vector<std::string>& headers = {});
    HttpResponse del(const std::string& url,
                     const std::vector<std::string>& headers = {});

    static std::string urlencode(const std::string& value);

    // Path to a CA bundle (cacert.pem) for TLS verification on platforms
    // without a system store (the Switch). Empty = use libcurl defaults.
    static void set_ca_bundle(std::string path);

    // Process-wide X-Forwarded-For, added to every request when non-empty.
    // Presents a supported-region IP to xCloud's geo gate (region bypass).
    // Empty clears it. Thread-safe; the app only changes it while no HTTP
    // worker is in flight (from Settings), and reads it under the same lock.
    static void set_forwarded_for(std::string ip);

    // When set, in-flight transfers abort promptly once *flag becomes true.
    // Lets a worker thread unblock its HTTP call at shutdown.
    void set_abort_flag(std::atomic<bool>* flag) { abort_flag_ = flag; }

private:
    HttpResponse request(const char* method, const std::string& url,
                         const std::string* body,
                         const std::vector<std::string>& headers);
    void* curl_ = nullptr;  // CURL*
    std::atomic<bool>* abort_flag_ = nullptr;
};

}  // namespace gnx
