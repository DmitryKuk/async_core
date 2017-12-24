// Author: Dmitry Kukovinets (d1021976@gmail.com), 25.12.2017, 00:03

#include <exception>

#include <boost/asio/io_context.hpp>

#include <dkuk/spawn.hpp>


namespace {


int
sum_3(int a, int b, int c, dkuk::coroutine_context context)
{
	throw std::logic_error{"As expected"};
}


};	// namespace



int
main()
{
	boost::asio::io_context context;
	
	auto f = dkuk::run_until_complete(context, dkuk::spawn_with_future(context, sum_3, 1, 2, 3));
	
	try {
		if (f.get() != 6)
			return 1;
	} catch (const std::logic_error &) {
		return 0;
	}
	
	return 1;
}
