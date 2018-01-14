// Author: Dmitry Kukovinets (d1021976@gmail.com), 02.01.2018, 01:41


// Minimalistic context handler with Round-Robbin switching.
// 
// NOTE: dkuk::context_group is almost thread-safe. Only non-thread-safe are move constructor and move assign operator.
// 
// Usage:
// // Create contexts and group
// boost::asio::io_context context1, context2, ...;
// dkuk::context_group group{context1, context2, ...};	// NOTE: Group does NOT take ownership on io_contexts.
// 
// // Get io_contexts from group using .get_io_context()
// boost::asio::ip::tcp::socket socket{group.get_io_context()};


#ifndef DKUK_CONTEXT_GROUP_HPP
#define DKUK_CONTEXT_GROUP_HPP

#include <atomic>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include <boost/asio/io_context.hpp>


namespace dkuk {


class context_group
{
public:
	context_group() = default;
	
	
	template<class InputIterator>
	explicit
	context_group(
		InputIterator first,
		InputIterator last
	)
	{
		std::unordered_set<boost::asio::io_context *> contexts_filter;
		for ( ; first != last; ++first) {
			boost::asio::io_context &context = *first;
			if (contexts_filter.insert(&context).second)
				this->contexts_.push_back(&context);
		}
		this->contexts_.shrink_to_fit();
	}
	
	
	explicit inline
	context_group(
		std::initializer_list<std::reference_wrapper<boost::asio::io_context>> contexts
	):
		context_group{contexts.begin(), contexts.end()}
	{}
	
	
	context_group(
		const context_group &other
	) = delete;
	
	
	context_group &
	operator=(
		const context_group &other
	) = delete;
	
	
	// Thread-safe: No.
	inline
	context_group(
		context_group &&other
	) noexcept:
		index_{other.index_.exchange(0, std::memory_order_acq_rel)},
		contexts_{std::move(other.contexts_)}
	{}
	
	
	// Thread-safe: No.
	inline
	context_group &
	operator=(
		context_group &&other
	) noexcept
	{
		this->index_ = other.index_.exchange(0, std::memory_order_acq_rel);
		this->contexts_ = std::move(other.contexts_);
		return *this;
	}
	
	
	inline
	boost::asio::io_context &
	get_io_context() const
	{
		if (!this->contexts_.empty())
			return *this->contexts_.at(this->index_.fetch_add(1) % this->contexts_.size());
		throw std::out_of_range{"Empty context group"};
	}
	
	
	inline
	boost::asio::io_context &
	get_io_context_unsafe() const noexcept
	{
		return *this->contexts_[this->index_.fetch_add(1) % this->contexts_.size()];
	}
	
	
	inline
	std::size_t
	size() const noexcept
	{
		return this->contexts_.size();
	}
	
	
	inline
	bool
	empty() const noexcept
	{
		return this->contexts_.empty();
	}
private:
	mutable std::atomic<std::size_t> index_{0};
	std::vector<boost::asio::io_context *> contexts_;
};	// class context_group


};	// namespace dkuk


#endif	// DKUK_CONTEXT_GROUP_HPP
