#include "Common.h"

#include <cstdlib>

namespace phylo::providers
{

namespace
{
constexpr std::size_t kMaxErrorLength = 300;

std::string vendorMessage(const std::string& body)
{
    const auto j = detail::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return {};
    // Anthropic, OpenAI and Gemini all use {"error": {"message": "..."}}.
    if (j.is_object() && j.contains("error"))
    {
        const auto& e = j["error"];
        if (e.is_object() && e.contains("message") && e["message"].is_string())
            return e["message"].get<std::string>();
        if (e.is_string())
            return e.get<std::string>();
    }
    if (j.is_object() && j.contains("message") && j["message"].is_string())
        return j["message"].get<std::string>();
    return {};
}
} // namespace

assistant::ChatResponse responseFromHttp(const HttpResult& result,
                                         const std::string& vendor,
                                         const std::string& secret)
{
    assistant::ChatResponse r;
    if (result.status >= 200 && result.status < 300)
    {
        r.ok = true;
        return r;
    }
    try
    {
        std::string msg;
        if (result.status == 0)
        {
            msg = vendor + ": network error";
            if (!result.error.empty())
                msg += ": " + result.error;
        }
        else
        {
            msg = vendor + ": HTTP " + std::to_string(result.status);
            const std::string vm = vendorMessage(result.body);
            if (!vm.empty())
                msg += ": " + vm;
        }
        return detail::fail(msg, secret);
    }
    catch (...)
    {
        return detail::fail(vendor + ": request failed");
    }
}

namespace detail
{

std::string envValue(const std::string& name)
{
    if (name.empty())
        return {};
    const char* v = std::getenv(name.c_str());
    return v ? std::string(v) : std::string();
}

std::string sanitise(std::string text, const std::string& secret)
{
    if (!secret.empty())
    {
        const std::string mask = "[redacted]";
        std::size_t pos = 0;
        while ((pos = text.find(secret, pos)) != std::string::npos)
        {
            text.replace(pos, secret.size(), mask);
            pos += mask.size();
        }
    }
    if (text.size() > kMaxErrorLength)
    {
        text.resize(kMaxErrorLength);
        text += "...";
    }
    return text;
}

assistant::ChatResponse fail(const std::string& message, const std::string& secret)
{
    assistant::ChatResponse r;
    r.ok = false;
    r.error = sanitise(message, secret);
    return r;
}

std::string dumpJson(const json& j)
{
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool parseObject(const std::string& text, json& out, bool allowEmpty)
{
    if (text.empty())
    {
        if (!allowEmpty)
            return false;
        out = json::object();
        return true;
    }
    out = json::parse(text, nullptr, false);
    return !out.is_discarded() && out.is_object();
}

std::string joinUrl(std::string base, const std::string& path)
{
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    if (!path.empty() && path.front() == '/')
        return base + path;
    return base + "/" + path;
}

bool send(const HttpPost& post,
          const std::string& url,
          const HttpHeaders& headers,
          const std::string& body,
          const std::string& vendor,
          const std::string& secret,
          std::string& responseBody,
          assistant::ChatResponse& failure)
{
    if (!post)
    {
        failure = fail(vendor + ": no HTTP transport configured");
        return false;
    }
    HttpResult result;
    try
    {
        result = post(url, headers, body);
    }
    catch (const std::exception& e)
    {
        failure = fail(vendor + ": network error: " + e.what(), secret);
        return false;
    }
    catch (...)
    {
        failure = fail(vendor + ": network error");
        return false;
    }
    if (result.status < 200 || result.status >= 300)
    {
        failure = responseFromHttp(result, vendor, secret);
        return false;
    }
    responseBody = std::move(result.body);
    return true;
}

} // namespace detail
} // namespace phylo::providers
