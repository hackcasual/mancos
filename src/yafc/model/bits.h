// Port of Yafc.Model/Math/Bits.cs — growable bitset used by the milestone
// analysis (bit 0 = "accessible", bit i+1 = "requires milestone i").
// Comparison follows big-integer semantics (used to pick the cheapest
// milestone mask among alternatives).
#pragma once

#include <bit>
#include <compare>
#include <cstdint>
#include <vector>

namespace yafc {

class Bits {
 public:
  Bits() = default;
  explicit Bits(bool setBit0) {
    if (setBit0) Set(0, true);
  }

  bool operator[](int i) const {
    size_t word = static_cast<size_t>(i) / 64;
    return word < data_.size() && (data_[word] >> (i % 64)) & 1;
  }

  void Set(int i, bool value) {
    size_t word = static_cast<size_t>(i) / 64;
    if (word >= data_.size()) {
      if (!value) return;
      data_.resize(word + 1, 0);
    }
    if (value) {
      data_[word] |= uint64_t{1} << (i % 64);
    } else {
      data_[word] &= ~(uint64_t{1} << (i % 64));
    }
  }

  Bits& operator|=(const Bits& other) {
    if (other.data_.size() > data_.size()) data_.resize(other.data_.size(), 0);
    for (size_t i = 0; i < other.data_.size(); ++i) data_[i] |= other.data_[i];
    return *this;
  }
  friend Bits operator|(Bits a, const Bits& b) { return a |= b; }

  Bits& operator&=(const Bits& other) {
    if (data_.size() > other.data_.size()) data_.resize(other.data_.size());
    for (size_t i = 0; i < data_.size(); ++i) data_[i] &= other.data_[i];
    return *this;
  }
  friend Bits operator&(Bits a, const Bits& b) { return a &= b; }

  // Big-integer subtraction of a small constant (upstream operator-(Bits, ulong)).
  Bits& operator-=(uint64_t value) {
    for (size_t i = 0; i < data_.size() && value != 0; ++i) {
      uint64_t before = data_[i];
      data_[i] -= value;
      value = before < value ? 1 : 0;  // borrow
    }
    return *this;
  }
  friend Bits operator-(Bits a, uint64_t b) { return a -= b; }

  int HighestBitSet() const {
    for (size_t i = data_.size(); i-- > 0;) {
      if (data_[i] != 0) {
        return static_cast<int>(i) * 64 + 63 - std::countl_zero(data_[i]);
      }
    }
    return -1;
  }

  bool IsClear() const { return HighestBitSet() == -1; }

  int PopCount() const {
    int count = 0;
    for (uint64_t w : data_) count += std::popcount(w);
    return count;
  }

  bool operator==(uint64_t value) const {
    for (size_t i = 1; i < data_.size(); ++i) {
      if (data_[i] != 0) return false;
    }
    return (data_.empty() ? 0 : data_[0]) == value;
  }

  bool operator==(const Bits& other) const {
    size_t n = std::max(data_.size(), other.data_.size());
    for (size_t i = 0; i < n; ++i) {
      uint64_t a = i < data_.size() ? data_[i] : 0;
      uint64_t b = i < other.data_.size() ? other.data_[i] : 0;
      if (a != b) return false;
    }
    return true;
  }

  std::strong_ordering operator<=>(const Bits& other) const {
    size_t n = std::max(data_.size(), other.data_.size());
    for (size_t i = n; i-- > 0;) {
      uint64_t a = i < data_.size() ? data_[i] : 0;
      uint64_t b = i < other.data_.size() ? other.data_[i] : 0;
      if (a != b) return a < b ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    return std::strong_ordering::equal;
  }

 private:
  std::vector<uint64_t> data_;
};

}  // namespace yafc
