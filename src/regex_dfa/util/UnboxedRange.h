// This file is part of the "x0" project, http://github.com/christianparpart/x0>
//   (c) 2009-2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT
#pragma once

#include <iterator>

namespace regex_dfa::util {

template <typename T>
class UnboxedRange {
  public:
	using BoxedContainer = T;
	using BoxedIterator = typename BoxedContainer::iterator;
	using element_type = typename BoxedContainer::value_type::element_type;

	class iterator {  // {{{
	  public:
		typedef typename BoxedContainer::iterator::difference_type difference_type;
		typedef typename BoxedContainer::iterator::value_type::element_type value_type;
		typedef typename BoxedContainer::iterator::value_type::element_type* pointer;
		typedef typename BoxedContainer::iterator::value_type::element_type& reference;
		typedef typename BoxedContainer::iterator::iterator_category iterator_category;

		explicit iterator(BoxedIterator boxed) : it_(boxed) {}

		const element_type& operator->() const { return **it_; }
		element_type& operator->() { return **it_; }

		const element_type* operator*() const { return (*it_).get(); }
		element_type* operator*() { return (*it_).get(); }

		iterator& operator++()
		{
			++it_;
			return *this;
		}
		iterator& operator++(int)
		{
			++it_;
			return *this;
		}

		bool operator==(const iterator& other) const { return it_ == other.it_; }
		bool operator!=(const iterator& other) const { return it_ != other.it_; }

	  private:
		BoxedIterator it_;
	};  // }}}

	UnboxedRange(BoxedIterator begin, BoxedIterator end) : begin_(begin), end_(end) {}
	explicit UnboxedRange(BoxedContainer& c) : begin_(c.begin()), end_(c.end()) {}
	explicit UnboxedRange(const BoxedContainer& c) : UnboxedRange{const_cast<BoxedContainer&>(c)} {}

	iterator begin() const { return begin_; }
	iterator end() const { return end_; }
	iterator cbegin() const { return begin_; }
	iterator cend() const { return end_; }
	size_t size() const { return std::distance(begin_, end_); }

  private:
	iterator begin_;
	iterator end_;
};

/**
 * Unboxes boxed element types in containers.
 *
 * Good examples are:
 *
 * \code
 *    std::vector<std::unique_ptr<int>> numbers;
 *    // ...
 *    for (int number: unbox(numbers)) {
 *      // ... juse use number here, instead of number.get() or *number.
 *    };
 * \endcode
 */
template <typename BoxedContainer>
UnboxedRange<BoxedContainer> unbox(BoxedContainer& boxedContainer)
{
	return UnboxedRange<BoxedContainer>(boxedContainer);
}

template <typename BoxedContainer>
UnboxedRange<BoxedContainer> unbox(const BoxedContainer& boxedContainer)
{
	return UnboxedRange<BoxedContainer>(boxedContainer);
}

}  // namespace regex_dfa::util
