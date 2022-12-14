/* Copyright 2022 Florian Wagner <florian@wagner-flo.net>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef COVENT_TASK_HH
#define COVENT_TASK_HH

#include <covent/base.hh>
#include <covent/concepts.hh>
#include <covent/exceptions.hh>

#include <coroutine>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>

namespace covent::detail {

  template <concepts::Awaitable T>
  class id_awaiter {
    public:
      using result_type =
        decltype(std::declval<T>().await_resume());

      constexpr static bool ResultIsVoid =
        std::is_void_v<result_type>;

    protected:
      T& aw;

    public:
      id_awaiter(T& a) : aw(a) {
        /* nothing to do here */
      }

      bool await_ready() {
        return aw.await_ready();
      }

      std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting_coro) {
        return aw.await_suspend(awaiting_coro);
      }

      template<typename V = result_type>
      V& await_resume() const requires (!ResultIsVoid) {
        return aw.await_resume();
      }

      void await_resume() const requires (ResultIsVoid) {
        aw.await_resume();
      }
  };

  // ...
  class final_awaiter {
    public:
      bool await_ready() const noexcept {
        return false;
      }

      void await_resume() const noexcept {
        /* nothing to do here */
      }

      template<typename P>
      std::coroutine_handle<> await_suspend(std::coroutine_handle<P> c)
        const noexcept {
        auto& parent = c.promise().parent;
        if (parent && !parent.done())
          return parent;
        return std::noop_coroutine();
      }
  };

  // ...
  template<
    concepts::Awaitable InitialSuspendType,
    typename TaskType,
    typename ResultType
  >
  class promise final {
    friend final_awaiter;
    friend TaskType;

    public:
      constexpr static bool ResultIsVoid =
        std::is_void_v<ResultType>;

      constexpr static bool ResultIsRef =
        std::is_lvalue_reference_v<ResultType>;

      using ResultTypeUnref =
        std::remove_reference_t<ResultType>;

      using ResultHolderType =
        std::variant<                    // variant that contains either
          std::exception_ptr,            //   pointer to exception
          std::conditional_t<
            ResultIsVoid,              // if ResultType == void
            std::monostate,            //   empty value
            std::conditional_t<        // else
              ResultIsRef,             //   if ResultType is a reference
              ResultTypeUnref*,        //     pointer to ResultType
              ResultType               //   else value of ResultType
            >
          >
        >;

    protected:
      detail::evloop_base& loop;
      std::coroutine_handle<> parent;
      ResultHolderType result;

    public:
      promise() noexcept
        : loop(get_active_loop()) {
        /* nothing to do here */
      }

      TaskType get_return_object() noexcept {
        return { TaskType::handle_type::from_promise(*this) };
      }

      auto initial_suspend() noexcept {
        return InitialSuspendType { };
      }

      auto final_suspend() noexcept {
        return final_awaiter { };
      }

      void unhandled_exception() noexcept {
        result.template emplace<0>(std::current_exception());
      }

      template<typename V>
      void return_value(V&& val)
        noexcept ( std::is_nothrow_constructible_v<ResultType, V&&> )
        requires ( !ResultIsVoid &&
                   !ResultIsRef &&
                   std::is_convertible_v<V&&, ResultType> ) {
        result.template emplace<1>(val);
      }

      template<typename V = ResultType>
      void return_value(V& val) noexcept requires (ResultIsRef) {
        result.template emplace<1>(std::addressof(val));
      }

      void return_void() noexcept {
        if constexpr(ResultIsVoid)
          result.template emplace<1>(std::monostate {});
      }

      template<concepts::Awaitable T>
      id_awaiter<std::remove_reference_t<T>> await_transform(T&& aw) {
        return { aw };
      }

      template<typename ...Args>
      event_awaiter await_transform(Args... args) const noexcept {
        return loop.create_event_awaiter(std::forward<Args>(args)...);
      }
  };

}

namespace covent {

  // ...
  template<
    typename ResultType = void,
    concepts::Awaitable InitialSuspendType = std::suspend_always
  >
  class [[nodiscard]] task {
    public:
      using result_type = ResultType;
      using promise_type = detail::promise<
        InitialSuspendType,
        task,
        ResultType
      >;
      using handle_type = std::coroutine_handle<promise_type>;

    protected:
      handle_type coro;
      bool was_started = true;

      void start() const noexcept {
        if (done())
          throw std::runtime_error("Cannot start a task that is already done.");

        if (was_started)
          throw std::runtime_error("Cannot start a task that is already running.");

        was_started = true;
        coro.resume();
      }

      void resume() const noexcept {
        if (!was_started)
          throw std::runtime_error("Cannot resume a task that is not running.");

        if (done())
          throw std::runtime_error("Cannot resume a task that is already done.");

        coro.resume();
      }

    public:
      task() = delete;
      task(const task&) = delete;
      task& operator=(const task&) = delete;

      task(handle_type c) noexcept : coro(c) {
        /* nothing to do here */
      }

      task(task&& other) noexcept : coro(other.coro) {
        other.coro = nullptr;
      }

      task& operator=(task&& other) noexcept {
        if (std::addressof(other) != this) {
          if (coro != nullptr)
            coro.destroy();
          coro = other.coro;
          other.coro = nullptr;
        }
        return *this;
      }

      ~task() noexcept {
        if (coro != nullptr)
          coro.destroy();
      }

      bool started() const noexcept {
        return was_started;
      }

      bool done() const noexcept {
        return coro == nullptr || coro.done();
      }

      bool await_ready() const noexcept {
        return done();
      }

      std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting_coro)
        const noexcept {
        coro.promise().parent = awaiting_coro;
        return coro;
      }

      template<typename V = ResultType>
      V& await_resume()
        const requires (promise_type::ResultIsRef) {
        if (coro == nullptr)
          throw broken_promise();
        if (auto* exc = std::get_if<0>(&coro.promise().result))
          std::rethrow_exception(*exc);
        return **std::get_if<1>(&coro.promise().result);
      }

      template<typename V = ResultType>
      V& await_resume()
        const requires (!promise_type::ResultIsRef) {
        if (coro == nullptr)
          throw broken_promise();
        if (auto* exc = std::get_if<0>(&coro.promise().result))
          std::rethrow_exception(*exc);
        return *std::get_if<1>(&coro.promise().result);
      }

      void await_resume()
        const requires (promise_type::ResultIsVoid) {
        if (coro == nullptr)
          throw broken_promise();
        if (auto* exc = std::get_if<0>(&coro.promise().result))
          std::rethrow_exception(*exc);
      }

      decltype(auto) result() const {
        if constexpr(!promise_type::ResultIsVoid)
          return await_resume();
      }
  };

}

#endif
