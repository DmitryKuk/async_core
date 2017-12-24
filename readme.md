# [DmitryKuk](https://github.com/DmitryKuk) / async_core
#### Minimalistic multithreaded asynchronous core implementation based on boost::asio::io_context
---

# What is this?
async_core is small project containing several parts (which can be used separately). You can just include header files from [`include/`](include) directory and link some Boost libraries to your application.

# What is it for?
- For example, you write application with asyncronous core, but you don't want manage your io_contexts manually.
- Another example: you want to use stackful coroutines in your application, but [`boost::asio::spawn`](http://www.boost.org/doc/libs/1_66_0/doc/html/boost_asio/reference/spawn.html) uses deprecated [Boost.Coroutine](http://www.boost.org/doc/libs/1_66_0/libs/coroutine/doc/html/index.html).
- Or, maybe, you know about [asyncio](https://docs.python.org/3/library/asyncio) from Python and want something like `asyncio_loop.run_until_complete(my_coro())` (now in progress) or want to use [`std::future`](http://en.cppreference.com/w/cpp/thread/future)-like access to coroutines result (now in progress).

# Components
- *Header-only* asyncronous core implementation:
    + `dkuk::async_core` in [`include/dkuk/async_core.hpp`](include/dkuk/async_core.hpp)
    + dependencies: [Boost.Asio](http://www.boost.org/doc/libs/1_66_0/doc/html/boost_asio.html), [Boost.Optional](http://www.boost.org/doc/libs/1_66_0/libs/optional/doc/html/index.html)
- *Header-only* helpers for coroutines:
    + `dkuk::spawn` + `dkuk::coroutine_context` in [`include/dkuk/spawn.hpp`](include/dkuk/spawn.hpp)
    + based on [`boost::asio::spawn`](http://www.boost.org/doc/libs/1_66_0/doc/html/boost_asio/reference/spawn.html) and [`boost::context::continuation`](http://www.boost.org/doc/libs/1_66_0/libs/context/doc/html/context/cc/class__continuation_.html)
    + dependencies: [Boost.Asio](http://www.boost.org/doc/libs/1_66_0/doc/html/boost_asio.html), [Boost.Context](http://www.boost.org/doc/libs/1_66_0/libs/context/doc/html/index.html)

# Requirements
- Compiler with C++14 support. For example, try `g++` of versions 5/6/... or modern `clang`.
    + *Tested: `clang++ --version`: **Apple LLVM version 8.0.0 (clang-800.0.42.1)** on **Mac OS X 10.11***
    + *Tested: `g++-mp-6 --version`: **g++-mp-6 (MacPorts gcc6 6.4.0_0) 6.4.0** on **Mac OS X 10.11***
- [Boost](http://www.boost.org/) (or just some Boost libs) (`>= 1.66.0` because of [Boost.Asio](http://www.boost.org/doc/libs/1_66_0/doc/html/boost_asio.html) refactoring since `1.65`)
    + *Tested: **Boost 1.66.0** on **Mac OS X 10.11***
- [Boost.Build](http://www.boost.org/build/) (or any other build system -- it's simple), if you want to build exemples and run tests.
    + *Note: Set environment variable `BOOST_ROOT` to unpacked Boost directory.*

# License
MIT license. See [license.txt](license.txt).

# What next?
- See detailed description in `*.hpp` files.
- Check examples in [`example/`](example) directory.
- Build all examples and run test with [Boost.Build](http://www.boost.org/build/) *(in this project you can use `b2` from your Boost installation)*.
- Check `build/bin/` directory.
- Star or fork [the repository](https://github.com/DmitryKuk/async_core), open issues and have a nice day!

---

Author: [Dmitry Kukovinets](https://github.com/DmitryKuk), <d1021976@gmail.com>, 24.12.2017 01:54
