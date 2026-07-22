#include "http.hpp"

#include <curl/curl.h>

#include <stdexcept>

namespace gnx {

namespace {

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string g_ca_bundle;

int abort_cb(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* flag = static_cast<std::atomic<bool>*>(userdata);
    return (flag && flag->load()) ? 1 : 0;  // non-zero aborts the transfer
}

}  // namespace

void Http::set_ca_bundle(std::string path) { g_ca_bundle = std::move(path); }

Http::Http() {
    curl_ = curl_easy_init();
    if (!curl_) throw std::runtime_error("curl_easy_init failed");
}

Http::~Http() {
    if (curl_) curl_easy_cleanup(static_cast<CURL*>(curl_));
}

std::string Http::urlencode(const std::string& value) {
    char* escaped = curl_escape(value.c_str(), static_cast<int>(value.size()));
    std::string result = escaped ? escaped : "";
    curl_free(escaped);
    return result;
}

HttpResponse Http::get(const std::string& url,
                       const std::vector<std::string>& headers) {
    return request("GET", url, nullptr, headers);
}

HttpResponse Http::post(const std::string& url, const std::string& body,
                        const std::vector<std::string>& headers) {
    return request("POST", url, &body, headers);
}

HttpResponse Http::del(const std::string& url,
                       const std::vector<std::string>& headers) {
    return request("DELETE", url, nullptr, headers);
}

HttpResponse Http::request(const char* method, const std::string& url,
                           const std::string* body,
                           const std::vector<std::string>& headers) {
    CURL* curl = static_cast<CURL*>(curl_);
    curl_easy_reset(curl);

    HttpResponse response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    if (!g_ca_bundle.empty())
        curl_easy_setopt(curl, CURLOPT_CAINFO, g_ca_bundle.c_str());
    if (abort_flag_) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abort_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, abort_flag_);
    }

    if (body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(body->size()));
    }

    curl_slist* list = nullptr;
    for (const auto& header : headers)
        list = curl_slist_append(list, header.c_str());
    if (list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);

    CURLcode code = curl_easy_perform(curl);
    if (list) curl_slist_free_all(list);
    if (code != CURLE_OK)
        throw std::runtime_error(std::string("HTTP request failed: ") +
                                 curl_easy_strerror(code) + " (" + url + ")");

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    return response;
}

}  // namespace gnx
