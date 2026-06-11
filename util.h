#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <utility>

class temp_file {
    std::string m_path;
    bool m_owning = true;
public:
    temp_file() = default;
    explicit temp_file(const std::string& path) : m_path(path) {}
    temp_file(const temp_file&) = delete;
    temp_file& operator=(const temp_file&) = delete;
    temp_file(temp_file&& other) noexcept : m_path(std::move(other.m_path)), m_owning(other.m_owning) { other.m_owning = false; }
    temp_file& operator=(temp_file&& other) noexcept { if (this != &other) { release(); m_path = std::move(other.m_path); m_owning = other.m_owning; other.m_owning = false; } return *this; }
    ~temp_file() { release(); }
    void release() { if (m_owning && !m_path.empty()) { std::filesystem::remove(m_path); m_path.clear(); m_owning = false; } }
    const std::string& path() const { return m_path; }
    bool empty() const { return m_path.empty(); }
    std::string detach() { m_owning = false; return std::exchange(m_path, {}); }
};

class scope_guard {
    std::function<void()> m_fn;
    bool m_active = true;
public:
    explicit scope_guard(std::function<void()> fn) : m_fn(std::move(fn)) {}
    scope_guard(const scope_guard&) = delete;
    scope_guard& operator=(const scope_guard&) = delete;
    scope_guard(scope_guard&& other) noexcept : m_fn(std::move(other.m_fn)), m_active(other.m_active) { other.m_active = false; }
    ~scope_guard() { if (m_active && m_fn) m_fn(); }
    void dismiss() { m_active = false; }
};