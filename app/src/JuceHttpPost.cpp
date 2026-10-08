#include "JuceHttpPost.h"

void HttpCancel::cancel()
{
    std::lock_guard<std::mutex> lock(mutex);
    cancelled = true;
    if (active != nullptr)
        active->cancel();
}

bool HttpCancel::isCancelled() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return cancelled;
}

bool HttpCancel::begin(juce::WebInputStream* stream)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (cancelled)
        return false;
    active = stream;
    return true;
}

void HttpCancel::end()
{
    std::lock_guard<std::mutex> lock(mutex);
    active = nullptr;
}

// Registers a stream with the cancel token for the lifetime of the scope.
struct HttpCancelScope
{
    HttpCancelScope(HttpCancel& c, juce::WebInputStream& s) : cancel(c), ok(c.begin(&s)) {}
    ~HttpCancelScope()
    {
        if (ok)
            cancel.end();
    }
    HttpCancel& cancel;
    const bool ok;
};

namespace
{
juce::String utf8(const std::string& s)
{
    return juce::String::fromUTF8(s.data(), static_cast<int>(s.size()));
}

phylo::providers::HttpResult post(HttpCancel& cancel,
                                  int timeoutMs,
                                  const std::string& url,
                                  const phylo::providers::HttpHeaders& headers,
                                  const std::string& body)
{
    phylo::providers::HttpResult result;

    // withPOSTData sets the raw body. The stream must be created with
    // addParametersToRequestBody = true for JUCE to send it as a POST.
    const auto target = juce::URL(utf8(url)).withPOSTData(juce::MemoryBlock(body.data(), body.size()));

    juce::String extraHeaders;
    for (const auto& h : headers)
        extraHeaders << utf8(h.first) << ": " << utf8(h.second) << "\r\n";

    juce::WebInputStream stream(target, true);
    stream.withExtraHeaders(extraHeaders)
        .withConnectionTimeout(timeoutMs)
        .withNumRedirectsToFollow(0);

    HttpCancelScope scope(cancel, stream);
    if (!scope.ok)
    {
        result.error = "request cancelled";
        return result;
    }

    // Use WebInputStream directly rather than URL::createInputStream, which
    // returns no stream for 4xx/5xx; the adapters need the error body.
    const bool connected = stream.connect(nullptr);
    const int status = stream.getStatusCode();
    if (!connected || status <= 0)
    {
        result.error = cancel.isCancelled() ? "request cancelled"
                                            : "could not reach the server (network error or timeout)";
        return result;
    }

    juce::MemoryOutputStream out;
    out.writeFromInputStream(stream, -1);
    result.status = status;
    result.body.assign(static_cast<const char*>(out.getData()), out.getDataSize());
    return result;
}
} // namespace

phylo::providers::HttpPost makeJuceHttpPost(std::shared_ptr<HttpCancel> cancel, int timeoutMs)
{
    return [cancel = std::move(cancel), timeoutMs](const std::string& url,
                                                   const phylo::providers::HttpHeaders& headers,
                                                   const std::string& body)
    {
        return post(*cancel, timeoutMs, url, headers, body);
    };
}
