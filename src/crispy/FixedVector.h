#include <array>

namespace crispy
{

template <typename T, std::size_t N>
class FixedVector
{
  public:
    constexpr T& at(std::size_t i) { return _storage.at(i); }
    constexpr T const& at(std::size_t i) const { return _storage.at(i); }

    constexpr T& operator[](std::size_t i) noexcept { return _storage[i]; }
    constexpr T const& operator[](std::size_t i) const noexcept { return _storage[i]; }

    constexpr std::size_t size() const noexcept { return _size; }
    constexpr std::size_t capacity() const noexcept { return N; }

    constexpr void clear() { _size = 0; }

    void emplace_back(T value)
    {
        assert(_size < capacity());
        _storage[_size++] = std::move(value);
    }

  private:
    std::array<T, N> _storage {};
    std::size_t _size = 0;
};

} // namespace crispy
