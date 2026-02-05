// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <tmc/all_headers.hpp>

export namespace tmc {
using tmc::async_main;
using tmc::atomic_condvar;
using tmc::auto_reset_event;
using tmc::aw_acquire;
using tmc::aw_atomic_condvar;
using tmc::aw_atomic_condvar_co_notify;
using tmc::aw_auto_reset_event_co_set;
using tmc::aw_barrier;
using tmc::aw_ex_scope_enter;
using tmc::aw_ex_scope_exit;
using tmc::aw_fork_clang;
using tmc::aw_fork_group;
using tmc::aw_fork_tuple_clang;
using tmc::aw_manual_reset_event;
using tmc::aw_manual_reset_event_co_set;
using tmc::aw_mutex_co_unlock;
using tmc::aw_mutex_lock_scope;
using tmc::aw_reschedule;
using tmc::aw_resume_on;
using tmc::aw_semaphore_acquire_scope;
using tmc::aw_semaphore_co_release;
using tmc::aw_spawn;
using tmc::aw_spawn_fork;
using tmc::aw_spawn_fork_impl;
using tmc::aw_spawn_func;
using tmc::aw_spawn_func_fork;
using tmc::aw_spawn_func_fork_impl;
using tmc::aw_spawn_func_impl;
using tmc::aw_spawn_group;
using tmc::aw_spawn_impl;
using tmc::aw_spawn_many;
using tmc::aw_spawn_many_each;
using tmc::aw_spawn_many_fork;
using tmc::aw_spawn_many_impl;
using tmc::aw_spawn_tuple;
using tmc::aw_spawn_tuple_each;
using tmc::aw_spawn_tuple_fork;
using tmc::aw_spawn_tuple_impl;
using tmc::aw_task;
using tmc::aw_yield;
using tmc::aw_yield_counter;
using tmc::aw_yield_counter_dynamic;
using tmc::aw_yield_if_requested;
using tmc::barrier;
using tmc::chan_default_config;
using tmc::chan_err;
using tmc::chan_tok;
using tmc::chan_zc_scope;
using tmc::change_priority;
using tmc::channel;
using tmc::check_yield_counter;
using tmc::check_yield_counter_dynamic;
using tmc::cpu_executor;
using tmc::current_executor;
using tmc::current_priority;
using tmc::current_thread_index;
using tmc::enter;
using tmc::ex_any;
using tmc::ex_braid;
using tmc::ex_cpu;
using tmc::ex_cpu_st;
using tmc::ex_manual_st;
using tmc::fork_clang;
using tmc::fork_group;
using tmc::fork_tuple_clang;
using tmc::iter_adapter;
using tmc::latch;
using tmc::make_channel;
using tmc::manual_reset_event;
using tmc::mutex;
using tmc::mutex_scope;
using tmc::post;
using tmc::post_bulk;
using tmc::post_bulk_waitable;
using tmc::post_waitable;
using tmc::reschedule;
using tmc::resume_on;
using tmc::semaphore;
using tmc::semaphore_scope;
using tmc::set_default_executor;
using tmc::spawn;
using tmc::spawn_clang;
using tmc::spawn_func;
using tmc::spawn_func_many;
using tmc::spawn_group;
using tmc::spawn_many;
using tmc::spawn_tuple;
using tmc::task;
using tmc::work_item;
using tmc::yield;
using tmc::yield_if_requested;
using tmc::yield_requested;

namespace topology {
using tmc::topology::core_group;
using tmc::topology::cpu_kind;
using tmc::topology::thread_info;
using tmc::topology::thread_packing_strategy;
using tmc::topology::thread_pinning_level;

#ifdef TMC_USE_HWLOC
using tmc::topology::cpu_topology;
using tmc::topology::pin_thread;
using tmc::topology::topology_filter;
#endif
} // namespace topology

namespace traits {
using tmc::traits::awaitable_result_t;
using tmc::traits::callable_result_t;
using tmc::traits::executable_kind;
using tmc::traits::executable_kind_v;
using tmc::traits::executable_result_t;
using tmc::traits::executable_traits;
using tmc::traits::is_awaitable;
using tmc::traits::is_callable;
using tmc::traits::is_func;
using tmc::traits::is_func_nonvoid;
using tmc::traits::is_func_result;
using tmc::traits::is_func_void;
using tmc::traits::is_task;
using tmc::traits::is_task_nonvoid;
using tmc::traits::is_task_result;
using tmc::traits::is_task_void;
using tmc::traits::unknown_t;
} // namespace traits

#ifdef TMC_DEBUG_TASK_ALLOC_COUNT
namespace debug {
using tmc::debug::get_task_alloc_count;
using tmc::debug::set_task_alloc_count;
} // namespace debug
#endif
} // namespace tmc
