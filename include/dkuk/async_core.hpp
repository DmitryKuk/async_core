// Author: Dmitry Kukovinets (d1021976@gmail.com), 07.12.2017, 22:42


// Minimalistic multithreaded asynchronous core implementation based on boost::asio::io_context.
// Uses hierarchy of io_contexts to execute tasks and balance workers.
// 
// How does async_core "see" things:
// - There are objects of type boost::asio::io_context (io_contexts). All tasks will be posted (dispatched)
//   by user to io_contexts (or some of them).
// - There are workers. Each worker can run tasks from one or more io_contexts.
// - Some contexts are "parents" for other contexts in terms of async_core. Worker associated with one of those
//   can run tasks posted to that context (called self context) and tasks posted to its child contexts.
// - async_core manages contexts. async_core allows user set contexts hierarchy and their parameters. async_core
//   will automatically create and destroy contexts.
// - async_core manages workers. async_core allows user set workers for each context. async_core will automatically
//   launch workers (see start() method and constructors) and stop them (see stop() and destructor).
// - async_core allows user use contexts managed by async_core: post tasks, stop/start, etc., but remember: contexts
//   will be started, when user call start(), stopped, when user calls stop(), and destroyed, when async_core will be
//   destroyed.
// 
// Common workflow:
// 0. Design your application structure, answer following questions:
//     - How many boost::asio::io_context objects you need?
//     - How many threads should run these contexts and which of them?
//     - Are some of your io_contexts under high-load? Do you want save some CPU percents?
// 1. Map your application structure to async_core's terms:
//     - Create async_core::context_tree.
//     - Add contexts with their parent-child relationship. NOTE: Contexts ids guaranteed to be sequence: 0, 1, 2, ...
//     - Set workers with appropriate parameters for each context.
// 2. Create and start async_core.
// 3. Using async_core::get_io_context() get your io_contexts, post tasks, etc...
// 4. Use async_core::join() to freeze current thread until async_core::stop() will be called from another thread.
// 5. When you need to stop, just do all you usually do (close your sockets etc.) and call async_core::stop().
// 
// Why you don't need async_core:
// - You have single io_context and one or more workers (1) => you can use boost::asio::io_context itself.
// - You have some io_contexts and some workers on some of them (2) => use boost::asio::io_context again.
// 
// Why you probably need async_core:
// - Your application is too complex for (1) or you don't want to balance tasks between overloaded workers
//   and idle workers in (2).
// - You application is something like: "I have lots of lightweight tasks, which should be executed quickly, but
//   sometimes I need to run some heavy tasks, so all my workers are busy and can't execute lightweight tasks."
// - You think about same solution author found:
//     + io_context + some workers for lightweight tasks only;
//     + io_context + some workers for heavyweight tasks only;
//     + (parent io_context +) some (maybe, most of) workers for common purposes: runs tasks of both types.
// 
// Thread-safety:
// - async_core:
//     + distinct objects: safe;
//     + shared object: safe;
// - async_core::context_tree, async_core::worker, async_core::workers::parameters:
//     + distinct objects: safe;
//     + shared object: unsafe.


#ifndef DKUK_ASYNC_CORE_HPP
#define DKUK_ASYNC_CORE_HPP

#include <atomic>
#include <functional>
#include <iterator>
#include <mutex>
#include <new>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/optional.hpp>


namespace dkuk {


class async_core
{
public:
	using context_id_type        = std::size_t;
	using worker_id_type         = std::size_t;
	using exception_handler_type = std::function<void (const std::exception &)>;
	
	
	
	enum class state
	{
		idle     = 0,
		starting = 1,
		running  = 2,
		stopping = 3
	};	// enum class state
	
	
	
	class worker
	{
	public:
		// Worker io_contexts poll policy.
		// Implies call of io_context's methods poll_one, poll or run_one.
		// NOTE: 'run_all' is forbidden, because it will disable your worker for processing any child contexts.
		//       Anyway, if worker has no children contexts, it will use boost::asio::io_context::run,
		//       ignoring poll and delay settings.
		enum class poll
		{
			disabled,	// Worker should ignore io_context (or group of io_contexts).
			poll_one,	// Guaranteed fast Round-Robbin on contexts.
			poll_all,	// May slow down on all contexts, but speeds up executing tasks on the specific one.
			run_one		// Use this only if you know, why boost::asio::io_context::run_one can freeze your worker.
		};	// enum class poll
		
		
		
		// Worker delay policy.
		// If worker has children io_contexts, it should poll each of them. After every poll cycle worker can
		// yield execution or sleep. Use these options to save CPU, if your contexts are not extremely loaded.
		enum class delay
		{
			no_delay,	// Just continue executing. May be fastest, but eats CPU. For really loaded contexts
			yield,		// std::this_thread::yield();
			sleep		// std::this_thread::sleep_for(delay_value);
		};	// enum class delay
		
		
		
		using default_delay =
			std::integral_constant<
				std::chrono::nanoseconds::rep,
				static_cast<std::chrono::nanoseconds::rep>(500) * 1000 * 1000	// 500 milliseconds
			>;
		
		
		
		struct parameters
		{
			// Poll settings (actual for workers with children contexts)
			poll                     self_poll_policy     = poll::poll_all;	// Execute all self tasks
			poll                     children_poll_policy = poll::poll_one;	// Round-Robbin on children contexts
			
			
			// Delay settings (mostly, actual for workers with children contexts)
			std::size_t              delay_rounds         = 1;	// Delay after rounds without tasks executed (actual
																// for worker without children too, if self
																// io_context is stopped).
			delay                    delay_policy         = delay::yield;
			std::chrono::nanoseconds delay_value          = std::chrono::nanoseconds{default_delay::value};
		};	// struct parameters
	};	// class worker
	
	
	
	class context_tree
	{
	public:
		inline
		context_id_type
		add_context(
			context_id_type parent_id = 0,
			std::size_t workers_count = 0,
			bool enabled = true
		)
		{
			return this->add_context_(parent_id, workers_count, enabled, boost::none);
		}
		
		
		inline
		context_id_type
		add_context(
			context_id_type parent_id,
			std::size_t workers_count,
			bool enabled,
			int concurrency_hint
		)
		{
			return this->add_context_(parent_id, workers_count, enabled, concurrency_hint);
		}
		
		
		inline
		void
		set_worker_parameters(
			context_id_type context_id,
			worker_id_type worker,
			const worker::parameters &parameters
		)
		{
			this->nodes_.at(context_id).worker_parameters_.at(worker) =
				async_core::fixed_worker_parameters_(parameters);
		}
		
		
		inline
		worker_id_type
		add_worker(
			context_id_type context_id,
			const worker::parameters &parameters
		)
		{
			node &n = this->nodes_.at(context_id);
			const worker_id_type worker_id = n.worker_parameters_.size();
			n.worker_parameters_.push_back(async_core::fixed_worker_parameters_(parameters));
			return worker_id;
		}
		
		
		inline
		worker_id_type
		add_worker(
			context_id_type context_id
		)
		{
			node &n = this->nodes_.at(context_id);
			const worker_id_type worker_id = n.worker_parameters_.size();
			n.worker_parameters_.emplace_back();
			return worker_id;
		}
	private:
		friend class async_core;
		
		
		
		struct node
		{
			inline
			node(
				context_id_type parent_id,
				std::size_t workers_count,
				bool enabled,
				boost::optional<int> concurrency_hint
			) noexcept:
				parent_id_{parent_id},
				worker_parameters_(workers_count),
				concurrency_hint_{concurrency_hint},
				enabled_{enabled}
			{}
			
			
			
			context_id_type parent_id_;
			std::size_t children_count_ = 0;
			std::vector<worker::parameters> worker_parameters_;
			boost::optional<int> concurrency_hint_;
			bool enabled_;
		};	// struct node
		
		
		
		inline
		context_id_type
		add_context_(
			context_id_type parent_id,
			std::size_t workers_count,
			bool enabled,
			boost::optional<int> concurrency_hint
		)
		{
			const context_id_type new_id = this->nodes_.size();
			if (parent_id >= new_id && parent_id != 0)
				throw std::out_of_range{"Incorrect context parent id"};
			
			this->nodes_.emplace_back(parent_id, workers_count, enabled, concurrency_hint);
			if (new_id != 0)
				++this->nodes_[parent_id].children_count_;
			
			return new_id;
		}
		
		
		
		std::vector<node> nodes_;
	};	// class context_tree
	
	
	
	async_core(
		const context_tree &t,
		exception_handler_type exception_handler,
		bool start_immediately = true
	):
		nodes_{t},
		nodes_count_{t.nodes_.size()},
		exception_handler_{std::move(exception_handler)}
	{
		if (start_immediately)
			this->start();
	}
	
	
	async_core(
		const context_tree &t,
		bool start_immediately = true
	):
		nodes_{t},
		nodes_count_{t.nodes_.size()}
	{
		if (start_immediately)
			this->start();
	}
	
	
	inline
	~async_core()
	{
		this->stop();
	}
	
	
	inline
	state
	get_state() const noexcept
	{
		return this->state_;
	}
	
	
	inline
	bool
	joinable() const noexcept
	{
		return this->state_ == state::running && !this->joined_;
	}
	
	
	void
	start()
	{
		if (this->nodes_.empty())
			return;
		
		std::lock(this->stop_mutex_, this->join_mutex_);
		std::lock_guard<std::mutex> stop_lock{this->stop_mutex_, std::adopt_lock};
		this->join_mutex_.unlock();
		
		this->state_ = state::starting;
		try {
			this->start_workers_();
		} catch (...) {
			this->state_.store(state::idle, std::memory_order_release);
			throw;
		}
		this->state_.store(state::running, std::memory_order_release);
	}
	
	
	void
	stop()
	{
		if (this->nodes_.empty())
			return;
		
		std::lock_guard<std::mutex> stop_lock{this->stop_mutex_};
		this->state_.store(state::stopping, std::memory_order_release);
		this->stop_workers_();
		this->join_workers_();
	}
	
	
	void
	join()
	{
		if (this->state_ != state::running || !this->join_workers_())
			throw std::invalid_argument{"Core is not joinable"};
	}
	
	
	inline
	boost::asio::io_context &
	get_io_context(
		context_id_type context_id
	)
	{
		return this->nodes_.at(context_id).io_context_;
	}
	
	
	inline
	const boost::asio::io_context &
	get_io_context(
		context_id_type context_id
	) const
	{
		return this->nodes_.at(context_id).io_context_;
	}
private:
	struct node
	{
		inline
		node(
			std::size_t children_count,
			const std::vector<worker::parameters> &worker_parameters,
			bool enabled
		):
			worker_parameters_{worker_parameters},
			enabled_{(enabled)? true: false}
		{
			this->children_ptrs_.reserve(children_count);
			this->workers_.reserve(this->worker_parameters_.size());
		}
		
		
		inline
		node(
			std::size_t children_count,
			const std::vector<worker::parameters> &worker_parameters,
			bool enabled,
			int concurrency_hint
		):
			io_context_{concurrency_hint},
			worker_parameters_{worker_parameters},
			enabled_{(enabled)? true: false}
		{
			this->children_ptrs_.reserve(children_count);
			this->workers_.reserve(this->worker_parameters_.size());
		}
		
		
		
		boost::asio::io_context io_context_;
		std::vector<node *> children_ptrs_;
		std::vector<std::thread> workers_;
		boost::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard_;
		std::vector<worker::parameters> worker_parameters_;
		bool enabled_;
	};	// struct node
	
	
	
	class node_array
	{
	public:
		using node_storage_type = typename std::aligned_storage<sizeof(node), std::alignment_of<node>::value>::type;
		
		
		
		class iterator
		{
		public:
			using iterator_category = std::random_access_iterator_tag;
			
			
			iterator(
				const iterator &other
			) = default;
			
			
			iterator &
			operator=(
				const iterator &other
			) = default;
			
			
			inline
			async_core::node &
			operator*() const noexcept
			{
				return *this->operator->();
			}
			
			
			inline
			async_core::node *
			operator->() const noexcept
			{
				return reinterpret_cast<async_core::node *>(this->ptr_);
			}
			
			
			inline
			iterator &
			operator++() noexcept
			{
				++this->ptr_;
				return *this;
			}
			
			
			inline
			iterator
			operator++(
				int
			) noexcept
			{
				auto tmp = *this;
				++this->ptr_;
				return tmp;
			}
			
			
			inline
			iterator &
			operator--() noexcept
			{
				--this->ptr_;
				return *this;
			}
			
			
			inline
			iterator
			operator--(
				int
			) noexcept
			{
				auto tmp = *this;
				--this->ptr_;
				return tmp;
			}
			
			
			inline
			iterator &
			operator+=(
				std::size_t n
			) noexcept
			{
				this->ptr_ += n;
				return *this;
			}
			
			
			inline
			iterator &
			operator-=(
				std::size_t n
			) noexcept
			{
				this->ptr_ += n;
				return *this;
			}
			
			
			friend inline
			bool
			operator==(
				const iterator &a,
				const iterator &b
			) noexcept
			{
				return a.ptr_ == b.ptr_;
			}
			
			
			friend inline
			bool
			operator!=(
				const iterator &a,
				const iterator &b
			) noexcept
			{
				return a.ptr_ != b.ptr_;
			}
			
			
			friend inline
			bool
			operator<(
				const iterator &a,
				const iterator &b
			) noexcept
			{
				return a.ptr_ < b.ptr_;
			}
			
			
			friend inline
			bool
			operator<=(
				const iterator &a,
				const iterator &b
			) noexcept
			{
				return a.ptr_ <= b.ptr_;
			}
			
			
			friend inline
			bool
			operator>(
				const iterator &a,
				const iterator &b
			) noexcept
			{
				return a.ptr_ > b.ptr_;
			}
			
			
			friend inline
			bool
			operator>=(
				const iterator &a,
				const iterator &b
			) noexcept
			{
				return a.ptr_ >= b.ptr_;
			}
		private:
			friend class node_array;
			
			
			inline
			iterator(
				node_storage_type *ptr
			) noexcept:
				ptr_{ptr}
			{}
			
			
			
			node_storage_type *ptr_;
		};	// class iterator
		
		
		
		node_array(
			const context_tree &t
		):
			nodes_ptr_{(t.nodes_.size() > 0)? new node_storage_type[t.nodes_.size()]: nullptr},
			size_{t.nodes_.size()}
		{
			std::size_t nodes_initialized = 0;
			
			try {
				for (const auto &n: t.nodes_) {
					if (static_cast<bool>(n.concurrency_hint_)) {
						new(&(*this)[nodes_initialized]) node{
							n.children_count_,
							n.worker_parameters_,
							n.enabled_,
							n.concurrency_hint_.get()
						};
					} else {
						new(&(*this)[nodes_initialized]) node{
							n.children_count_,
							n.worker_parameters_,
							n.enabled_
						};
					}
					++nodes_initialized;
					
					const std::size_t current_id = nodes_initialized - 1;
					if (n.parent_id_ != current_id)
						this->at(n.parent_id_).children_ptrs_.push_back(&(*this)[current_id]);
				}
			} catch (...) {
				this->delete_nodes_(nodes_initialized);
				throw;
			}
		}
		
		
		inline
		~node_array()
		{
			this->delete_nodes_(this->size_);
		}
		
		
		inline
		std::size_t
		size() const noexcept
		{
			return this->size_;
		}
		
		
		inline
		bool
		empty() const noexcept
		{
			return this->size_ == 0;
		}
		
		
		inline
		node &
		operator[](
			std::size_t i
		) noexcept
		{
			return *reinterpret_cast<node *>(this->nodes_ptr_ + i);
		}
		
		
		inline
		const node &
		operator[](
			std::size_t i
		) const noexcept
		{
			return *reinterpret_cast<node *>(this->nodes_ptr_ + i);
		}
		
		
		inline
		node &
		at(
			std::size_t i
		)
		{
			if (i < this->size_)
				return (*this)[i];
			throw std::out_of_range{"Incorrect context id"};
		}
		
		
		inline
		const node &
		at(
			std::size_t i
		) const
		{
			if (i < this->size_)
				return (*this)[i];
			throw std::out_of_range{"Incorrect context id"};
		}
		
		
		inline
		node &
		front()
		{
			return this->at(0);
		}
		
		
		inline
		std::size_t
		index_of(
			const node &n
		) const noexcept
		{
			return static_cast<std::size_t>(reinterpret_cast<const node_storage_type *>(&n) - this->nodes_ptr_);
		}
		
		
		inline
		iterator
		begin() noexcept
		{
			return this->nodes_ptr_;
		}
		
		
		inline
		iterator
		end() noexcept
		{
			return this->nodes_ptr_ + this->size_;
		}
	private:
		void
		delete_nodes_(
			std::size_t nodes_initialized
		) noexcept
		{
			if (nodes_initialized > 0) {
				do {
					--nodes_initialized;
					(*this)[nodes_initialized].~node();
					::operator delete(&(*this)[nodes_initialized], &(*this)[nodes_initialized]);
				} while (nodes_initialized > 0);
			}
			
			if (this->nodes_ptr_ != nullptr)
				delete [] this->nodes_ptr_;
		}
		
		
		
		node_storage_type * const nodes_ptr_;
		const std::size_t size_;
	};	// class node_array
	
	
	
	using poll_method_type = std::size_t (boost::asio::io_context::*)();
	
	
	
	static
	worker::parameters
	fixed_worker_parameters_(
		worker::parameters parameters
	)
	{
		const worker::parameters default_parameters{};
		
		// Poll
		switch (parameters.self_poll_policy) {
			case worker::poll::disabled:
				break;
			case worker::poll::poll_one:
				break;
			case worker::poll::poll_all:
				break;
			case worker::poll::run_one:
				break;
			default:
				parameters.self_poll_policy = default_parameters.self_poll_policy;
				break;
		}
		
		switch (parameters.children_poll_policy) {
			case worker::poll::disabled:
				break;
			case worker::poll::poll_one:
				break;
			case worker::poll::poll_all:
				break;
			case worker::poll::run_one:
				break;
			default:
				parameters.children_poll_policy = default_parameters.children_poll_policy;
				break;
		}
		
		
		// Delay
		if (parameters.delay_rounds < 1)
			parameters.delay_rounds = default_parameters.delay_rounds;
		
		switch (parameters.delay_policy) {
			case worker::delay::no_delay:
				break;
			case worker::delay::yield:
				break;
			case worker::delay::sleep:
				break;
			default:
				parameters.delay_policy = default_parameters.delay_policy;
				break;
		}
		
		return parameters;
	}
	
	
	static inline
	poll_method_type
	worker_get_poll_method_(
		worker::poll poll_policy
	) noexcept
	{
		switch (poll_policy) {
			case worker::poll::disabled:
				return nullptr;
			case worker::poll::poll_one:
				return &boost::asio::io_context::poll_one;
			case worker::poll::poll_all:
				return &boost::asio::io_context::poll;
			case worker::poll::run_one:
				return &boost::asio::io_context::run_one;
		}
		return nullptr;
	}
	
	
	void
	start_workers_()
	{
		if (this->nodes_.empty())
			return;
		
		std::vector<node *> ordered_node_ptrs = this->order_nodes_();
		for (auto it = ordered_node_ptrs.rbegin(), end = ordered_node_ptrs.rend(); it < end; ++it) {
			auto &n = **it;
			n.work_guard_.emplace(boost::asio::make_work_guard(n.io_context_));
			
			for (const worker::parameters &parameters: n.worker_parameters_)
				n.workers_.emplace_back(&async_core::worker_run_, this, std::ref(n), std::cref(parameters));
		}
	}
	
	
	std::vector<node *>
	order_nodes_()
	{
		std::vector<node *> ordered_node_ptrs;
		ordered_node_ptrs.reserve(this->nodes_.size());
		ordered_node_ptrs.push_back(&this->nodes_.front());
		
		std::vector<bool> visited(this->nodes_.size(), false);
		visited[0] = true;
		
		auto ordered_it = ordered_node_ptrs.begin();
		while (ordered_node_ptrs.size() < this->nodes_.size()) {
			for (auto child_ptr: (*ordered_it)->children_ptrs_) {
				const auto child_id = this->nodes_.index_of(*child_ptr);
				if (!visited[child_id]) {
					visited[child_id] = true;
					ordered_node_ptrs.push_back(child_ptr);
				}
			}
			++ordered_it;
		}
		
		return ordered_node_ptrs;
	}
	
	
	void
	stop_workers_()
	{
		for (auto &n: this->nodes_)
			n.work_guard_ = boost::none;
		for (auto &n: this->nodes_)
			n.io_context_.stop();
	}
	
	
	bool
	join_workers_()
	{
		if (!this->joined_.exchange(true)) {	// Not joined before
			std::lock_guard<std::mutex> join_lock{this->join_mutex_};
			for (auto &n: this->nodes_) {
				for (auto &worker: n.workers_)
					worker.join();
				n.workers_.clear();
			}
			this->joined_.store(false, std::memory_order_release);
			this->state_.store(state::idle, std::memory_order_release);
			return true;
		}
		return false;
	}
	
	
	void
	worker_run_(
		node &n,
		const worker::parameters &parameters
	) const
	{
		boost::asio::io_context *self_context_ptr =
			(parameters.self_poll_policy != worker::poll::disabled && n.enabled_)? &n.io_context_: nullptr;
		
		std::vector<boost::asio::io_context *> child_context_ptrs =
			worker_get_child_contexts_to_run_(n, parameters);
		
		if (child_context_ptrs.empty() && self_context_ptr != nullptr) {
			return this->worker_run_single_(n, parameters, *self_context_ptr);
		} else if (child_context_ptrs.size() == 1 && self_context_ptr == nullptr) {
			return this->worker_run_single_(n, parameters, *child_context_ptrs.front());
		} else if (child_context_ptrs.size() > 1 || self_context_ptr != nullptr) {
			return this->worker_run_multiple_(n, parameters, self_context_ptr, std::move(child_context_ptrs));
		}
	}
	
	
	std::vector<boost::asio::io_context *>
	worker_get_child_contexts_to_run_(
		node &n,
		const worker::parameters &parameters
	) const
	{
		std::vector<boost::asio::io_context *> child_context_ptrs;
		
		if (parameters.children_poll_policy != worker::poll::disabled) {
			std::queue<node *> nodes_queue_;
			
			for (node *child_ptr: n.children_ptrs_)
				nodes_queue_.push(child_ptr);
			
			while (!nodes_queue_.empty()) {
				node *node_ptr = nodes_queue_.front();
				nodes_queue_.pop();
				
				if (node_ptr->enabled_)
					child_context_ptrs.push_back(&node_ptr->io_context_);
				
				for (node *child_ptr: node_ptr->children_ptrs_)
					nodes_queue_.push(child_ptr);
			}
			child_context_ptrs.shrink_to_fit();
		}
		
		return child_context_ptrs;
	}
	
	
	void
	worker_run_single_(
		node &n,
		const worker::parameters &parameters,
		boost::asio::io_context &context
	) const
	{
		std::size_t wait_rounds = 0;
		while (this->get_state() != state::stopping) {
			if (wait_rounds >= parameters.delay_rounds) {
				wait_rounds = 0;
				this->worker_delay_(parameters);
			}
			
			this->worker_poll_context_(context, &boost::asio::io_context::run);
			if (context.stopped())
				++wait_rounds;
		}
	}
	
	
	void
	worker_run_multiple_(
		node &n,
		const worker::parameters &parameters,
		boost::asio::io_context *self_context_ptr,
		std::vector<boost::asio::io_context *> child_context_ptrs
	) const
	{
		const poll_method_type self_poll_method =
			(self_context_ptr == nullptr)? nullptr: async_core::worker_get_poll_method_(parameters.self_poll_policy);
		
		const poll_method_type children_poll_method =
			async_core::worker_get_poll_method_(parameters.children_poll_policy);
		
		std::size_t wait_rounds = 0;
		while (this->get_state() != state::stopping) {
			if (wait_rounds >= parameters.delay_rounds) {
				wait_rounds = 0;
				this->worker_delay_(parameters);
			}
			
			std::size_t executed = 0;
			if (self_poll_method != nullptr)
				executed += this->worker_poll_context_(*self_context_ptr, self_poll_method);
			if (children_poll_method != nullptr)
				executed += this->worker_poll_contexts_(child_context_ptrs, children_poll_method);
			
			if (executed == 0)
				++wait_rounds;
		}
	}
	
	
	inline
	std::size_t
	worker_poll_contexts_(
		std::vector<boost::asio::io_context *> &child_context_ptrs,
		poll_method_type poll_method
	) const
	{
		std::size_t executed = 0;
		for (const auto child_context_ptr: child_context_ptrs)
			executed += this->worker_poll_context_(*child_context_ptr, poll_method);
		return executed;
	}
	
	
	inline
	std::size_t
	worker_poll_context_(
		boost::asio::io_context &context,
		poll_method_type poll_method
	) const
	{
		try {
			return (context.*poll_method)();
		} catch (const std::exception &e) {
			if (this->exception_handler_)
				this->exception_handler_(e);
			return 0;
		}
	}
	
	
	inline
	void
	worker_delay_(
		const worker::parameters &parameters
	) const
	{
		switch (parameters.delay_policy) {
			case worker::delay::no_delay:
				break;
			case worker::delay::yield:
				std::this_thread::yield();
				break;
			case worker::delay::sleep:
				std::this_thread::sleep_for(parameters.delay_value);
				break;
		}
	}
	
	
	
	std::atomic<state> state_{state::idle};
	std::mutex stop_mutex_, join_mutex_;
	node_array nodes_;
	const std::size_t nodes_count_ = 0;
	exception_handler_type exception_handler_;
	std::atomic<bool> joined_{false};
};	// class async_core


};	// namespace dkuk


#endif	// DKUK_ASYNC_CORE_HPP
