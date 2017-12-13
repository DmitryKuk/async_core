// Author: Dmitry Kukovinets (d1021976@gmail.com), 10.03.2017, 17:24


// Spawn signatures:
//     spawn(
//            <strand or io_service or coroutine_context>
//         [, std::allocator_arg, stack_alloc [, boost::context::preallocated],]
//          , <function object with signature: void (Args &&..., coroutine_context)>
//         [, Args... args]
//     )
// 
// NOTE:
// - If strand gived, new coroutine will be attached to it. Use this for serialize several coroutines/etc.
// - If io_service given, new strand will be created.
// - If coroutine_context given, new strand will be created with given coroutine's io_service.
// - Args will be passed as object, not references (like std::thread). See std::ref().
// - Args and allocators are optional.


#ifndef DKUK_SPAWN_HPP
#define DKUK_SPAWN_HPP

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <boost/asio/async_result.hpp>
#include <boost/asio/handler_type.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/strand.hpp>
#include <boost/context/all.hpp>
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
			boost::asio::io_service::strand &strand,
			Fn &&fn,
			Args &&... args
		):
			coro_caller_{
				boost::context::callcc(
					this->wrap_fn(std::forward<Fn>(fn), std::forward<Args>(args)...)
				)
			},
			strand_{strand}
		{}
		
		
		template<class StackAlloc, class Fn, class... Args>
		inline
		coro_data(
			boost::asio::io_service::strand &strand,
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
			strand_{strand}
		{}
		
		
		template<class StackAlloc, class Fn, class... Args>
		inline
		coro_data(
			boost::asio::io_service::strand &strand,
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
			strand_{strand}
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
		boost::asio::io_service::strand &
		strand() noexcept
		{
			return this->strand_;
		}
		
		
		inline
		void
		coro_start()
		{
			this->strand().post(coroutine_context::primitive_caller{this->shared_from_this()});
		}
		
		
		inline
		void
		coro_call()
		{
			if (!static_cast<bool>(this->coro_caller_))
				throw coroutine_expired{};
			this->coro_caller_ = this->coro_caller_.resume();
			if (this->exception_ptr_)
				std::rethrow_exception(this->exception_ptr_);
		}
		
		
		inline
		void
		coro_yield()
		{
			if (!static_cast<bool>(*this->coro_execution_context_ptr_))
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
			std::bind(
				std::move(fn),
				std::move(std::get<Is>(std::forward<ArgsTuple>(args_tuple)))...,
				std::move(context)
			)();
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
		boost::asio::io_service::strand strand_;
		std::exception_ptr exception_ptr_;
	};	// class coro_data
	
	
	
	class primitive_caller
	{
	public:
		inline
		primitive_caller(
			std::shared_ptr<coroutine_context::coro_data> coro_data_ptr
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
		std::shared_ptr<coroutine_context::coro_data> coro_data_ptr_;
	};	// class primitive_caller
public:
	template<class... Ts>
	class value;
	
	template<class... Ts>
	class asio_caller;
	
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
	boost::asio::io_service &
	get_io_service() const
	{
		const auto coro_data_ptr = this->lock();
		return coro_data_ptr->strand().get_io_service();
	}
	
	
	template<class... Ts>
	inline
	asio_caller<Ts...>
	get_asio_caller(
		std::tuple<Ts...> &value
	) const
	{
		return coroutine_context::asio_caller<Ts...>{*this, value};
	}
	
	
	template<class... Ts>
	inline
	asio_caller<Ts...>
	get_asio_caller() const
	{
		return coroutine_context::asio_caller<Ts...>{*this};
	}
	
	
	template<class... Ts>
	inline
	caller<Ts...>
	get_caller(
		std::tuple<Ts...> &value
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
	void
	yield() const
	{
		const auto raw_coro_data_ptr = this->lock().get();	// Don't share ownership while suspended!
		raw_coro_data_ptr->coro_yield();
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
	
	
	inline
	std::shared_ptr<coro_data>
	lock() const
	{
		auto res = this->weak_coro_data_ptr_.lock();
		if (res == nullptr)
			throw coroutine_expired{};
		return res;
	}
	
	
	inline
	boost::system::error_code &
	best_ec(
		boost::system::error_code &internal_ec
	) const noexcept
	{
		if (this->ec_ptr_ != nullptr)
			return *this->ec_ptr_;
		return internal_ec;
	}
	
	
	
	std::weak_ptr<coro_data> weak_coro_data_ptr_;
	boost::system::error_code *ec_ptr_ = nullptr;
	
	
	template<class... Ts>
	friend class value;
	
	template<class... CoroArgs>
	friend inline void spawn(boost::asio::io_service::strand strand, CoroArgs &&... coro_args);
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
	yield_and_get()
	{
		if (++this->ready_ != 2)
			this->context_.yield();
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
	yield_and_get()
	{
		if (++this->ready_ != 2)
			this->context_.yield();
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
	yield_and_get()
	{
		if (++this->ready_ != 2)
			this->context_.yield();
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
			this->context_.best_ec(this->ec_) = std::move(ec);
			this->value_ = std::forward_as_tuple(std::forward<Args>(args)...);
			return true;
		}
		return false;
	}
	
	
	inline
	type &&
	yield_and_get()
	{
		if (++this->ready_ != 2)
			this->context_.yield();
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
			this->context_.best_ec(this->ec_) = std::move(ec);
			this->value_ = std::forward<Arg>(arg);
			return true;
		}
		return false;
	}
	
	
	inline
	type &&
	yield_and_get()
	{
		if (++this->ready_ != 2)
			this->context_.yield();
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
			this->context_.best_ec(this->ec_) = std::move(ec);
			return true;
		}
		return false;
	}
	
	
	inline
	void
	yield_and_get()
	{
		if (++this->ready_ != 2)
			this->context_.yield();
		if (this->ec_)	// Don't assert external ec, if it is set!
			throw boost::system::system_error{this->ec_};
	}
private:
	coroutine_context context_;
	std::atomic<unsigned int> ready_{0};
	boost::system::error_code ec_;
};	// class coroutine_context::value<boost::system::error_code>



template<class... Ts>
class coroutine_context::asio_caller
{
public:
	using value_type = coroutine_context::value<Ts...>;
	
	
	
	inline
	asio_caller(
		const coroutine_context &c
	):
		coro_data_ptr_{c.lock()}
	{}
	
	
	inline
	asio_caller(
		const coroutine_context &c,
		value_type &value
	):
		coro_data_ptr_{c.lock()},
		value_ptr_{std::addressof(value)}
	{}
	
	
	asio_caller(
		const asio_caller &other
	) = default;
	
	
	asio_caller &
	operator=(
		const asio_caller &other
	) = default;
	
	
	asio_caller(
		asio_caller &&other
	) = default;
	
	
	asio_caller &
	operator=(
		asio_caller &&other
	) = default;
	
	
	inline
	boost::asio::io_service &
	get_io_service() const noexcept
	{
		return this->coro_data_ptr_->strand().get_io_service();
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
		if (this->value_ptr_->set(std::forward<Args>(args)...))	// Direct call
			this->coro_data_ptr_->coro_call();
	}
private:
	std::shared_ptr<coroutine_context::coro_data> coro_data_ptr_;
	value_type *value_ptr_ = nullptr;
};	// class coroutine_context::asio_caller



template<class... Ts>
class coroutine_context::caller
{
public:
	using value_type = coroutine_context::value<Ts...>;
	
	
	
	inline
	caller(
		const coroutine_context &c
	):
		coro_data_ptr_{c.lock()}
	{}
	
	
	inline
	caller(
		const coroutine_context &c,
		value_type &value
	):
		coro_data_ptr_{c.lock()},
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
	boost::asio::io_service &
	get_io_service() const noexcept
	{
		return this->coro_data_ptr_->strand().get_io_service();
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
		if (this->value_ptr_->set(std::forward<Args>(args)...))	// Guaranteed call (may be slower, than direct)
			this->coro_data_ptr_->strand().post(coroutine_context::primitive_caller{this->coro_data_ptr_});
	}
private:
	std::shared_ptr<coroutine_context::coro_data> coro_data_ptr_;
	value_type *value_ptr_ = nullptr;
};	// class coroutine_context::caller



template<class... CoroArgs>
inline
void
spawn(
	boost::asio::io_service::strand strand,
	CoroArgs &&... coro_args
)
{
	auto coro_data_ptr = std::make_shared<coroutine_context::coro_data>(strand, std::forward<CoroArgs>(coro_args)...);
	coro_data_ptr->coro_start();
}


template<class... CoroArgs>
inline
void
spawn(
	boost::asio::io_service &io_service,
	CoroArgs &&... coro_args
)
{
	return spawn(boost::asio::io_service::strand{io_service}, std::forward<CoroArgs>(coro_args)...);
}


template<class... CoroArgs>
inline
void
spawn(
	const coroutine_context &context,
	CoroArgs &&... coro_args
)
{
	return spawn(context.get_io_service(), std::forward<CoroArgs>(coro_args)...);
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
		using type = dkuk::coroutine_context::asio_caller<Args...>;			\
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
class async_result<dkuk::coroutine_context::asio_caller<Ts...>>
{
public:
	using type = typename dkuk::coroutine_context::value<Ts...>::type;
	
	
	
	inline
	async_result(
		dkuk::coroutine_context::asio_caller<Ts...> &c
	) noexcept:
		value_{c.get_context()}
	{
		c.bind_value(this->value_);
	}
	
	
	inline
	typename dkuk::spawn_impl::void_or_rvalue<type>::type
	get()
	{
		return this->value_.yield_and_get();
	}
private:
	dkuk::coroutine_context::value<Ts...> value_;
};	// class async_result<dkuk::coroutine_context::asio_caller<Ts...>>


};	// namespace asio
};	// namespace boost


#endif	// DKUK_SPAWN_HPP
