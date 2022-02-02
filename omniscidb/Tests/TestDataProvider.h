/*
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

#include "DataMgr/AbstractBufferMgr.h"

namespace TestHelpers {

template <typename T>
void set_datum(Datum& d, const T& v) {
  UNREACHABLE();
}

template <>
void set_datum<int32_t>(Datum& d, const int32_t& v) {
  d.intval = v;
}

template <>
void set_datum<int64_t>(Datum& d, const int64_t& v) {
  d.bigintval = v;
}

template <>
void set_datum<float>(Datum& d, const float& v) {
  d.floatval = v;
}

template <>
void set_datum<double>(Datum& d, const double& v) {
  d.doubleval = v;
}

class TestTableData {
 public:
  TestTableData(int db_id, int table_id, int cols, SchemaProviderPtr schema_provider_)
      : ref_(db_id, table_id) {
    info_.chunkKeyPrefix.push_back(db_id);
    info_.chunkKeyPrefix.push_back(table_id);
    data_.resize(cols);

    auto col_infos = schema_provider_->listColumns(ref_);
    for (auto& col_info : col_infos) {
      col_types_[col_info->column_id] = col_info->type;
    }
  }

  template <typename T>
  void addColFragment(size_t col_id, std::vector<T> vals) {
    CHECK_LE(col_id, data_.size());
    CHECK_EQ(col_types_.count(col_id), 1);
    auto& frag_data = data_[col_id - 1].emplace_back();
    frag_data.resize(vals.size() * sizeof(T));
    memcpy(frag_data.data(), vals.data(), frag_data.size());

    if (info_.fragments.size() < data_[col_id - 1].size()) {
      Fragmenter_Namespace::FragmentInfo& frag_info = info_.fragments.emplace_back();
      frag_info.fragmentId = info_.fragments.size();
      frag_info.physicalTableId = info_.chunkKeyPrefix[CHUNK_KEY_TABLE_IDX];
      frag_info.setPhysicalNumTuples(vals.size());
      frag_info.deviceIds.push_back(0);  // Data_Namespace::DISK_LEVEL
      frag_info.deviceIds.push_back(0);  // Data_Namespace::CPU_LEVEL
      frag_info.deviceIds.push_back(0);  // Data_Namespace::GPU_LEVEL

      info_.setPhysicalNumTuples(info_.getPhysicalNumTuples() + vals.size());
    }

    auto chunk_meta = std::make_shared<ChunkMetadata>();
    chunk_meta->sqlType = col_types_.at(col_id);
    chunk_meta->numBytes = frag_data.size();
    chunk_meta->numElements = vals.size();

    // No computed meta.
    chunk_meta->chunkStats.has_nulls = true;
    set_datum(chunk_meta->chunkStats.min, *std::min_element(vals.begin(), vals.end()));
    set_datum(chunk_meta->chunkStats.max, *std::max_element(vals.begin(), vals.end()));

    auto& frag_info = info_.fragments[data_[col_id - 1].size() - 1];
    frag_info.setChunkMetadata(col_id, chunk_meta);
  }

  void fetchData(int col_id, int frag_id, int8_t* dst, size_t size) {
    CHECK_LE(col_id, data_.size());
    CHECK_LE(frag_id, data_[col_id - 1].size());
    auto& chunk = data_[col_id - 1][frag_id - 1];
    CHECK_LE(chunk.size(), size);
    memcpy(dst, data_[col_id - 1][frag_id - 1].data(), size);
  }

  const Fragmenter_Namespace::TableInfo& getTableInfo() const { return info_; }

 private:
  TableRef ref_;
  std::vector<std::vector<std::vector<int8_t>>> data_;
  Fragmenter_Namespace::TableInfo info_;
  std::unordered_map<int, SQLTypeInfo> col_types_;
};

class TestDataProvider : public AbstractBufferMgr {
 public:
  TestDataProvider(int db_id, SchemaProviderPtr schema_provider)
      : AbstractBufferMgr(0), db_id_(db_id), schema_provider_(schema_provider) {}

  AbstractBuffer* createBuffer(const ChunkKey& key,
                               const size_t pageSize = 0,
                               const size_t initialSize = 0) override {
    UNREACHABLE();
    return nullptr;
  }

  void deleteBuffer(const ChunkKey& key, const bool purge = true) override {
    UNREACHABLE();
  }

  void deleteBuffersWithPrefix(const ChunkKey& keyPrefix,
                               const bool purge = true) override {
    UNREACHABLE();
  }

  AbstractBuffer* getBuffer(const ChunkKey& key, const size_t numBytes = 0) override {
    UNREACHABLE();
    return nullptr;
  }

  void fetchBuffer(const ChunkKey& key,
                   AbstractBuffer* destBuffer,
                   const size_t numBytes = 0) override {
    CHECK_EQ(key[CHUNK_KEY_DB_IDX], db_id_);
    CHECK_EQ(tables_.count(key[CHUNK_KEY_TABLE_IDX]), 1);
    auto& data = tables_.at(key[CHUNK_KEY_TABLE_IDX]);
    data.fetchData(key[CHUNK_KEY_COLUMN_IDX],
                   key[CHUNK_KEY_FRAGMENT_IDX],
                   destBuffer->getMemoryPtr(),
                   numBytes);
  }

  AbstractBuffer* putBuffer(const ChunkKey& key,
                            AbstractBuffer* srcBuffer,
                            const size_t numBytes = 0) override {
    UNREACHABLE();
    return nullptr;
  }

  void getChunkMetadataVecForKeyPrefix(ChunkMetadataVector& chunkMetadataVec,
                                       const ChunkKey& keyPrefix) override {
    UNREACHABLE();
  }

  bool isBufferOnDevice(const ChunkKey& key) override {
    UNREACHABLE();
    return false;
  }

  std::string printSlabs() override {
    UNREACHABLE();
    return "";
  }

  size_t getMaxSize() override {
    UNREACHABLE();
    return 0;
  }

  size_t getInUseSize() override {
    UNREACHABLE();
    return 0;
  }

  size_t getAllocated() override {
    UNREACHABLE();
    return 0;
  }

  bool isAllocationCapped() override { UNREACHABLE(); }

  void checkpoint() override { UNREACHABLE(); }

  void checkpoint(const int db_id, const int tb_id) override { UNREACHABLE(); }

  void removeTableRelatedDS(const int db_id, const int table_id) override {
    UNREACHABLE();
  }

  const DictDescriptor* getDictMetadata(int db_id,
                                        int dict_id,
                                        bool load_dict = true) override {
    UNREACHABLE();
  }

  Fragmenter_Namespace::TableInfo getTableInfo(int db_id, int table_id) const override {
    CHECK_EQ(db_id, db_id_);
    CHECK_EQ(tables_.count(table_id), 1);
    return tables_.at(table_id).getTableInfo();
  }

  // Buffer API
  AbstractBuffer* alloc(const size_t numBytes = 0) override {
    UNREACHABLE();
    return nullptr;
  }

  void free(AbstractBuffer* buffer) override { UNREACHABLE(); }

  MgrType getMgrType() override {
    UNREACHABLE();
    return static_cast<MgrType>(0);
  }

  std::string getStringMgrType() override {
    UNREACHABLE();
    return "";
  }

  size_t getNumChunks() override {
    UNREACHABLE();
    return 0;
  }

 protected:
  int db_id_;
  SchemaProviderPtr schema_provider_;
  std::unordered_map<int, TestTableData> tables_;
};

}  // namespace TestHelpers
