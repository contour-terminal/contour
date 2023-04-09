#pragma once

/**
 * Encapsulates std::vector<INT> with speed improvements.
 *
 */
template<typename T>
class IntVector {
 public:
  using value_type = T;
  using vector = std::vector<T>;
  using iterator = Vector::iterator;
  using const_iterator = Vector::const_iterator;

  IntVector() : vector_{}, hash_{2166136261llu} {}

  void clear() {
    vector_.clear();
    hash_ = 2166136261llu;
  }

  void push_back(T v) {
    vector_.push_back(v);

    hash_ ^= v;
    hash_ *= 16777619llu;
  }

  bool operator==(const IntVector& rhs) const noexcept {
    return hash_ == rhs.hash_ && vector_ == rhs.vector_;
  }

  bool operator!=(const IntVector& rhs) const noexcept {
    return !(*this == rhs);
  }

 private:
  Vector vector_;
  unsigned hash_;
};
