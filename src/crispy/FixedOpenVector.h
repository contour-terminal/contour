#pragma once

#include <functional>
#include <iterator>
#include <memory>

namespace crispy
{

template <typename T>
class FixedOpenVector
{
  private:
    explicit FixedOpenVector(std::size_t capacity, T init = T {});

  public:
    ~FixedOpenVector();

    using Ptr = std::unique_ptr<FixedOpenVector<T>, std::function<void(FixedOpenVector*)>>;

    template <typename Allocator = std::allocator<unsigned char>>
    static Ptr create(std::size_t capacity, T init = T {});

    T* data() noexcept { return _entries; }
    size_t size() const noexcept { return _capacity; }

    T& operator[](size_t i) noexcept { return _entries[i]; }
    T const& operator[](size_t i) const noexcept { return _entries[i]; }

    T* begin() noexcept { return _entries; }
    T* end() noexcept { return _entries + _capacity; }

    T const* begin() const noexcept { return _entries; }
    T const* end() const noexcept { return _entries + _capacity; }

  private:
    size_t _capacity;
    T* _entries;
};

template <typename T>
template <typename Allocator = std::allocator<unsigned char>>
static typename FixedOpenVector<T>::Ptr FixedOpenVector<T>::create(std::size_t capacity, T init)
{
    Allocator allocator;
    auto const size = 15; // TODO
    auto* obj = (FixedOpenVector*) allocator.allocate(size);
}

template <typename T>
FixedOpenVector<T>::FixedOpenVector(std::size_t capacity, T init):
    _capacity { capacity }, _entries { (T*) (this + 1) }
{
    for (auto i = begin(); i != end(); ++i)
        new (i) T(init);
}

template <typename T>
FixedOpenVector<T>::~FixedOpenVector()
{
    std::destroy(begin(), end());
}

} // namespace crispy
