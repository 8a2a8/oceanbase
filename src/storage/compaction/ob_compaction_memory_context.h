/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#ifndef STORAGE_OB_COMPACTION_MEMORY_CONTEXT_H_
#define STORAGE_OB_COMPACTION_MEMORY_CONTEXT_H_

#include "lib/utility/ob_print_utils.h"
#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/allocator/page_arena.h"
#include "lib/allocator/ob_fifo_allocator.h"
#include "share/rc/ob_tenant_base.h"
#include "share/scheduler/ob_tenant_dag_scheduler.h"
#include "lib/utility/ob_template_utils.h"
#include "lib/lock/ob_spin_lock.h"
#include "lib/list/ob_dlist.h"
#include "share/ob_delegate.h"


#define DELEGATE_WITH_SPIN_LOCK(delegate_obj, lock_obj, func_name, ret) \
  template <typename ...Args>                                           \
    ret func_name(Args &&...args) {                                     \
      common::ObSpinLockGuard guard(lock_obj);                          \
      return delegate_obj.func_name(std::forward<Args>(args)...);       \
    }

#define DELEGATE_WITHOUT_RET(delegate_obj, func_name)           \
  template <typename ...Args>                                   \
    void func_name(Args &&...args) {                            \
    delegate_obj.func_name(std::forward<Args>(args)...);        \
  }

#define MONITOR_MERGE_MEM(var_name)                    \
    OB_INLINE void inc_##var_name(const int64_t val) { \
      ATOMIC_FAAx(&mem_monitor_. var_name##_, val, 0); \
    }


namespace oceanbase
{

namespace compaction
{
struct ObTabletMergeDagParam;
class ObCompactionMemoryContext;


struct ObCompactionMemMonitor
{
public:
  ObCompactionMemMonitor();
  ~ObCompactionMemMonitor() = default;
  int64_t get_hold_mem() const;

  TO_STRING_KV(K_(buffer_hold_mem), K_(buffer_free_mem), K_(local_hold_mem), K_(local_free_mem));
public:
  int64_t buffer_hold_mem_;
  int64_t buffer_free_mem_;
  int64_t local_hold_mem_;
  int64_t local_free_mem_;
};


class ObCompactionMemoryContext
{
public:
  // only used for compaction task, which needs to monitor memory
  ObCompactionMemoryContext(
      const ObTabletMergeDagParam &param,
      common::ObArenaAllocator &allocator);
  // used for non-compaction task, no need to monitor memory
  ObCompactionMemoryContext(common::ObArenaAllocator &allocator);
  virtual ~ObCompactionMemoryContext();
  void destroy();
  ObArenaAllocator &get_allocator() { return arena_; }
  void *alloc(const int64_t size) { return arena_.alloc(size); }
  void free(void *ptr) { UNUSED(ptr); }
  bool is_reserve_mode() const { return is_reserve_mode_; }
  int64_t get_ctx_id() const { return ctx_id_; }
  void *local_alloc(const int64_t size);
  void local_free(void *ptr);
  ObSafeArenaAllocator &get_safe_arena() { return safe_arena_; }
  void mem_click();
  int64_t get_total_mem_peak() const { return mem_peak_total_; }

  MONITOR_MERGE_MEM(local_hold_mem);
  MONITOR_MERGE_MEM(local_free_mem);
  MONITOR_MERGE_MEM(buffer_hold_mem);
  MONITOR_MERGE_MEM(buffer_free_mem);

  TO_STRING_KV(K_(is_reserve_mode));
private:
  void inner_init(const ObTabletMergeDagParam &param);
private:
  common::ObArenaAllocator &arena_;
  int64_t ctx_id_;
  common::ObArenaAllocator inner_arena_;
  common::ObSafeArenaAllocator safe_arena_;
  common::ObSpinLock free_lock_;
  common::DefaultPageAllocator free_alloc_;
  ObCompactionMemMonitor mem_monitor_;
  int64_t mem_peak_total_;
  bool is_reserve_mode_;
  DISABLE_COPY_ASSIGN(ObCompactionMemoryContext);
};

template<class T>
class ObLocalAllocator : public common::ObIAllocator
{
public:
  ObLocalAllocator(
      const lib::ObLabel &label,
      const int64_t page_size = OB_MALLOC_NORMAL_BLOCK_SIZE)
    : ref_mem_ctx_(nullptr),
      allocator_(label, page_size, MTL_ID(), ObCtxIds::DEFAULT_CTX_ID),
      hist_mem_hold_(0)
  {
    ObCompactionMemoryContext *mem_ctx = CURRENT_MEM_CTX();
    if (nullptr != mem_ctx) {
      bind_mem_ctx(*mem_ctx);
    }
  }

  ObLocalAllocator(
      const uint64_t tenant_id,
      const lib::ObLabel &label)
    : ref_mem_ctx_(nullptr),
      allocator_(label, tenant_id),
      hist_mem_hold_(0)
  {
    static_assert(std::is_same<T, common::DefaultPageAllocator>::value, "error allocator type");
    ObCompactionMemoryContext *mem_ctx = CURRENT_MEM_CTX();
    if (nullptr != mem_ctx) {
      bind_mem_ctx(*mem_ctx);
    }
  }
  ObLocalAllocator(
      ObCompactionMemoryContext &mem_ctx,
      const lib::ObLabel &label,
      const int64_t page_size = OB_MALLOC_NORMAL_BLOCK_SIZE)
    : ref_mem_ctx_(nullptr),
      allocator_(label, page_size, MTL_ID(), ObCtxIds::DEFAULT_CTX_ID),
      hist_mem_hold_(0)
  {
    bind_mem_ctx(mem_ctx);
  }

  ~ObLocalAllocator()
  {
    reset();
    ref_mem_ctx_ = nullptr;
  }

  void bind_mem_ctx(ObCompactionMemoryContext &mem_ctx)
  {
    if (NULL == ref_mem_ctx_) {
      ref_mem_ctx_ = &mem_ctx;
    }
    allocator_.set_ctx_id(ref_mem_ctx_->get_ctx_id());
  }
  void unbind_mem_ctx() { ref_mem_ctx_ = nullptr; }
  common::ObArenaAllocator &get_arena_allocator() { return allocator_; }

  virtual void* alloc(const int64_t size) override
  {
    void *buf = allocator_.alloc(size);
    if (OB_NOT_NULL(buf)) {
      update_mem_monitor();
    }
    return buf;
  }
  virtual void* alloc(const int64_t size, const ObMemAttr &attr) override
  {
    void *buf = allocator_.alloc(size, attr);
    if (OB_NOT_NULL(buf)) {
      update_mem_monitor();
    }
    return buf;
  }
  virtual void free(void *ptr) override { allocator_.free(ptr); }
  virtual void reset() override
  {
    allocator_.reset();
    update_mem_monitor();
  }

  template <typename = T>
  void clear()
  {
    allocator_.clear();
    update_mem_monitor();
  }

  DELEGATE_WITH_RET(allocator_, alloc_aligned, void*);
  DELEGATE_WITH_RET(allocator_, realloc, void*);
  DELEGATE_WITHOUT_RET(allocator_, reset_remain_one_page);
  DELEGATE_WITHOUT_RET(allocator_, reuse);
  DELEGATE_WITHOUT_RET(allocator_, set_label);
  DELEGATE_WITHOUT_RET(allocator_, set_tenant_id);
  DELEGATE_WITH_RET(allocator_, set_tracer, bool);
  DELEGATE_WITH_RET(allocator_, revert_tracer, bool);
  DELEGATE_WITHOUT_RET(allocator_, set_ctx_id);
  DELEGATE_WITHOUT_RET(allocator_, set_attr);
  DELEGATE_WITH_RET(allocator_, get_arena, ModuleArena&);
  DELEGATE_WITH_RET(allocator_, mprotect_arena_allocator, int);
  CONST_DELEGATE_WITH_RET(allocator_, used, int64_t);
  CONST_DELEGATE_WITH_RET(allocator_, total, int64_t);
  CONST_DELEGATE_WITH_RET(allocator_, to_string, int64_t);

private:
  void update_mem_monitor()
  {
    ref_mem_ctx_ = nullptr == ref_mem_ctx_
                ? CURRENT_MEM_CTX()
                : ref_mem_ctx_;

    int64_t cur_mem_hold = allocator_.total();
    if (nullptr != ref_mem_ctx_ && hist_mem_hold_ != cur_mem_hold) {
      if (cur_mem_hold > hist_mem_hold_) {
        ref_mem_ctx_->inc_local_hold_mem(cur_mem_hold - hist_mem_hold_);
      } else {
        ref_mem_ctx_->inc_local_free_mem(hist_mem_hold_ - cur_mem_hold);
      }
      hist_mem_hold_ = cur_mem_hold;
    }
  }
protected:
  ObCompactionMemoryContext *ref_mem_ctx_;
  T allocator_;
  int64_t hist_mem_hold_;
};

using ObLocalArena=ObLocalAllocator<common::ObArenaAllocator>;

class ObLocalSafeArena final : public ObLocalArena
{
public:
  ObLocalSafeArena(const lib::ObLabel &label, const int64_t page_size = OB_MALLOC_NORMAL_BLOCK_SIZE)
    : ObLocalArena(label, page_size), lock_() {}
  virtual ~ObLocalSafeArena() {}
  virtual void *alloc(const int64_t sz) override
  {
    ObSpinLockGuard guard(lock_);
    return allocator_.alloc(sz);
  }
  virtual void* alloc(const int64_t size, const ObMemAttr &attr) override
  {
    ObSpinLockGuard guard(lock_);
    return allocator_.alloc(size, attr);
  }
  DELEGATE_WITH_SPIN_LOCK(allocator_, lock_, clear, void);
  DELEGATE_WITH_SPIN_LOCK(allocator_, lock_, reuse, void);
  DELEGATE_WITH_SPIN_LOCK(allocator_, lock_, reset, void);
private:
  common::ObSpinLock lock_;
};

} // compaction
} // oceanbase

#endif //STORAGE_OB_COMPACTION_MEMORY_CONTEXT_H_