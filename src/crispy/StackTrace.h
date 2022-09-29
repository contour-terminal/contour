#pragma once

#include <optional>
#include <string>
#include <vector>

namespace crispy
{

struct DebugInfo
{
    std::string text;
};

class StackTrace
{
  public:
    StackTrace();
    StackTrace(StackTrace&&) = default;
    StackTrace& operator=(StackTrace&&) = default;
    StackTrace(const StackTrace&) = default;
    StackTrace& operator=(const StackTrace&) = default;
    ~StackTrace() = default;

    [[nodiscard]] std::vector<std::string> symbols() const;
    [[nodiscard]] size_t size() const noexcept { return frames_.size(); }
    [[nodiscard]] bool empty() const noexcept { return frames_.empty(); }

    static std::string demangleSymbol(const char* symbol);
    static std::vector<void*> getFrames(size_t _skip = 2, size_t _max = 64);
    static std::optional<DebugInfo> getDebugInfoForFrame(void const* _frameAddress);

  private:
    std::vector<void*> frames_;
};

} // namespace crispy
