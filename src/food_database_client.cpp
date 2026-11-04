// food_database_client.cpp
// Send a food lookup to Open Food Facts and return the raw JSON response.
// The actual parsing happens elsewhere so this file stays focused on the HTTP request.
#include "food_database_client.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

namespace health {
namespace {

#ifdef _WIN32
const wchar_t* kHost = L"search.openfoodfacts.org";
#else
constexpr std::size_t kMaxResponseSize = 2 * 1024 * 1024;
#endif

#ifdef _WIN32
// Turn the request path into the wide-character format that WinHTTP expects.
std::wstring toWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size == 0) {
        throw std::runtime_error("Could not encode the food search query.");
    }

    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), size);
    result.pop_back();
    return result;
}
#endif

// Percent-encode query bytes so user input is treated as data in the request URL.
// Encode a query string for the API request.
std::string urlEncode(const std::string& value) {
    static const char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char ch : value) {
        const bool asciiAlphaNumeric =
            (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9');
        if (asciiAlphaNumeric || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[ch >> 4]);
            encoded.push_back(hex[ch & 0x0F]);
        }
    }
    return encoded;
}

#ifdef _WIN32
// ---------------------------------------------------------------------------
// Helper: InternetHandle
// Wrap a WinHTTP handle so every successful open is paired with
// WinHttpCloseHandle, including when a later request step throws an error.
// ---------------------------------------------------------------------------
class InternetHandle {
public:
    explicit InternetHandle(HINTERNET handle) : handle_(handle) {}
    ~InternetHandle() {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
    }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    HINTERNET get() const { return handle_; }

private:
    HINTERNET handle_;
};
#else
struct ResponseBuffer {
    std::string body;
    bool tooLarge = false;
};

// Append response chunks to the result while enforcing the response-size cap.
std::size_t appendResponse(char* data, std::size_t size, std::size_t count,
                           void* context) {
    if (size != 0 && count > static_cast<std::size_t>(-1) / size) {
        return 0;
    }
    const std::size_t bytes = size * count;
    auto* response = static_cast<ResponseBuffer*>(context);
    if (bytes > kMaxResponseSize - response->body.size()) {
        response->tooLarge = true;
        return 0;
    }
    response->body.append(data, bytes);
    return bytes;
}

// Release the libcurl easy handle when the request completes or throws.
struct CurlHandleDeleter {
    // Handle operator.
    void operator()(CURL* handle) const {
        if (handle != nullptr) {
            curl_easy_cleanup(handle);
        }
    }
};

// Check whether a libcurl option call succeeded.
void checkCurlOption(CURLcode result) {
    if (result != CURLE_OK) {
        throw std::runtime_error(
            std::string("Could not configure the food search request: ") +
            curl_easy_strerror(result));
    }
}
#endif

}  // namespace

// Fetch the food search results from the API.
std::string downloadOpenFoodFactsSearchResults(const std::string& query) {
#ifdef _WIN32
    // -----------------------------------------------------------------------
    // Open the session, connect to the API host, and build a secure GET request.
    // The returned body contains only the product fields needed by the app.
    // -----------------------------------------------------------------------
    InternetHandle session(WinHttpOpen(
        L"DigitalHealthCompanion/1.0 (local desktop app)",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (session.get() == nullptr) {
        throw std::runtime_error("Could not initialize the secure HTTP connection.");
    }

    InternetHandle connection(WinHttpConnect(session.get(), kHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (connection.get() == nullptr) {
        throw std::runtime_error("Could not connect to Open Food Facts.");
    }

    const std::string path =
        "/search?q=" + urlEncode(query) +
        "&page_size=10&fields=code,product_name,brands,nutriments";
    InternetHandle request(WinHttpOpenRequest(
        connection.get(),
        L"GET",
        toWide(path).c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (request.get() == nullptr) {
        throw std::runtime_error("Could not create the Open Food Facts request.");
    }

    // Give each network phase a finite timeout, then send and receive the
    // response. A connection or server failure is reported to the caller.
    DWORD timeoutMs = 10000;
    WinHttpSetTimeouts(request.get(), timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    if (!WinHttpSendRequest(
            request.get(),
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        throw std::runtime_error("Food search failed. Check your internet connection and try again.");
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(
            request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX)) {
        throw std::runtime_error("Could not read the Open Food Facts response status.");
    }
    if (statusCode < 200 || statusCode >= 300) {
        throw std::runtime_error(
            "Open Food Facts returned HTTP " + std::to_string(statusCode) +
            ". Try again later.");
    }

    // Read available data in chunks. Cap the total body to avoid unbounded
    // memory use if the server response is unexpectedly large.
    std::string response;
    DWORD available = 0;
    while (true) {
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            throw std::runtime_error("Could not read the Open Food Facts response.");
        }
        if (available == 0) {
            break;
        }
        if (response.size() + available > 2 * 1024 * 1024) {
            throw std::runtime_error("Open Food Facts response was too large.");
        }
        const std::size_t offset = response.size();
        response.resize(offset + available);
        DWORD bytesRead = 0;
        if (!WinHttpReadData(request.get(), response.data() + offset, available, &bytesRead)) {
            throw std::runtime_error("Could not read the Open Food Facts response.");
        }
        response.resize(offset + bytesRead);
    }
    return response;
#else
    // -----------------------------------------------------------------------
    // Initialize libcurl once, then request the API over verified HTTPS.
    // -----------------------------------------------------------------------
    static const bool curlInitialized = [] {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw std::runtime_error("Could not initialize the secure HTTP connection.");
        }
        return true;
    }();
    (void)curlInitialized;

    std::unique_ptr<CURL, CurlHandleDeleter> request(curl_easy_init());
    if (!request) {
        throw std::runtime_error("Could not create the Open Food Facts request.");
    }

    const std::string url =
        "https://search.openfoodfacts.org/search?q=" + urlEncode(query) +
        "&page_size=10&fields=code,product_name,brands,nutriments";
    ResponseBuffer response;
    checkCurlOption(curl_easy_setopt(request.get(), CURLOPT_URL, url.c_str()));
    checkCurlOption(curl_easy_setopt(
        request.get(), CURLOPT_USERAGENT,
        "DigitalHealthCompanion/1.0 (local desktop app)"));
    checkCurlOption(curl_easy_setopt(request.get(), CURLOPT_CONNECTTIMEOUT, 10L));
    checkCurlOption(curl_easy_setopt(request.get(), CURLOPT_TIMEOUT, 10L));
    checkCurlOption(curl_easy_setopt(
        request.get(), CURLOPT_WRITEFUNCTION, appendResponse));
    checkCurlOption(curl_easy_setopt(request.get(), CURLOPT_WRITEDATA, &response));

    const CURLcode result = curl_easy_perform(request.get());
    if (response.tooLarge) {
        throw std::runtime_error("Open Food Facts response was too large.");
    }
    if (result != CURLE_OK) {
        throw std::runtime_error(
            std::string("Food search failed. Check your internet connection and try again: ") +
            curl_easy_strerror(result));
    }

    long statusCode = 0;
    if (curl_easy_getinfo(request.get(), CURLINFO_RESPONSE_CODE, &statusCode) != CURLE_OK) {
        throw std::runtime_error("Could not read the Open Food Facts response status.");
    }
    if (statusCode < 200 || statusCode >= 300) {
        throw std::runtime_error(
            "Open Food Facts returned HTTP " + std::to_string(statusCode) +
            ". Try again later.");
    }
    return response.body;
#endif
}

}  // namespace health
