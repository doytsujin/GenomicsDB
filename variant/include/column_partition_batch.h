#ifndef COLUMN_PARTITION_BATCH_H
#define COLUMN_PARTITION_BATCH_H

#include "headers.h"

class CircularBufferController
{
  public:
    CircularBufferController(unsigned num_entries)
    {
      m_num_entries = num_entries;
      m_num_entries_with_valid_data = 0u;
      m_curr_write_idx = num_entries-1u;
      m_curr_read_idx = 0u;
    }
    //Advance write idx
    inline void advance_write_idx()
    {
      m_curr_write_idx = (m_curr_write_idx+1u)%m_num_entries;
      assert(m_num_entries_with_valid_data < m_num_entries);
      ++m_num_entries_with_valid_data;
    }
    inline void advance_read_idx()
    {
      m_curr_read_idx = (m_curr_read_idx+1u)%m_num_entries;
      assert(m_num_entries_with_valid_data > 0u);
      --m_num_entries_with_valid_data;
    }
    inline unsigned get_num_entries_with_valid_data() const
    { return m_num_entries_with_valid_data; }
    inline unsigned get_num_empty_entries() const
    { return m_num_entries - m_num_entries_with_valid_data; }
    //Get idx
    inline unsigned get_write_idx() const { return m_curr_write_idx; }
    inline unsigned get_read_idx() const { return m_curr_read_idx; }
  protected:
    //Points to latest entry with valid data
    unsigned m_curr_write_idx;
    //Points to entry being read currently
    unsigned m_curr_read_idx;
    unsigned m_num_entries;
    unsigned m_num_entries_with_valid_data;
};

class ColumnPartitionFileBatch : public CircularBufferController
{
  public:
    ColumnPartitionFileBatch(unsigned num_entries_in_circular_buffer)
      : CircularBufferController(num_entries_in_circular_buffer)
    {
      m_fetch = false;
      m_completed = false;
      m_buffer_offset = -1;
      m_num_callsets = 1;
    }
    /*
     * For a given local callset idx, compute offset
     */
    inline int64_t get_offset_for_local_callset_idx(int to_process_local_callset_idx, size_t max_size_per_callset) const
    {
      assert(to_process_local_callset_idx < m_num_callsets);
      return m_buffer_offset + to_process_local_callset_idx*max_size_per_callset;
    }
    inline unsigned get_buffer_idx() const { return get_write_idx(); }
    void update_buffer_offset(int64_t& offset, size_t max_size_per_callset)
    {
      m_buffer_offset = offset;
      offset += m_num_callsets*max_size_per_callset;
    }
    //Members
    bool m_fetch;
    bool m_completed;
    int64_t m_num_callsets;
  private:
    int64_t m_buffer_offset;
};

class ColumnPartitionBatch
{
  public:
    ColumnPartitionBatch(int column_partition_idx, uint64_t max_size_per_callset, const std::vector<int64_t>& num_callsets_in_file,
        unsigned num_entries_in_circular_buffer)
    {
      m_num_completed = 0;
      m_total_num_callsets = 0;
      m_idx = column_partition_idx;
      m_max_size_per_callset = max_size_per_callset;
      m_file_batches.resize(num_callsets_in_file.size(), ColumnPartitionFileBatch(num_entries_in_circular_buffer));
      for(auto i=0ull;i<num_callsets_in_file.size();++i)
        set_num_callsets_in_file(i, num_callsets_in_file[i]);
      //In the global buffer, data for the current partition begins here
      m_partition_begin_offset = m_total_num_callsets*m_max_size_per_callset*m_idx;
      //First time initialization of buffer offsets
      update_buffer_offsets(true);
    }
    /*
     * Requests that new data be fetched for current column partition for the files for which m_fetch=true
     */
    bool activate_file(int64_t file_idx)
    {
      assert(static_cast<size_t>(file_idx) < m_file_batches.size());
      auto& file_batch = m_file_batches[file_idx];
      if(!file_batch.m_fetch && !file_batch.m_completed)
      {
        //set fetch
        file_batch.m_fetch = true;
        //Advance circular buffer idx
        file_batch.advance_write_idx();
        return true;
      }
      else
        return false;
    }
    /*
     * Specifies that for the next batch, the files with m_fetch=true are to be read
     * and that buffer offsets need to be set correctly
     */
    void update_buffer_offsets(bool force_update=false)
    {
      auto offset = m_partition_begin_offset;
      for(auto& file_batch : m_file_batches)
      {
        if(force_update || (file_batch.m_fetch && !file_batch.m_completed))
          file_batch.update_buffer_offset(offset, m_max_size_per_callset);
      }
    }
    /*
     * Compute #completed files
     */
    void update_num_completed_files()
    {
      m_num_completed = 0;
      for(const auto& file_batch : m_file_batches)
        if(file_batch.m_completed)
          ++m_num_completed;
    }
    /*
     * Check whether this partition is complete
     */
    inline bool is_completed(bool recompute=true)
    {
      if(recompute)
        update_num_completed_files();
      return m_num_completed == m_file_batches.size();
    }
    /*
     * Get file batch
     */
    ColumnPartitionFileBatch& get_partition_file_batch(int64_t file_idx)
    {
      assert(static_cast<size_t>(file_idx) < m_file_batches.size());
      return m_file_batches[file_idx];
    }
    const ColumnPartitionFileBatch& get_partition_file_batch(int64_t file_idx) const
    {
      assert(static_cast<size_t>(file_idx) < m_file_batches.size());
      return m_file_batches[file_idx];
    }
    inline size_t get_max_size_per_callset() const { return m_max_size_per_callset; }
    inline int64_t get_partition_begin_offset() const { return m_partition_begin_offset; }
  private:
    void set_num_callsets_in_file(int64_t file_idx, int64_t num_callsets)
    {
      assert(file_idx < m_file_batches.size());
      auto& curr_file_batch = m_file_batches[file_idx];
      curr_file_batch.m_num_callsets = num_callsets;
      m_total_num_callsets += num_callsets;
    }
    //Members
    int m_idx;
    size_t m_max_size_per_callset;
    int64_t m_num_completed;
    int64_t m_total_num_callsets;
    int64_t m_partition_begin_offset;
    std::vector<ColumnPartitionFileBatch> m_file_batches;
#if 0
    /*
     * Requests that new data be fetched for current column partition for the files listed in file_idx_vec
     * Total size of buffer available for this partition is given by per_partition_size
     */
    int64_t activate_partition_batch(const size_t per_partition_size, int64_t offset, const std::vector<int64_t>& file_idx_vec)
    {
      auto num_callsets = 0ll;
      for(auto file_idx : file_idx_vec)
      {
        assert(file_idx < m_file_batches.size());
        auto& curr_file_batch = m_file_batches[file_idx];
        if(curr_file_batch.m_completed) //already done
          continue;
        curr_file_batch.m_fetch = true;
        num_callsets += curr_file_batch.m_num_callsets;
      }
      m_max_size_per_callset = per_partition_size/num_callsets;
      for(auto file_idx : file_idx_vec)
      {
        auto& curr_file_batch = m_file_batches[file_idx];
        curr_file_batch.m_buffer_offset = offset;
        offset = (curr_file_batch.m_fetch) ? offset + curr_file_batch.m_num_callsets*m_max_size_per_callset : offset;
      }
      return offset;
    }
#endif
};

#endif