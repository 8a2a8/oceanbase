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

#ifndef OB_STORAGE_CKPT_TENANT_CHECKPOINT_SLOG_HANDLER_H_
#define OB_STORAGE_CKPT_TENANT_CHECKPOINT_SLOG_HANDLER_H_

#include "common/log/ob_log_cursor.h"
#include "storage/slog_ckpt/ob_linked_macro_block_struct.h"
#include "storage/slog_ckpt/ob_tenant_storage_checkpoint_reader.h"
#include "storage/meta_mem/ob_tablet_map_key.h"
#include "storage/ob_super_block_struct.h"
#include "storage/slog/ob_storage_log_replayer.h"
#include "storage/blockstore/ob_shared_block_reader_writer.h"
#include "storage/ls/ob_ls_meta.h"
#include "storage/tx/ob_dup_table_base.h"
#include "storage/high_availability/ob_tablet_transfer_info.h"
#include "observer/ob_server_startup_task_handler.h"

namespace oceanbase
{

namespace share
{
class ObLSID;
}
namespace storage
{
class ObTenantSuperBlock;
struct ObMetaDiskAddr;
class ObTenantStorageCheckpointWriter;
class ObRedoModuleReplayParam;
class ObStorageLogger;
class ObLSTabletService;
class ObLSHandle;

struct ObLSCkptMember final
{
public:
  static const int64_t LS_CKPT_MEM_VERSION = 1;
  ObLSCkptMember();
  ~ObLSCkptMember();
  DISALLOW_COPY_AND_ASSIGN(ObLSCkptMember);
  void reset();

  int serialize(char *buf, const int64_t len, int64_t &pos) const;
  int deserialize(
      const char *buf,
      const int64_t len,
      int64_t &pos);
  int64_t get_serialize_size() const;
  TO_STRING_KV(K_(version), K_(ls_meta), K_(tablet_meta_entry), K_(dup_ls_meta));

public:
  int32_t version_;
  int32_t length_;
  ObLSMeta ls_meta_;
  transaction::ObDupTableLSCheckpoint::ObLSDupTableMeta dup_ls_meta_;
  blocksstable::MacroBlockId tablet_meta_entry_;
};

class ObTenantCheckpointSlogHandler : public ObIRedoModule
{
public:
  #define SLOG_CKPT_READ_GUARD TCRLockGuard guard(MTL(ObTenantCheckpointSlogHandler*)->get_slog_ckpt_lock());
  class ObWriteCheckpointTask : public common::ObTimerTask
  {
  public:
    static const int64_t FAIL_WRITE_CHECKPOINT_ALERT_INTERVAL = 1000L * 1000L * 3600LL;  // 6h
    static const int64_t WRITE_CHECKPOINT_INTERVAL_US = 1000L * 1000L * 60L;             // 1min
    static const int64_t RETRY_WRITE_CHECKPOINT_MIN_INTERVAL = 1000L * 1000L * 300L;     // 5min
    static const int64_t MIN_WRITE_CHECKPOINT_LOG_CNT = 100000; // TODO(fenggu)

    explicit ObWriteCheckpointTask(ObTenantCheckpointSlogHandler *handler) : handler_(handler)
    {
      disable_timeout_check();
    }
    virtual ~ObWriteCheckpointTask() = default;
    virtual void runTimerTask() override;

  private:
    ObTenantCheckpointSlogHandler *handler_;
  };

  class ObReplayCreateTabletTask : public observer::ObServerStartupTask
  {
  public:
    ObReplayCreateTabletTask()
      : is_inited_(false),
        idx_(-1),
        tenant_base_(nullptr),
        tnt_ckpt_slog_handler_(nullptr) {}

    virtual ~ObReplayCreateTabletTask()
    {
      destroy();
    }
    int init(const int64_t task_idx, ObTenantBase *tenant_base, ObTenantCheckpointSlogHandler *handler);
    int execute() override;
    int add_tablet_addr(const ObTabletMapKey &tablet_key, const ObMetaDiskAddr &tablet_addr, bool &is_enough);

    VIRTUAL_TO_STRING_KV(K_(idx), KP(this), KP_(tenant_base), "tablet_count", tablet_addr_arr_.count());

  private:
    static const int64_t TABLET_NUM_PER_TASK = 200;
    void destroy();

  private:
    bool is_inited_;
    int64_t idx_;
    ObTenantBase *tenant_base_;
    ObTenantCheckpointSlogHandler *tnt_ckpt_slog_handler_;
    common::ObSEArray<std::pair<ObTabletMapKey, ObMetaDiskAddr>, TABLET_NUM_PER_TASK> tablet_addr_arr_;
  };

  ObTenantCheckpointSlogHandler();
  ~ObTenantCheckpointSlogHandler() = default;
  ObTenantCheckpointSlogHandler(const ObTenantCheckpointSlogHandler &) = delete;
  ObTenantCheckpointSlogHandler &operator=(const ObTenantCheckpointSlogHandler &) = delete;

  static int mtl_init(ObTenantCheckpointSlogHandler *&handler);
  int init();
  int start();
  void stop();
  void wait();
  void destroy();
  int report_slog(const ObTabletMapKey &tablet_key, const ObMetaDiskAddr &slog_addr);
  int check_slog(const ObTabletMapKey &tablet_key, bool &has_slog);
  int read_tablet_checkpoint_by_addr(
    const ObMetaDiskAddr &addr, char *item_buf, int64_t &item_buf_len);

  int get_meta_block_list(common::ObIArray<blocksstable::MacroBlockId> &block_list);
  ObSharedBlockReaderWriter &get_shared_block_reader_writer() { return shared_block_rwriter_; }
  // only used by MACRO
  common::TCRWLock &get_slog_ckpt_lock() { return slog_ckpt_lock_; }
  virtual int replay(const ObRedoModuleReplayParam &param) override;
  virtual int replay_over() override;
  int write_checkpoint(bool is_force);
  int read_from_disk(
      const ObMetaDiskAddr &addr,
      common::ObArenaAllocator &allocator,
      char *&buf,
      int64_t &buf_len);

  void inc_inflight_replay_tablet_task_cnt() { ATOMIC_INC(&inflight_replay_tablet_task_cnt_); }
  void dec_inflight_replay_tablet_task_cnt() { ATOMIC_DEC(&inflight_replay_tablet_task_cnt_); }
  void inc_finished_replay_tablet_cnt(const int64_t cnt) { (void)ATOMIC_FAA(&finished_replay_tablet_cnt_, cnt); }
  void set_replay_create_tablet_errcode(const int errcode)
  {
    ATOMIC_STORE(&replay_create_tablet_errcode_, errcode);
  };
  int replay_create_tablets_per_task(const common::ObIArray<std::pair<ObTabletMapKey, ObMetaDiskAddr>> &tablet_addr_arr);

private:
  int get_cur_cursor();
  void clean_copy_status();
  virtual int parse(const int32_t cmd, const char *buf, const int64_t len, FILE *stream) override;
  int replay_checkpoint_and_slog(const ObTenantSuperBlock &super_block);
  int replay_checkpoint(const ObTenantSuperBlock &super_block);
  int replay_new_checkpoint(const ObTenantSuperBlock &super_block);
  int replay_old_checkpoint(const ObTenantSuperBlock &super_block);
  int replay_new_tablet_checkpoint(
      const blocksstable::MacroBlockId &tablet_entry_block,
      common::ObIArray<blocksstable::MacroBlockId> &tablet_block_list);
  int replay_ls_meta(const ObMetaDiskAddr &addr, const char *buf, const int64_t buf_len);
  int replay_tablet(const ObMetaDiskAddr &addr, const char *buf, const int64_t buf_len);
  int update_tablet_meta_addr_and_block_list(
      const bool is_replay_old, ObTenantStorageCheckpointWriter &ckpt_writer);
  int replay_dup_table_ls_meta(const transaction::ObDupTableLSCheckpoint::ObLSDupTableMeta &dup_ls_meta);
  int replay_tenant_slog(const common::ObLogCursor &start_point);
  int concurrent_replay_load_tablets();
  int inner_replay_update_ls_slog(const ObRedoModuleReplayParam &param);
  int inner_replay_create_ls_slog(const ObRedoModuleReplayParam &param);
  int inner_replay_create_ls_commit_slog(const ObRedoModuleReplayParam &param);
  int inner_replay_delete_ls(const ObRedoModuleReplayParam &param);
  int inner_replay_put_old_tablet(const ObRedoModuleReplayParam &param);
  int inner_replay_update_tablet(const ObRedoModuleReplayParam &param);
  int inner_replay_dup_table_ls_slog(const ObRedoModuleReplayParam &param);
  int inner_replay_delete_tablet(const ObRedoModuleReplayParam &param);
  int inner_replay_empty_shell_tablet(const ObRedoModuleReplayParam &param);
  int inner_replay_gts_record(const ObRedoModuleReplayParam &param);
  int inner_replay_gti_record(const ObRedoModuleReplayParam &param);
  int inner_replay_das_record(const ObRedoModuleReplayParam &param);
  int get_tablet_svr(const share::ObLSID &ls_id, ObLSTabletService *&ls_tablet_svr, ObLSHandle &ls_handle);
  int read_from_disk_addr(const ObMetaDiskAddr &phy_addr, char *buf, const int64_t buf_len, char *&r_buf, int64_t &r_len);
  int read_from_share_blk(const ObMetaDiskAddr &addr, common::ObArenaAllocator &allocator, char *&buf, int64_t &buf_len);
  int read_from_ckpt(const ObMetaDiskAddr &phy_addr, char *buf, const int64_t buf_len, int64_t &r_len);
  int read_from_slog(const ObMetaDiskAddr &phy_addr, char *buf, const int64_t buf_len, int64_t &pos);
  int read_empty_shell_file(const ObMetaDiskAddr &phy_addr, common::ObArenaAllocator &allocator, char *&buf, int64_t &buf_len);
  int remove_tablets_from_replay_map_(const share::ObLSID &ls_id);

  int inner_replay_deserialize(
      const char *buf,
      const int64_t buf_len,
      bool allow_override /* allow to overwrite the map's element or not */);
  int inner_replay_old_deserialize(
      const ObMetaDiskAddr &addr,
      const char *buf,
      const int64_t buf_len,
      bool allow_override /* allow to overwrite the map's element or not */);
  int record_ls_transfer_info(
      const ObLSHandle &ls_handle,
      const ObTabletID &tablet_id,
      const ObTabletTransferInfo &tablet_transfer_info);
  int check_is_need_record_transfer_info(
      const share::ObLSID &src_ls_id,
      const share::SCN &transfer_start_scn,
      bool &is_need);
  int add_replay_create_tablet_task(ObReplayCreateTabletTask *task);

private:
  const static int64_t BUCKET_NUM = 109;
private:
  typedef common::hash::ObHashMap<ObTabletMapKey, ObMetaDiskAddr> ReplayTabletDiskAddrMap;
  bool is_inited_;
  bool is_writing_checkpoint_;
  int64_t last_ckpt_time_;
  int64_t last_frozen_version_;
  int64_t inflight_replay_tablet_task_cnt_;
  int64_t finished_replay_tablet_cnt_;
  int replay_create_tablet_errcode_;
  common::TCRWLock lock_;  // protect block_handle
  common::TCRWLock slog_ckpt_lock_; // protect is_copying_tablets_
  common::hash::ObHashSet<ObTabletMapKey> tablet_key_set_;
  bool is_copying_tablets_;
  ObLogCursor ckpt_cursor_;
  ObMetaBlockListHandle ls_block_handle_;
  ObMetaBlockListHandle tablet_block_handle_;

  int tg_id_;
  ObWriteCheckpointTask write_ckpt_task_;
  ReplayTabletDiskAddrMap replay_tablet_disk_addr_map_;
  ObSharedBlockReaderWriter shared_block_rwriter_;
};

}  // end namespace storage
}  // namespace oceanbase

#endif  // OB_STORAGE_CKPT_TENANT_CHECKPOINT_SLOG_HANDLER_H_
