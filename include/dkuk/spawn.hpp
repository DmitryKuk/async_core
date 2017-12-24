// Author: Dmitry Kukovinets (d1021976@gmail.com), 10.03.2017, 17:24


// Improved version of boost::asio::spawn() function. Based on Boost.Context (boost::context::continuation)
// instead of Boost.Coroutine (which is deprecated). Also, it allows to pass additional arguments to your function
// (see example 1). Spawned coroutines can be used witout Boost.Asio'a async operations manually (see example 2).
// 
// 
// Example 1: spawning coroutine with arguments.
// void my_fn(boost::asio::ip::tcp::socket &s, int count, dkuk::coroutine_context context)
// {
//     boost::asio::system_timer timer{context.get_executor()};
//     for (int i = 0; i < count; ++i) {
//         std::cout << "Sleep #" << i << "..." << std::endl;
//         timer.expires_from_now(std::chrono::seconds(1));
//         timer.async_wait(context);
//     }
//     
//     std::size_t bytes_transferred1 = s.async_receive(/* ... */, context);
//     
//     boost::system::error_code ec;
//     std::size_t bytes_transferred2 = s.async_receive(/* ... */, context[ec]);	// External error code
//     if (ec)
//         throw boost::system::system_error{ec};
// }
// 
// boost::asio::io_context io_context;
// boost::asio::ip::tcp::socket s;
// dkuk::spawn(io_context, my_fn, std::ref(s), 10);
// io_context.run();
// 
// 
// Example 2: continue coroutine manually.
// void call_later_0(std::function<void()> fn) { ... }
// void call_later_1(std::function<void(int)> fn) { ... }
// void call_later_2(std::function<void(int, std::string)> fn)
// {
//     // Save fn somewhere, (maybe, even return) and call later:
//     fn(123, "Hello, world!");
// }
// 
// void my_fn(dkuk::coroutine_context context)
// {
//     dkuk::coroutine_context::value<> value_0;
//     call_later_0(context.get_caller(value_0));
//     value_0.get();
//     std::cout << "Just continued." << std::endl;
//     
//     dkuk::coroutine_context::value<int> value_1;
//     call_later_1(context.get_caller(value_1));
//     int res_1 = value_1.get();
//     std::cout << "Got: " << res_1 << '.' << std::endl;
//     
//     dkuk::coroutine_context::value<int, std::string> value_2;
//     call_later_2(context.get_caller(value_2));
//     std::tuple<int, std::string> res_2 = value_2.get();
//     std::cout << "Got: " << std::get<0>(res_2) << ' ' << std::get<1>(res_2) << '.' << std::endl;
// }
// 
// 
// Spawn signatures:
//     spawn(
//            <strand or io_context or coroutine_context>
//         [, std::allocator_arg, stack_alloc [, boost::context::preallocated],]
//          , <function object with signature: void (Args &&..., coroutine_context)>
//         [, Args... args]
//     )
// 
// NOTE:
// - If strand gived, new coroutine will be attached to it. Use this for serialize several coroutines/etc.
// - If io_context given, new strand will be created.
// - If coroutine_context given, new strand will be created with given coroutine's io_context.
// - Args will be passed as object, not references (like std::thread). See std::ref().
// - Args and allocators are optional.


#ifndef DKUK_SPAWN_HPP
#define DKUK_SPAWN_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <boost/asio/async_result.hpp>
#include <boost/asio/handler_type.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/context/continuation.hpp>
#include <boost/optional.hpp>
#include <boost/system/error_code.hpp>


namespace dkuk {


class coroutine_expired: public std::runtime_error
{
public:
	inline
	coroutine_expired():
		std::runtime_error{"Coroutine expired"}
	{}
};	// class coroutine_expired



class coroutine_context
{
private:
	class coro_data: public std::enable_shared_from_this<coro_data>
	{
	public:
		template<class Fn, class... Args>
		inline
		coro_data(
			boost::asio::io_context::strand strand,
			Fn &&fn,
			Args &&... args
		):
			coro_caller_{
				boost::context::callcc(
					this->wrap_fn(std::forward<Fn>(fn), std::forward<Args>(args)...)
				)
			},
			strand_{std::move(strand)}
		{}
		
		
		template<class StackAlloc, class Fn, class... Args>
		inline
		coro_data(
			boost::asio::io_context::strand strand,
			std::allocator_arg_t,
			StackAlloc salloc,
			Fn &&fn,
			Args &&... args
		):
			coro_caller_{
				boost::context::callcc(
					std::allocator_arg, std::move(salloc),
					this->wrap_fn(std::forward<Fn>(fn), std::forward<Args>(args)...)
				)
			},
			strand_{std::move(strand)}
		{}
		
		
		template<class StackAlloc, class Fn, class... Args>
		inline
		coro_data(
			boost::asio::io_context::strand strand,
			std::allocator_arg_t,
			boost::context::preallocated palloc,
			StackAlloc salloc,
			Fn &&fn,
			Args &&... args
		):
			coro_caller_{
				boost::context::callcc(
					std::allocator_arg, std::move(salloc), std::move(palloc),
					this->wrap_fn(std::forward<Fn>(fn), std::forward<Args>(args)...)
				)
			},
			strand_{std::move(strand)}
		{}
		
		
		coro_data(
			const coro_data &other
		) = delete;
		
		
		coro_data &
		operator=(
			const coro_data &other
		) = delete;
		
		
		coro_data(
			coro_data &&other
		) = delete;
		
		
		coro_data &
		operator=(
			coro_data &&other
		) = delete;
		
		
		inline
		boost::asio::io_context::strand &
		strand() noexcept
		{
			return this->strand_;
		}
		
		
		inline
		void
		coro_start()
		{
			boost::asio::post(this->strand(), coroutine_context::primitive_caller{this->shared_from_this()});
		}
		
		
		inline
		void
		coro_call()
		{
			if (!this->coro_caller_)
				throw coroutine_expired{};
			this->coro_caller_ = this->coro_caller_.resume();
			if (this->exception_ptr_)
				std::rethrow_exception(this->exception_ptr_);
		}
		
		
		inline
		void
		coro_yield()
		{
			if (!*this->coro_execution_context_ptr_)
				throw coroutine_expired{};
			*this->coro_execution_context_ptr_ = this->coro_execution_context_ptr_->resume();
		}
	private:
		template<class Fn, class ArgsTuple, std::size_t... Is>
		static inline
		void
		apply_impl(
			coroutine_context &&context,
			Fn &&fn,
			ArgsTuple &&args_tuple,
			const std::index_sequence<Is...> * = nullptr
		)
		{
			std::forward<Fn>(fn)(std::move(std::get<Is>(std::forward<ArgsTuple>(args_tuple)))..., std::move(context));
		}
		
		
		template<class Fn, class... Args>
		inline
		auto
		wrap_fn(
			Fn &&fn,
			Args &&... args
		)
		{
			return
				[
					this,
					fn = std::forward<Fn>(fn),
					args_tuple = std::tuple<std::decay_t<Args>...>{std::forward<Args>(args)...}
				](
					boost::context::continuation &&coro_execution_context
				) mutable
				{
					// False-start (thanks to Boost.Context in Boost 1.64)
					this->coro_execution_context_ptr_ = std::addressof(coro_execution_context);
					this->coro_yield();
					
					// Coroutine init
					coroutine_context context{this->shared_from_this()};
					
					// Coroutine start
					try {
						coro_data::apply_impl(
							std::move(context),
							std::move(fn),
							std::move(args_tuple),
							static_cast<const std::make_index_sequence<sizeof...(Args)> *>(nullptr)
						);
					} catch (const std::exception & /* e */) {
						this->exception_ptr_ = std::current_exception();
					}
					
					return std::move(coro_execution_context);
				};
		}
		
		
		
		boost::context::continuation coro_caller_, *coro_execution_context_ptr_;
		boost::asio::io_context::strand strand_;
		std::exception_ptr exception_ptr_;
	};	// class coro_data
	
	
	
	class primitive_caller
	{
	public:
		inline
		primitive_caller(
			std::shared_ptr<coro_data> coro_data_ptr
		) noexcept:
			coro_data_ptr_{std::move(coro_data_ptr)}
		{}
		
		
		primitive_caller(
			const primitive_caller &other
		) = default;
		
		
		primitive_caller &
		operator=(
			const primitive_caller &other
		) = default;
		
		
		primitive_caller(
			primitive_caller &&other
		) = default;
		
		
		primitive_caller &
		operator=(
			primitive_caller &&other
		) = default;
		
		
		inline
		void
		operator()() const
		{
			this->coro_data_ptr_->coro_call();
		}
	private:
		std::shared_ptr<coro_data> coro_data_ptr_;
	};	// class primitive_caller
public:
	template<class... Ts>
	class value;
	
	template<class... Ts>
	class caller;
	
	
	
	coroutine_context(
		const coroutine_context &other
	) = default;
	
	
	coroutine_context &
	operator=(
		const coroutine_context &other
	) = default;
	
	
	coroutine_context(
		coroutine_context &&other
	) = default;
	
	
	coroutine_context &
	operator=(
		coroutine_context &&other
	) = default;
	
	
	inline
	boost::asio::io_context::strand &
	get_executor() const
	{
		const auto coro_data_ptr = this->lock_();
		return coro_data_ptr->strand();
	}
	
	
	template<class... Ts>
	inline
	caller<Ts...>
	get_caller(
		typename caller<Ts...>::value_type &value
	) const
	{
		return coroutine_context::caller<Ts...>{*this, value};
	}
	
	
	template<class... Ts>
	inline
	caller<Ts...>
	get_caller() const
	{
		return coroutine_context::caller<Ts...>{*this};
	}
	
	
	inline
	coroutine_context
	operator[](
		boost::system::error_code &ec
	) const noexcept
	{
		coroutine_context res = *this;
		res.ec_ptr_ = std::addressof(ec);
		return res;
	}
private:
	inline
	coroutine_context(
		const std::shared_ptr<coro_data> &coro_data_ptr
	) noexcept:
		weak_coro_data_ptr_{coro_data_ptr}
	{}
	
	
	static inline
	void
	continue_(std::shared_ptr<coro_data> coro_data_ptr)
	{
		coro_data_ptr->coro_start();
	}
	
	
	inline
	void
	yield_() const
	{
		const auto raw_coro_data_ptr = this->lock_().get();	// Don't share ownership while suspended!
		raw_coro_data_ptr->coro_yield();
	}
	
	
	inline
	std::shared_ptr<coro_data>
	lock_() const
	{
		auto res = this->weak_coro_data_ptr_.lock();
		if (res == nullptr)
			throw coroutine_expired{};
		return res;
	}
	
	
	inline
	boost::system::error_code &
	best_ec_(
		boost::system::error_code &internal_ec
	) const noexcept
	{
		if (this->ec_ptr_ != nullptr)
			return *this->ec_ptr_;
		return internal_ec;
	}
	
	
	
	std::weak_ptr<coro_data> weak_coro_data_ptr_;
	boost::system::error_code *ec_ptr_ = nullptr;
	
	
	
	template<class T>
	class coroutine_future_state;
	
	
	
	template<class... Ts>
	friend class value;
	
	template<class... Ts>
	friend class caller;
	
	template<class T>
	friend class coroutine_future;
	
	template<class T>
	friend class coroutine_promise;
	
	template<class... CoroArgs>
	friend inline void spawn(boost::asio::io_context::strand strand, CoroArgs &&... coro_args);
};	// class coroutine_context



// Without error code
template<class T1, class T2, class... Ts>
class coroutine_context::value<T1, T2, Ts...>
{
public:
	using type = std::tuple<T1, T2, Ts...>;
	
	
	
	inline
	value(
		coroutine_context context
	) noexcept:
		context_{std::move(context)}
	{}
	
	
	template<class... Args>
	inline
	bool
	set(
		Args &&... args
	)
	{
		if (++this->ready_ == 2) {
			this->value_ = std::forward_as_tuple(std::forward<Args>(args)...);
			return true;
		}
		return false;
	}
	
	
	inline
	type &&
	get()
	{
		if (++this->ready_ != 2)
			this->context_.yield_();
		return std::move(this->value_);
	}
private:
	coroutine_context context_;
	std::atomic<unsigned int> ready_{0};
	type value_;
};	// class coroutine_context::value<T1, T2, Ts...>



template<class T1>
class coroutine_context::value<T1>
{
public:
	using type = T1;
	
	
	
	inline
	value(
		coroutine_context context
	) noexcept:
		context_{std::move(context)}
	{}
	
	
	template<class Arg>
	inline
	bool
	set(
		Arg &&arg
	)
	{
		if (++this->ready_ == 2) {
			this->value_ = std::forward<Arg>(arg);
			return true;
		}
		return false;
	}
	
	
	inline
	type &&
	get()
	{
		if (++this->ready_ != 2)
			this->context_.yield_();
		return std::move(this->value_);
	}
private:
	coroutine_context context_;
	std::atomic<unsigned int> ready_{0};
	type value_;
};	// class coroutine_context::value<T1>



template<>
class coroutine_context::value<>
{
public:
	using type = void;
	
	
	
	inline
	value(
		coroutine_context context
	) noexcept:
		context_{std::move(context)}
	{}
	
	
	inline
	bool
	set()
	{
		if (++this->ready_ == 2)
			return true;
		return false;
	}
	
	
	inline
	void
	get()
	{
		if (++this->ready_ != 2)
			this->context_.yield_();
	}
private:
	coroutine_context context_;
	std::atomic<unsigned int> ready_{0};
};	// class coroutine_context::value<>



// With error code
template<class T1, class T2, class... Ts>
class coroutine_context::value<boost::system::error_code, T1, T2, Ts...>
{
public:
	using type = std::tuple<T1, T2, Ts...>;
	
	
	
	inline
	value(
		coroutine_context context
	) noexcept:
		context_{std::move(context)}
	{}
	
	
	template<class... Args>
	inline
	bool
	set(
		boost::system::error_code ec,
		Args &&... args
	)
	{
		if (++this->ready_ == 2) {
			this->context_.best_ec_(this->ec_) = std::move(ec);
			this->value_ = std::forward_as_tuple(std::forward<Args>(args)...);
			return true;
		}
		return false;
	}
	
	
	inline
	type &&
	get()
	{
		if (++this->ready_ != 2)
			this->context_.yield_();
		if (this->ec_)	// Don't assert external ec, if it is set!
			throw boost::system::system_error{this->ec_};
		return std::move(this->value_);
	}
private:
	coroutine_context context_;
	std::atomic<unsigned int> ready_{0};
	boost::system::error_code ec_;
	type value_;
};	// class coroutine_context::value<boost::system::error_code, T1, T2, Ts...>



template<class T1>
class coroutine_context::value<boost::system::error_code, T1>
{
public:
	using type = T1;
	
	
	
	inline
	value(
		coroutine_context context
	) noexcept:
		context_{std::move(context)}
	{}
	
	
	template<class Arg>
	inline
	bool
	set(
		boost::system::error_code ec,
		Arg &&arg
	)
	{
		if (++this->ready_ == 2) {
			this->context_.best_ec_(this->ec_) = std::move(ec);
			this->value_ = std::forward<Arg>(arg);
			return true;
		}
		return false;
	}
	
	
	inline
	type &&
	get()
	{
		if (++this->ready_ != 2)
			this->context_.yield_();
		if (this->ec_)	// Don't assert external ec, if it is set!
			throw boost::system::system_error{this->ec_};
		return std::move(this->value_);
	}
private:
	coroutine_context context_;
	std::atomic<unsigned int> ready_{0};
	boost::system::error_code ec_;
	type value_;
};	// class coroutine_context::value<boost::system::error_code, T1>



template<>
class coroutine_context::value<boost::system::error_code>
{
public:
	using type = void;
	
	
	
	inline
	value(
		coroutine_context context
	) noexcept:
		context_{std::move(context)}
	{}
	
	
	inline
	bool
	set(
		boost::system::error_code ec
	)
	{
		if (++this->ready_ == 2) {
			this->context_.best_ec_(this->ec_) = std::move(ec);
			return true;
		}
		return false;
	}
	
	
	inline
	void
	get()
	{
		if (++this->ready_ != 2)
			this->context_.yield_();
		if (this->ec_)	// Don't assert external ec, if it is set!
			throw boost::system::system_error{this->ec_};
	}
private:
	coroutine_context context_;
	std::atomic<unsigned int> ready_{0};
	boost::system::error_code ec_;
};	// class coroutine_context::value<boost::system::error_code>



template<class... Ts>
class coroutine_context::caller
{
public:
	using value_type = coroutine_context::value<Ts...>;
	
	
	
	inline
	caller(
		const coroutine_context &c
	):
		coro_data_ptr_{c.lock_()}
	{}
	
	
	inline
	caller(
		const coroutine_context &c,
		value_type &value
	):
		coro_data_ptr_{c.lock_()},
		value_ptr_{std::addressof(value)}
	{}
	
	
	caller(
		const caller &other
	) = default;
	
	
	caller &
	operator=(
		const caller &other
	) = default;
	
	
	caller(
		caller &&other
	) = default;
	
	
	caller &
	operator=(
		caller &&other
	) = default;
	
	
	inline
	boost::asio::io_context::strand &
	get_executor() const noexcept
	{
		return this->coro_data_ptr_->strand();
	}
	
	
	inline
	coroutine_context
	get_context() const noexcept
	{
		return this->coro_data_ptr_;
	}
	
	
	inline
	void
	bind_value(
		value_type &value
	) noexcept
	{
		this->value_ptr_ = std::addressof(value);
	}
	
	
	template<class... Args>
	inline
	void
	operator()(
		Args &&... args
	) const
	{
		if (this->value_ptr_ == nullptr)
			throw std::logic_error{"Incorrect coroutine caller: Value not bound"};
		if (this->value_ptr_->set(std::forward<Args>(args)...))
			coroutine_context::continue_(this->coro_data_ptr_);
	}
private:
	std::shared_ptr<coroutine_context::coro_data> coro_data_ptr_;
	value_type *value_ptr_ = nullptr;
};	// class coroutine_context::caller



template<class T>
class coroutine_context::coroutine_future_state
{
public:
	inline
	coroutine_future_state(
		boost::asio::io_context &io_context
	) noexcept:
		io_context_ptr_{&io_context},
		ready_{false}
	{}
	
	
	inline
	bool
	ready() const
	{
		return this->ready_;
	}
	
	
	template<class T1>
	inline
	void
	set_value(T1 &&value)
	{
		std::unique_lock<std::mutex> lock{this->mutex_};
		if (this->ready())
			throw std::future_error{std::future_errc::promise_already_satisfied};
		
		this->value_.emplace(std::forward<T1>(value));
		this->notify_ready_();
	}
	
	
	inline
	void
	set_exception(
		std::exception_ptr exception_ptr
	)
	{
		std::lock_guard<std::mutex> lock{this->mutex_};
		if (this->ready())
			throw std::future_error{std::future_errc::promise_already_satisfied};
		
		this->exception_ptr_ = std::move(exception_ptr);
		this->notify_ready_();
	}
	
	
	inline
	T
	get()
	{
		this->wait();
		if (this->exception_ptr_ != nullptr)
			std::rethrow_exception(this->exception_ptr_);
		return this->value_.get();
	}
	
	
	inline
	void
	wait()
	{
		std::mutex mutex;
		std::unique_lock<std::mutex> lock{mutex};
		this->ready_condition_.wait(
			lock,
			[&ready = this->ready_]() noexcept -> bool { return ready; }
		);
	}
	
	
	template<class Rep, class Period>
	inline
	std::future_status
	wait_for(
		const std::chrono::duration<Rep, Period> &timeout_duration
	)
	{
		std::mutex mutex;
		std::unique_lock<std::mutex> lock{mutex};
		bool ready =
			this->ready_condition_.wait_for(
				lock,
				timeout_duration,
				[&ready = this->ready_]() noexcept -> bool { return ready; }
			);
		return (ready)? std::future_status::ready: std::future_status::timeout;
	}
	
	
	template<class Clock, class Duration>
	inline
	std::future_status
	wait_until(
		const std::chrono::time_point<Clock, Duration> &timeout_time
	)
	{
		std::mutex mutex;
		std::unique_lock<std::mutex> lock{mutex};
		bool ready =
			this->ready_condition_.wait_for(
				lock,
				timeout_time,
				[&ready = this->ready_]() noexcept -> bool { return ready; }
			);
		return (ready)? std::future_status::ready: std::future_status::timeout;
	}
	
	
	template<class Handler>
	inline
	typename boost::asio::async_result<Handler, void ()>::result_type
	async_wait(
		Handler &&handler
	)
	{
		boost::asio::async_completion<Handler, void ()> init{handler};
		std::unique_lock<std::mutex> lock{this->mutex_};
		if (this->ready()) {
			lock.unlock();
			boost::asio::post(*this->io_context_ptr_, std::move(init.completion_handler));
		} else {
			this->handlers_.emplace_back(std::move(init.completion_handler));
		}
		return init.result.get();
	}
private:
	inline
	void
	notify_ready_()
	{
		this->ready_.store(true, std::memory_order_release);
		
		this->ready_condition_.notify_all();
		
		for (auto &handler: this->handlers_)
			boost::asio::post(*this->io_context_ptr_, std::move(handler));
		this->handlers_.clear();
		this->handlers_.shrink_to_fit();
	}
	
	
	
	boost::asio::io_context *io_context_ptr_;
	std::mutex mutex_;
	std::condition_variable ready_condition_;
	std::vector<std::function<void ()>> handlers_;
	std::atomic<bool> ready_;
	boost::optional<T> value_;
	std::exception_ptr exception_ptr_;
};	// class coroutine_context::coroutine_future_state



template<class T>
class coroutine_context::coroutine_future_state<T &>
{
public:
	inline
	coroutine_future_state(
		boost::asio::io_context &io_context
	) noexcept:
		io_context_ptr_{&io_context},
		ready_{false}
	{}
	
	
	inline
	bool
	ready() const
	{
		return this->ready_;
	}
	
	
	inline
	void
	set_value(T &value)
	{
		std::unique_lock<std::mutex> lock{this->mutex_};
		if (this->ready())
			throw std::future_error{std::future_errc::promise_already_satisfied};
		
		this->value_ptr_ = std::addressof(value);
		this->notify_ready_();
	}
	
	
	inline
	void
	set_exception(
		std::exception_ptr exception_ptr
	)
	{
		std::lock_guard<std::mutex> lock{this->mutex_};
		if (this->ready())
			throw std::future_error{std::future_errc::promise_already_satisfied};
		this->exception_ptr_ = std::move(exception_ptr);
		this->notify_ready_();
	}
	
	
	inline
	T &
	get()
	{
		this->wait();
		if (this->exception_ptr_ != nullptr)
			std::rethrow_exception(this->exception_ptr_);
		return *this->value_ptr_;
	}
	
	
	inline
	void
	wait()
	{
		std::mutex mutex;
		std::unique_lock<std::mutex> lock{mutex};
		this->ready_condition_.wait(
			lock,
			[&ready = this->ready_]() noexcept -> bool { return ready; }
		);
	}
	
	
	template<class Rep, class Period>
	inline
	std::future_status
	wait_for(
		const std::chrono::duration<Rep, Period> &timeout_duration
	)
	{
		std::mutex mutex;
		std::unique_lock<std::mutex> lock{mutex};
		bool ready =
			this->ready_condition_.wait_for(
				lock,
				timeout_duration,
				[&ready = this->ready_]() noexcept -> bool { return ready; }
			);
		return (ready)? std::future_status::ready: std::future_status::timeout;
	}
	
	
	template<class Clock, class Duration>
	inline
	std::future_status
	wait_until(
		const std::chrono::time_point<Clock, Duration> &timeout_time
	)
	{
		std::mutex mutex;
		std::unique_lock<std::mutex> lock{mutex};
		bool ready =
			this->ready_condition_.wait_for(
				lock,
				timeout_time,
				[&ready = this->ready_]() noexcept -> bool { return ready; }
			);
		return (ready)? std::future_status::ready: std::future_status::timeout;
	}
	
	
	template<class Handler>
	inline
	typename boost::asio::async_result<Handler, void ()>::result_type
	async_wait(
		Handler &&handler
	)
	{
		boost::asio::async_completion<Handler, void ()> init{handler};
		std::unique_lock<std::mutex> lock{this->mutex_};
		if (this->ready()) {
			lock.unlock();
			boost::asio::post(*this->io_context_ptr_, std::move(init.completion_handler));
		} else {
			this->handlers_.emplace_back(std::move(init.completion_handler));
		}
		return init.result.get();
	}
private:
	inline
	void
	notify_ready_()
	{
		this->ready_.store(true, std::memory_order_release);
		
		this->ready_condition_.notify_all();
		
		for (auto &handler: this->handlers_)
			boost::asio::post(*this->io_context_ptr_, std::move(handler));
		this->handlers_.clear();
		this->handlers_.shrink_to_fit();
	}
	
	
	
	boost::asio::io_context *io_context_ptr_;
	std::mutex mutex_;
	std::condition_variable ready_condition_;
	std::vector<std::function<void ()>> handlers_;
	std::atomic<bool> ready_;
	T * value_ptr_;
	std::exception_ptr exception_ptr_;
};	// class coroutine_context::coroutine_future_state<T &>



template<>
class coroutine_context::coroutine_future_state<void>
{
public:
	inline
	coroutine_future_state(
		boost::asio::io_context &io_context
	) noexcept:
		io_context_ptr_{&io_context},
		ready_{false}
	{}
	
	
	inline
	bool
	ready() const
	{
		return this->ready_;
	}
	
	
	inline
	void
	set_value()
	{
		std::unique_lock<std::mutex> lock{this->mutex_};
		if (this->ready())
			throw std::future_error{std::future_errc::promise_already_satisfied};
		
		this->notify_ready_();
	}
	
	
	inline
	void
	set_exception(
		std::exception_ptr exception_ptr
	)
	{
		std::lock_guard<std::mutex> lock{this->mutex_};
		if (this->ready())
			throw std::future_error{std::future_errc::promise_already_satisfied};
		this->exception_ptr_ = std::move(exception_ptr);
		this->notify_ready_();
	}
	
	
	inline
	void
	get()
	{
		this->wait();
		if (this->exception_ptr_ != nullptr)
			std::rethrow_exception(this->exception_ptr_);
	}
	
	
	inline
	void
	wait()
	{
		std::mutex mutex;
		std::unique_lock<std::mutex> lock{mutex};
		this->ready_condition_.wait(
			lock,
			[&ready = this->ready_]() noexcept -> bool { return ready; }
		);
	}
	
	
	template<class Rep, class Period>
	inline
	std::future_status
	wait_for(
		const std::chrono::duration<Rep, Period> &timeout_duration
	)
	{
		std::mutex mutex;
		std::unique_lock<std::mutex> lock{mutex};
		bool ready =
			this->ready_condition_.wait_for(
				lock,
				timeout_duration,
				[&ready = this->ready_]() noexcept -> bool { return ready; }
			);
		return (ready)? std::future_status::ready: std::future_status::timeout;
	}
	
	
	template<class Clock, class Duration>
	inline
	std::future_status
	wait_until(
		const std::chrono::time_point<Clock, Duration> &timeout_time
	)
	{
		std::mutex mutex;
		std::unique_lock<std::mutex> lock{mutex};
		bool ready =
			this->ready_condition_.wait_for(
				lock,
				timeout_time,
				[&ready = this->ready_]() noexcept -> bool { return ready; }
			);
		return (ready)? std::future_status::ready: std::future_status::timeout;
	}
	
	
	template<class Handler>
	inline
	typename boost::asio::async_result<Handler, void ()>::result_type
	async_wait(
		Handler &&handler
	)
	{
		boost::asio::async_completion<Handler, void ()> init{handler};
		std::unique_lock<std::mutex> lock{this->mutex_};
		if (this->ready()) {
			lock.unlock();
			boost::asio::post(*this->io_context_ptr_, std::move(init.completion_handler));
		} else {
			this->handlers_.emplace_back(std::move(init.completion_handler));
		}
		return init.result.get();
	}
private:
	inline
	void
	notify_ready_()
	{
		this->ready_.store(true, std::memory_order_release);
		
		this->ready_condition_.notify_all();
		
		for (auto &handler: this->handlers_)
			boost::asio::post(*this->io_context_ptr_, std::move(handler));
		this->handlers_.clear();
		this->handlers_.shrink_to_fit();
	}
	
	
	
	boost::asio::io_context *io_context_ptr_;
	std::mutex mutex_;
	std::condition_variable ready_condition_;
	std::vector<std::function<void ()>> handlers_;
	std::atomic<bool> ready_;
	std::exception_ptr exception_ptr_;
};	// class coroutine_context::coroutine_future_state<void>



template<class T>
class coroutine_future
{
public:
	inline
	coroutine_future(
		boost::asio::io_context &io_context
	) noexcept:
		io_context_ptr_{&io_context}
	{}
	
	
	coroutine_future(
		coroutine_future &&other
	) = default;
	
	
	coroutine_future &
	operator=(
		coroutine_future &&other
	) = default;
	
	
	coroutine_future(
		const coroutine_future &other
	) = default;
	
	
	coroutine_future &
	operator=(
		const coroutine_future &other
	) = default;
	
	
	inline
	std::future<T>
	get_std_future()
	{
		const std::shared_ptr<coroutine_context::coroutine_future_state<T>> state_ptr = this->state_ptr_;
		if (state_ptr == nullptr)
			return std::future<T>{};
		
		std::promise<T> promise;
		std::future<T> future = promise.get_future();
		this->async_wait(
			[state_ptr = std::move(state_ptr), promise = std::move(promise)]
			{
				try {
					promise.set_value(state_ptr->get());
				} catch (const std::exception & /* e */) {
					promise.set_exception(std::current_exception());
				}
			}
		);
		return future;
	}
	
	
	inline
	bool
	ready() const
	{
		coroutine_context::coroutine_future_state<T> * const state_ptr = this->state_ptr_.get();
		if (state_ptr != nullptr)
			return state_ptr->ready();
		throw std::future_error{std::future_errc::no_state};
	}
	
	
	inline
	auto
	get() const
		-> decltype(auto)
	{
		coroutine_context::coroutine_future_state<T> * const state_ptr = this->state_ptr_.get();
		if (state_ptr != nullptr)
			return state_ptr->get();
		throw std::future_error{std::future_errc::no_state};
	}
	
	
	inline
	bool
	valid() const noexcept
	{
		return (this->state_ptr_ != nullptr)? true: false;
	}
	
	
	inline
	void
	wait() const
	{
		coroutine_context::coroutine_future_state<T> * const state_ptr = this->state_ptr_.get();
		if (state_ptr != nullptr)
			return state_ptr->wait();
		throw std::future_error{std::future_errc::no_state};
	}
	
	
	template<class Rep, class Period>
	inline
	std::future_status
	wait_for(
		const std::chrono::duration<Rep, Period> &timeout_duration
	) const
	{
		coroutine_context::coroutine_future_state<T> * const state_ptr = this->state_ptr_.get();
		if (state_ptr != nullptr)
			return state_ptr->wait_for(timeout_duration);
		throw std::future_error{std::future_errc::no_state};
	}
	
	
	template<class Clock, class Duration>
	inline
	std::future_status
	wait_until(
		const std::chrono::time_point<Clock, Duration> &timeout_time
	) const
	{
		coroutine_context::coroutine_future_state<T> * const state_ptr = this->state_ptr_.get();
		if (state_ptr != nullptr)
			return state_ptr->wait_until(timeout_time);
		throw std::future_error{std::future_errc::no_state};
	}
	
	
	template<class Handler>
	inline
	typename boost::asio::async_result<Handler, void ()>::result_type
	async_wait(
		Handler &&handler
	) const
	{
		coroutine_context::coroutine_future_state<T> * const state_ptr = this->state_ptr_.get();
		if (state_ptr != nullptr)
			return state_ptr->async_wait(std::forward<Handler>(handler));
		throw std::future_error{std::future_errc::no_state};
	}
private:
	template<class T1>
	friend class coroutine_promise;
	
	
	
	inline
	coroutine_future(
		boost::asio::io_context &io_context,
		std::shared_ptr<coroutine_context::coroutine_future_state<T>> state_ptr
	) noexcept:
		io_context_ptr_{&io_context},
		state_ptr_{std::move(state_ptr)}
	{}
	
	
	
	boost::asio::io_context *io_context_ptr_;
	std::shared_ptr<coroutine_context::coroutine_future_state<T>> state_ptr_;
};	// class coroutine_future



template<class T>
class coroutine_promise
{
public:
	inline
	coroutine_promise(
		boost::asio::io_context &io_context
	):
		io_context_ptr_{&io_context},
		state_ptr_{std::make_shared<coroutine_context::coroutine_future_state<T>>(io_context)}
	{}
	
	
	template<class Alloc>
	inline
	coroutine_promise(
		boost::asio::io_context &io_context,
		std::allocator_arg_t,
		const Alloc &alloc
	):
		io_context_ptr_{&io_context},
		state_ptr_{std::allocate_shared<coroutine_context::coroutine_future_state<T>>(alloc, io_context)}
	{}
	
	
	coroutine_promise(
		coroutine_promise &&other
	) = default;
	
	
	coroutine_promise &
	operator=(
		coroutine_promise &&other
	) = default;
	
	
	coroutine_promise(
		coroutine_promise &other
	) = default;
	
	
	coroutine_promise &
	operator=(
		coroutine_promise &other
	) = default;
	
	
	inline
	coroutine_future<T>
	get_future() const noexcept
	{
		return coroutine_future<T>{*this->io_context_ptr_, this->state_ptr_};
	}
	
	
	inline
	void
	set_value(
		T &&value
	)
	{
		coroutine_context::coroutine_future_state<T> * const state_ptr = this->state_ptr_.get();
		if (state_ptr != nullptr)
			return state_ptr->set_value(std::move(value));
		throw std::future_error{std::future_errc::no_state};
	}
	
	
	inline
	void
	set_value(
		const T &value
	)
	{
		coroutine_context::coroutine_future_state<T> * const state_ptr = this->state_ptr_.get();
		if (state_ptr != nullptr)
			return state_ptr->set_value(value);
		throw std::future_error{std::future_errc::no_state};
	}
	
	
	inline
	void
	set_exception(
		std::exception_ptr exception_ptr
	)
	{
		coroutine_context::coroutine_future_state<T> * const state_ptr = this->state_ptr_.get();
		if (state_ptr != nullptr)
			return state_ptr->set_exception(std::move(exception_ptr));
		throw std::future_error{std::future_errc::no_state};
	}
private:
	boost::asio::io_context *io_context_ptr_;
	std::shared_ptr<coroutine_context::coroutine_future_state<T>> state_ptr_;
};	// class coroutine_promise



template<class T>
class coroutine_promise<T &>
{
public:
	inline
	coroutine_promise(
		boost::asio::io_context &io_context
	):
		io_context_ptr_{&io_context},
		state_ptr_{std::make_shared<coroutine_context::coroutine_future_state<T &>>(io_context)}
	{}
	
	
	template<class Alloc>
	inline
	coroutine_promise(
		boost::asio::io_context &io_context,
		std::allocator_arg_t,
		const Alloc &alloc
	):
		io_context_ptr_{&io_context},
		state_ptr_{std::allocate_shared<coroutine_context::coroutine_future_state<T &>>(alloc, io_context)}
	{}
	
	
	coroutine_promise(
		coroutine_promise &&other
	) = default;
	
	
	coroutine_promise &
	operator=(
		coroutine_promise &&other
	) = default;
	
	
	coroutine_promise(
		coroutine_promise &other
	) = default;
	
	
	coroutine_promise &
	operator=(
		coroutine_promise &other
	) = default;
	
	
	inline
	coroutine_future<T &>
	get_future() const noexcept
	{
		return coroutine_future<T &>{*this->io_context_ptr_, this->state_ptr_};
	}
	
	
	inline
	void
	set_value(
		T &value
	)
	{
		coroutine_context::coroutine_future_state<T &> * const state_ptr = this->state_ptr_.get();
		if (state_ptr != nullptr)
			return state_ptr->set_value(value);
		throw std::future_error{std::future_errc::no_state};
	}
	
	
	inline
	void
	set_exception(
		std::exception_ptr exception_ptr
	)
	{
		coroutine_context::coroutine_future_state<T &> * const state_ptr = this->state_ptr_.get();
		if (state_ptr != nullptr)
			return state_ptr->set_exception(std::move(exception_ptr));
		throw std::future_error{std::future_errc::no_state};
	}
private:
	boost::asio::io_context *io_context_ptr_;
	std::shared_ptr<coroutine_context::coroutine_future_state<T &>> state_ptr_;
};	// class coroutine_promise<T &>



template<>
class coroutine_promise<void>
{
public:
	inline
	coroutine_promise(
		boost::asio::io_context &io_context
	):
		io_context_ptr_{&io_context},
		state_ptr_{std::make_shared<coroutine_context::coroutine_future_state<void>>(io_context)}
	{}
	
	
	template<class Alloc>
	inline
	coroutine_promise(
		boost::asio::io_context &io_context,
		std::allocator_arg_t,
		const Alloc &alloc
	):
		io_context_ptr_{&io_context},
		state_ptr_{std::allocate_shared<coroutine_context::coroutine_future_state<void>>(alloc, io_context)}
	{}
	
	
	coroutine_promise(
		coroutine_promise &&other
	) = default;
	
	
	coroutine_promise &
	operator=(
		coroutine_promise &&other
	) = default;
	
	
	coroutine_promise(
		coroutine_promise &other
	) = default;
	
	
	coroutine_promise &
	operator=(
		coroutine_promise &other
	) = default;
	
	
	inline
	coroutine_future<void>
	get_future() const noexcept
	{
		return coroutine_future<void>{*this->io_context_ptr_, this->state_ptr_};
	}
	
	
	inline
	void
	set_value()
	{
		coroutine_context::coroutine_future_state<void> * const state_ptr = this->state_ptr_.get();
		if (state_ptr != nullptr)
			return state_ptr->set_value();
		throw std::future_error{std::future_errc::no_state};
	}
	
	
	inline
	void
	set_exception(
		std::exception_ptr exception_ptr
	)
	{
		coroutine_context::coroutine_future_state<void> * const state_ptr = this->state_ptr_.get();
		if (state_ptr != nullptr)
			return state_ptr->set_exception(std::move(exception_ptr));
		throw std::future_error{std::future_errc::no_state};
	}
private:
	boost::asio::io_context *io_context_ptr_;
	std::shared_ptr<coroutine_context::coroutine_future_state<void>> state_ptr_;
};	// class coroutine_promise<void>



template<class... CoroArgs>
inline
void
spawn(
	boost::asio::io_context::strand strand,
	CoroArgs &&... coro_args
)
{
	coroutine_context::continue_(
		std::make_shared<coroutine_context::coro_data>(std::move(strand), std::forward<CoroArgs>(coro_args)...)
	);
}


template<class... CoroArgs>
inline
void
spawn(
	boost::asio::io_context &io_context,
	CoroArgs &&... coro_args
)
{
	return spawn(boost::asio::io_context::strand{io_context}, std::forward<CoroArgs>(coro_args)...);
}


template<class... CoroArgs>
inline
void
spawn(
	const coroutine_context &context,
	CoroArgs &&... coro_args
)
{
	return spawn(context.get_executor().context(), std::forward<CoroArgs>(coro_args)...);
}



template<class Fn, class... Args>
inline
auto
spawn_with_future(
	boost::asio::io_context::strand strand,
	Fn &&fn,
	Args &&... args
)
{
	using result_type = decltype(std::forward<Fn>(fn)(std::move(args)..., std::declval<coroutine_context>()));
	
	coroutine_promise<result_type> result_promise{strand.context()};
	coroutine_future<result_type> result_future = result_promise.get_future();
	
	spawn(
		std::move(strand),
		
		[fn = std::forward<Fn>(fn), result_promise = std::move(result_promise)](auto &&... args) mutable
		{
			try {
				result_promise.set_value(std::move(fn)(std::move(args)...));
			} catch (const std::exception & /* e */) {
				result_promise.set_exception(std::current_exception());
			}
		},
		
		std::forward<Args>(args)...
	);
	
	return result_future;
}


template<class StackAlloc, class Fn, class... Args>
inline
auto
spawn_with_future(
	boost::asio::io_context::strand strand,
	std::allocator_arg_t,
	StackAlloc salloc,
	Fn &&fn,
	Args &&... args
)
{
	using result_type = decltype(std::forward<Fn>(fn)(std::move(args)..., std::declval<coroutine_context>()));
	
	coroutine_promise<result_type> result_promise{strand.context()};
	coroutine_future<result_type> result_future = result_promise.get_future();
	
	spawn(
		std::move(strand),
		
		std::allocator_arg,
		std::move(salloc),
		
		[fn = std::forward<Fn>(fn), result_promise = std::move(result_promise)](auto &&... args) mutable
		{
			try {
				result_promise.set_value(std::move(fn)(std::move(args)...));
			} catch (const std::exception & /* e */) {
				result_promise.set_exception(std::current_exception());
			}
		},
		
		std::forward<Args>(args)...
	);
	
	return result_future;
}


template<class StackAlloc, class Fn, class... Args>
inline
auto
spawn_with_future(
	boost::asio::io_context::strand strand,
	std::allocator_arg_t,
	boost::context::preallocated palloc,
	StackAlloc salloc,
	Fn &&fn,
	Args &&... args
)
{
	using result_type = decltype(std::forward<Fn>(fn)(std::move(args)..., std::declval<coroutine_context>()));
	
	coroutine_promise<result_type> result_promise{strand.context()};
	coroutine_future<result_type> result_future = result_promise.get_future();
	
	spawn(
		std::move(strand),
		
		std::allocator_arg,
		std::move(palloc),
		std::move(salloc),
		
		[fn = std::forward<Fn>(fn), result_promise = std::move(result_promise)](auto &&... args) mutable
		{
			try {
				result_promise.set_value(std::move(fn)(std::move(args)...));
			} catch (const std::exception & /* e */) {
				result_promise.set_exception(std::current_exception());
			}
		},
		
		std::forward<Args>(args)...
	);
	
	return result_future;
}


template<class... CoroArgs>
inline
auto
spawn_with_future(
	boost::asio::io_context &io_context,
	CoroArgs &&... coro_args
)
{
	return spawn_with_future(boost::asio::io_context::strand{io_context}, std::forward<CoroArgs>(coro_args)...);
}


template<class... CoroArgs>
inline
auto
spawn_with_future(
	const coroutine_context &context,
	CoroArgs &&... coro_args
)
{
	return spawn_with_future(context.get_executor(), std::forward<CoroArgs>(coro_args)...);
}



template<class T, class Rep, class Period>
inline
coroutine_future<T>
run_until_complete(
	boost::asio::io_context &io_context,
	coroutine_future<T> result_future,
	std::chrono::duration<Rep, Period> timeout_duration
)
{
	while (!result_future.ready())
		io_context.run_one_for(timeout_duration);
	return result_future;
}


template<class T>
inline
coroutine_future<T>
run_until_complete(
	boost::asio::io_context &io_context,
	coroutine_future<T> result_future
)
{
	return run_until_complete(io_context, std::move(result_future), std::chrono::seconds{1});
}



namespace spawn_impl {


template<class T>
class void_or_rvalue
{
public:
	using type = T &&;
};	// class void_or_rvalue

template<>
class void_or_rvalue<void>
{
public:
	using type = void;
};	// class void_or_rvalue<void>


};	// namespace spawn_impl
};	// namespace dkuk



namespace boost {
namespace asio {


#define DKUK_SPAWN_DEFINE_ASIO_HANDLER_TYPE(src_type)						\
	template<class Ret, class... Args>										\
	struct handler_type<src_type, Ret (Args...)>							\
	{																		\
		using type = dkuk::coroutine_context::caller<Args...>;				\
	}	/* struct handler_type<src_type, Ret (Args...)> */


#define DKUK_SPAWN_DEFINE_ASIO_HANDLER_TYPE_ADD_REF(src_type)				\
	DKUK_SPAWN_DEFINE_ASIO_HANDLER_TYPE(src_type   );						\
	DKUK_SPAWN_DEFINE_ASIO_HANDLER_TYPE(src_type & );						\
	DKUK_SPAWN_DEFINE_ASIO_HANDLER_TYPE(src_type &&)


#define DKUK_SPAWN_DEFINE_ASIO_HANDLER_TYPE_SERIES(src_type)				\
	DKUK_SPAWN_DEFINE_ASIO_HANDLER_TYPE_ADD_REF(src_type               );	\
	DKUK_SPAWN_DEFINE_ASIO_HANDLER_TYPE_ADD_REF(src_type const         );	\
	DKUK_SPAWN_DEFINE_ASIO_HANDLER_TYPE_ADD_REF(src_type       volatile);	\
	DKUK_SPAWN_DEFINE_ASIO_HANDLER_TYPE_ADD_REF(src_type const volatile)


DKUK_SPAWN_DEFINE_ASIO_HANDLER_TYPE_SERIES(dkuk::coroutine_context);


#undef DKUK_SPAWN_DEFINE_ASIO_HELPERS_SERIES
#undef DKUK_SPAWN_DEFINE_ASIO_HANDLER_TYPE_ADD_REF
#undef DKUK_SPAWN_DEFINE_ASIO_HELPERS



template<class... Ts>
class async_result<dkuk::coroutine_context::caller<Ts...>>
{
public:
	using type = typename dkuk::coroutine_context::value<Ts...>::type;
	
	
	
	inline
	async_result(
		dkuk::coroutine_context::caller<Ts...> &c
	) noexcept:
		value_{c.get_context()}
	{
		c.bind_value(this->value_);
	}
	
	
	inline
	typename dkuk::spawn_impl::void_or_rvalue<type>::type
	get()
	{
		return this->value_.get();
	}
private:
	dkuk::coroutine_context::value<Ts...> value_;
};	// class async_result<dkuk::coroutine_context::caller<Ts...>>


};	// namespace asio
};	// namespace boost


#endif	// DKUK_SPAWN_HPP
