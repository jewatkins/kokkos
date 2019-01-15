/*
//@HEADER
// ************************************************************************
//
//                        Kokkos v. 2.0
//              Copyright (2014) Sandia Corporation
//
// Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
// the U.S. Government retains certain rights in this software.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY SANDIA CORPORATION "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SANDIA CORPORATION OR THE
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions? Contact Christian R. Trott (crtrott@sandia.gov)
//
// ************************************************************************
//@HEADER
*/

#ifndef KOKKOS_HPX_HPP
#define KOKKOS_HPX_HPP

#include <Kokkos_Macros.hpp>
#if defined(KOKKOS_ENABLE_HPX)

#include <Kokkos_Core_fwd.hpp>

#include <Kokkos_HostSpace.hpp>
#include <cstddef>
#include <iosfwd>

#ifdef KOKKOS_ENABLE_HBWSPACE
#include <Kokkos_HBWSpace.hpp>
#endif

#include <Kokkos_HostSpace.hpp>
#include <Kokkos_Layout.hpp>
#include <Kokkos_MemoryTraits.hpp>
#include <Kokkos_Parallel.hpp>
#include <Kokkos_ScratchSpace.hpp>
#include <Kokkos_TaskScheduler.hpp>
#include <impl/Kokkos_FunctorAdapter.hpp>
#include <impl/Kokkos_FunctorAnalysis.hpp>
#include <impl/Kokkos_Profiling_Interface.hpp>
#include <impl/Kokkos_Tags.hpp>
#include <impl/Kokkos_TaskQueue.hpp>

#include <KokkosExp_MDRangePolicy.hpp>

#include <hpx/hpx_start.hpp>
#include <hpx/include/apply.hpp>
#include <hpx/include/local_lcos.hpp>
#include <hpx/include/parallel_executor_parameters.hpp>
#include <hpx/include/parallel_for_loop.hpp>
#include <hpx/include/parallel_reduce.hpp>
#include <hpx/include/run_as.hpp>
#include <hpx/include/runtime.hpp>

#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

// There are currently two different implementations for the parallel dispatch
// functions:
//
// - 0: The HPX way. Unfortunately, this comes with unnecessary
//      overheads at the moment, so there is
// - 1: The manual way. This way is more verbose and does not take advantage of
//      e.g. parallel::for_loop in HPX but it is significantly faster in many
//      benchmarks.
//
// In the long run 0 should be the preferred implementation, but until HPX is
// improved 1 will be the default.
#ifndef KOKKOS_HPX_IMPLEMENTATION
#define KOKKOS_HPX_IMPLEMENTATION 1
#endif

#if (KOKKOS_HPX_IMPLEMENTATION < 0) || (KOKKOS_HPX_IMPLEMENTATION > 1)
#error "You have chosen an invalid value for KOKKOS_HPX_IMPLEMENTATION"
#endif

namespace Kokkos {
namespace Impl {
template <typename F> inline void run_hpx_function(F &&f) {
  if (hpx::threads::get_self_ptr()) {
    f();
  } else {
    hpx::threads::run_as_hpx_thread(std::move(f));
  }
}

class thread_buffer {
  static constexpr std::size_t m_cache_line_size = 64;

  std::size_t m_num_threads;
  std::size_t m_size_per_thread;
  std::size_t m_size_total;
  std::unique_ptr<char[]> m_data;

  void pad_to_cache_line(std::size_t &size) {
    size = ((size + m_cache_line_size - 1) / m_cache_line_size) *
           m_cache_line_size;
  }

public:
  thread_buffer()
      : m_num_threads(0), m_size_per_thread(0), m_size_total(0),
        m_data(nullptr) {}
  thread_buffer(const std::size_t num_threads,
                const std::size_t size_per_thread) {
    resize(num_threads, size_per_thread);
  }

  void resize(const std::size_t num_threads,
              const std::size_t size_per_thread) {
    m_num_threads = num_threads;
    m_size_per_thread = size_per_thread;

    pad_to_cache_line(m_size_per_thread);

    std::size_t size_total_new = m_num_threads * m_size_per_thread;

    if (m_size_total < size_total_new) {
      m_data.reset(new char[size_total_new]);
      m_size_total = size_total_new;
    }
  }

  char *get(std::size_t thread_num) {
    assert(thread_num < m_num_threads);
    assert(m_data.get() != nullptr);
    return &m_data.get()[thread_num * m_size_per_thread];
  }

  std::size_t size_per_thread() const noexcept { return m_size_per_thread; }
  std::size_t size_total() const noexcept { return m_size_total; }
};
} // namespace Impl

class HPX {
private:
  static bool m_hpx_initialized;
  static Impl::thread_buffer m_buffer;

public:
  using execution_space = HPX;
  using memory_space = HostSpace;
  using device_type = Kokkos::Device<execution_space, memory_space>;
  using array_layout = LayoutRight;
  using size_type = memory_space::size_type;
  using scratch_memory_space = ScratchMemorySpace<HPX>;

  HPX() noexcept {}
  static void print_configuration(std::ostream &,
                                  const bool /* verbose */ = false) {
    std::cout << "HPX backend" << std::endl;
  }

  static bool in_parallel(HPX const & = HPX()) noexcept { return false; }
  static void fence(HPX const & = HPX()) noexcept {}

  static bool is_asynchronous(HPX const & = HPX()) noexcept { return false; }

  static std::vector<HPX> partition(...) {
    Kokkos::abort("HPX::partition_master: can't partition an HPX instance\n");
    return std::vector<HPX>();
  }

  template <typename F>
  static void partition_master(F const &f, int requested_num_partitions = 0,
                               int requested_partition_size = 0) {
    if (requested_num_partitions > 1) {
      Kokkos::abort("HPX::partition_master: can't partition an HPX instance\n");
    }
  }

  static int concurrency();
  static void impl_initialize(int thread_count);
  static void impl_initialize();
  static bool impl_is_initialized() noexcept;
  static void impl_finalize();

  static int impl_thread_pool_size() noexcept {
    hpx::runtime *rt = hpx::get_runtime_ptr();
    if (rt == nullptr) {
      return 0;
    } else {
      if (hpx::threads::get_self_ptr() == nullptr) {
        return hpx::resource::get_thread_pool(0).get_os_thread_count();
      } else {
        return hpx::this_thread::get_pool()->get_os_thread_count();
      }
    }
  }

  static int impl_thread_pool_rank() noexcept {
    hpx::runtime *rt = hpx::get_runtime_ptr();
    if (rt == nullptr) {
      return 0;
    } else {
      if (hpx::threads::get_self_ptr() == nullptr) {
        return 0;
      } else {
        return hpx::this_thread::get_pool()->get_pool_index();
      }
    }
  }

  static int impl_thread_pool_size(int depth) {
    if (depth == 0) {
      return impl_thread_pool_size();
    } else {
      return 1;
    }
  }

  static int impl_max_hardware_threads() noexcept {
    return hpx::threads::hardware_concurrency();
  }

  static int impl_hardware_thread_id() noexcept {
    return hpx::get_worker_thread_num();
  }

  static constexpr const char *name() noexcept { return "HPX"; }
  static Impl::thread_buffer &get_buffer() noexcept { return m_buffer; }
};
} // namespace Kokkos

namespace Kokkos {
namespace Impl {
template <>
struct MemorySpaceAccess<Kokkos::HPX::memory_space,
                         Kokkos::HPX::scratch_memory_space> {
  enum { assignable = false };
  enum { accessible = true };
  enum { deepcopy = false };
};

template <>
struct VerifyExecutionCanAccessMemorySpace<Kokkos::HPX::memory_space,
                                           Kokkos::HPX::scratch_memory_space> {
  enum { value = true };
  inline static void verify(void) {}
  inline static void verify(const void *) {}
};
} // namespace Impl
} // namespace Kokkos

namespace Kokkos {
namespace Experimental {
template <> class UniqueToken<HPX, UniqueTokenScope::Instance> {
public:
  using execution_space = HPX;
  using size_type = int;
  UniqueToken(execution_space const & = execution_space()) noexcept {}

  // NOTE: Currently this assumes that there is no oversubscription.
  // hpx::get_num_worker_threads can't be used directly because it may yield
  // it's task (problematic if called after hpx::get_worker_thread_num).
  int size() const noexcept { return HPX::impl_max_hardware_threads(); }
  int acquire() const noexcept { return HPX::impl_hardware_thread_id(); }
  void release(int) const noexcept {}
};

template <> class UniqueToken<HPX, UniqueTokenScope::Global> {
public:
  using execution_space = HPX;
  using size_type = int;
  UniqueToken(execution_space const & = execution_space()) noexcept {}

  // NOTE: Currently this assumes that there is no oversubscription.
  // hpx::get_num_worker_threads can't be used directly because it may yield
  // it's task (problematic if called after hpx::get_worker_thread_num).
  int size() const noexcept { return HPX::impl_max_hardware_threads(); }
  int acquire() const noexcept { return HPX::impl_hardware_thread_id(); }
  void release(int) const noexcept {}
};
} // namespace Experimental
} // namespace Kokkos

namespace Kokkos {
namespace Impl {

struct HPXTeamMember {
private:
  using execution_space = Kokkos::HPX;
  using scratch_memory_space = Kokkos::ScratchMemorySpace<Kokkos::HPX>;

  scratch_memory_space m_team_shared;
  std::size_t m_team_shared_size;

  int m_league_size;
  int m_league_rank;
  int m_team_size;
  int m_team_rank;

public:
  KOKKOS_INLINE_FUNCTION
  const scratch_memory_space &team_shmem() const {
    return m_team_shared.set_team_thread_mode(0, 1, 0);
  }

  KOKKOS_INLINE_FUNCTION
  const execution_space::scratch_memory_space &team_scratch(const int) const {
    return m_team_shared.set_team_thread_mode(0, 1, 0);
  }

  KOKKOS_INLINE_FUNCTION
  const execution_space::scratch_memory_space &thread_scratch(const int) const {
    return m_team_shared.set_team_thread_mode(0, team_size(), team_rank());
  }

  KOKKOS_INLINE_FUNCTION int league_rank() const noexcept {
    return m_league_rank;
  }

  KOKKOS_INLINE_FUNCTION int league_size() const noexcept {
    return m_league_size;
  }

  KOKKOS_INLINE_FUNCTION int team_rank() const noexcept { return m_team_rank; }
  KOKKOS_INLINE_FUNCTION int team_size() const noexcept { return m_team_size; }

  template <class... Properties>
  constexpr KOKKOS_INLINE_FUNCTION
  HPXTeamMember(const TeamPolicyInternal<Kokkos::HPX, Properties...> &policy,
                const int team_rank, const int league_rank, void *scratch,
                int scratch_size) noexcept
      : m_team_shared(scratch, scratch_size, scratch, scratch_size),
        m_team_shared_size(scratch_size), m_league_size(policy.league_size()),
        m_league_rank(league_rank), m_team_size(policy.team_size()),
        m_team_rank(team_rank) {}

  KOKKOS_INLINE_FUNCTION
  void team_barrier() const {}

  template <class ValueType>
  KOKKOS_INLINE_FUNCTION void team_broadcast(ValueType &value,
                                             const int &thread_id) const {
    static_assert(std::is_trivially_default_constructible<ValueType>(),
                  "Only trivial constructible types can be broadcasted");
  }

  template <class Closure, class ValueType>
  KOKKOS_INLINE_FUNCTION void team_broadcast(const Closure &f, ValueType &value,
                                             const int &thread_id) const {
    static_assert(std::is_trivially_default_constructible<ValueType>(),
                  "Only trivial constructible types can be broadcasted");
  }

  template <class ValueType, class JoinOp>
  KOKKOS_INLINE_FUNCTION ValueType team_reduce(const ValueType &value,
                                               const JoinOp &op_in) const {
    return value;
  }

  template <class ReducerType>
  KOKKOS_INLINE_FUNCTION
      typename std::enable_if<is_reducer<ReducerType>::value>::type
      team_reduce(const ReducerType &reducer) const {}

  template <typename Type>
  KOKKOS_INLINE_FUNCTION Type
  team_scan(const Type &value, Type *const global_accum = nullptr) const {
    if (global_accum) {
      Kokkos::atomic_fetch_add(global_accum, value);
    }

    return 0;
  }
};

template <class... Properties>
class TeamPolicyInternal<Kokkos::HPX, Properties...>
    : public PolicyTraits<Properties...> {
  using traits = PolicyTraits<Properties...>;

  int m_league_size;
  int m_team_size;
  std::size_t m_team_scratch_size[2];
  std::size_t m_thread_scratch_size[2];
  int m_chunk_size;

public:
  using member_type = HPXTeamMember;

  // NOTE: Max size is 1 for simplicity. In most cases more than 1 is not
  // necessary on CPU. Implement later if there is a need.
  template <class FunctorType>
  inline static int team_size_max(const FunctorType &) {
    return 1;
  }

  template <class FunctorType>
  inline static int team_size_recommended(const FunctorType &) {
    return 1;
  }

  template <class FunctorType>
  inline static int team_size_recommended(const FunctorType &, const int &) {
    return 1;
  }

  template <class FunctorType>
  int team_size_max(const FunctorType &, const ParallelForTag &) const {
    return 1;
  }

  template <class FunctorType>
  int team_size_max(const FunctorType &, const ParallelReduceTag &) const {
    return 1;
  }
  template <class FunctorType>
  int team_size_recommended(const FunctorType &, const ParallelForTag &) const {
    return 1;
  }
  template <class FunctorType>
  int team_size_recommended(const FunctorType &,
                            const ParallelReduceTag &) const {
    return 1;
  }

private:
  inline void init(const int league_size_request, const int team_size_request) {
    m_league_size = league_size_request;
    const int max_team_size = 1; // TODO: Can't use team_size_max(...) because
                                 // it requires a functor as argument.
    m_team_size =
        team_size_request > max_team_size ? max_team_size : team_size_request;

    if (m_chunk_size > 0) {
      if (!Impl::is_integral_power_of_two(m_chunk_size))
        Kokkos::abort("TeamPolicy blocking granularity must be power of two");
    } else {
      int new_chunk_size = 1;
      while (new_chunk_size * 4 * HPX::concurrency() < m_league_size) {
        new_chunk_size *= 2;
      }

      if (new_chunk_size < 128) {
        new_chunk_size = 1;
        while ((new_chunk_size * HPX::concurrency() < m_league_size) &&
               (new_chunk_size < 128))
          new_chunk_size *= 2;
      }

      m_chunk_size = new_chunk_size;
    }
  }

public:
  inline int team_size() const { return m_team_size; }
  inline int league_size() const { return m_league_size; }

  inline size_t scratch_size(const int &level, int team_size_ = -1) const {
    if (team_size_ < 0) {
      team_size_ = m_team_size;
    }
    return m_team_scratch_size[level] +
           team_size_ * m_thread_scratch_size[level];
  }

public:
  TeamPolicyInternal(typename traits::execution_space &,
                     int league_size_request, int team_size_request,
                     int /* vector_length_request */ = 1)
      : m_team_scratch_size{0, 0}, m_thread_scratch_size{0, 0},
        m_chunk_size(0) {
    init(league_size_request, team_size_request);
  }

  TeamPolicyInternal(typename traits::execution_space &,
                     int league_size_request,
                     const Kokkos::AUTO_t &team_size_request,
                     int /* vector_length_request */ = 1)
      : m_team_scratch_size{0, 0}, m_thread_scratch_size{0, 0},
        m_chunk_size(0) {
    init(league_size_request, 1);
  }

  TeamPolicyInternal(int league_size_request, int team_size_request,
                     int /* vector_length_request */ = 1)
      : m_team_scratch_size{0, 0}, m_thread_scratch_size{0, 0},
        m_chunk_size(0) {
    init(league_size_request, team_size_request);
  }

  TeamPolicyInternal(int league_size_request,
                     const Kokkos::AUTO_t &team_size_request,
                     int /* vector_length_request */ = 1)
      : m_team_scratch_size{0, 0}, m_thread_scratch_size{0, 0},
        m_chunk_size(0) {
    init(league_size_request, 1);
  }

  inline int chunk_size() const { return m_chunk_size; }

  inline TeamPolicyInternal &
  set_chunk_size(typename traits::index_type chunk_size_) {
    m_chunk_size = chunk_size_;
    return *this;
  }

  inline TeamPolicyInternal &set_scratch_size(const int &level,
                                              const PerTeamValue &per_team) {
    m_team_scratch_size[level] = per_team.value;
    return *this;
  }

  inline TeamPolicyInternal &
  set_scratch_size(const int &level, const PerThreadValue &per_thread) {
    m_thread_scratch_size[level] = per_thread.value;
    return *this;
  }

  inline TeamPolicyInternal &
  set_scratch_size(const int &level, const PerTeamValue &per_team,
                   const PerThreadValue &per_thread) {
    m_team_scratch_size[level] = per_team.value;
    m_thread_scratch_size[level] = per_thread.value;
    return *this;
  }
};
} // namespace Impl
} // namespace Kokkos

namespace Kokkos {
namespace Impl {

template <class FunctorType, class... Traits>
class ParallelFor<FunctorType, Kokkos::RangePolicy<Traits...>, Kokkos::HPX> {
private:
  using Policy = Kokkos::RangePolicy<Traits...>;
  using WorkTag = typename Policy::work_tag;
  using WorkRange = typename Policy::WorkRange;
  using Member = typename Policy::member_type;

  const FunctorType m_functor;
  const Policy m_policy;

  template <class TagType>
  static typename std::enable_if<std::is_same<TagType, void>::value>::type
  execute_functor(const FunctorType &functor, const Member i) {
    functor(i);
  }

  template <class TagType>
  static typename std::enable_if<!std::is_same<TagType, void>::value>::type
  execute_functor(const FunctorType &functor, const Member i) {
    const TagType t{};
    functor(t, i);
  }

  template <class TagType>
  static typename std::enable_if<std::is_same<TagType, void>::value>::type
  execute_functor_range(const FunctorType &functor, const Member i_begin,
                        const Member i_end) {
    for (Member i = i_begin; i < i_end; ++i) {
      functor(i);
    }
  }

  template <class TagType>
  static typename std::enable_if<!std::is_same<TagType, void>::value>::type
  execute_functor_range(const FunctorType &functor, const Member i_begin,
                        const Member i_end) {
    const TagType t{};
    for (Member i = i_begin; i < i_end; ++i) {
      functor(t, i);
    }
  }

public:
  void execute() const {
    Kokkos::Impl::run_hpx_function([this]() {

#if KOKKOS_HPX_IMPLEMENTATION == 0
      using hpx::parallel::execution::par;
      using hpx::parallel::execution::static_chunk_size;
      using hpx::parallel::for_loop;

      for_loop(execution::par.with(
                   execution::static_chunk_size(m_policy.chunk_size())),
               m_policy.begin(), m_policy.end(), [this](const Member i) {
                 execute_functor<WorkTag>(m_functor, i);
               });

#elif KOKKOS_HPX_IMPLEMENTATION == 1
      using hpx::apply;
      using hpx::lcos::local::counting_semaphore;

      counting_semaphore sem(0);
      std::size_t num_tasks = 0;

      for (Member i_begin = m_policy.begin(); i_begin < m_policy.end();
           i_begin += m_policy.chunk_size()) {
        apply([this, &sem, i_begin]() {
          const Member i_end =
              (std::min)(i_begin + m_policy.chunk_size(), m_policy.end());
          execute_functor_range<WorkTag>(m_functor, i_begin, i_end);

          sem.signal(1);
        });

        ++num_tasks;
      }

      sem.wait(num_tasks);
#endif
    });
  }

  inline ParallelFor(const FunctorType &arg_functor, Policy arg_policy)
      : m_functor(arg_functor), m_policy(arg_policy) {}
}; // namespace Impl

template <class FunctorType, class... Traits>
class ParallelFor<FunctorType, Kokkos::MDRangePolicy<Traits...>, Kokkos::HPX> {
private:
  using MDRangePolicy = Kokkos::MDRangePolicy<Traits...>;
  using Policy = typename MDRangePolicy::impl_range_policy;
  using WorkTag = typename MDRangePolicy::work_tag;
  using WorkRange = typename Policy::WorkRange;
  using Member = typename Policy::member_type;
  using iterate_type =
      typename Kokkos::Impl::HostIterateTile<MDRangePolicy, FunctorType,
                                             WorkTag, void>;

  const FunctorType m_functor;
  const MDRangePolicy m_mdr_policy;
  const Policy m_policy;

public:
  inline void execute() const {
    Kokkos::Impl::run_hpx_function([this]() {
#if KOKKOS_HPX_IMPLEMENTATION == 0
      using hpx::parallel::execution::par;
      using hpx::parallel::execution::static_chunk_size;
      using hpx::parallel::for_loop;

      for_loop(par.with(static_chunk_size(m_policy.chunk_size())),
               m_policy.begin(), m_policy.end(), [this](const Member i) {
                 iterate_type(m_mdr_policy, m_functor)(i);
               });

#elif KOKKOS_HPX_IMPLEMENTATION == 1
      using hpx::apply;
      using hpx::lcos::local::counting_semaphore;

      counting_semaphore sem(0);
      std::size_t num_tasks = 0;

      for (Member i_begin = m_policy.begin(); i_begin < m_policy.end();
           i_begin += m_policy.chunk_size()) {
        apply([this, &sem, i_begin]() {
          const Member i_end =
              (std::min)(i_begin + m_policy.chunk_size(), m_policy.end());
          for (Member i = i_begin; i < i_end; ++i) {
            iterate_type(m_mdr_policy, m_functor)(i);
          }

          sem.signal(1);
        });

        ++num_tasks;
      }

      sem.wait(num_tasks);
#endif
    });
  }

  inline ParallelFor(const FunctorType &arg_functor, MDRangePolicy arg_policy)
      : m_functor(arg_functor), m_mdr_policy(arg_policy),
        m_policy(Policy(0, m_mdr_policy.m_num_tiles).set_chunk_size(1)) {}
}; // namespace Impl
} // namespace Impl
} // namespace Kokkos

namespace Kokkos {
namespace Impl {
template <class FunctorType, class ReducerType, class... Traits>
class ParallelReduce<FunctorType, Kokkos::RangePolicy<Traits...>, ReducerType,
                     Kokkos::HPX> {
private:
  using Policy = Kokkos::RangePolicy<Traits...>;
  using WorkTag = typename Policy::work_tag;
  using WorkRange = typename Policy::WorkRange;
  using Member = typename Policy::member_type;
  using Analysis =
      FunctorAnalysis<FunctorPatternInterface::REDUCE, Policy, FunctorType>;
  using ReducerConditional =
      Kokkos::Impl::if_c<std::is_same<InvalidType, ReducerType>::value,
                         FunctorType, ReducerType>;
  using ReducerTypeFwd = typename ReducerConditional::type;
  using WorkTagFwd =
      typename Kokkos::Impl::if_c<std::is_same<InvalidType, ReducerType>::value,
                                  WorkTag, void>::type;
  using ValueInit = Kokkos::Impl::FunctorValueInit<ReducerTypeFwd, WorkTagFwd>;
  using ValueJoin = Kokkos::Impl::FunctorValueJoin<ReducerTypeFwd, WorkTagFwd>;
  using ValueOps = Kokkos::Impl::FunctorValueOps<ReducerTypeFwd, WorkTagFwd>;
  using value_type = typename Analysis::value_type;
  using pointer_type = typename Analysis::pointer_type;
  using reference_type = typename Analysis::reference_type;

  const FunctorType m_functor;
  const Policy m_policy;
  const ReducerType m_reducer;
  const pointer_type m_result_ptr;

  template <class TagType>
  inline static
      typename std::enable_if<std::is_same<TagType, void>::value>::type
      execute_functor(const FunctorType &functor, const Member i,
                      reference_type update) {
    functor(i, update);
  }

  template <class TagType>
  inline static
      typename std::enable_if<!std::is_same<TagType, void>::value>::type
      execute_functor(const FunctorType &functor, const Member i,
                      reference_type update) {
    const TagType t{};
    functor(t, i, update);
  }

  template <class TagType>
  inline typename std::enable_if<std::is_same<TagType, void>::value>::type
  execute_functor_range(reference_type update, const Member i_begin,
                        const Member i_end) const {
    for (Member i = i_begin; i < i_end; ++i) {
      m_functor(i, update);
    }
  }

  template <class TagType>
  inline typename std::enable_if<!std::is_same<TagType, void>::value>::type
  execute_functor_range(reference_type update, const Member i_begin,
                        const Member i_end) const {
    const TagType t{};

    for (Member i = i_begin; i < i_end; ++i) {
      m_functor(t, i, update);
    }
  }

public:
  inline void execute() const {

    Kokkos::Impl::run_hpx_function([this]() {
      const int num_worker_threads = HPX::concurrency();

      std::size_t value_size = Analysis::value_size(
          ReducerConditional::select(m_functor, m_reducer));

      thread_buffer &buffer = HPX::get_buffer();
      buffer.resize(num_worker_threads, value_size);

      using hpx::parallel::execution::par;
      using hpx::parallel::execution::static_chunk_size;
      using hpx::parallel::for_loop;

      for_loop(par, 0, num_worker_threads, [this, &buffer](std::size_t t) {
        ValueInit::init(ReducerConditional::select(m_functor, m_reducer),
                        reinterpret_cast<pointer_type>(buffer.get(t)));
      });

      // TODO: parallel::for_loop with reduction
      // NOTE: At least works for simple types, not tested for cases where
      // reference_type is a pointer.

      // using hpx::parallel::execution::static_chunk_size;
      // using hpx::parallel::reduction;
      //
      // value_type init{};
      // for_loop(
      //     par.with(static_chunk_size(m_policy.chunk_size())),
      //     m_policy.begin(), m_policy.end(), reduction(
      //         ValueOps::reference(reinterpret_cast<pointer_type>(buffer.get(0))),
      //         init,
      //         [this](reference_type a, reference_type b) {
      //           ValueJoin::join(
      //               ReducerConditional::select(m_functor, m_reducer),
      //               reinterpret_cast<pointer_type>(&a),
      //               reinterpret_cast<pointer_type>(&b));
      //           return a;
      //         }),
      //     [this](Member i, reference_type update) {
      //       execute_functor<WorkTag>(m_functor, i, update);
      //     });

#if KOKKOS_HPX_IMPLEMENTATION == 0
      for_loop(par.with(static_chunk_size(m_policy.chunk_size())),
               m_policy.begin(), m_policy.end(),
               [this, &buffer](const Member i) {
                 reference_type update =
                     ValueOps::reference(reinterpret_cast<pointer_type>(
                         buffer.get(HPX::impl_hardware_thread_id())));
                 execute_functor<WorkTag>(m_functor, i, update);
               });

#elif KOKKOS_HPX_IMPLEMENTATION == 1
      using hpx::apply;
      using hpx::lcos::local::counting_semaphore;

      counting_semaphore sem(0);
      std::size_t num_tasks = 0;

      for (Member i_begin = m_policy.begin(); i_begin < m_policy.end();
           i_begin += m_policy.chunk_size()) {
        apply([this, &buffer, &sem, i_begin]() {
          reference_type update =
              ValueOps::reference(reinterpret_cast<pointer_type>(
                  buffer.get(HPX::impl_hardware_thread_id())));
          const Member i_end =
              (std::min)(i_begin + m_policy.chunk_size(), m_policy.end());
          execute_functor_range<WorkTag>(update, i_begin, i_end);

          sem.signal(1);
        });

        ++num_tasks;
      }

      sem.wait(num_tasks);
#endif

      for (int i = 1; i < num_worker_threads; ++i) {
        ValueJoin::join(ReducerConditional::select(m_functor, m_reducer),
                        reinterpret_cast<pointer_type>(buffer.get(0)),
                        reinterpret_cast<pointer_type>(buffer.get(i)));
      }

      Kokkos::Impl::FunctorFinal<ReducerTypeFwd, WorkTagFwd>::final(
          ReducerConditional::select(m_functor, m_reducer),
          reinterpret_cast<pointer_type>(buffer.get(0)));

      if (m_result_ptr != nullptr) {
        const int n = Analysis::value_count(
            ReducerConditional::select(m_functor, m_reducer));

        for (int j = 0; j < n; ++j) {
          m_result_ptr[j] = reinterpret_cast<pointer_type>(buffer.get(0))[j];
        }
      }
    });
  }

  template <class ViewType>
  inline ParallelReduce(
      const FunctorType &arg_functor, Policy arg_policy,
      const ViewType &arg_view,
      typename std::enable_if<Kokkos::is_view<ViewType>::value &&
                                  !Kokkos::is_reducer_type<ReducerType>::value,
                              void *>::type = NULL)
      : m_functor(arg_functor), m_policy(arg_policy), m_reducer(InvalidType()),
        m_result_ptr(arg_view.data()) {}

  inline ParallelReduce(const FunctorType &arg_functor, Policy arg_policy,
                        const ReducerType &reducer)
      : m_functor(arg_functor), m_policy(arg_policy), m_reducer(reducer),
        m_result_ptr(reducer.view().data()) {}
};

template <class FunctorType, class ReducerType, class... Traits>
class ParallelReduce<FunctorType, Kokkos::MDRangePolicy<Traits...>, ReducerType,
                     Kokkos::HPX> {
private:
  using MDRangePolicy = Kokkos::MDRangePolicy<Traits...>;
  using Policy = typename MDRangePolicy::impl_range_policy;
  using WorkTag = typename MDRangePolicy::work_tag;
  using WorkRange = typename Policy::WorkRange;
  using Member = typename Policy::member_type;
  using Analysis = FunctorAnalysis<FunctorPatternInterface::REDUCE,
                                   MDRangePolicy, FunctorType>;
  using ReducerConditional =
      Kokkos::Impl::if_c<std::is_same<InvalidType, ReducerType>::value,
                         FunctorType, ReducerType>;
  using ReducerTypeFwd = typename ReducerConditional::type;
  using WorkTagFwd =
      typename Kokkos::Impl::if_c<std::is_same<InvalidType, ReducerType>::value,
                                  WorkTag, void>::type;
  using ValueInit = Kokkos::Impl::FunctorValueInit<ReducerTypeFwd, WorkTagFwd>;
  using ValueJoin = Kokkos::Impl::FunctorValueJoin<ReducerTypeFwd, WorkTagFwd>;
  using ValueOps = Kokkos::Impl::FunctorValueOps<ReducerTypeFwd, WorkTagFwd>;
  using pointer_type = typename Analysis::pointer_type;
  using value_type = typename Analysis::value_type;
  using reference_type = typename Analysis::reference_type;
  using iterate_type =
      typename Kokkos::Impl::HostIterateTile<MDRangePolicy, FunctorType,
                                             WorkTag, reference_type>;

  const FunctorType m_functor;
  const MDRangePolicy m_mdr_policy;
  const Policy m_policy;
  const ReducerType m_reducer;
  const pointer_type m_result_ptr;

public:
  inline void execute() const {
    Kokkos::Impl::run_hpx_function([this]() {
      const int num_worker_threads = HPX::concurrency();
      const std::size_t value_size = Analysis::value_size(
          ReducerConditional::select(m_functor, m_reducer));

      thread_buffer &buffer = HPX::get_buffer();
      buffer.resize(num_worker_threads, value_size);

      using hpx::parallel::execution::par;
      using hpx::parallel::for_loop;

      for_loop(par, 0, num_worker_threads, [this, &buffer](std::size_t t) {
        ValueInit::init(ReducerConditional::select(m_functor, m_reducer),
                        reinterpret_cast<pointer_type>(buffer.get(t)));
      });

#if KOKKOS_HPX_IMPLEMENTATION == 0
      using hpx::parallel::execution::static_chunk_size;

      for_loop(par.with(static_chunk_size(m_policy.chunk_size())),
               m_policy.begin(), m_policy.end(),
               [this, &buffer](const Member i) {
                 reference_type update =
                     ValueOps::reference(reinterpret_cast<pointer_type>(
                         buffer.get(HPX::impla_hardware_thread_id())));
                 iterate_type(m_mdr_policy, m_functor, update)(i);
               });

#elif KOKKOS_HPX_IMPLEMENTATION == 1
      using hpx::apply;
      using hpx::lcos::local::counting_semaphore;

      counting_semaphore sem(0);
      std::size_t num_tasks = 0;

      for (Member i_begin = m_policy.begin(); i_begin < m_policy.end();
           i_begin += m_policy.chunk_size()) {
        apply([this, &buffer, &sem, i_begin]() {
          reference_type update =
              ValueOps::reference(reinterpret_cast<pointer_type>(
                  buffer.get(HPX::impl_hardware_thread_id())));
          const Member i_end =
              (std::min)(i_begin + m_policy.chunk_size(), m_policy.end());

          for (Member i = i_begin; i < i_end; ++i) {
            iterate_type(m_mdr_policy, m_functor, update)(i);
          }

          sem.signal(1);
        });

        ++num_tasks;
      }

      sem.wait(num_tasks);
#endif

      for (int i = 1; i < num_worker_threads; ++i) {
        ValueJoin::join(ReducerConditional::select(m_functor, m_reducer),
                        reinterpret_cast<pointer_type>(buffer.get(0)),
                        reinterpret_cast<pointer_type>(buffer.get(i)));
      }

      Kokkos::Impl::FunctorFinal<ReducerTypeFwd, WorkTagFwd>::final(
          ReducerConditional::select(m_functor, m_reducer),
          reinterpret_cast<pointer_type>(buffer.get(0)));

      if (m_result_ptr != nullptr) {
        const int n = Analysis::value_count(
            ReducerConditional::select(m_functor, m_reducer));

        for (int j = 0; j < n; ++j) {
          m_result_ptr[j] = reinterpret_cast<pointer_type>(buffer.get(0))[j];
        }
      }
    });
  }

  template <class ViewType>
  inline ParallelReduce(
      const FunctorType &arg_functor, MDRangePolicy arg_policy,
      const ViewType &arg_view,
      typename std::enable_if<Kokkos::is_view<ViewType>::value &&
                                  !Kokkos::is_reducer_type<ReducerType>::value,
                              void *>::type = NULL)
      : m_functor(arg_functor), m_mdr_policy(arg_policy),
        m_policy(Policy(0, m_mdr_policy.m_num_tiles).set_chunk_size(1)),
        m_reducer(InvalidType()), m_result_ptr(arg_view.data()) {}

  inline ParallelReduce(const FunctorType &arg_functor,
                        MDRangePolicy arg_policy, const ReducerType &reducer)
      : m_functor(arg_functor), m_mdr_policy(arg_policy),
        m_policy(Policy(0, m_mdr_policy.m_num_tiles).set_chunk_size(1)),
        m_reducer(reducer), m_result_ptr(reducer.view().data()) {}
};
} // namespace Impl
} // namespace Kokkos

namespace Kokkos {
namespace Impl {

template <class FunctorType, class... Traits>
class ParallelScan<FunctorType, Kokkos::RangePolicy<Traits...>, Kokkos::HPX> {
private:
  using Policy = Kokkos::RangePolicy<Traits...>;
  using WorkTag = typename Policy::work_tag;
  using WorkRange = typename Policy::WorkRange;
  using Member = typename Policy::member_type;
  using Analysis =
      FunctorAnalysis<FunctorPatternInterface::SCAN, Policy, FunctorType>;
  using ValueInit = Kokkos::Impl::FunctorValueInit<FunctorType, WorkTag>;
  using ValueJoin = Kokkos::Impl::FunctorValueJoin<FunctorType, WorkTag>;
  using ValueOps = Kokkos::Impl::FunctorValueOps<FunctorType, WorkTag>;
  using pointer_type = typename Analysis::pointer_type;
  using reference_type = typename Analysis::reference_type;
  using value_type = typename Analysis::value_type;

  const FunctorType m_functor;
  const Policy m_policy;

  template <class TagType>
  inline static
      typename std::enable_if<std::is_same<TagType, void>::value>::type
      execute_functor_range(const FunctorType &functor, const Member i_begin,
                            const Member i_end, reference_type update,
                            const bool final) {
    for (Member i = i_begin; i < i_end; ++i) {
      functor(i, update, final);
    }
  }

  template <class TagType>
  inline static
      typename std::enable_if<!std::is_same<TagType, void>::value>::type
      execute_functor_range(const FunctorType &functor, const Member i_begin,
                            const Member i_end, reference_type update,
                            const bool final) {
    const TagType t{};
    for (Member i = i_begin; i < i_end; ++i) {
      functor(t, i, update, final);
    }
  }

public:
  inline void execute() const {
    Kokkos::Impl::run_hpx_function([this]() {
      const int num_worker_threads = HPX::concurrency();
      const int value_count = Analysis::value_count(m_functor);
      const std::size_t value_size = Analysis::value_size(m_functor);

      thread_buffer &buffer = HPX::get_buffer();
      buffer.resize(num_worker_threads, 2 * value_size);

      using hpx::lcos::local::barrier;
      using hpx::parallel::execution::par;
      using hpx::parallel::execution::static_chunk_size;
      using hpx::parallel::for_loop;

      barrier bar(num_worker_threads);

      for_loop(
          par.with(static_chunk_size(1)), 0, num_worker_threads,
          [this, &buffer, &bar, num_worker_threads, value_count,
           value_size](std::size_t const t) {
            reference_type update_sum = ValueInit::init(
                m_functor, reinterpret_cast<pointer_type>(buffer.get(t)));

            const WorkRange range(m_policy, t, num_worker_threads);
            execute_functor_range<WorkTag>(m_functor, range.begin(),
                                           range.end(), update_sum, false);

            bar.wait();

            if (t == 0) {
              ValueInit::init(m_functor, reinterpret_cast<pointer_type>(
                                             buffer.get(0) + value_size));

              for (int i = 1; i < num_worker_threads; ++i) {
                pointer_type ptr_1_prev =
                    reinterpret_cast<pointer_type>(buffer.get(i - 1));
                pointer_type ptr_2_prev = reinterpret_cast<pointer_type>(
                    buffer.get(i - 1) + value_size);
                pointer_type ptr_2 =
                    reinterpret_cast<pointer_type>(buffer.get(i) + value_size);

                for (int j = 0; j < value_count; ++j) {
                  ptr_2[j] = ptr_2_prev[j];
                }

                ValueJoin::join(m_functor, ptr_2, ptr_1_prev);
              }
            }

            bar.wait();

            reference_type update_base = ValueOps::reference(
                reinterpret_cast<pointer_type>(buffer.get(t) + value_size));

            execute_functor_range<WorkTag>(m_functor, range.begin(),
                                           range.end(), update_base, true);
          });
    });
  }

  inline ParallelScan(const FunctorType &arg_functor, const Policy &arg_policy)
      : m_functor(arg_functor), m_policy(arg_policy) {}
}; // namespace Impl

template <class FunctorType, class ReturnType, class... Traits>
class ParallelScanWithTotal<FunctorType, Kokkos::RangePolicy<Traits...>,
                            ReturnType, Kokkos::HPX> {
private:
  using Policy = Kokkos::RangePolicy<Traits...>;
  using WorkTag = typename Policy::work_tag;
  using WorkRange = typename Policy::WorkRange;
  using Member = typename Policy::member_type;
  using Analysis =
      FunctorAnalysis<FunctorPatternInterface::SCAN, Policy, FunctorType>;
  using ValueInit = Kokkos::Impl::FunctorValueInit<FunctorType, WorkTag>;
  using ValueJoin = Kokkos::Impl::FunctorValueJoin<FunctorType, WorkTag>;
  using ValueOps = Kokkos::Impl::FunctorValueOps<FunctorType, WorkTag>;
  using pointer_type = typename Analysis::pointer_type;
  using reference_type = typename Analysis::reference_type;
  using value_type = typename Analysis::value_type;

  const FunctorType m_functor;
  const Policy m_policy;
  ReturnType &m_returnvalue;

  template <class TagType>
  inline static
      typename std::enable_if<std::is_same<TagType, void>::value>::type
      execute_functor_range(const FunctorType &functor, const Member i_begin,
                            const Member i_end, reference_type update,
                            const bool final) {
    for (Member i = i_begin; i < i_end; ++i) {
      functor(i, update, final);
    }
  }

  template <class TagType>
  inline static
      typename std::enable_if<!std::is_same<TagType, void>::value>::type
      execute_functor_range(const FunctorType &functor, const Member i_begin,
                            const Member i_end, reference_type update,
                            const bool final) {
    const TagType t{};
    for (Member i = i_begin; i < i_end; ++i) {
      functor(t, i, update, final);
    }
  }

public:
  inline void execute() const {
    Kokkos::Impl::run_hpx_function([this]() {
      const int num_worker_threads = HPX::concurrency();
      const int value_count = Analysis::value_count(m_functor);
      const std::size_t value_size = Analysis::value_size(m_functor);

      thread_buffer &buffer = HPX::get_buffer();
      buffer.resize(num_worker_threads, 2 * value_size);

      using hpx::lcos::local::barrier;
      using hpx::parallel::execution::par;
      using hpx::parallel::execution::static_chunk_size;
      using hpx::parallel::for_loop;

      barrier bar(num_worker_threads);

      for_loop(
          par.with(static_chunk_size(1)), 0, num_worker_threads,
          [this, &buffer, &bar, num_worker_threads, value_count,
           value_size](std::size_t const t) {
            reference_type update_sum = ValueInit::init(
                m_functor, reinterpret_cast<pointer_type>(buffer.get(t)));

            const WorkRange range(m_policy, t, num_worker_threads);
            execute_functor_range<WorkTag>(m_functor, range.begin(),
                                           range.end(), update_sum, false);

            bar.wait();

            if (t == 0) {
              ValueInit::init(m_functor, reinterpret_cast<pointer_type>(
                                             buffer.get(0) + value_size));

              for (int i = 1; i < num_worker_threads; ++i) {
                pointer_type ptr_1_prev =
                    reinterpret_cast<pointer_type>(buffer.get(i - 1));
                pointer_type ptr_2_prev = reinterpret_cast<pointer_type>(
                    buffer.get(i - 1) + value_size);
                pointer_type ptr_2 =
                    reinterpret_cast<pointer_type>(buffer.get(i) + value_size);

                for (int j = 0; j < value_count; ++j) {
                  ptr_2[j] = ptr_2_prev[j];
                }

                ValueJoin::join(m_functor, ptr_2, ptr_1_prev);
              }
            }

            bar.wait();

            reference_type update_base = ValueOps::reference(
                reinterpret_cast<pointer_type>(buffer.get(t) + value_size));

            execute_functor_range<WorkTag>(m_functor, range.begin(),
                                           range.end(), update_base, true);

            if (t == std::size_t(num_worker_threads - 1)) {
              m_returnvalue = update_base;
            }
          });
    });
  }

  inline ParallelScanWithTotal(const FunctorType &arg_functor,
                               const Policy &arg_policy,
                               ReturnType &arg_returnvalue)
      : m_functor(arg_functor), m_policy(arg_policy),
        m_returnvalue(arg_returnvalue) {}
}; // namespace Impl
} // namespace Impl
} // namespace Kokkos

namespace Kokkos {
namespace Impl {
template <class FunctorType, class... Properties>
class ParallelFor<FunctorType, Kokkos::TeamPolicy<Properties...>, Kokkos::HPX> {
private:
  using Policy = TeamPolicyInternal<Kokkos::HPX, Properties...>;
  using WorkTag = typename Policy::work_tag;
  using Member = typename Policy::member_type;
  using memory_space = Kokkos::HostSpace;

  const FunctorType m_functor;
  const Policy m_policy;
  const int m_league;
  const std::size_t m_shared;

  template <class TagType>
  inline static
      typename std::enable_if<std::is_same<TagType, void>::value>::type
      execute_functor(const FunctorType &functor, const Policy &policy,
                      const int league_rank, char *local_buffer,
                      const std::size_t local_buffer_size) {
    functor(Member(policy, 0, league_rank, local_buffer, local_buffer_size));
  }

  template <class TagType>
  inline static
      typename std::enable_if<!std::is_same<TagType, void>::value>::type
      execute_functor(const FunctorType &functor, const Policy &policy,
                      const int league_rank, char *local_buffer,
                      const std::size_t local_buffer_size) {
    const TagType t{};
    functor(t, Member(policy, 0, league_rank, local_buffer, local_buffer_size));
  }

  template <class TagType>
  inline static
      typename std::enable_if<std::is_same<TagType, void>::value>::type
      execute_functor_range(const FunctorType &functor, const Policy &policy,
                            const int league_rank_begin,
                            const int league_rank_end, char *local_buffer,
                            const std::size_t local_buffer_size) {
    for (int league_rank = league_rank_begin; league_rank < league_rank_end;
         ++league_rank) {
      functor(Member(policy, 0, league_rank, local_buffer, local_buffer_size));
    }
  }

  template <class TagType>
  inline static
      typename std::enable_if<!std::is_same<TagType, void>::value>::type
      execute_functor_range(const FunctorType &functor, const Policy &policy,
                            const int league_rank_begin,
                            const int league_rank_end, char *local_buffer,
                            const std::size_t local_buffer_size) {
    const TagType t{};
    for (int league_rank = league_rank_begin; league_rank < league_rank_end;
         ++league_rank) {
      functor(t,
              Member(policy, 0, league_rank, local_buffer, local_buffer_size));
    }
  }

public:
  inline void execute() const {
    Kokkos::Impl::run_hpx_function([this]() {
      const int num_worker_threads = HPX::concurrency();

      thread_buffer &buffer = HPX::get_buffer();
      buffer.resize(num_worker_threads, m_shared);

#if KOKKOS_HPX_IMPLEMENTATION == 0
      using hpx::parallel::execution::par;
      using hpx::parallel::execution::static_chunk_size;
      using hpx::parallel::for_loop;

      for_loop(par.with(static_chunk_size(chunk_size)), 0,
               m_policy.league_size(), [this, &buffer](const int league_rank) {
                 execute_functor<WorkTag>(
                     m_functor, m_policy, league_rank,
                     buffer.get(HPX::impl_hardware_thread_id()), m_shared);
               });

#elif KOKKOS_HPX_IMPLEMENTATION == 1
      using hpx::apply;
      using hpx::lcos::local::counting_semaphore;

      counting_semaphore sem(0);
      std::size_t num_tasks = 0;

      for (int league_rank_begin = 0;
           league_rank_begin < m_policy.league_size();
           league_rank_begin += m_policy.chunk_size()) {
        apply([this, &buffer, &sem, league_rank_begin]() {
          const int league_rank_end =
              (std::min)(league_rank_begin + m_policy.chunk_size(),
                         m_policy.league_size());
          execute_functor_range<WorkTag>(
              m_functor, m_policy, league_rank_begin, league_rank_end,
              buffer.get(HPX::impl_hardware_thread_id()), m_shared);

          sem.signal(1);
        });

        ++num_tasks;
      }

      sem.wait(num_tasks);
#endif
    });
  }

  ParallelFor(const FunctorType &arg_functor, const Policy &arg_policy)
      : m_functor(arg_functor), m_policy(arg_policy),
        m_league(arg_policy.league_size()),
        m_shared(arg_policy.scratch_size(0) + arg_policy.scratch_size(1) +
                 FunctorTeamShmemSize<FunctorType>::value(
                     arg_functor, arg_policy.team_size())) {}
}; // namespace Impl

template <class FunctorType, class ReducerType, class... Properties>
class ParallelReduce<FunctorType, Kokkos::TeamPolicy<Properties...>,
                     ReducerType, Kokkos::HPX> {
private:
  using Policy = TeamPolicyInternal<Kokkos::HPX, Properties...>;
  using Analysis =
      FunctorAnalysis<FunctorPatternInterface::REDUCE, Policy, FunctorType>;
  using Member = typename Policy::member_type;
  using WorkTag = typename Policy::work_tag;
  using ReducerConditional =
      Kokkos::Impl::if_c<std::is_same<InvalidType, ReducerType>::value,
                         FunctorType, ReducerType>;
  using ReducerTypeFwd = typename ReducerConditional::type;
  using WorkTagFwd =
      typename Kokkos::Impl::if_c<std::is_same<InvalidType, ReducerType>::value,
                                  WorkTag, void>::type;
  using ValueInit = Kokkos::Impl::FunctorValueInit<ReducerTypeFwd, WorkTagFwd>;
  using ValueJoin = Kokkos::Impl::FunctorValueJoin<ReducerTypeFwd, WorkTagFwd>;
  using ValueOps = Kokkos::Impl::FunctorValueOps<ReducerTypeFwd, WorkTagFwd>;
  using pointer_type = typename Analysis::pointer_type;
  using reference_type = typename Analysis::reference_type;
  using value_type = typename Analysis::value_type;

  const FunctorType m_functor;
  const int m_league;
  const Policy m_policy;
  const ReducerType m_reducer;
  const std::size_t m_shared;
  pointer_type m_result_ptr;

  template <class TagType>
  inline static
      typename std::enable_if<std::is_same<TagType, void>::value>::type
      execute_functor(const FunctorType &functor, const Policy &policy,
                      const int league_rank, char *local_buffer,
                      const std::size_t local_buffer_size,
                      reference_type update) {
    functor(Member(policy, 0, league_rank, local_buffer, local_buffer_size),
            update);
  }

  template <class TagType>
  inline static
      typename std::enable_if<!std::is_same<TagType, void>::value>::type
      execute_functor(const FunctorType &functor, const Policy &policy,
                      const int league_rank, char *local_buffer,
                      const std::size_t local_buffer_size,
                      reference_type update) {
    const TagType t{};
    functor(t, Member(policy, 0, league_rank, local_buffer, local_buffer_size),
            update);
  }

  template <class TagType>
  inline static
      typename std::enable_if<std::is_same<TagType, void>::value>::type
      execute_functor_range(const FunctorType &functor, const Policy &policy,
                            const int league_rank_begin,
                            const int league_rank_end, char *local_buffer,
                            const std::size_t local_buffer_size,
                            reference_type update) {
    for (int league_rank = league_rank_begin; league_rank < league_rank_end;
         ++league_rank) {
      functor(Member(policy, 0, league_rank, local_buffer, local_buffer_size),
              update);
    }
  }

  template <class TagType>
  inline static
      typename std::enable_if<!std::is_same<TagType, void>::value>::type
      execute_functor_range(const FunctorType &functor, const Policy &policy,
                            const int league_rank_begin,
                            const int league_rank_end, char *local_buffer,
                            const std::size_t local_buffer_size,
                            reference_type update) {
    const TagType t{};
    for (int league_rank = league_rank_begin; league_rank < league_rank_end;
         ++league_rank) {
      functor(t,
              Member(policy, 0, league_rank, local_buffer, local_buffer_size),
              update);
    }
  }

public:
  inline void execute() const {
    Kokkos::Impl::run_hpx_function([this]() {
      const int league_size = m_policy.league_size();
      const int chunk_size = m_policy.chunk_size();
      const int num_worker_threads = HPX::concurrency();
      const std::size_t value_size = Analysis::value_size(
          ReducerConditional::select(m_functor, m_reducer));

      thread_buffer &buffer = HPX::get_buffer();
      buffer.resize(num_worker_threads, value_size + m_shared);

      using hpx::parallel::execution::par;
      using hpx::parallel::for_loop;

      for_loop(par, 0, num_worker_threads, [this, &buffer](std::size_t t) {
        ValueInit::init(ReducerConditional::select(m_functor, m_reducer),
                        reinterpret_cast<pointer_type>(buffer.get(t)));
      });

#if KOKKOS_HPX_IMPLEMENTATION == 0
      using hpx::parallel::execution::static_chunk_size;

      hpx::parallel::for_loop(
          par.with(static_chunk_size(chunk_size)), 0, league_size,
          [this, &buffer, league_size, value_size](const int league_rank) {
            std::size_t t = HPX::impl_hardware_thread_id();
            reference_type update = ValueOps::reference(
                reinterpret_cast<pointer_type>(buffer.get(t)));

            execute_functor<WorkTag>(m_functor, m_policy, league_rank,
                                     buffer.get(t) + value_size, m_shared,
                                     update);
          });

#elif KOKKOS_HPX_IMPLEMENTATION == 1
      using hpx::apply;
      using hpx::lcos::local::counting_semaphore;

      counting_semaphore sem(0);
      std::size_t num_tasks = 0;

      for (int league_rank_begin = 0; league_rank_begin < league_size;
           league_rank_begin += m_policy.chunk_size()) {
        apply([this, &buffer, &sem, league_rank_begin, league_size,
               value_size]() {
          std::size_t t = HPX::impl_hardware_thread_id();
          reference_type update = ValueOps::reference(
              reinterpret_cast<pointer_type>(buffer.get(t)));
          const int league_rank_end = (std::min)(
              league_rank_begin + m_policy.chunk_size(), league_size);
          execute_functor_range<WorkTag>(
              m_functor, m_policy, league_rank_begin, league_rank_end,
              buffer.get(t) + value_size, m_shared, update);

          sem.signal(1);
        });

        ++num_tasks;
      }

      sem.wait(num_tasks);
#endif

      const pointer_type ptr = reinterpret_cast<pointer_type>(buffer.get(0));
      for (int t = 1; t < num_worker_threads; ++t) {
        ValueJoin::join(ReducerConditional::select(m_functor, m_reducer), ptr,
                        reinterpret_cast<pointer_type>(buffer.get(t)));
      }

      Kokkos::Impl::FunctorFinal<ReducerTypeFwd, WorkTagFwd>::final(
          ReducerConditional::select(m_functor, m_reducer), ptr);

      if (m_result_ptr) {
        const int n = Analysis::value_count(
            ReducerConditional::select(m_functor, m_reducer));

        for (int j = 0; j < n; ++j) {
          m_result_ptr[j] = ptr[j];
        }
      }
    });
  }

  template <class ViewType>
  ParallelReduce(
      const FunctorType &arg_functor, const Policy &arg_policy,
      const ViewType &arg_result,
      typename std::enable_if<Kokkos::is_view<ViewType>::value &&
                                  !Kokkos::is_reducer_type<ReducerType>::value,
                              void *>::type = NULL)
      : m_functor(arg_functor), m_league(arg_policy.league_size()),
        m_policy(arg_policy), m_reducer(InvalidType()),
        m_result_ptr(arg_result.data()),
        m_shared(arg_policy.scratch_size(0) + arg_policy.scratch_size(1) +
                 FunctorTeamShmemSize<FunctorType>::value(
                     m_functor, arg_policy.team_size())) {}

  inline ParallelReduce(const FunctorType &arg_functor, Policy arg_policy,
                        const ReducerType &reducer)
      : m_functor(arg_functor), m_league(arg_policy.league_size()),
        m_policy(arg_policy), m_reducer(reducer),
        m_result_ptr(reducer.view().data()),
        m_shared(arg_policy.scratch_size(0) + arg_policy.scratch_size(1) +
                 FunctorTeamShmemSize<FunctorType>::value(
                     arg_functor, arg_policy.team_size())) {}
};
} // namespace Impl
} // namespace Kokkos

namespace Kokkos {

template <typename iType>
KOKKOS_INLINE_FUNCTION
    Impl::TeamThreadRangeBoundariesStruct<iType, Impl::HPXTeamMember>
    TeamThreadRange(const Impl::HPXTeamMember &thread, const iType &count) {
  return Impl::TeamThreadRangeBoundariesStruct<iType, Impl::HPXTeamMember>(
      thread, count);
}

template <typename iType1, typename iType2>
KOKKOS_INLINE_FUNCTION Impl::TeamThreadRangeBoundariesStruct<
    typename std::common_type<iType1, iType2>::type, Impl::HPXTeamMember>
TeamThreadRange(const Impl::HPXTeamMember &thread, const iType1 &i_begin,
                const iType2 &i_end) {
  using iType = typename std::common_type<iType1, iType2>::type;
  return Impl::TeamThreadRangeBoundariesStruct<iType, Impl::HPXTeamMember>(
      thread, iType(i_begin), iType(i_end));
}

template <typename iType>
KOKKOS_INLINE_FUNCTION
    Impl::ThreadVectorRangeBoundariesStruct<iType, Impl::HPXTeamMember>
    ThreadVectorRange(const Impl::HPXTeamMember &thread, const iType &count) {
  return Impl::ThreadVectorRangeBoundariesStruct<iType, Impl::HPXTeamMember>(
      thread, count);
}

template <typename iType>
KOKKOS_INLINE_FUNCTION
    Impl::ThreadVectorRangeBoundariesStruct<iType, Impl::HPXTeamMember>
    ThreadVectorRange(const Impl::HPXTeamMember &thread, const iType &i_begin,
                      const iType &i_end) {
  return Impl::ThreadVectorRangeBoundariesStruct<iType, Impl::HPXTeamMember>(
      thread, i_begin, i_end);
}

KOKKOS_INLINE_FUNCTION
Impl::ThreadSingleStruct<Impl::HPXTeamMember>
PerTeam(const Impl::HPXTeamMember &thread) {
  return Impl::ThreadSingleStruct<Impl::HPXTeamMember>(thread);
}

KOKKOS_INLINE_FUNCTION
Impl::VectorSingleStruct<Impl::HPXTeamMember>
PerThread(const Impl::HPXTeamMember &thread) {
  return Impl::VectorSingleStruct<Impl::HPXTeamMember>(thread);
}

/** \brief  Inter-thread parallel_for. Executes lambda(iType i) for each
 * i=0..N-1.
 *
 * The range i=0..N-1 is mapped to all threads of the the calling thread team.
 * This functionality requires C++11 support.*/
template <typename iType, class Lambda>
KOKKOS_INLINE_FUNCTION void parallel_for(
    const Impl::TeamThreadRangeBoundariesStruct<iType, Impl::HPXTeamMember>
        &loop_boundaries,
    const Lambda &lambda) {
  for (iType i = loop_boundaries.start; i < loop_boundaries.end;
       i += loop_boundaries.increment)
    lambda(i);
}

/** \brief  Inter-thread vector parallel_reduce. Executes lambda(iType i,
 * ValueType & val) for each i=0..N-1.
 *
 * The range i=0..N-1 is mapped to all threads of the the calling thread team
 * and a summation of val is performed and put into result. This functionality
 * requires C++11 support.*/
template <typename iType, class Lambda, typename ValueType>
KOKKOS_INLINE_FUNCTION void parallel_reduce(
    const Impl::TeamThreadRangeBoundariesStruct<iType, Impl::HPXTeamMember>
        &loop_boundaries,
    const Lambda &lambda, ValueType &result) {
  for (iType i = loop_boundaries.start; i < loop_boundaries.end;
       i += loop_boundaries.increment) {
    lambda(i, result);
  }
}

/** \brief  Intra-thread vector parallel_for. Executes lambda(iType i) for each
 * i=0..N-1.
 *
 * The range i=0..N-1 is mapped to all vector lanes of the the calling thread.
 * This functionality requires C++11 support.*/
template <typename iType, class Lambda>
KOKKOS_INLINE_FUNCTION void parallel_for(
    const Impl::ThreadVectorRangeBoundariesStruct<iType, Impl::HPXTeamMember>
        &loop_boundaries,
    const Lambda &lambda) {
#ifdef KOKKOS_ENABLE_PRAGMA_IVDEP
#pragma ivdep
#endif
  for (iType i = loop_boundaries.start; i < loop_boundaries.end;
       i += loop_boundaries.increment) {
    lambda(i);
  }
}

/** \brief  Intra-thread vector parallel_reduce. Executes lambda(iType i,
 * ValueType & val) for each i=0..N-1.
 *
 * The range i=0..N-1 is mapped to all vector lanes of the the calling thread
 * and a summation of val is performed and put into result. This functionality
 * requires C++11 support.*/
template <typename iType, class Lambda, typename ValueType>
KOKKOS_INLINE_FUNCTION void parallel_reduce(
    const Impl::ThreadVectorRangeBoundariesStruct<iType, Impl::HPXTeamMember>
        &loop_boundaries,
    const Lambda &lambda, ValueType &result) {
#ifdef KOKKOS_ENABLE_PRAGMA_IVDEP
#pragma ivdep
#endif
  for (iType i = loop_boundaries.start; i < loop_boundaries.end;
       i += loop_boundaries.increment) {
    lambda(i, result);
  }
}

/** \brief  Intra-thread vector parallel_reduce. Executes lambda(iType i,
 * ValueType & val) for each i=0..N-1.
 *
 * The range i=0..N-1 is mapped to all vector lanes of the the calling thread
 * and a reduction of val is performed using JoinType(ValueType& val, const
 * ValueType& update) and put into init_result. The input value of init_result
 * is used as initializer for temporary variables of ValueType. Therefore the
 * input value should be the neutral element with respect to the join operation
 * (e.g. '0 for +-' or '1 for *'). This functionality requires C++11 support.*/
template <typename iType, class Lambda, typename ValueType, class JoinType>
KOKKOS_INLINE_FUNCTION void parallel_reduce(
    const Impl::ThreadVectorRangeBoundariesStruct<iType, Impl::HPXTeamMember>
        &loop_boundaries,
    const Lambda &lambda, const JoinType &join, ValueType &result) {

#ifdef KOKKOS_ENABLE_PRAGMA_IVDEP
#pragma ivdep
#endif
  for (iType i = loop_boundaries.start; i < loop_boundaries.end;
       i += loop_boundaries.increment) {
    lambda(i, result);
  }
}

template <typename iType, class Lambda, typename ReducerType>
KOKKOS_INLINE_FUNCTION void parallel_reduce(
    const Impl::TeamThreadRangeBoundariesStruct<iType, Impl::HPXTeamMember>
        &loop_boundaries,
    const Lambda &lambda, const ReducerType &reducer) {
  reducer.init(reducer.reference());

#ifdef KOKKOS_ENABLE_PRAGMA_IVDEP
#pragma ivdep
#endif
  for (iType i = loop_boundaries.start; i < loop_boundaries.end;
       i += loop_boundaries.increment) {
    lambda(i, reducer.reference());
  }
}

template <typename iType, class Lambda, typename ReducerType>
KOKKOS_INLINE_FUNCTION void parallel_reduce(
    const Impl::ThreadVectorRangeBoundariesStruct<iType, Impl::HPXTeamMember>
        &loop_boundaries,
    const Lambda &lambda, const ReducerType &reducer) {
  reducer.init(reducer.reference());

#ifdef KOKKOS_ENABLE_PRAGMA_IVDEP
#pragma ivdep
#endif
  for (iType i = loop_boundaries.start; i < loop_boundaries.end;
       i += loop_boundaries.increment) {
    lambda(i, reducer.reference());
  }
}

template <typename iType, class FunctorType>
KOKKOS_INLINE_FUNCTION void parallel_scan(
    Impl::TeamThreadRangeBoundariesStruct<iType, Impl::HPXTeamMember> const
        &loop_boundaries,
    const FunctorType &lambda) {
  using value_type = typename Kokkos::Impl::FunctorAnalysis<
      Kokkos::Impl::FunctorPatternInterface::SCAN, void,
      FunctorType>::value_type;

  value_type accum = 0;

  // Intra-member scan
  for (iType i = loop_boundaries.start; i < loop_boundaries.end;
       i += loop_boundaries.increment) {
    lambda(i, accum, false);
  }

  // 'accum' output is the exclusive prefix sum
  accum = loop_boundaries.thread.team_scan(accum);

  for (iType i = loop_boundaries.start; i < loop_boundaries.end;
       i += loop_boundaries.increment) {
    lambda(i, accum, true);
  }
}

/** \brief  Intra-thread vector parallel exclusive prefix sum. Executes
 * lambda(iType i, ValueType & val, bool final) for each i=0..N-1.
 *
 * The range i=0..N-1 is mapped to all vector lanes in the thread and a scan
 * operation is performed. Depending on the target execution space the operator
 * might be called twice: once with final=false and once with final=true. When
 * final==true val contains the prefix sum value. The contribution of this "i"
 * needs to be added to val no matter whether final==true or not. In a serial
 * execution (i.e. team_size==1) the operator is only called once with
 * final==true. Scan_val will be set to the final sum value over all vector
 * lanes. This functionality requires C++11 support.*/
template <typename iType, class FunctorType>
KOKKOS_INLINE_FUNCTION void parallel_scan(
    const Impl::ThreadVectorRangeBoundariesStruct<iType, Impl::HPXTeamMember>
        &loop_boundaries,
    const FunctorType &lambda) {
  using ValueTraits = Kokkos::Impl::FunctorValueTraits<FunctorType, void>;
  using value_type = typename ValueTraits::value_type;

  value_type scan_val = value_type();

#ifdef KOKKOS_ENABLE_PRAGMA_IVDEP
#pragma ivdep
#endif
  for (iType i = loop_boundaries.start; i < loop_boundaries.end;
       i += loop_boundaries.increment) {
    lambda(i, scan_val, true);
  }
}

template <class FunctorType>
KOKKOS_INLINE_FUNCTION void
single(const Impl::VectorSingleStruct<Impl::HPXTeamMember> &single_struct,
       const FunctorType &lambda) {
  lambda();
}

template <class FunctorType>
KOKKOS_INLINE_FUNCTION void
single(const Impl::ThreadSingleStruct<Impl::HPXTeamMember> &single_struct,
       const FunctorType &lambda) {
  lambda();
}

template <class FunctorType, class ValueType>
KOKKOS_INLINE_FUNCTION void
single(const Impl::VectorSingleStruct<Impl::HPXTeamMember> &single_struct,
       const FunctorType &lambda, ValueType &val) {
  lambda(val);
}

template <class FunctorType, class ValueType>
KOKKOS_INLINE_FUNCTION void
single(const Impl::ThreadSingleStruct<Impl::HPXTeamMember> &single_struct,
       const FunctorType &lambda, ValueType &val) {
  lambda(val);
}

} // namespace Kokkos

#include <HPX/Kokkos_HPX_Task.hpp>

#endif /* #if defined( KOKKOS_ENABLE_HPX ) */
#endif /* #ifndef KOKKOS_HPX_HPP */
