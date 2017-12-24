// Author: Dmitry Kukovinets (d1021976@gmail.com), 24.12.2017, 15:16

#include <boost/asio/io_context.hpp>
#include <boost/asio/system_timer.hpp>

#include <dkuk/coroutine.hpp>


namespace {


int
async_sum_2(int a, int b, dkuk::coroutine_context context)
{
	boost::asio::system_timer timer{context.get_executor().context(), std::chrono::seconds{1}};
	timer.async_wait(context);
	return a + b;
}


int
async_sum_3(int a, int b, int c, dkuk::coroutine_context context)
{
	return async_sum_2(a, b, context) + c;
}


};	// namespace



int
main()
{
	boost::asio::io_context context;
	
	auto f = dkuk::run_until_complete(context, dkuk::spawn_with_future(context, async_sum_3, 1, 2, 3));
	if (f.get() != 6)
		return 1;
	
	return 0;
}
