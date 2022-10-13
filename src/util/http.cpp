#include "http.hpp"

#define CPPHTTPLIB_NO_EXCEPTIONS
#define CPPHTTPLIB_CONNECTION_TIMEOUT_SECOND 5
#include "cpp-httplib/httplib.h"

struct THttpClient::TImpl {
    TImpl(const std::string &host)
        : Host(host)
        , Client(host)
    {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        Client.enable_server_certificate_verification(false);
#endif
    }

    TError HandleResult(const httplib::Result &res, const std::string &path, std::string &response, const THeaders &headers = {}, const TRequest *request = nullptr) {
        if (!res)
            return TError::System("HTTP request to {} failed: {}", Host + path, res.error());

        if (res->status != 200) {
            if (res->status >= 300 && res->status < 400) {
                auto location = res->get_header_value("Location");
                if (!location.empty()) {
                    if (location[0] == '/')
                        location = Host + location;
                    return SingleRequest(location, response, headers, request);
                }
            }
            return TError::System("HTTP request to {} failed: status {}", Host + path, res->status);
        }

        response = res->body;

        return OK;
    }

    std::string Host;
    httplib::Client Client;
};

THttpClient::THttpClient(const std::string &host) : Impl(new TImpl(host)) {}
THttpClient::~THttpClient() = default;

TError THttpClient::MakeRequest(const std::string &path, std::string &response, const THeaders &headers, const TRequest *request) const {
    httplib::Headers hdrs(headers.cbegin(), headers.cend());

    if (request)
        return Impl->HandleResult(Impl->Client.Post(path.c_str(), hdrs, request->Body, request->ContentType), path, response);

    return Impl->HandleResult(Impl->Client.Get(path.c_str(), hdrs), path, response);
}

TError THttpClient::SingleRequest(const std::string &url, std::string &response, const THeaders &headers, const TRequest *request) {
    auto hostPos = url.find("://");
    if (hostPos == std::string::npos)
        hostPos = 0;
    else
        hostPos += 3;

    auto pathPos = url.find('/', hostPos);

    auto host = url.substr(0, pathPos);
    auto path = pathPos == std::string::npos ? "/" : url.substr(pathPos);

    return THttpClient(host).MakeRequest(path, response, headers, request);
}
