// Author: Dmitry Kukovinets (d1021976@gmail.com), 02.01.2018, 02:44

#include <iostream>
#include <stdexcept>
#include <string>

#include <dkuk/context_group.hpp>


int
main()
{
	boost::asio::io_context context1, context2, context3;
	const dkuk::context_group group{context1, context2, context3};
	
	
	try {
		if (group.size() != 3)
			throw std::logic_error{"Expected size: 3, but got: " + std::to_string(group.size())};
		
		if (&context1 != &group.get_io_context())
			throw std::logic_error{"Expected context1"};
		if (&context2 != &group.get_io_context())
			throw std::logic_error{"Expected context2"};
		if (&context3 != &group.get_io_context())
			throw std::logic_error{"Expected context3"};
		
		if (&context1 != &group.get_io_context())
			throw std::logic_error{"Expected context1 (repeat)"};
		if (&context2 != &group.get_io_context())
			throw std::logic_error{"Expected context2 (repeat)"};
		if (&context3 != &group.get_io_context())
			throw std::logic_error{"Expected context3 (repeat)"};
	} catch (const std::exception &e) {
		std::cout << "Error: " << e.what() << '.' << std::endl;
		return 1;
	}
	
	return 0;
}
