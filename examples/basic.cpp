// Author: Dmitry Kukovinets (d1021976@gmail.com), 08.12.2017, 14:45

#include <chrono>
#include <exception>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>

#include <dkuk/async_core.hpp>


int
main()
{
	std::mutex cout_mutex;
	int status = 0;
	
	std::mutex gen_mutex;
	std::minstd_rand gen;
	std::uniform_int_distribution<int> sleep_ms_dist{100, 3000};
	
	const auto handle_exception =
		[&](const std::exception &e)
		{
			std::lock_guard<std::mutex> cout_lock{cout_mutex};
			std::cout << "Exception caught: " << e.what() << '.' << std::endl;
			status = 1;
		};
	
	
	dkuk::async_core::service_tree t;
	const auto s0 = t.add_service(0, 1);
	const auto s1 = t.add_service(s0, 1);
	const auto s2 = t.add_service(s1, 1);
	
	
	dkuk::async_core core{t, handle_exception, false};
	
	
	std::size_t task_id = 0;
	const auto post_task =
		[&](auto service_id)
		{
			core.get_io_service(service_id).post(
				[&, service_id, task_id]
				{
					{
						std::lock_guard<std::mutex> cout_lock{cout_mutex};
						std::cout
							<< "Task " << task_id
							<< " from service "
							<< service_id
							<< " runned by thread: " << std::this_thread::get_id() << '.' << std::endl;
					}
					
					{
						std::lock_guard<std::mutex> gen_lock{gen_mutex};
						std::this_thread::sleep_for(std::chrono::milliseconds{sleep_ms_dist(gen)});
					}
				}
			);
			
			{
				std::lock_guard<std::mutex> cout_lock{cout_mutex};
				std::cout << "Task " << task_id << " posted to service " << service_id << '.' << std::endl;
			}
			++task_id;
		};
	
	
	
	post_task(s0);
	post_task(s1);
	post_task(s2);
	post_task(s0);
	post_task(s1);
	post_task(s2);
	
	for (auto i = 0; i < 50; ++i)
		post_task(s2);
	
	
	
	{
		std::lock_guard<std::mutex> cout_lock{cout_mutex};
		std::cout << "Starting core..." << std::endl;
	}
	
	core.start();
	
	{
		std::lock_guard<std::mutex> cout_lock{cout_mutex};
		std::cout << "Core started..." << std::endl;
	}
	
	{
		using namespace std::literals;
		std::this_thread::sleep_for(20s);
	}
	
	{
		std::lock_guard<std::mutex> cout_lock{cout_mutex};
		std::cout << "Stopping core..." << std::endl;
	}
	
	core.stop();
	
	{
		std::lock_guard<std::mutex> cout_lock{cout_mutex};
		std::cout << "Core stopped." << std::endl;
	}
	
	return status;
}
