#include "condition.hpp"
#include <memory/allocator.hpp>
#include <fd/scheduler.hpp>
#include <global.hpp>

using namespace cloudos;

thread_condition::thread_condition(thread_condition_signaler *s)
: signaler(s)
, satisfied(false)
{}

void thread_condition::satisfy()
{
	auto thr = thread.lock();
	assert(thr);
	satisfied = true;
	// unblock if it was blocked
	thr->thread_unblock();
}

thread_condition_signaler::thread_condition_signaler()
: satisfied_function(nullptr)
, satisfied_function_userdata(nullptr)
, conditions(nullptr)
{}

void thread_condition_signaler::set_already_satisfied_function(thread_condition_satisfied_function_t function, void *userdata) {
	satisfied_function = function;
	satisfied_function_userdata = userdata;
}

bool thread_condition_signaler::already_satisfied(thread_condition *c) {
	return satisfied_function ? satisfied_function(satisfied_function_userdata, c) : false;
}

void thread_condition_signaler::subscribe_condition(thread_condition *c)
{
	auto item = get_allocator()->allocate<thread_condition_list>();
	item->data = c;
	item->next = nullptr;
	// append myself, don't prepend, to prevent starvation
	append(&conditions, item);
}

void thread_condition_signaler::cancel_condition(thread_condition *c)
{
	if(c->satisfied) {
		kernel_panic("Condition is cancelled, but already satisfied");
	}

	remove_one(&conditions, [&](thread_condition_list *item){
		return item->data == c;
	}, [&](thread_condition*){});
}

void thread_condition_signaler::condition_notify() {
	if(conditions) {
		thread_condition_list *item = conditions;
		conditions = item->next;
		item->next = nullptr;
		item->data->satisfy();
	}
}

void thread_condition_signaler::condition_broadcast() {
	while(conditions) {
		condition_notify();
	}
}

thread_condition_waiter::thread_condition_waiter()
: conditions(nullptr)
{}

thread_condition_waiter::~thread_condition_waiter()
{
	if(conditions != nullptr) {
		finish();
	}
}

void thread_condition_waiter::add_condition(thread_condition *c) {
	auto item = get_allocator()->allocate<thread_condition_list>();
	item->data = c;
	item->next = nullptr;
	append(&conditions, item);
}

void thread_condition_waiter::wait() {
	auto thr = get_scheduler()->get_running_thread();
	bool initially_satisfied = false;

	// Subscribe on all conditions
	iterate(conditions, [&](thread_condition_list *item) {
		thread_condition *c = item->data;
		if(!initially_satisfied) {
			c->thread = thr;
			c->signaler->subscribe_condition(c);
		}

		if(c->signaler->already_satisfied(c)) {
			c->satisfied = true;
			initially_satisfied = true;
		} else {
			c->satisfied = false;
		}
	});

	// TODO: there is a race condition here, where a condition is not
	// satisfied when we check it, but it becomes satisfied before our
	// thread blocks. Then, the thread would never be unblocked because the
	// condition would not be re-satisfied.
	// I can think of two fixes:
	// - Spinlocking a subscribed signaler, so that it won't signal conditions
	//   before we're done blocking our thread
	// - Marking our thread blocked without actually yielding, before checking
	//   whether conditions are already satisfied; then, if a condition becomes
	//   satisfied during that period, the thread will be unblocked and will
	//   automatically be scheduled again sometime after the yield

	if(!initially_satisfied) {
		// Block ourselves waiting on a satisfaction
		thr->thread_block();
	}

	// Cancel all subscribed, non-satisfied conditions (satisfied
	// conditions will be already removed by the signaler)
	iterate(conditions, [&](thread_condition_list *item) {
		thread_condition *c = item->data;
		if(!c->thread.expired() && !c->satisfied) {
			c->signaler->cancel_condition(c);
			c->thread.reset();
		}
	});
}

thread_condition_list *thread_condition_waiter::finish() {
	// find the first thread condition that is satisfied, then
	// set its 'next' ptr to the next one continuously

	thread_condition_list *head = nullptr;
	thread_condition_list *tail = nullptr;

	iterate(conditions, [&](thread_condition_list *item) {
		if(!item->data->satisfied) {
			return;
		}
		if(!head) {
			head = tail = item;
		} else {
			tail->next = item;
			tail = item;
		}
	});

	if(tail == nullptr) {
		kernel_panic("Condition waiter is finishing, but has no satisfied thread conditions");
	}
	tail->next = nullptr;
	conditions = nullptr;
	return head;
}
