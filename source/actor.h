// Copyright Leon Timmermans 2012-2017

#ifndef __ACTOR_H__
#define __ACTOR_H__

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <memory>
#include <list>
#include <tuple>

#include <experimental/tuple>
#include <boost/any.hpp>
namespace std {
	using any = boost::any;
	using boost::any_cast;
	using experimental::apply;
}

namespace actor {
	namespace {
		template<typename T> struct function_traits : public function_traits<decltype(&T::operator())> {
		};
		template <typename ClassType, typename ReturnType, typename... Args> struct function_traits<ReturnType(ClassType::*)(Args...) const> {
			using args = std::tuple<typename std::decay<Args>::type...>;
		};

		template<size_t pos = 0, typename C, typename... T> static typename std::enable_if<pos >= sizeof...(T), size_t>::type match_if(std::any&, const C&, const std::tuple<T...>&) {
			return false;
		}
		template<size_t pos = 0, typename C, typename... T> static typename std::enable_if<pos < sizeof...(T), size_t>::type match_if(std::any& any, const C& callback, const std::tuple<T...>& tuple) {
			using current = typename std::tuple_element<pos, std::tuple<T...>>::type;
			using arg_type = typename function_traits<current>::args;
			if (arg_type* pointer = std::any_cast<arg_type>(&any)) {
				arg_type value = std::move(*pointer);
				callback();
				std::apply(std::get<pos>(tuple), std::move(value));
				return true;
			}
			else
				return match_if<pos+1>(any, callback, tuple);
		}

	}

	class queue {
		std::mutex mutex;
		std::condition_variable cond;
		std::queue<std::any> incoming;
		std::list<std::any> pending;
		queue(const queue&) = delete;
		queue& operator=(const queue&) = delete;
		public:
		queue()
		: mutex()
		, cond()
		, incoming()
		, pending()
		{ }
		template<typename T> void push(T&& value) {
			std::lock_guard<std::mutex> lock(mutex);
			incoming.push(std::move(value));
			cond.notify_one();
		}
		template<typename... A> void match(A&&... args) {
			std::tuple<A...> matchers(std::forward<A>(args)...);

			for (auto current = pending.begin(); current != pending.end(); ++current)
				if (match_if(*current, [this, &current] { pending.erase(current); }, matchers))
					return;
			std::unique_lock<std::mutex> lock(mutex);
			while (1) {
				cond.wait(lock, [&] { return !incoming.empty(); });
				if (match_if(incoming.front(), [this, &lock] { incoming.pop(); lock.unlock(); }, matchers))
					return;
				else {
					pending.push_back(std::move(incoming.front()));
					incoming.pop();
				}
			}
		}
		template<typename Clock, typename Rep, typename Period, typename... A> bool match_until(const std::chrono::time_point<Clock, std::chrono::duration<Rep, Period>>& until, A&&... args) {
			std::tuple<A...> matchers(std::forward<A>(args)...);

			for (auto current = pending.begin(); current != pending.end(); ++current)
				if (match_if(*current, [this, &current] { pending.erase(current); }, matchers))
					return true;
			std::unique_lock<std::mutex> lock(mutex);
			while (1) {
				if (!cond.wait_until(lock, until, [&] { return !incoming.empty(); }))
					return false;
				else if (match_if(incoming.front(), [this, &lock] { incoming.pop(); lock.unlock(); }, matchers))
					return true;
				else {
					pending.push_back(std::move(incoming.front()));
					incoming.pop();
				}
			}
		}
	};

	class handle {
		std::weak_ptr<queue> weak_queue;
		public:
		explicit handle(const std::shared_ptr<queue>& other) noexcept : weak_queue(other) {}
		template<typename... Args> void send(Args&&... args) const {
			auto strong_queue = weak_queue.lock();
			if (strong_queue)
				strong_queue->push(std::make_tuple(std::forward<Args>(args)...));
		}
		bool zombie() const noexcept {
			return weak_queue.expired();
		}
		friend void swap(handle& left, handle& right) noexcept {
			swap(left.weak_queue, right.weak_queue);
		}
	};

	namespace hidden {
		extern const thread_local std::shared_ptr<queue> mailbox = std::make_shared<queue>();
		extern const thread_local handle self_var(hidden::mailbox);
	}
	static inline const handle& self() {
		return hidden::self_var;
	}

	template<typename... Matchers> void receive(Matchers&&... matchers) {
		hidden::mailbox->match(std::forward<Matchers>(matchers)...);
	}

	template<typename Condition, typename... Matchers> void receive_while(const Condition& condition, Matchers&&... matchers) {
		while (condition)
			receive(std::forward<Matchers>(matchers)...);
	}

	template<typename Clock, typename Rep, typename Period, typename... Matchers> bool receive_until(const std::chrono::time_point<Clock, std::chrono::duration<Rep, Period>>& until, Matchers&&... matchers) {
		return hidden::mailbox->match_until(until, std::forward<Matchers>(matchers)...);
	}

	template<typename Rep, typename Period, typename... Matchers> bool receive_for(const std::chrono::duration<Rep, Period>& until, Matchers&&... matchers) {
		return receive_until(std::chrono::steady_clock::now() + until, std::forward<Matchers>(matchers)...);
	}

	template<typename Func, typename... Args> handle spawn(Func&& func, Args&&... params) {
		std::promise<handle> promise;
		auto callback = [&promise](auto function, auto... args) {
			promise.set_value(self());
			function(std::forward<Args>(args)...);
		};
		std::thread(callback, std::forward<Func>(func), std::forward<Args>(params)...).detach();
		return promise.get_future().get();
	}
}

#endif
