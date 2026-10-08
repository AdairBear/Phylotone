// HttpPost for the provider adapters, implemented with juce::WebInputStream.
//
// Blocking: call it only from a background thread, never the message thread.
// A shared HttpCancel lets the owner abort the request in flight (on shutdown)
// and makes every later call fail at once.

#pragma once

#include "phylo/providers/Providers.h"

#include <juce_core/juce_core.h>

#include <memory>
#include <mutex>

class HttpCancel
{
public:
    // Aborts the request in flight, if any, and refuses all later requests.
    void cancel();
    bool isCancelled() const;

private:
    friend struct HttpCancelScope;
    bool begin(juce::WebInputStream* stream);
    void end();

    mutable std::mutex mutex;
    bool cancelled = false;
    juce::WebInputStream* active = nullptr;
};

// Returns an HttpPost bound to the given cancel token and timeout.
phylo::providers::HttpPost makeJuceHttpPost(std::shared_ptr<HttpCancel> cancel, int timeoutMs);
