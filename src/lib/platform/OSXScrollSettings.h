#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

// Native preferences may wait on another process. Never read them from a
// synchronous input tap: conversion uses only the last published scalar.
class OSXScrollSettings {
public:
    using Reader = std::function<double()>;
    explicit OSXScrollSettings(Reader reader = readNativeScaling);
    ~OSXScrollSettings();
    OSXScrollSettings(const OSXScrollSettings&) = delete;
    OSXScrollSettings& operator=(const OSXScrollSettings&) = delete;

    std::int32_t convert(float delta) const;
    // Called by the screen's maintenance timer, never by the input tap.
    // Starts the worker lazily; disabled/never-enabled screens do no work.
    void requestRefresh();

private:
    struct State;
    static double readNativeScaling();
    static void run(const std::shared_ptr<State>& state);
    std::shared_ptr<State> m_state;
    std::thread m_thread;
};
