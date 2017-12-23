// Author: Dmitry Kukovinets (d1021976@gmail.com), 10.07.2017, 15:38

#include <iostream>
#include <string>
#include <utility>
#include <functional>
#include <exception>

#include <boost/asio/io_service.hpp>
#include <boost/system/error_code.hpp>

#include <dkuk/spawn.hpp>


namespace {


template<class... Args>
auto
async_apply(dkuk::coroutine_context context, Args... args)
	-> BOOST_ASIO_INITFN_RESULT_TYPE(dkuk::coroutine_context, void (Args...))
{
	auto &io_service = context.get_io_service();
	
	boost::asio::detail::async_result_init<dkuk::coroutine_context, void (Args...)>
		init{std::move(context)};
	
	io_service.post(std::bind(std::move(init.handler), std::move(args)...));
	
	return init.result.get();
}



template<class... Args>
void
check_no_args(dkuk::coroutine_context context, int &status, Args &&... args)
{
	static_assert(
		std::is_same<
			void,
			decltype(async_apply(std::move(context), std::forward<Args>(args)...))
		>::value,
		
		"Incorrect result type."
	);
	
	
	try {
		async_apply(std::move(context), std::forward<Args>(args)...);
	} catch (const std::exception &e) {
		status = 1;
		std::cout << "Error in check_no_args: " << e.what() << '.' << std::endl;
	}
}



template<class Res, class... Args>
void
check_args(dkuk::coroutine_context context, int &status, Res &&expected_result, Args &&... args)
{
	static_assert(
		std::is_same<
			std::decay_t<Res>,
			std::decay_t<decltype(async_apply(std::move(context), std::forward<Args>(args)...))>
		>::value,
		
		"Incorrect result type."
	);
	
	
	try {
		if (expected_result != async_apply(std::move(context), std::forward<Args>(args)...))
			throw std::logic_error{"Incorrect result"};
	} catch (const std::exception &e) {
		status = 1;
		std::cout << "Error in check_args: " << e.what() << '.' << std::endl;
	}
}


};	// namespace



int
main()
{
	int status = 0;
	boost::asio::io_service io_service;
	
	const int i = 100500;
	const std::string s = "hello, world";
	
	
	
	// Without boost::system::error_code
	dkuk::spawn(
		io_service,
		
		[&](auto context)
		{
			static_assert(
				std::is_same<
					void,
					std::decay_t<decltype(async_apply(std::move(context)))>
				>::value,
				"Incorrect result type."
			);
			
			try {
				async_apply(std::move(context));
			} catch (const std::exception &e) {
				status = 1;
				std::cout << __FILE__ << ':' << __LINE__ << ": Error: " << e.what() << '.' << std::endl;
			}
		}
	);
	
	
	dkuk::spawn(
		io_service,
		
		[&](auto context)
		{
			static_assert(
				std::is_same<
					int,
					std::decay_t<decltype(async_apply(std::move(context), i))>
				>::value,
				"Incorrect result type."
			);
			
			try {
				if (i != async_apply(std::move(context), i))
					throw std::logic_error{"Incorrect result"};
			} catch (const std::exception &e) {
				status = 1;
				std::cout << __FILE__ << ':' << __LINE__ << ": Error: " << e.what() << '.' << std::endl;
			}
		}
	);
	
	
	dkuk::spawn(
		io_service,
		
		[&](auto context)
		{
			static_assert(
				std::is_same<
					std::tuple<int, std::string>,
					std::decay_t<decltype(async_apply(std::move(context), i, s))>
				>::value,
				"Incorrect result type."
			);
			
			try {
				if (std::forward_as_tuple(i, s) != async_apply(std::move(context), i, s))
					throw std::logic_error{"Incorrect result"};
			} catch (const std::exception &e) {
				status = 1;
				std::cout << __FILE__ << ':' << __LINE__ << ": Error: " << e.what() << '.' << std::endl;
			}
		}
	);
	
	
	
	// With boost::system::error_code
	dkuk::spawn(
		io_service,
		
		[&](auto context)
		{
			static_assert(
				std::is_same<
					void,
					std::decay_t<decltype(async_apply(std::move(context), boost::system::error_code{}))>
				>::value,
				"Incorrect result type."
			);
			
			try {
				async_apply(std::move(context), boost::system::error_code{});
			} catch (const std::exception &e) {
				status = 1;
				std::cout << __FILE__ << ':' << __LINE__ << ": Error: " << e.what() << '.' << std::endl;
			}
		}
	);
	
	
	dkuk::spawn(
		io_service,
		
		[&](auto context)
		{
			static_assert(
				std::is_same<
					int,
					std::decay_t<decltype(async_apply(std::move(context), boost::system::error_code{}, i))>
				>::value,
				"Incorrect result type."
			);
			
			try {
				if (i != async_apply(std::move(context), boost::system::error_code{}, i))
					throw std::logic_error{"Incorrect result"};
			} catch (const std::exception &e) {
				status = 1;
				std::cout << __FILE__ << ':' << __LINE__ << ": Error: " << e.what() << '.' << std::endl;
			}
		}
	);
	
	
	dkuk::spawn(
		io_service,
		
		[&](auto context)
		{
			static_assert(
				std::is_same<
					std::tuple<int, std::string>,
					std::decay_t<decltype(async_apply(std::move(context), boost::system::error_code{}, i, s))>
				>::value,
				"Incorrect result type."
			);
			
			try {
				if (std::forward_as_tuple(i, s) != async_apply(std::move(context), boost::system::error_code{}, i, s))
					throw std::logic_error{"Incorrect result"};
			} catch (const std::exception &e) {
				status = 1;
				std::cout << __FILE__ << ':' << __LINE__ << ": Error: " << e.what() << '.' << std::endl;
			}
		}
	);
	
	
	io_service.run();
	
	return status;
}
