// Author: Dmitry Kukovinets (d1021976@gmail.com), 12.12.2017, 22:57


// This example demonstrates usage of async_core in some real simple application.
// 
// Application and architecture details:
// - 1 io_service and 1 worker for lightweight tasks (0.3s sleep);
// - 1 io_service and 1 worker for heavyweight tasks (5s sleep);
// - 90% of all tasks are lightweight.
// 
// Output table columns:
// - time (in seconds)
// - lightweight tasks (+ added) / executed lightweight tasks
// - heavyweight tasks (+ added) / executed heavyweight tasks
// - total tasks (+ added) / executed total tasks
// - average performance (tasks per second) from start: lightweight / heavyweight / total
// 
// Use Ctrl+C for exit.


#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <thread>

#include <boost/asio/signal_set.hpp>
#include <boost/asio/system_timer.hpp>

#include <dkuk/async_core.hpp>
#include <dkuk/spawn.hpp>


namespace {


inline
void
post_task(
	dkuk::async_core &core,
	dkuk::async_core::service_id_type service_id,
	std::chrono::milliseconds::rep milliseconds,
	std::atomic<std::size_t> &executed
)
{
	core.get_io_service(service_id).post(
		[milliseconds, &executed]
		{
			std::this_thread::sleep_for(std::chrono::milliseconds{milliseconds});
			++executed;
		}
	);
}


void
post_tasks(
	dkuk::async_core &core,
	std::chrono::milliseconds::rep lw_task_ms,
	std::chrono::milliseconds::rep hw_task_ms,
	double lw_tasks_part,
	dkuk::async_core::service_id_type lw_service,
	dkuk::async_core::service_id_type hw_service,
	std::atomic<std::size_t> &lw_posted,
	std::atomic<std::size_t> &lw_executed,
	std::atomic<std::size_t> &hw_posted,
	std::atomic<std::size_t> &hw_executed,
	dkuk::coroutine_context context
)
{
	std::minstd_rand gen;
	std::uniform_int_distribution<std::chrono::seconds::rep> sleep_seconds_dist{0, 5};
	std::bernoulli_distribution is_lw_task_dist{lw_tasks_part};
	
	boost::asio::system_timer timer{context.get_io_service()};
	
	while (core.get_state() == dkuk::async_core::state::running) {
		// Post new task avoiding too many tasks
		while (lw_executed.load() + hw_executed.load() + 200 > lw_posted.load() + hw_posted.load()) {
			if (is_lw_task_dist(gen)) {
				post_task(core, lw_service, lw_task_ms, lw_executed);
				++lw_posted;
			} else {
				post_task(core, hw_service, hw_task_ms, hw_executed);
				++hw_posted;
			}
		}
		
		// Delay
		timer.expires_from_now(
			std::chrono::duration_cast<std::chrono::system_clock::duration>(
				std::chrono::seconds{sleep_seconds_dist(gen)}
			)
		);
		timer.async_wait(context);
	}
}


inline
std::ostream &
print_time_statistics(
	std::ostream &stream,
	std::chrono::steady_clock::time_point global_start,
	std::chrono::steady_clock::time_point start,
	std::chrono::steady_clock::time_point stop
)
{
	constexpr std::size_t global_width = 8, step_width = 7, step_precision = 3;
	
	const auto global_diff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(stop - global_start);
	const auto diff_ms        = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
	stream
		<< std::setw(global_width) << std::right << std::fixed << std::setprecision(step_precision)
		<< static_cast<double>(global_diff_ms.count()) / 1000 << std::setw(0)
		<< std::setw(step_width) << std::right << std::fixed << std::setprecision(step_precision) << std::showpos
		<< static_cast<double>(diff_ms.count()) / 1000 << std::setw(0) << std::noshowpos;
	return stream;
}


inline
std::ostream &
print_tasks_statistics(
	std::ostream &stream,
	std::size_t executed_old,
	std::size_t posted_old,
	std::size_t executed,
	std::size_t posted
)
{
	constexpr std::size_t tasks_width = 5, diff_width = 5;
	
	stream << std::setw(tasks_width) << std::right << executed << std::setw(diff_width) << std::right << std::showpos;
	if (executed > executed_old)
		stream << ('+' + std::to_string(executed - executed_old));
	else
		stream << ' ';
	stream << std::setw(0) << std::noshowpos << " / ";
	
	stream << std::setw(tasks_width) << std::right << posted << std::setw(diff_width) << std::right << std::showpos;
	if (posted > posted_old)
		stream << ('+' + std::to_string(posted - posted_old));
	else
		stream << ' ';
	stream << std::setw(0) << std::noshowpos;
	
	return stream;
}


inline
std::ostream &
print_avg_tasks(
	std::ostream &stream,
	std::size_t tasks,
	std::chrono::steady_clock::duration duration
)
{
	constexpr std::size_t width = 5, precision = 2;
	
	const auto global_seconds = std::chrono::duration_cast<std::chrono::duration<double>>(duration);
	std::cout
		<< std::setw(width) << std::setprecision(precision)
		<< tasks / global_seconds.count() << std::setw(0);
	return stream;
}


void
print_statistics(
	dkuk::async_core &core,
	std::atomic<std::size_t> &lw_posted,
	std::atomic<std::size_t> &lw_executed,
	std::atomic<std::size_t> &hw_posted,
	std::atomic<std::size_t> &hw_executed,
	dkuk::coroutine_context context
)
{
	boost::asio::system_timer timer{context.get_io_service()};
	
	std::chrono::steady_clock steady_clock;
	const auto global_start_time = steady_clock.now();
	auto start_time = global_start_time;
	
	std::size_t
		lw_posted_value_old = 0, lw_executed_value_old = 0,
		hw_posted_value_old = 0, hw_executed_value_old = 0;
	
	
	while (core.get_state() == dkuk::async_core::state::running) {
		const std::size_t
			lw_posted_value = lw_posted, lw_executed_value = lw_executed,
			hw_posted_value = hw_posted, hw_executed_value = hw_executed;
		
		const auto stop_time = steady_clock.now();
		
		
		std::cout << "|  t: ";
		print_time_statistics(std::cout, global_start_time, start_time, stop_time);
		
		std::cout << "  |  L: ";
		print_tasks_statistics(
			std::cout,
			lw_executed_value_old, lw_posted_value_old,
			lw_executed_value,     lw_posted_value
		);
		
		std::cout << "  |  H: ";
		print_tasks_statistics(
			std::cout,
			hw_executed_value_old, hw_posted_value_old,
			hw_executed_value,     hw_posted_value
		);
		
		std::cout << "  |  T: ";
		print_tasks_statistics(
			std::cout,
			lw_executed_value_old + hw_executed_value_old, lw_posted_value_old + hw_posted_value_old,
			lw_executed_value     + hw_executed_value,     lw_posted_value     + hw_posted_value
		);
		
		std::cout << "  |  A: ";
		print_avg_tasks(std::cout, lw_executed_value, stop_time - global_start_time);
		std::cout << " / ";
		print_avg_tasks(std::cout, hw_executed_value, stop_time - global_start_time);
		std::cout << " / ";
		print_avg_tasks(std::cout, lw_executed_value + hw_executed_value, stop_time - global_start_time);
		std::cout << "  |" << std::endl;
		
		
		start_time = stop_time;
		
		lw_posted_value_old = lw_posted_value;
		lw_executed_value_old = lw_executed_value;
		hw_posted_value_old = hw_posted_value;
		hw_executed_value_old = hw_executed_value;
		
		
		// Delay
		timer.expires_from_now(
			std::chrono::duration_cast<std::chrono::system_clock::duration>(
				std::chrono::seconds{1}
			)
		);
		timer.async_wait(context);
	}
}


};	// namespace



int
main()
{
	constexpr std::chrono::milliseconds::rep
		lw_task_ms    = 300,
		hw_task_ms    = 5000;
	const double
		lw_tasks_part = 0.90;
	
	constexpr std::size_t
		lw_workers    = 1,
		hw_workers    = 1;
	
	
	dkuk::async_core::service_tree t;
	const auto root_service = t.add_service();	// Root service always has id 0
	const auto lw_service   = t.add_service(root_service, lw_workers);
	const auto hw_service   = t.add_service(root_service, hw_workers);
	
	
	dkuk::async_core core{t};
	
	std::atomic<std::size_t>
		lw_executed{0},
		lw_posted{0},
		hw_executed{0},
		hw_posted{0};
	
	
	boost::asio::io_service helper_io_service;
	
	
	// Add tasks automatically
	dkuk::spawn(
		helper_io_service,
		post_tasks,
		std::ref(core),
		lw_task_ms,
		hw_task_ms,
		lw_tasks_part,
		lw_service,
		hw_service,
		std::ref(lw_executed),
		std::ref(lw_posted),
		std::ref(hw_executed),
		std::ref(hw_posted)
	);
	
	
	// Display statistics
	dkuk::spawn(
		helper_io_service,
		print_statistics,
		std::ref(core),
		std::ref(lw_executed),
		std::ref(lw_posted),
		std::ref(hw_executed),
		std::ref(hw_posted)
	);
	
	
	// Wait for exit
	boost::asio::signal_set signals{helper_io_service, SIGINT, SIGTERM};
	signals.async_wait(
		[&core, &helper_io_service](boost::system::error_code /* ec */, int /* signal_number */)
		{
			core.stop();
			helper_io_service.stop();
		}
	);
	
	
	helper_io_service.run();
	
	
	return 0;
}
