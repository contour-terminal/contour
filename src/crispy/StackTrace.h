#pragma once

#include <optional>
#include <string>
#include <vector>

namespace crispy
{

struct debug_info
{
    std::string text;
};

class stack_trace
{
  public:
    stack_trace();
    stack_trace(stack_trace&&) = default;
    stack_trace& operator=(stack_trace&&) = default;
    stack_trace(const stack_trace&) = default;
    stack_trace& operator=(const stack_trace&) = default;
    ~stack_trace() = default;

    [[nodiscard]] std::vector<std::string> symbols() const;
    [[nodiscard]] size_t size() const noexcept { return _frames.size(); }
    [[nodiscard]] bool empty() const noexcept { return _frames.empty(); }

    static std::string demangleSymbol(const char* symbol);
    static std::vector<void*> getFrames(size_t skip = 2, size_t max = 64);
    static std::optional<debug_info> getDebugInfoForFrame(void const* frameAddress);

  private:
    std::vector<void*> _frames;
};

} // namespace crispy
