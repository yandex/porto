#pragma once

#include <memory>

#include "error.hpp"

struct THttpClient {
    THttpClient(const std::string &host);
    ~THttpClient();

    using THeader = std::pair<std::string, std::string>;
    using THeaders = std::vector<THeader>;

    struct TRequest {
        std::string Body;
        const char *ContentType;
    };

    TError MakeRequest(const std::string &path, std::string &response, const THeaders &headers = {}, const TRequest *request = nullptr) const;
    static TError SingleRequest(const std::string &url, std::string &response, const THeaders &headers = {}, const TRequest *request = nullptr);

private:
    struct TImpl;
    std::unique_ptr<TImpl> Impl;
};
